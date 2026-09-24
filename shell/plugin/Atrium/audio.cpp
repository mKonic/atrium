#include "audio.hpp"

#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

#include <QRegularExpression>

#include <algorithm>
#include <cstring>

namespace atrium {

namespace {

double fraction(const pa_cvolume& cv) {
    return double(pa_cvolume_max(&cv)) / PA_VOLUME_NORM;
}

QByteArray pack(const pa_cvolume& cv) {
    return QByteArray(reinterpret_cast<const char*>(&cv), sizeof cv);
}

QString prop(pa_proplist* p, const char* key) {
    const char* v = p ? pa_proplist_gets(p, key) : nullptr;
    return v ? QString::fromUtf8(v) : QString();
}

} // namespace

// --- AudioNode ---------------------------------------------------------------

AudioNode::AudioNode(Audio* audio, Kind kind, uint index) : QObject(audio), audio_(audio), kind_(kind), index_(index) {}

void AudioNode::update(const QString& name, const QString& label, const QString& icon, const QByteArray& cvolume,
                       double volume, bool muted) {
    name_ = name;
    label_ = label;
    icon_ = icon;
    cvolume_ = cvolume;
    volume_ = volume;
    muted_ = muted;
    emit changed();
}

bool AudioNode::isDefault() const {
    if (kind_ == Kind::Sink)
        return name_ == audio_->defaultSink();
    if (kind_ == Kind::Source)
        return name_ == audio_->defaultSource();
    return false;
}

QString AudioNode::glyph() const {
    if (kind_ == Kind::Source)
        return QStringLiteral("mic");
    static const QRegularExpression display(QStringLiteral("hdmi|display|dp"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression head(QStringLiteral("headset|headphone|usb"), QRegularExpression::CaseInsensitiveOption);
    if (display.match(name_).hasMatch())
        return QStringLiteral("tv");
    if (head.match(label_).hasMatch())
        return QStringLiteral("headphones");
    return QStringLiteral("speaker");
}

void AudioNode::setVolume(double v) {
    pa_context* c = audio_->context();
    if (!c || cvolume_.size() != sizeof(pa_cvolume))
        return;
    pa_cvolume cv;
    std::memcpy(&cv, cvolume_.constData(), sizeof cv);
    // Every channel scaled together: the balance stays.
    pa_cvolume_scale(&cv, pa_volume_t(std::clamp(v, 0.0, 1.5) * PA_VOLUME_NORM));
    cvolume_ = pack(cv);
    volume_ = fraction(cv);
    emit changed();
    pa_operation* op = nullptr;
    switch (kind_) {
    case Kind::Sink: op = pa_context_set_sink_volume_by_index(c, index_, &cv, nullptr, nullptr); break;
    case Kind::Source: op = pa_context_set_source_volume_by_index(c, index_, &cv, nullptr, nullptr); break;
    case Kind::App: op = pa_context_set_sink_input_volume(c, index_, &cv, nullptr, nullptr); break;
    }
    if (op)
        pa_operation_unref(op);
}

void AudioNode::setMuted(bool m) {
    pa_context* c = audio_->context();
    if (!c)
        return;
    muted_ = m;
    emit changed();
    pa_operation* op = nullptr;
    switch (kind_) {
    case Kind::Sink: op = pa_context_set_sink_mute_by_index(c, index_, m, nullptr, nullptr); break;
    case Kind::Source: op = pa_context_set_source_mute_by_index(c, index_, m, nullptr, nullptr); break;
    case Kind::App: op = pa_context_set_sink_input_mute(c, index_, m, nullptr, nullptr); break;
    }
    if (op)
        pa_operation_unref(op);
}

void AudioNode::makeDefault() {
    pa_context* c = audio_->context();
    if (!c || kind_ == Kind::App)
        return;
    const QByteArray n = name_.toUtf8();
    pa_operation* op = kind_ == Kind::Sink ? pa_context_set_default_sink(c, n.constData(), nullptr, nullptr)
                                           : pa_context_set_default_source(c, n.constData(), nullptr, nullptr);
    if (op)
        pa_operation_unref(op);
}

// --- Audio -------------------------------------------------------------------

Audio* Audio::instance() {
    static auto* self = new Audio;
    return self;
}

Audio::Audio() {
    loop_ = pa_glib_mainloop_new(nullptr);  // GLib's default context: Qt's event loop
    retry_.setSingleShot(true);
    retry_.setInterval(1000);
    connect(&retry_, &QTimer::timeout, this, &Audio::connectToServer);
    connectToServer();
}

void Audio::connectToServer() {
    if (context_) {
        pa_context_disconnect(context_);
        pa_context_unref(context_);
    }
    pa_proplist* p = pa_proplist_new();
    pa_proplist_sets(p, PA_PROP_APPLICATION_NAME, "atrium");
    pa_proplist_sets(p, PA_PROP_APPLICATION_ID, "atrium-shell");
    pa_proplist_sets(p, PA_PROP_APPLICATION_ICON_NAME, "audio-card");
    context_ = pa_context_new_with_proplist(pa_glib_mainloop_get_api(loop_), "atrium", p);
    pa_proplist_free(p);
    pa_context_set_state_callback(context_, [](pa_context*, void* self) { static_cast<Audio*>(self)->onState(); }, this);
    if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOFAIL, nullptr) < 0)
        retry_.start();
}

void Audio::onState() {
    const pa_context_state_t s = pa_context_get_state(context_);
    if (s == PA_CONTEXT_READY) {
        ready_ = true;
        emit readyChanged();
        pa_context_set_subscribe_callback(context_, [](pa_context*, pa_subscription_event_type_t t, uint32_t idx, void* self) {
            static_cast<Audio*>(self)->onEvent(t, idx);
        }, this);
        const auto mask = pa_subscription_mask_t(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE |
                                                 PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SERVER);
        if (pa_operation* op = pa_context_subscribe(context_, mask, nullptr, nullptr))
            pa_operation_unref(op);
        refreshAll();
    } else if (s == PA_CONTEXT_FAILED || s == PA_CONTEXT_TERMINATED) {
        // The sound server restarted (or isn't up yet): start over.
        const bool was = ready_;
        ready_ = false;
        for (AudioNode* n : nodes_)
            n->deleteLater();
        nodes_.clear();
        if (was) {
            emit readyChanged();
            emit nodesChanged();
            emit defaultsChanged();
        }
        retry_.start();
    }
}

void Audio::refreshAll() {
    for (pa_operation* op : {pa_context_get_server_info(context_, &Audio::serverCb, this),
                             pa_context_get_sink_info_list(context_, &Audio::sinkCb, this),
                             pa_context_get_source_info_list(context_, &Audio::sourceCb, this),
                             pa_context_get_sink_input_info_list(context_, &Audio::appCb, this)})
        if (op)
            pa_operation_unref(op);
}

void Audio::onEvent(unsigned type, unsigned index) {
    const unsigned facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    const unsigned what = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    pa_operation* op = nullptr;
    auto kind = AudioNode::Kind::Sink;
    switch (facility) {
    case PA_SUBSCRIPTION_EVENT_SERVER:
        op = pa_context_get_server_info(context_, &Audio::serverCb, this);
        break;
    case PA_SUBSCRIPTION_EVENT_SINK:
        kind = AudioNode::Kind::Sink;
        if (what != PA_SUBSCRIPTION_EVENT_REMOVE)
            op = pa_context_get_sink_info_by_index(context_, index, &Audio::sinkCb, this);
        break;
    case PA_SUBSCRIPTION_EVENT_SOURCE:
        kind = AudioNode::Kind::Source;
        if (what != PA_SUBSCRIPTION_EVENT_REMOVE)
            op = pa_context_get_source_info_by_index(context_, index, &Audio::sourceCb, this);
        break;
    case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
        kind = AudioNode::Kind::App;
        if (what != PA_SUBSCRIPTION_EVENT_REMOVE)
            op = pa_context_get_sink_input_info(context_, index, &Audio::appCb, this);
        break;
    default:
        return;
    }
    if (op)
        pa_operation_unref(op);
    if (what == PA_SUBSCRIPTION_EVENT_REMOVE && facility != PA_SUBSCRIPTION_EVENT_SERVER)
        remove(kind, index);
}

AudioNode* Audio::find(AudioNode::Kind kind, uint index) const {
    for (AudioNode* n : nodes_)
        if (n->kind() == kind && n->index() == index)
            return n;
    return nullptr;
}

AudioNode* Audio::ensure(AudioNode::Kind kind, uint index) {
    if (AudioNode* n = find(kind, index))
        return n;
    auto* n = new AudioNode(this, kind, index);
    nodes_.append(n);
    return n;
}

void Audio::remove(AudioNode::Kind kind, uint index) {
    AudioNode* n = find(kind, index);
    if (!n)
        return;
    nodes_.removeOne(n);
    emit nodesChanged();
    emit defaultsChanged();
    n->deleteLater();
}

void Audio::sinkCb(pa_context*, const pa_sink_info* i, int eol, void* data) {
    auto* self = static_cast<Audio*>(data);
    if (eol || !i)
        return;
    const bool fresh = !self->find(AudioNode::Kind::Sink, i->index);
    AudioNode* n = self->ensure(AudioNode::Kind::Sink, i->index);
    n->update(QString::fromUtf8(i->name), QString::fromUtf8(i->description ? i->description : i->name),
              prop(i->proplist, PA_PROP_DEVICE_ICON_NAME), pack(i->volume), fraction(i->volume), i->mute);
    if (fresh) {
        emit self->nodesChanged();
        emit self->defaultsChanged();
    }
}

void Audio::sourceCb(pa_context*, const pa_source_info* i, int eol, void* data) {
    auto* self = static_cast<Audio*>(data);
    // A sink's monitor is a source too, but no one talks into it.
    if (eol || !i || i->monitor_of_sink != PA_INVALID_INDEX)
        return;
    const bool fresh = !self->find(AudioNode::Kind::Source, i->index);
    AudioNode* n = self->ensure(AudioNode::Kind::Source, i->index);
    n->update(QString::fromUtf8(i->name), QString::fromUtf8(i->description ? i->description : i->name),
              prop(i->proplist, PA_PROP_DEVICE_ICON_NAME), pack(i->volume), fraction(i->volume), i->mute);
    if (fresh) {
        emit self->nodesChanged();
        emit self->defaultsChanged();
    }
}

void Audio::appCb(pa_context*, const pa_sink_input_info* i, int eol, void* data) {
    auto* self = static_cast<Audio*>(data);
    if (eol || !i || !i->has_volume)
        return;
    const bool fresh = !self->find(AudioNode::Kind::App, i->index);
    AudioNode* n = self->ensure(AudioNode::Kind::App, i->index);
    QString label = prop(i->proplist, PA_PROP_APPLICATION_NAME);
    if (label.isEmpty())
        label = QString::fromUtf8(i->name);
    n->update(QString::fromUtf8(i->name), label, prop(i->proplist, PA_PROP_APPLICATION_ICON_NAME), pack(i->volume),
              fraction(i->volume), i->mute);
    if (fresh)
        emit self->nodesChanged();
}

void Audio::serverCb(pa_context*, const pa_server_info* i, void* data) {
    auto* self = static_cast<Audio*>(data);
    if (!i)
        return;
    self->defaultSink_ = QString::fromUtf8(i->default_sink_name ? i->default_sink_name : "");
    self->defaultSource_ = QString::fromUtf8(i->default_source_name ? i->default_source_name : "");
    emit self->defaultsChanged();
    for (AudioNode* n : self->nodes_)
        emit n->changed();  // isDefault
}

AudioNode* Audio::sink() const {
    for (AudioNode* n : nodes_)
        if (n->kind() == AudioNode::Kind::Sink && n->name() == defaultSink_)
            return n;
    return nullptr;
}

AudioNode* Audio::source() const {
    for (AudioNode* n : nodes_)
        if (n->kind() == AudioNode::Kind::Source && n->name() == defaultSource_)
            return n;
    return nullptr;
}

QList<QObject*> Audio::of(AudioNode::Kind kind) const {
    QList<QObject*> out;
    for (AudioNode* n : nodes_)
        if (n->kind() == kind)
            out.append(n);
    return out;
}

QList<QObject*> Audio::outputs() const {
    return of(AudioNode::Kind::Sink);
}

QList<QObject*> Audio::inputs() const {
    return of(AudioNode::Kind::Source);
}

QList<QObject*> Audio::apps() const {
    return of(AudioNode::Kind::App);
}

} // namespace atrium
