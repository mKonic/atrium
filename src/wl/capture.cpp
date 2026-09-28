#include "wl/capture.hpp"

#include "wl/buffer.hpp"
#include "wl/output.hpp"
#include "wl/shm.hpp"

#include "ext-image-capture-source-v1-server.hpp"
#include "ext-image-copy-capture-v1-server.hpp"
#include "hyprland-toplevel-export-v1-server.hpp"
#include "wlr-export-dmabuf-unstable-v1-server.hpp"
#include "wlr-screencopy-unstable-v1-server.hpp"

#include <unistd.h>

#include <algorithm>

namespace atrium::wl {

namespace {

template <class List>
void detach_all(List& list) {
    for (auto& w : list)
        if (w)
            w->detach();
}

void split_time(const timespec& t, uint32_t* hi, uint32_t* lo, uint32_t* nsec) {
    const uint64_t sec = uint64_t(t.tv_sec);
    *hi = uint32_t(sec >> 32);
    *lo = uint32_t(sec);
    *nsec = uint32_t(t.tv_nsec);
}

// A capture source (ext_image_capture_source_v1) and what it points at.
class Source : public ExtImageCaptureSourceV1 {
public:
    Source(wl_client* client, uint32_t version, uint32_t id, Capture::Target t)
        : ExtImageCaptureSourceV1(client, version, id), target(std::move(t)) {}
    Capture::Target target;
};

// The frame-style protocols (wlr screencopy and Hyprland's toplevel export)
// announce their buffer, then copy once.
template <class Frame>
void announce(Frame* f, const Capture::Constraints& c) {
    f->send_buffer(Shm::from_drm(c.shm_format), uint32_t(c.width), uint32_t(c.height), c.shm_stride);
    if (f->version() >= 3 || std::is_same_v<Frame, HyprlandToplevelExportFrameV1>) {
        if (c.dmabuf_format)
            f->send_linux_dmabuf(*c.dmabuf_format, uint32_t(c.width), uint32_t(c.height));
        f->send_buffer_done();
    }
}

// The buffer a client gave fits the constraints.
bool fits(wl_resource* buffer_res, const Capture::Constraints& c, wlr_buffer** out) {
    ClientBuffer* b = ClientBuffer::from(buffer_res);
    if (!b || b->width() != c.width || b->height() != c.height)
        return false;
    *out = b->buffer();
    return true;
}

} // namespace

// ext_image_copy_capture_session_v1, with the frame being captured.
struct Capture::Session {
    Target target;
    Weak<ExtImageCopyCaptureSessionV1> resource;
    Weak<ExtImageCopyCaptureFrameV1> frame;
    std::optional<Constraints> sent;
    bool stopped = false;
};

struct Capture::CursorSession {
    Target target;
    Weak<ExtImageCopyCaptureCursorSessionV1> resource;
    bool inside = false;
    std::pair<int, int> hotspot{-1, -1};
};

Capture::Capture(wl_display* display, Seat& seat, ForeignToplevels& toplevels)
    : display_(display), seat_(seat), toplevels_(toplevels) {
    // ---- wlr-screencopy ----
    screencopy_ = Global::create<ZwlrScreencopyManagerV1>(display, 3, [this](wl_client* client, uint32_t version,
                                                                            uint32_t id) {
        auto* m = make<ZwlrScreencopyManagerV1>(client, version, id);
        if (!m)
            return;
        auto start = [this](ZwlrScreencopyManagerV1* self, uint32_t id, int32_t cursor, wl_resource* output_res,
                            std::optional<Box> region) {
            auto* f = make<ZwlrScreencopyFrameV1>(self->client(), self->version(), id);
            if (!f)
                return;
            Output* o = Output::from(output_res);
            Target t{o, nullptr, region, cursor != 0};
            std::optional<Constraints> c = o && constraints ? constraints(t) : std::nullopt;
            if (!c || (region && (region->width <= 0 || region->height <= 0))) {
                f->send_failed();
                f->detach();
                return;
            }
            announce(f, *c);
            auto used = std::make_shared<bool>(false);
            auto do_copy = [this, t, c, used](ZwlrScreencopyFrameV1* self, wl_resource* buffer_res, bool damage) {
                wlr_buffer* buffer = nullptr;
                if (std::exchange(*used, true)) {
                    self->post_error(uint32_t(ZwlrScreencopyFrameV1::Error::AlreadyUsed), "the frame was already used");
                    return;
                }
                if (!fits(buffer_res, *c, &buffer)) {
                    self->post_error(uint32_t(ZwlrScreencopyFrameV1::Error::InvalidBuffer),
                                     "the buffer doesn't match what was offered");
                    return;
                }
                Weak<ZwlrScreencopyFrameV1> w = self;
                Copy cp{t, buffer, damage, [w, damage](const Result& r) {
                            auto* f = w.get();
                            if (!f)
                                return;
                            if (!r.ok) {
                                f->send_failed();
                                return;
                            }
                            f->send_flags(r.y_invert ? 1 : 0);
                            if (damage && f->version() >= 2)
                                for (const Box& b : r.damage)
                                    f->send_damage(uint32_t(b.x), uint32_t(b.y), uint32_t(b.width), uint32_t(b.height));
                            uint32_t hi, lo, ns;
                            split_time(r.when, &hi, &lo, &ns);
                            f->send_ready(hi, lo, ns);
                        }};
                if (copy.empty())
                    cp.done(Result{});
                else
                    copy.emit(cp);
            };
            f->on_copy([do_copy](ZwlrScreencopyFrameV1* self, wl_resource* b) { do_copy(self, b, false); });
            f->on_copy_with_damage([do_copy](ZwlrScreencopyFrameV1* self, wl_resource* b) { do_copy(self, b, true); });
            std::erase_if(frames_, [](const auto& w) { return !w; });
            frames_.push_back(f);
        };
        m->on_capture_output([start](ZwlrScreencopyManagerV1* self, uint32_t id, int32_t cursor, wl_resource* output) {
            start(self, id, cursor, output, std::nullopt);
        });
        m->on_capture_output_region([start](ZwlrScreencopyManagerV1* self, uint32_t id, int32_t cursor,
                                            wl_resource* output, int32_t x, int32_t y, int32_t w, int32_t h) {
            start(self, id, cursor, output, Box{x, y, w, h});
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });

    // ---- hyprland-toplevel-export ----
    hypr_export_ = Global::create<HyprlandToplevelExportManagerV1>(display, 2, [this](wl_client* client,
                                                                                      uint32_t version, uint32_t id) {
        auto* m = make<HyprlandToplevelExportManagerV1>(client, version, id);
        if (!m)
            return;
        auto start = [this](HyprlandToplevelExportManagerV1* self, uint32_t id, int32_t cursor,
                            ForeignToplevels::Handle* h) {
            auto* f = make<HyprlandToplevelExportFrameV1>(self->client(), self->version(), id);
            if (!f)
                return;
            Target t{nullptr, h, std::nullopt, cursor != 0};
            std::optional<Constraints> c = h && constraints ? constraints(t) : std::nullopt;
            if (!c) {
                f->send_failed();
                f->detach();
                return;
            }
            announce(f, *c);
            auto used = std::make_shared<bool>(false);
            f->on_copy([this, t, c, used](HyprlandToplevelExportFrameV1* self, wl_resource* buffer_res,
                                          int32_t ignore_damage) {
                wlr_buffer* buffer = nullptr;
                if (std::exchange(*used, true)) {
                    self->post_error(uint32_t(HyprlandToplevelExportFrameV1::Error::AlreadyUsed),
                                     "the frame was already used");
                    return;
                }
                if (!fits(buffer_res, *c, &buffer)) {
                    self->post_error(uint32_t(HyprlandToplevelExportFrameV1::Error::InvalidBuffer),
                                     "the buffer doesn't match what was offered");
                    return;
                }
                Weak<HyprlandToplevelExportFrameV1> w = self;
                Copy cp{t, buffer, !ignore_damage, [w](const Result& r) {
                            auto* f = w.get();
                            if (!f)
                                return;
                            if (!r.ok) {
                                f->send_failed();
                                return;
                            }
                            f->send_flags(r.y_invert ? 1 : 0);
                            for (const Box& b : r.damage)
                                f->send_damage(uint32_t(b.x), uint32_t(b.y), uint32_t(b.width), uint32_t(b.height));
                            uint32_t hi, lo, ns;
                            split_time(r.when, &hi, &lo, &ns);
                            f->send_ready(hi, lo, ns);
                        }};
                if (copy.empty())
                    cp.done(Result{});
                else
                    copy.emit(cp);
            });
            std::erase_if(frames_, [](const auto& w) { return !w; });
            frames_.push_back(f);
        };
        // Hyprland's own window addresses mean nothing here.
        m->on_capture_toplevel([start](HyprlandToplevelExportManagerV1* self, uint32_t id, int32_t cursor, uint32_t) {
            start(self, id, cursor, nullptr);
        });
        m->on_capture_toplevel_with_wlr_toplevel_handle([this, start](HyprlandToplevelExportManagerV1* self,
                                                                      uint32_t id, int32_t cursor,
                                                                      wl_resource* handle) {
            start(self, id, cursor, toplevels_.from(handle));
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });

    // ---- wlr-export-dmabuf ----
    export_dmabuf_ = Global::create<ZwlrExportDmabufManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                                  uint32_t id) {
        auto* m = make<ZwlrExportDmabufManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_capture_output([this](ZwlrExportDmabufManagerV1* self, uint32_t id, int32_t cursor,
                                    wl_resource* output_res) {
            auto* f = make<ZwlrExportDmabufFrameV1>(self->client(), self->version(), id);
            if (!f)
                return;
            Output* o = Output::from(output_res);
            if (!o || export_frame.empty()) {
                f->send_cancel(uint32_t(ZwlrExportDmabufFrameV1::CancelReason::Permanent));
                f->detach();
                return;
            }
            Weak<ZwlrExportDmabufFrameV1> w = f;
            Export e{o, cursor != 0, [w](const wlr_dmabuf_attributes* a, const timespec& when) {
                         auto* f = w.get();
                         if (!f)
                             return;
                         if (!a) {
                             f->send_cancel(uint32_t(ZwlrExportDmabufFrameV1::CancelReason::Temporary));
                             return;
                         }
                         f->send_frame(uint32_t(a->width), uint32_t(a->height), 0, 0, 0, 1, a->format,
                                       uint32_t(a->modifier >> 32), uint32_t(a->modifier), uint32_t(a->n_planes));
                         for (int i = 0; i < a->n_planes; ++i) {
                             const off_t size = lseek(a->fd[i], 0, SEEK_END);
                             f->send_object(uint32_t(i), a->fd[i], uint32_t(size > 0 ? size : 0), a->offset[i],
                                            a->stride[i], uint32_t(i));
                         }
                         uint32_t hi, lo, ns;
                         split_time(when, &hi, &lo, &ns);
                         f->send_ready(hi, lo, ns);
                     }};
            export_frame.emit(e);
            std::erase_if(frames_, [](const auto& x) { return !x; });
            frames_.push_back(f);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });

    // ---- ext-image-capture-source ----
    output_sources_ = Global::create<ExtOutputImageCaptureSourceManagerV1>(display, 1, [this](wl_client* client,
                                                                                              uint32_t version,
                                                                                              uint32_t id) {
        auto* m = make<ExtOutputImageCaptureSourceManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_create_source([this](ExtOutputImageCaptureSourceManagerV1* self, uint32_t id, wl_resource* output) {
            if (auto* s = make<Source>(self->client(), self->version(), id, Target{Output::from(output)})) {
                std::erase_if(sources_, [](const auto& w) { return !w; });
                sources_.push_back(s);
            }
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
    toplevel_sources_ = Global::create<ExtForeignToplevelImageCaptureSourceManagerV1>(display, 1, [this](
                                                                                           wl_client* client,
                                                                                           uint32_t version,
                                                                                           uint32_t id) {
        auto* m = make<ExtForeignToplevelImageCaptureSourceManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_create_source([this](ExtForeignToplevelImageCaptureSourceManagerV1* self, uint32_t id,
                                   wl_resource* handle) {
            Target t;
            t.toplevel = toplevels_.from(handle);
            if (auto* s = make<Source>(self->client(), self->version(), id, t)) {
                std::erase_if(sources_, [](const auto& w) { return !w; });
                sources_.push_back(s);
            }
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });

    // ---- ext-image-copy-capture ----
    image_copy_ = Global::create<ExtImageCopyCaptureManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                                  uint32_t id) {
        auto* m = make<ExtImageCopyCaptureManagerV1>(client, version, id);
        if (!m)
            return;
        auto make_session = [this](wl_client* client, uint32_t version, uint32_t id, Target t) {
            auto* r = make<ExtImageCopyCaptureSessionV1>(client, version, id);
            if (!r)
                return;
            auto owned = std::make_unique<Session>(Session{t, r});
            Session* s = owned.get();
            sessions_.push_back(std::move(owned));
            r->on_create_frame([this, s](ExtImageCopyCaptureSessionV1* self, uint32_t id) {
                auto* f = make<ExtImageCopyCaptureFrameV1>(self->client(), self->version(), id);
                if (!f)
                    return;
                if (s->frame) {
                    self->post_error(uint32_t(ExtImageCopyCaptureSessionV1::Error::DuplicateFrame),
                                     "a frame is already being captured");
                    return;
                }
                s->frame = f;
                auto buffer = std::make_shared<wl_resource*>(nullptr);
                auto captured = std::make_shared<bool>(false);
                f->on_attach_buffer([buffer](ExtImageCopyCaptureFrameV1*, wl_resource* b) { *buffer = b; });
                f->on_damage_buffer([](ExtImageCopyCaptureFrameV1* self, int32_t x, int32_t y, int32_t w, int32_t h) {
                    if (x < 0 || y < 0 || w <= 0 || h <= 0)
                        self->post_error(uint32_t(ExtImageCopyCaptureFrameV1::Error::InvalidBufferDamage),
                                         "bad damage");
                });
                f->on_capture([this, s, buffer, captured](ExtImageCopyCaptureFrameV1* self) {
                    using E = ExtImageCopyCaptureFrameV1::Error;
                    using Reason = ExtImageCopyCaptureFrameV1::FailureReason;
                    if (std::exchange(*captured, true)) {
                        self->post_error(uint32_t(E::AlreadyCaptured), "the frame was already captured");
                        return;
                    }
                    if (!*buffer) {
                        self->post_error(uint32_t(E::NoBuffer), "no buffer attached");
                        return;
                    }
                    if (s->stopped) {
                        self->send_failed(uint32_t(Reason::Stopped));
                        return;
                    }
                    wlr_buffer* b = nullptr;
                    if (!s->sent || !fits(*buffer, *s->sent, &b)) {
                        self->send_failed(uint32_t(Reason::BufferConstraints));
                        return;
                    }
                    Weak<ExtImageCopyCaptureFrameV1> w = self;
                    Copy cp{s->target, b, true, [w](const Result& r) {
                                auto* f = w.get();
                                if (!f)
                                    return;
                                if (!r.ok) {
                                    f->send_failed(r.fail_reason);
                                    return;
                                }
                                f->send_transform(r.transform);
                                for (const Box& d : r.damage)
                                    f->send_damage(d.x, d.y, d.width, d.height);
                                uint32_t hi, lo, ns;
                                split_time(r.when, &hi, &lo, &ns);
                                f->send_presentation_time(hi, lo, ns);
                                f->send_ready();
                            }};
                    if (copy.empty())
                        cp.done(Result{});
                    else
                        copy.emit(cp);
                });
            });
            r->on_gone([this, s] { std::erase_if(sessions_, [s](const auto& x) { return x.get() == s; }); });
            send_constraints(s);
        };
        m->on_create_session([make_session](ExtImageCopyCaptureManagerV1* self, uint32_t id, wl_resource* source_res,
                                            uint32_t options) {
            auto* src = dynamic_cast<Source*>(ExtImageCaptureSourceV1::from(source_res));
            Target t = src ? src->target : Target{};
            t.cursor = options & uint32_t(ExtImageCopyCaptureManagerV1::Options::PaintCursors);
            make_session(self->client(), self->version(), id, t);
        });
        m->on_create_pointer_cursor_session([this, make_session](ExtImageCopyCaptureManagerV1* self, uint32_t id,
                                                                 wl_resource* source_res, wl_resource*) {
            auto* r = make<ExtImageCopyCaptureCursorSessionV1>(self->client(), self->version(), id);
            if (!r)
                return;
            auto* src = dynamic_cast<Source*>(ExtImageCaptureSourceV1::from(source_res));
            Target t = src ? src->target : Target{};
            auto owned = std::make_unique<CursorSession>(CursorSession{t, r});
            CursorSession* cs = owned.get();
            cursor_sessions_.push_back(std::move(owned));
            r->on_get_capture_session([make_session, cs](ExtImageCopyCaptureCursorSessionV1* self, uint32_t id) {
                Target t = cs->target;
                t.cursor_only = true;
                make_session(self->client(), self->version(), id, t);
            });
            r->on_gone([this, cs] {
                std::erase_if(cursor_sessions_, [cs](const auto& x) { return x.get() == cs; });
            });
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

Capture::~Capture() {
    for (auto* g : {&screencopy_, &export_dmabuf_, &output_sources_, &toplevel_sources_, &image_copy_, &hypr_export_})
        g->reset();
    detach_all(managers_);
    detach_all(frames_);
    detach_all(sources_);
    for (auto& s : sessions_) {
        if (auto* f = s->frame.get())
            f->detach();
        if (auto* r = s->resource.get()) {
            r->on_gone(nullptr);
            r->detach();
        }
    }
    for (auto& c : cursor_sessions_)
        if (auto* r = c->resource.get()) {
            r->on_gone(nullptr);
            r->detach();
        }
}

void Capture::send_constraints(Session* s) {
    auto* r = s->resource.get();
    if (!r || s->stopped)
        return;
    const bool valid_target = s->target.output || s->target.toplevel;
    std::optional<Constraints> c = valid_target && constraints ? constraints(s->target) : std::nullopt;
    if (!c) {
        s->stopped = true;
        r->send_stopped();
        return;
    }
    if (s->sent == c)
        return;
    s->sent = c;
    r->send_buffer_size(uint32_t(c->width), uint32_t(c->height));
    r->send_shm_format(Shm::from_drm(c->shm_format));
    if (c->dmabuf_format) {
        dev_t dev = c->dmabuf_device;
        wl_array d{sizeof dev, sizeof dev, &dev};
        r->send_dmabuf_device(&d);
        std::vector<uint64_t> mods = c->dmabuf_modifiers;
        wl_array m{mods.size() * sizeof(uint64_t), mods.size() * sizeof(uint64_t), mods.data()};
        r->send_dmabuf_format(*c->dmabuf_format, &m);
    }
    r->send_done();
}

void Capture::constraints_changed(const Target& target) {
    for (auto& s : sessions_)
        if (s->target.output == target.output && s->target.toplevel == target.toplevel)
            send_constraints(s.get());
}

void Capture::stop(const Target& target) {
    for (auto& s : sessions_)
        if (s->target.output == target.output && s->target.toplevel == target.toplevel && !s->stopped) {
            s->stopped = true;
            if (auto* r = s->resource.get())
                r->send_stopped();
            if (auto* f = s->frame.get())
                f->send_failed(uint32_t(ExtImageCopyCaptureFrameV1::FailureReason::Stopped));
        }
}

void Capture::cursor(const Target& target, std::optional<std::pair<int, int>> position, std::pair<int, int> hotspot) {
    for (auto& c : cursor_sessions_) {
        if (c->target.output != target.output || c->target.toplevel != target.toplevel)
            continue;
        auto* r = c->resource.get();
        if (!r)
            continue;
        if (!position) {
            if (std::exchange(c->inside, false))
                r->send_leave();
            continue;
        }
        if (!std::exchange(c->inside, true))
            r->send_enter();
        if (c->hotspot != hotspot) {
            c->hotspot = hotspot;
            r->send_hotspot(hotspot.first, hotspot.second);
        }
        r->send_position(position->first, position->second);
    }
}

} // namespace atrium::wl
