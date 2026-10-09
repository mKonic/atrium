#pragma once
// The menu bar's privacy dots, as a Mac's: while an app records from a
// microphone (orange), holds a camera open (green), or shares the screen
// (purple), and which apps. Nothing shows otherwise.
//
//   Privacy.microphone, .camera, .screen: in use now
//   Privacy.uses: [{kind: "microphone"|"camera"|"screen", app, icon, text}]

#include "privacy_core.hpp"

#include <QMap>
#include <QObject>
#include <QTimer>
#include <QVariantList>

struct pa_context;
struct pa_glib_mainloop;
struct pa_source_info;
struct pa_source_output_info;
class QSocketNotifier;

namespace atrium {

class Privacy : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool microphone READ microphone NOTIFY changed)
    Q_PROPERTY(bool camera READ camera NOTIFY changed)
    Q_PROPERTY(bool screen READ screen NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(QVariantList uses READ uses NOTIFY changed)

public:
    static Privacy* instance();

    bool microphone() const { return has(privacy::Kind::Microphone); }
    bool camera() const { return has(privacy::Kind::Camera); }
    bool screen() const { return has(privacy::Kind::Screen); }
    bool active() const { return !uses_.empty(); }
    QVariantList uses() const;

signals:
    void changed();

private:
    Privacy();
    bool has(privacy::Kind k) const;
    void update();

    // Microphones, through the sound server.
    void connectToServer();
    void onState();
    void refreshSound();
    static void sourceCb(pa_context*, const pa_source_info* i, int eol, void* self);
    static void outputCb(pa_context*, const pa_source_output_info* i, int eol, void* self);
    pa_glib_mainloop* loop_ = nullptr;
    pa_context* context_ = nullptr;
    QTimer retry_, soundSettle_;
    QMap<uint32_t, bool> monitors_;  // source → a speaker's monitor
    struct Stream {
        privacy::Capture capture;
        uint32_t source = 0;
    };
    QMap<uint32_t, Stream> streams_, pendingStreams_;
    bool listing_ = false;
    std::vector<std::string> microphones_;

    // Cameras: opened and closed, as inotify sees /dev/video*.
    void watchCameras();
    void scanCameras();
    int inotify_ = -1;
    QSocketNotifier* notifier_ = nullptr;
    QTimer cameraSettle_;
    std::vector<std::string> cameras_;

    std::vector<privacy::Use> uses_;
};

} // namespace atrium
