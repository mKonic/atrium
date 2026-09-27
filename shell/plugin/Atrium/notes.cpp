#include "notes.hpp"

#include "compositor.hpp"
#include "launcher_ranking.hpp"
#include "notes_core.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextDocument>
#include <QUrl>

#include <algorithm>

namespace atrium {

namespace {

constexpr int kHeadBytes = 4096;  // enough of an unnamed note for its first line

QString readHead(const QString& path, qint64 bytes = kHeadBytes) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(bytes)) : QString();
}

} // namespace

// --- Notes ---------------------------------------------------------------------------------

Notes::Notes(QObject* parent) : QObject(parent) {
    connect(Compositor::instance(), &Compositor::settingsChanged, this, &Notes::refresh);
    refresh();
}

QString Notes::folder() const {
    QString f = Compositor::instance()->setting("notes.folder", "~/Documents/Notes").toString().trimmed();
    if (f.isEmpty())
        f = "~/Documents/Notes";
    if (f == "~" || f.startsWith("~/"))
        f.replace(0, 1, QDir::homePath());
    return QDir::cleanPath(f);
}

QString Notes::path(const QString& file) const {
    // Only the folder's own files: a name, never a path out of it.
    if (file.isEmpty() || file.contains('/') || file.startsWith('.'))
        return {};
    return folder() + "/" + file;
}

bool Notes::unnamed(const QString& name) {
    return notes::unnamed(name.toStdString());
}

QString Notes::firstLine(const QString& text) {
    return QString::fromStdString(notes::first_line(text.toStdString()));
}

QString Notes::freeName(const QString& wanted, const QStringList& taken, const QString& self) {
    std::vector<std::string> t;
    for (const QString& n : taken)
        t.push_back(n.toStdString());
    // Plan and plán are one name.
    const auto fold = [](const std::string& s) {
        return QString::fromStdU32String(foldText(QString::fromStdString(s))).toStdString();
    };
    return QString::fromStdString(notes::free_name(wanted.toStdString(), t, self.toStdString(), fold));
}

void Notes::refresh() {
    QDir dir(folder());
    QVariantList out;
    const QFileInfoList files =
        dir.entryInfoList({"*.md"}, QDir::Files | QDir::NoDotAndDotDot, QDir::Time);  // newest first
    for (const QFileInfo& f : files) {
        if (f.isSymLink() || f.fileName().startsWith('.'))
            continue;
        const QString name = f.completeBaseName();
        const QString head = readHead(f.filePath());
        QString title = name;
        if (unnamed(name)) {
            const QString line = firstLine(head);
            if (!line.isEmpty())
                title = line;
        }
        // The first words that aren't the title, for the list.
        QString preview;
        for (const QString& l : head.split('\n')) {
            const QString t = l.trimmed();
            if (!t.isEmpty() && firstLine(t) != title && firstLine(t) != name) {
                preview = firstLine(t);
                break;
            }
        }
        out.push_back(QVariantMap{
            {"file", f.fileName()},
            {"name", name},
            {"title", title},
            {"preview", preview},
            {"modified", QLocale().toString(f.lastModified(), QLocale::ShortFormat)},
        });
    }
    notes_ = out;
    emit changed();
}

QVariantList Notes::search(const QString& query) const {
    const std::u32string q = foldText(query);
    if (q.empty())
        return notes_;
    struct Hit {
        int score;
        QVariant note;
    };
    std::vector<Hit> hits;
    for (const QVariant& v : notes_) {
        const QVariantMap m = v.toMap();
        const auto s = rank::score(q, prepareText(m.value("title").toString()));
        int score = s && rank::passes(*s, rank::query_length(q), rank::Sensitivity::Medium) ? 1000 + *s : 0;
        // The words in the text, all of them, anywhere.
        if (!score) {
            const QString body = readHead(path(m.value("file").toString()), 1 << 20).toCaseFolded();
            const QStringList words = query.toCaseFolded().split(' ', Qt::SkipEmptyParts);
            if (!words.isEmpty() && std::ranges::all_of(words, [&](const QString& w) { return body.contains(w); }))
                score = 1;
        }
        if (score)
            hits.push_back({score, v});
    }
    std::ranges::stable_sort(hits, [](const Hit& a, const Hit& b) { return a.score > b.score; });
    QVariantList out;
    for (const Hit& h : hits)
        out.push_back(h.note);
    return out;
}

QString Notes::create() {
    QDir().mkpath(folder());
    const QStringList taken = QDir(folder()).entryList({"*.md"}, QDir::Files);
    const QString file = freeName("Untitled", taken) + ".md";
    QFile f(path(file));
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return {};
    f.close();
    refresh();
    return file;
}

QString Notes::load(const QString& file) const {
    const QString p = path(file);
    if (p.isEmpty())
        return {};
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

bool Notes::save(const QString& file, const QString& text) {
    const QString p = path(file);
    if (p.isEmpty())
        return false;
    QDir().mkpath(folder());
    QSaveFile f(p);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(text.toUtf8());
    if (!f.commit())
        return false;
    refresh();
    return true;
}

QString Notes::rename(const QString& file, const QString& name) {
    QString wanted = name.trimmed();
    wanted.replace('/', '-');
    while (wanted.startsWith('.'))
        wanted.remove(0, 1);
    if (wanted.isEmpty() || path(file).isEmpty())
        return file;
    const QStringList taken = QDir(folder()).entryList({"*.md"}, QDir::Files);
    const QString target = freeName(wanted, taken, file) + ".md";
    if (target == file || !QFile::rename(path(file), path(target)))
        return file;
    refresh();
    return target;
}

bool Notes::trash(const QString& file) {
    const QString p = path(file);
    if (p.isEmpty() || !QFile::moveToTrash(p))
        return false;
    refresh();
    return true;
}

void Notes::openFolder() const {
    QDir().mkpath(folder());
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder()));
}

QString Notes::titleFor(const QString& file, const QString& text) const {
    const QString name = QFileInfo(file).completeBaseName();
    if (!unnamed(name))
        return name;
    const QString line = firstLine(text);
    return line.isEmpty() ? name : line;
}

QVariantMap Notes::newline(const QString& text, int cursor) const {
    // Offsets cross as UTF-8 bytes into the core and back as characters.
    const QString before = text.left(cursor);
    const std::string utf8 = before.toStdString();
    const notes::Newline n = notes::newline(utf8, utf8.size());
    if (n.kind == notes::Newline::Continue)
        return {{"kind", "continue"}, {"insert", QString::fromStdString(n.insert)}};
    if (n.kind == notes::Newline::EndList)
        return {{"kind", "end"}, {"from", QString::fromStdString(utf8.substr(0, n.line_start)).size()}};
    return {{"kind", "plain"}};
}

// --- MarkdownHighlighter --------------------------------------------------------------

MarkdownHighlighter::MarkdownHighlighter(QObject* parent) : QSyntaxHighlighter(parent) {
    connect(this, &MarkdownHighlighter::colorsChanged, this, [this] {
        if (document())
            rehighlight();
    });
}

void MarkdownHighlighter::setQuickDocument(QQuickTextDocument* doc) {
    if (doc == quick_)
        return;
    quick_ = doc;
    setDocument(doc ? doc->textDocument() : nullptr);
    emit documentChanged();
}

void MarkdownHighlighter::setCursorPosition(int position) {
    if (position == cursor_)
        return;
    cursor_ = position;
    emit cursorPositionChanged();
    if (!document())
        return;
    const QTextBlock now = document()->findBlock(position);
    if (now.blockNumber() == caretBlock_)
        return;
    const QTextBlock before = document()->findBlockByNumber(caretBlock_);
    caretBlock_ = now.blockNumber();
    // Only the two lines whose markers show or hide change.
    if (before.isValid())
        rehighlightBlock(before);
    if (now.isValid())
        rehighlightBlock(now);
}

bool MarkdownHighlighter::onCaretLine() const {
    return currentBlock().blockNumber() == caretBlock_;
}

QTextCharFormat MarkdownHighlighter::hidden() const {
    QTextCharFormat f;
    f.setForeground(Qt::transparent);
    f.setFontPointSize(1);
    f.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    f.setFontLetterSpacing(-1);
    return f;
}

void MarkdownHighlighter::highlightBlock(const QString& text) {
    const bool raw = onCaretLine();
    QTextCharFormat marker;
    marker.setForeground(secondary_);
    auto markerFormat = [&] { return raw ? marker : hidden(); };
    QTextCharFormat mono;
    mono.setFontFamilies({mono_});
    mono.setBackground(codeBackground_);

    // Fenced code: a block state carries "inside" to the next line.
    const bool fence = text.trimmed().startsWith("```");
    if (fence || previousBlockState() == 1) {
        setFormat(0, int(text.size()), mono);
        if (fence)
            setFormat(0, int(text.size()), raw ? marker : QTextCharFormat(mono));
        setCurrentBlockState(fence ? (previousBlockState() == 1 ? 0 : 1) : 1);
        return;
    }
    setCurrentBlockState(0);

    // Headings.
    static const QRegularExpression heading("^(#{1,6})\\s+");
    if (auto m = heading.match(text); m.hasMatch()) {
        static const qreal scale[] = {1.7, 1.45, 1.25, 1.1, 1.0, 1.0};
        QTextCharFormat h;
        h.setFontWeight(QFont::Bold);
        h.setFontPointSize(baseSize_ * scale[m.capturedLength(1) - 1]);
        setFormat(0, int(text.size()), h);
        setFormat(0, int(m.capturedLength()), markerFormat());
    }
    // A quote, a list item, a task, a rule.
    static const QRegularExpression quote("^\\s*>\\s?");
    if (auto m = quote.match(text); m.hasMatch()) {
        QTextCharFormat q;
        q.setFontItalic(true);
        q.setForeground(secondary_);
        setFormat(0, int(text.size()), q);
        setFormat(0, int(m.capturedLength()), marker);
    }
    static const QRegularExpression list("^\\s*([-*+]|\\d+\\.)\\s+(\\[[ xX]\\]\\s+)?");
    if (auto m = list.match(text); m.hasMatch()) {
        QTextCharFormat bullet;
        bullet.setForeground(accent_);
        bullet.setFontWeight(QFont::DemiBold);
        setFormat(int(m.capturedStart(1)), int(m.capturedLength(1)), bullet);
        if (m.capturedLength(2)) {
            setFormat(int(m.capturedStart(2)), int(m.capturedLength(2)), bullet);
            // A ticked task reads done.
            if (m.captured(2).contains('x', Qt::CaseInsensitive)) {
                QTextCharFormat done;
                done.setFontStrikeOut(true);
                done.setForeground(secondary_);
                setFormat(int(m.capturedLength()), int(text.size() - m.capturedLength()), done);
            }
        }
    }
    static const QRegularExpression rule("^\\s*([-*_])\\1{2,}\\s*$");
    if (rule.match(text).hasMatch())
        setFormat(0, int(text.size()), marker);

    // Inline: `code`, **bold**, *italic*, ~~struck~~, [links](url).
    auto apply = [&](const QRegularExpression& re, int markerLen, const std::function<void(QTextCharFormat&)>& style) {
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            const int start = int(m.capturedStart()), len = int(m.capturedLength());
            for (int i = start + markerLen; i < start + len - markerLen; ++i) {
                QTextCharFormat f = format(i);
                style(f);
                setFormat(i, 1, f);
            }
            setFormat(start, markerLen, markerFormat());
            setFormat(start + len - markerLen, markerLen, markerFormat());
        }
    };
    static const QRegularExpression code("`[^`\\n]+`");
    static const QRegularExpression bold("(\\*\\*|__)(?=\\S)(.+?)(?<=\\S)\\1");
    static const QRegularExpression italic("(?<![*_\\w])([*_])(?=\\S)([^*_]+?)(?<=\\S)\\1(?![*_\\w])");
    static const QRegularExpression strike("~~(?=\\S)(.+?)(?<=\\S)~~");
    apply(bold, 2, [](QTextCharFormat& f) { f.setFontWeight(QFont::Bold); });
    apply(italic, 1, [](QTextCharFormat& f) { f.setFontItalic(true); });
    apply(strike, 2, [](QTextCharFormat& f) { f.setFontStrikeOut(true); });
    apply(code, 1, [&](QTextCharFormat& f) {
        f.setFontFamilies({mono_});
        f.setBackground(codeBackground_);
    });
    static const QRegularExpression link("\\[([^\\]]+)\\]\\(([^)]*)\\)");
    auto it = link.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        QTextCharFormat l;
        l.setForeground(accent_);
        l.setFontUnderline(true);
        setFormat(int(m.capturedStart(1)), int(m.capturedLength(1)), l);
        setFormat(int(m.capturedStart()), 1, markerFormat());
        setFormat(int(m.capturedEnd(1)), int(m.capturedEnd() - m.capturedEnd(1)), markerFormat());
    }
}

} // namespace atrium
