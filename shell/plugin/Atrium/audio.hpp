#pragma once
// Sound devices and app volumes, through the PulseAudio API (PipeWire's
// pipewire-pulse serves it): `Audio.sink`, `Audio.outputs`, `Audio.apps`.
// Volumes are fractions on the cubic scale every mixer shows (1 = 100%),
// and setting one keeps the channels' balance.

#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariant>

struct pa_context;
struct pa_glib_mainloop;
struct pa_sink_info;
struct pa_source_info;
struct pa_sink_input_info;
struct pa_server_info;

namespace atrium {

class Audio;

class AudioNode : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name NOTIFY changed)         // the device's system name
    Q_PROPERTY(QString label READ label NOTIFY changed)       // what to call it
    Q_PROPERTY(QString glyph READ glyph NOTIFY changed)       // Material Symbols name
    Q_PROPERTY(QString icon READ icon NOTIFY changed)         // an app's icon name
    Q_PROPERTY(bool isSink READ isSink CONSTANT)
    Q_PROPERTY(bool isStream READ isStream CONSTANT)
    Q_PROPERTY(bool isDefault READ isDefault NOTIFY changed)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY changed)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY changed)

public:
    enum class Kind { Sink, Source, App };

    AudioNode(Audio* audio, Kind kind, uint index);

    Kind kind() const { return kind_; }
    uint index() const { return index_; }
    QString name() const { return name_; }
    QString label() const { return label_; }
    QString glyph() const;
    QString icon() const { return icon_; }
    bool isSink() const { return kind_ != Kind::Source; }
    bool isStream() const { return kind_ == Kind::App; }
    bool isDefault() const;
    double volume() const { return volume_; }
    void setVolume(double v);
    bool muted() const { return muted_; }
    void setMuted(bool m);

    Q_INVOKABLE void makeDefault();

    // From the server.
    void update(const QString& name, const QString& label, const QString& icon, const QByteArray& cvolume,
                double volume, bool muted);

signals:
    void changed();

private:
    Audio* audio_;
    Kind kind_;
    uint index_;
    QString name_, label_, icon_;
    QByteArray cvolume_;  // the server's pa_cvolume, to scale keeping balance
    double volume_ = 0;
    bool muted_ = false;
};

class Audio : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(atrium::AudioNode* sink READ sink NOTIFY defaultsChanged)
    Q_PROPERTY(atrium::AudioNode* source READ source NOTIFY defaultsChanged)
    Q_PROPERTY(QList<QObject*> outputs READ outputs NOTIFY nodesChanged)
    Q_PROPERTY(QList<QObject*> inputs READ inputs NOTIFY nodesChanged)
    Q_PROPERTY(QList<QObject*> apps READ apps NOTIFY nodesChanged)

public:
    static Audio* instance();

    bool ready() const { return ready_; }
    AudioNode* sink() const;
    AudioNode* source() const;
    QList<QObject*> outputs() const;
    QList<QObject*> inputs() const;
    QList<QObject*> apps() const;

    QString defaultSink() const { return defaultSink_; }
    QString defaultSource() const { return defaultSource_; }
    pa_context* context() const { return ready_ ? context_ : nullptr; }

signals:
    void readyChanged();
    void defaultsChanged();
    void nodesChanged();

private:
    Audio();
    void connectToServer();
    void onState();
    void refreshAll();
    void onEvent(unsigned type, unsigned index);
    AudioNode* find(AudioNode::Kind kind, uint index) const;
    AudioNode* ensure(AudioNode::Kind kind, uint index);
    void remove(AudioNode::Kind kind, uint index);
    QList<QObject*> of(AudioNode::Kind kind) const;

    static void sinkCb(pa_context*, const pa_sink_info* i, int eol, void* self);
    static void sourceCb(pa_context*, const pa_source_info* i, int eol, void* self);
    static void appCb(pa_context*, const pa_sink_input_info* i, int eol, void* self);
    static void serverCb(pa_context*, const pa_server_info* i, void* self);

    pa_glib_mainloop* loop_ = nullptr;
    pa_context* context_ = nullptr;
    bool ready_ = false;
    QList<AudioNode*> nodes_;
    QString defaultSink_, defaultSource_;
    QTimer retry_;
};

} // namespace atrium
