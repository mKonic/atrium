#pragma once
// The session's clipboard through ext-data-control-v1: what gets copied
// (by any app, focused or not), and putting a clip in it. Text and
// pictures; a password manager's copies (x-kde-passwordManagerHint) are
// left alone.

#include "clipsync_core.hpp"

#include <QObject>
#include <QSocketNotifier>

#include <memory>
#include <optional>
#include <vector>

struct wl_display;
struct wl_registry;
struct wl_seat;
struct ext_data_control_manager_v1;
struct ext_data_control_device_v1;
struct ext_data_control_offer_v1;
struct ext_data_control_source_v1;
struct wl_registry_listener;
struct ext_data_control_device_v1_listener;
struct ext_data_control_offer_v1_listener;
struct ext_data_control_source_v1_listener;

namespace atrium::clipsync {

class WaylandClipboard : public QObject {
    Q_OBJECT

public:
    explicit WaylandClipboard(QObject* parent = nullptr);
    ~WaylandClipboard() override;

    // Connected, with a seat and the protocol.
    bool ok() const { return device_ != nullptr; }
    // Makes `clip` the clipboard. Not reported back as copied().
    void set(const Clip& clip);
    // The clipboard's clip, as last read or set (empty until then).
    const std::optional<Clip>& last() const { return last_; }

signals:
    // Something new was copied (the clip's time is now).
    void copied(const atrium::clipsync::Clip& clip);

private:
    struct Offer {
        ext_data_control_offer_v1* offer;
        std::vector<std::string> mimes;
    };
    struct Read;
    struct Write;

    static const ::ext_data_control_device_v1_listener deviceListener;
    static const ::ext_data_control_offer_v1_listener offerListener;
    static const ::ext_data_control_source_v1_listener sourceListener;
    static const ::wl_registry_listener registryListener;

    void dispatch();
    void flush();
    void selection(ext_data_control_offer_v1* offer);
    Offer* find(ext_data_control_offer_v1* offer);
    void dropOffer(ext_data_control_offer_v1* offer);

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_seat* seat_ = nullptr;
    ext_data_control_manager_v1* manager_ = nullptr;
    ext_data_control_device_v1* device_ = nullptr;
    std::unique_ptr<QSocketNotifier> notifier_;
    std::vector<Offer> offers_;
    // The clipboard is ours (set()) until the source is cancelled.
    ext_data_control_source_v1* source_ = nullptr;
    Clip sourceClip_;
    std::optional<Clip> last_;
    bool first_ = true;  // the selection already there when we connected
    std::unique_ptr<Read> read_;  // one at a time; a newer copy replaces it
};

} // namespace atrium::clipsync
