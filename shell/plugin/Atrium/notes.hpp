#pragma once
// Notes, after Tinycast's: plain Markdown files in one folder (notes.folder,
// ~/Documents/Notes unless Settings says otherwise), one file one note, its
// name its title. A note still called "Untitled" shows its first line
// instead. No database, no index: the folder is the source and is listed
// each time it's asked. `Notes {}` lists; the Notes app edits.

#include <QObject>
#include <QQuickTextDocument>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVariant>

namespace atrium {

class Notes : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString folder READ folder NOTIFY changed)
    // [{ file, title, name, modified, preview }], newest first; `title` is
    // what to show (the first line for an unnamed note), `name` the file's.
    Q_PROPERTY(QVariantList notes READ notes NOTIFY changed)

public:
    explicit Notes(QObject* parent = nullptr);

    QString folder() const;
    QVariantList notes() const { return notes_; }

    Q_INVOKABLE void refresh();
    // The ones matching `query` in their title or text, best first.
    Q_INVOKABLE QVariantList search(const QString& query) const;
    // A new empty note ("Untitled", "Untitled 2", ...); its file.
    Q_INVOKABLE QString create();
    Q_INVOKABLE QString load(const QString& file) const;
    Q_INVOKABLE bool save(const QString& file, const QString& text);
    // A new name; the file it is now (the same on failure).
    Q_INVOKABLE QString rename(const QString& file, const QString& name);
    Q_INVOKABLE bool trash(const QString& file);
    Q_INVOKABLE void openFolder() const;
    // What a note shows as its title: its name, or for an unnamed one its first line.
    Q_INVOKABLE QString titleFor(const QString& file, const QString& text) const;
    // Enter pressed at `cursor` in `text`: { kind: "plain" | "continue" |
    // "end", insert, from } (a list carried on, or ended by an empty item).
    Q_INVOKABLE QVariantMap newline(const QString& text, int cursor) const;

    // Pure rules, public for the tests.
    static bool unnamed(const QString& name);
    static QString firstLine(const QString& text);
    static QString freeName(const QString& wanted, const QStringList& taken, const QString& self = {});

signals:
    void changed();

private:
    QString path(const QString& file) const;
    QVariantList notes_;
};

// Markdown drawn as it is written: headings larger, **bold**, *italic*,
// `code`, quotes, lists, links, fences. Off the caret's line the markers
// fade to nothing, so a note reads rendered and edits raw.
class MarkdownHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
    Q_PROPERTY(QQuickTextDocument* document READ quickDocument WRITE setQuickDocument NOTIFY documentChanged)
    Q_PROPERTY(int cursorPosition READ cursorPosition WRITE setCursorPosition NOTIFY cursorPositionChanged)
    Q_PROPERTY(QColor text MEMBER text_ NOTIFY colorsChanged)
    Q_PROPERTY(QColor secondary MEMBER secondary_ NOTIFY colorsChanged)
    Q_PROPERTY(QColor accent MEMBER accent_ NOTIFY colorsChanged)
    Q_PROPERTY(QColor codeBackground MEMBER codeBackground_ NOTIFY colorsChanged)
    Q_PROPERTY(QString monoFamily MEMBER mono_ NOTIFY colorsChanged)
    Q_PROPERTY(qreal baseSize MEMBER baseSize_ NOTIFY colorsChanged)

public:
    explicit MarkdownHighlighter(QObject* parent = nullptr);

    QQuickTextDocument* quickDocument() const { return quick_; }
    void setQuickDocument(QQuickTextDocument* doc);
    int cursorPosition() const { return cursor_; }
    void setCursorPosition(int position);

signals:
    void documentChanged();
    void cursorPositionChanged();
    void colorsChanged();

protected:
    void highlightBlock(const QString& text) override;

private:
    QTextCharFormat hidden() const;
    bool onCaretLine() const;

    QQuickTextDocument* quick_ = nullptr;
    int cursor_ = -1;
    int caretBlock_ = -1;
    QColor text_{Qt::black}, secondary_{Qt::gray}, accent_{Qt::blue}, codeBackground_{240, 240, 240};
    QString mono_ = QStringLiteral("monospace");
    qreal baseSize_ = 13;
};

} // namespace atrium
