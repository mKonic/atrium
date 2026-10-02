#include "util/timeline.hpp"

#include "util/log.hpp"

#include <xf86drm.h>

#include <cassert>
#include <cerrno>
#include <sys/eventfd.h>
#include <unistd.h>

namespace atrium {

namespace {

Timeline* wrap(int drm_fd, uint32_t handle) {
    auto* t = new Timeline;
    t->drm_fd = drm_fd;
    t->handle = handle;
    return t;
}

// A binary syncobj for moving one point in or out.
struct Temp {
    int fd;
    uint32_t handle = 0;
    bool ok;
    explicit Temp(int drm_fd) : fd(drm_fd), ok(drmSyncobjCreate(drm_fd, 0, &handle) == 0) {
        if (!ok)
            alog_errno(Log::Error, "drmSyncobjCreate failed");
    }
    ~Temp() {
        if (ok)
            drmSyncobjDestroy(fd, handle);
    }
};

} // namespace

Timeline* timeline_create(int drm_fd) {
    uint32_t handle = 0;
    if (drmSyncobjCreate(drm_fd, 0, &handle) != 0) {
        alog_errno(Log::Error, "drmSyncobjCreate failed");
        return nullptr;
    }
    return wrap(drm_fd, handle);
}

Timeline* timeline_import(int drm_fd, int syncobj_fd) {
    uint32_t handle = 0;
    if (drmSyncobjFDToHandle(drm_fd, syncobj_fd, &handle) != 0) {
        alog_errno(Log::Error, "drmSyncobjFDToHandle failed");
        return nullptr;
    }
    return wrap(drm_fd, handle);
}

Timeline* timeline_ref(Timeline* timeline) {
    ++timeline->n_refs;
    return timeline;
}

void timeline_unref(Timeline* timeline) {
    if (!timeline)
        return;
    assert(timeline->n_refs > 0);
    if (--timeline->n_refs > 0)
        return;
    drmSyncobjDestroy(timeline->drm_fd, timeline->handle);
    delete timeline;
}

int timeline_export(Timeline* timeline) {
    int fd = -1;
    if (drmSyncobjHandleToFD(timeline->drm_fd, timeline->handle, &fd) != 0) {
        alog_errno(Log::Error, "drmSyncobjHandleToFD failed");
        return -1;
    }
    return fd;
}

bool timeline_check(Timeline* timeline, uint64_t point, uint32_t flags, bool* result) {
    const int ret = drmSyncobjTimelineWait(timeline->drm_fd, &timeline->handle, &point, 1, 0, flags, nullptr);
    if (ret != 0 && ret != -ETIME) {
        alog_errno(Log::Error, "drmSyncobjTimelineWait failed");
        return false;
    }
    *result = ret == 0;
    return true;
}

bool timeline_signal(Timeline* timeline, uint64_t point) {
    if (drmSyncobjTimelineSignal(timeline->drm_fd, &timeline->handle, &point, 1) != 0) {
        alog_errno(Log::Error, "drmSyncobjTimelineSignal failed");
        return false;
    }
    return true;
}

int timeline_export_sync_file(Timeline* timeline, uint64_t point) {
    Temp tmp(timeline->drm_fd);
    if (!tmp.ok)
        return -1;
    if (drmSyncobjTransfer(timeline->drm_fd, tmp.handle, 0, timeline->handle, point, 0) != 0) {
        alog_errno(Log::Error, "drmSyncobjTransfer failed");
        return -1;
    }
    int fd = -1;
    if (drmSyncobjExportSyncFile(timeline->drm_fd, tmp.handle, &fd) != 0) {
        alog_errno(Log::Error, "drmSyncobjExportSyncFile failed");
        return -1;
    }
    return fd;
}

bool timeline_import_sync_file(Timeline* timeline, uint64_t point, int sync_file_fd) {
    Temp tmp(timeline->drm_fd);
    if (!tmp.ok)
        return false;
    if (drmSyncobjImportSyncFile(timeline->drm_fd, tmp.handle, sync_file_fd) != 0) {
        alog_errno(Log::Error, "drmSyncobjImportSyncFile failed");
        return false;
    }
    if (drmSyncobjTransfer(timeline->drm_fd, timeline->handle, point, tmp.handle, 0, 0) != 0) {
        alog_errno(Log::Error, "drmSyncobjTransfer failed");
        return false;
    }
    return true;
}

bool timeline_waiter_init(TimelineWaiter* waiter, Timeline* timeline, uint64_t point, uint32_t flags,
                          wl_event_loop* loop, TimelineReady callback) {
    assert(callback);
    const int ev_fd = eventfd(0, EFD_CLOEXEC);
    if (ev_fd < 0) {
        alog_errno(Log::Error, "eventfd failed");
        return false;
    }
    drm_syncobj_eventfd arg{};
    arg.handle = timeline->handle;
    arg.flags = flags;
    arg.point = point;
    arg.fd = ev_fd;
    if (drmIoctl(timeline->drm_fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &arg) != 0) {
        alog_errno(Log::Error, "DRM_IOCTL_SYNCOBJ_EVENTFD failed");
        close(ev_fd);
        return false;
    }
    auto ready = [](int fd, uint32_t mask, void* data) {
        auto* w = static_cast<TimelineWaiter*>(data);
        if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
            alog(Log::Error, "timeline wait: eventfd error");
        if (mask & WL_EVENT_READABLE) {
            uint64_t v;
            if (read(fd, &v, sizeof v) <= 0)
                alog(Log::Error, "timeline wait: read failed");
        }
        w->callback(w);
        return 0;
    };
    wl_event_source* source = wl_event_loop_add_fd(loop, ev_fd, WL_EVENT_READABLE, ready, waiter);
    if (!source) {
        alog(Log::Error, "timeline wait: can't watch the eventfd");
        close(ev_fd);
        return false;
    }
    *waiter = {ev_fd, source, callback};
    return true;
}

void timeline_waiter_finish(TimelineWaiter* waiter) {
    wl_event_source_remove(waiter->event_source);
    close(waiter->ev_fd);
}

} // namespace atrium
