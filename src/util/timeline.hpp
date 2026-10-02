#pragma once
// Explicit sync timelines: DRM syncobjs whose points (rising 64-bit values)
// say a buffer's writes or reads are done. After wlroots'
// render/drm_syncobj.c (MIT), whose shape atrium's code was written against.
#include <wayland-server-core.h>

#include <cstddef>
#include <cstdint>

namespace atrium {

struct Timeline {
    int drm_fd = -1;
    uint32_t handle = 0;
    size_t n_refs = 1;
};

Timeline* timeline_create(int drm_fd);
// From a client's drm_syncobj fd.
Timeline* timeline_import(int drm_fd, int syncobj_fd);
Timeline* timeline_ref(Timeline* timeline);
void timeline_unref(Timeline* timeline);
// A drm_syncobj fd for it, -1 on failure.
int timeline_export(Timeline* timeline);
// Whether `point` is signalled (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) or
// has a fence (..._WAIT_AVAILABLE) yet. False if it couldn't tell.
bool timeline_check(Timeline* timeline, uint64_t point, uint32_t flags, bool* result);
bool timeline_signal(Timeline* timeline, uint64_t point);
// `point`'s fence as a sync_file (it must have materialised), -1 on failure.
int timeline_export_sync_file(Timeline* timeline, uint64_t point);
// `point` signals when `sync_file_fd` does.
bool timeline_import_sync_file(Timeline* timeline, uint64_t point, int sync_file_fd);

// Waits on a point from the event loop.
struct TimelineWaiter;
using TimelineReady = void (*)(TimelineWaiter* waiter);
struct TimelineWaiter {
    int ev_fd = -1;
    wl_event_source* event_source = nullptr;
    TimelineReady callback = nullptr;
};
// `callback` runs once `point` is as `flags` asks (see timeline_check).
bool timeline_waiter_init(TimelineWaiter* waiter, Timeline* timeline, uint64_t point, uint32_t flags,
                          wl_event_loop* loop, TimelineReady callback);
void timeline_waiter_finish(TimelineWaiter* waiter);

} // namespace atrium
