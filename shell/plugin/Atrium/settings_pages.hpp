#pragma once
// The Settings app's table of contents: the compositor's settings grouped
// into pages in a sensible order, each with its icon and colour as System
// Settings has them, and search across all of them. `SettingsPages`.

#include <QObject>
#include <QVariant>

namespace atrium {

class SettingsPages : public QObject {
    Q_OBJECT
    // [{ name, icon, color, special }] in sidebar order; special pages
    // ("About") have their own view rather than generated rows.
    Q_PROPERTY(QVariantList pages READ pages NOTIFY changed)
    // page name → its settings, in schema order
    Q_PROPERTY(QVariantMap byPage READ byPage NOTIFY changed)

public:
    explicit SettingsPages(QObject* parent = nullptr);

    QVariantList pages() const { return pages_; }
    QVariantMap byPage() const { return byPage_; }

    // Settings whose title, description or page match `query`, best first.
    Q_INVOKABLE QVariantList search(const QString& query) const;

    // A key press as a shortcut is written ("Mod+Shift+E"): `modifier` is
    // the shortcut modifier's name ("super", "alt"), which becomes "Mod".
    // "" for a press of a modifier alone.
    // A choice setting's values as a dropdown's [{value, label}]: "right_alt"
    // as "Right Alt".
    Q_INVOKABLE QVariantList choiceOptions(const QStringList& choices) const;
    Q_INVOKABLE QString chord(int key, int modifiers, const QString& modifier) const;

    // Extra mouse buttons (pointer.buttons): a pressed Qt button as evdev
    // (0 for none), its name, what each can do, and the list with one
    // button's remap set or taken out.
    Q_INVOKABLE int evdevButton(int qtButton) const;
    Q_INVOKABLE QString buttonName(int code) const;
    Q_INVOKABLE QVariantList remapActions() const;  // [{value, label}]
    Q_INVOKABLE QString remapAction(const QVariantMap& remap) const;  // its value among them
    Q_INVOKABLE QVariantList remapWith(const QVariantList& remaps, int button, const QString& action, const QString& keys = {}) const;
    Q_INVOKABLE QVariantList remapWithout(const QVariantList& remaps, int button) const;

signals:
    void changed();

private:
    void rebuild();

    QVariantList pages_;
    QVariantMap byPage_;
};

} // namespace atrium
