#pragma once
// atrium-portal's own Wayland connection, for sharing the screen: the screens
// (with their place on the desktop) and windows there are, and the globals a
// capture needs (ext-image-copy-capture with its output and toplevel
// sources, linux-dmabuf and wl_shm for the buffers). Driven from Qt's loop.

#include <QObject>
#include <QString>

#include <memory>
#include <vector>

struct wl_display;
struct wl_registry;
struct wl_output;
struct wl_shm;
struct zxdg_output_manager_v1;
struct zxdg_output_v1;
struct zwp_linux_dmabuf_v1;
struct ext_image_copy_capture_manager_v1;
struct ext_output_image_capture_source_manager_v1;
struct ext_foreign_toplevel_image_capture_source_manager_v1;
struct ext_foreign_toplevel_list_v1;
struct ext_foreign_toplevel_handle_v1;
class QSocketNotifier;

namespace atrium {

class WaylandLink : public QObject {
    Q_OBJECT

public:
    struct Output {
        uint32_t global = 0;
        wl_output* wl = nullptr;
        zxdg_output_v1* xdg = nullptr;
        QString name, description;
        int x = 0, y = 0, width = 0, height = 0;  // logical, on the desktop
        int refresh_mhz = 0;
    };
    struct Toplevel {
        ext_foreign_toplevel_handle_v1* handle = nullptr;
        QString identifier, title, app_id;
    };

    // The connection, made on first use; null when there's no Wayland.
    static WaylandLink* instance();
    ~WaylandLink() override;

    wl_display* display() const { return display_; }
    const std::vector<std::unique_ptr<Output>>& outputs() const { return outputs_; }
    const std::vector<std::unique_ptr<Toplevel>>& toplevels() const { return toplevels_; }
    Output* output(const QString& name) const;
    Toplevel* toplevel(const QString& identifier) const;

    ext_image_copy_capture_manager_v1* copy_manager = nullptr;
    ext_output_image_capture_source_manager_v1* output_sources = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* toplevel_sources = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    wl_shm* shm = nullptr;

    void roundtrip();

    // Listener plumbing (the C callbacks reach these).
    void add_global(uint32_t name, const char* interface, uint32_t version);
    void remove_global(uint32_t name);
    void add_toplevel(ext_foreign_toplevel_handle_v1* handle);
    void remove_toplevel(Toplevel* t);
    void watch_output(Output* o);

private:
    WaylandLink() = default;
    bool connect_display();
    void dispatch();

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    zxdg_output_manager_v1* xdg_outputs_ = nullptr;
    ext_foreign_toplevel_list_v1* list_ = nullptr;
    QSocketNotifier* notifier_ = nullptr;
    std::vector<std::unique_ptr<Output>> outputs_;
    std::vector<std::unique_ptr<Toplevel>> toplevels_;
};

} // namespace atrium
