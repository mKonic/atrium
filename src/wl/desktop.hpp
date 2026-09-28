#pragma once
#include "wl/compositor.hpp"
#include "wl/positioner.hpp"

#include <map>
#include <optional>
#include <string>

namespace atrium::wl {

class Output;
class Seat;

// ext-foreign-toplevel-list-v1 and wlr-foreign-toplevel-management: the
// window list other programs see (task bars, the portal's window picker).
// The compositor keeps one Handle per window up to date; each is shown to
// every client that binds either protocol.
class ForeignToplevels {
public:
    struct Info {
        std::string title, app_id;
        bool maximized = false, minimized = false, activated = false, fullscreen = false;
        std::vector<Output*> outputs;
        bool operator==(const Info&) const = default;
    };

    class Handle {
    public:
        const Info& info() const { return info_; }
        const std::string& identifier() const { return identifier_; }
        void update(const Info& info);
        void set_parent(Handle* parent);

        struct {
            Signal<bool> request_maximize, request_minimize;
            Signal<Seat*> request_activate;
            Signal<> request_close;
            Signal<bool, Output*> request_fullscreen;
            // Where a task bar shows it (a minimize animation's target).
            Signal<Surface*, const Box&> set_rectangle;
        } events;

    private:
        friend class ForeignToplevels;
        ForeignToplevels* owner_;
        Info info_;
        std::string identifier_;
        Handle* parent_ = nullptr;
        std::vector<Weak<Resource>> ext_, wlr_;
    };

    explicit ForeignToplevels(wl_display* display);
    ~ForeignToplevels();

    Handle* create(const Info& info);
    void destroy(Handle* handle);
    // The window list's handle a wlr/ext handle object refers to.
    Handle* from(wl_resource* resource) const;

private:
    void announce_ext(Resource* list, Handle* h);
    void announce_wlr(Resource* manager, Handle* h);
    void send_wlr(Resource* r, const Handle& h, const Info* old);

    std::unique_ptr<Global> ext_global_, wlr_global_;
    std::vector<Weak<Resource>> ext_lists_, wlr_managers_;
    std::vector<std::unique_ptr<Handle>> handles_;
    uint64_t next_id_ = 1;
};

// ext-workspace-v1: spaces and their groups (one per screen) for bars and
// pagers, and their requests to switch, make or remove them.
class Workspaces {
public:
    struct Group;
    struct Workspace {
        std::string id, name;
        std::vector<uint32_t> coordinates;
        uint32_t state = 0;  // ext_workspace_handle_v1.state bits: 1 active, 2 urgent, 4 hidden
        uint32_t capabilities = 0;  // 1 activate, 2 deactivate, 4 remove, 8 assign
        Group* group = nullptr;
        std::vector<Weak<Resource>> resources;
    };
    struct Group {
        uint32_t capabilities = 0;  // 1 create workspace
        std::vector<Output*> outputs;
        std::vector<Weak<Resource>> resources;
    };

    explicit Workspaces(wl_display* display);
    ~Workspaces();

    Group* add_group(uint32_t capabilities);
    void remove_group(Group* group);
    void set_group_outputs(Group* group, std::vector<Output*> outputs);
    Workspace* add_workspace(Group* group, const std::string& id, const std::string& name);
    void remove_workspace(Workspace* workspace);
    void update(Workspace* workspace, const std::string& name, uint32_t state, std::vector<uint32_t> coordinates,
                uint32_t capabilities);
    void move(Workspace* workspace, Group* group);

    // Asked for in a batch (sent with one commit).
    struct Request {
        enum class Kind { Activate, Deactivate, Remove, Assign, Create } kind;
        Workspace* workspace = nullptr;
        Group* group = nullptr;
        std::string name;  // Create
    };
    Signal<const std::vector<Request>&> requests;

private:
    struct Manager;
    void schedule_done();
    void send_group(Manager& m, Group* g);
    void send_workspace(Manager& m, Workspace* w, bool fresh);

    wl_display* display_;
    std::unique_ptr<Global> global_;
    std::vector<std::unique_ptr<Manager>> managers_;
    std::vector<std::unique_ptr<Group>> groups_;
    std::vector<std::unique_ptr<Workspace>> workspaces_;
    wl_event_source* done_idle_ = nullptr;
};

// wlr-output-management-unstable-v1: display settings tools (kanshi,
// wlr-randr) read and change the screens.
class OutputManagement {
public:
    struct Mode {
        int width = 0, height = 0, refresh = 0;  // mHz
        bool preferred = false;
        bool operator==(const Mode&) const = default;
    };
    struct Head {
        std::string name, description, make, model, serial;
        int physical_width = 0, physical_height = 0;
        std::vector<Mode> modes;
        bool enabled = false;
        int current_mode = -1;  // into modes; -1: a custom one (below)
        Mode custom_mode;
        int x = 0, y = 0;
        int32_t transform = 0;
        double scale = 1;
        bool adaptive_sync = false;
        bool operator==(const Head&) const = default;
    };
    // One head of a configuration: enabled with these, or disabled.
    struct HeadConfig {
        std::string name;
        bool enabled = false;
        std::optional<Mode> mode;  // a listed or custom mode
        std::optional<std::pair<int, int>> position;
        std::optional<int32_t> transform;
        std::optional<double> scale;
        std::optional<bool> adaptive_sync;
    };
    struct Configuration {
        std::vector<HeadConfig> heads;
        bool test_only;
        // The compositor's answer: exactly once.
        std::function<void(bool ok)> done;
    };

    explicit OutputManagement(wl_display* display);
    ~OutputManagement();

    // The screens now: sent to every tool, with a new serial.
    void set_heads(std::vector<Head> heads);
    Signal<Configuration&> apply;

private:
    struct Client;
    void send_all(Client& c);

    wl_display* display_;
    std::vector<Head> heads_;
    uint32_t serial_ = 0;
    std::unique_ptr<Global> global_;
    std::vector<std::unique_ptr<Client>> clients_;
};

// zwlr_output_power_manager_v1 (screens off and on, for idle daemons) and
// zwlr_gamma_control_manager_v1 (gammastep, wlsunset).
class OutputPower {
public:
    explicit OutputPower(wl_display* display);
    ~OutputPower();
    // The screen's power now (true: on): told to every client watching it.
    void set_mode(Output* output, bool on);
    Signal<Output*, bool> request_mode;

private:
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    struct Watch {
        Output* output;
        Weak<Resource> resource;
    };
    std::vector<Watch> watches_;
    std::map<Output*, bool> modes_;
};

class GammaControls {
public:
    explicit GammaControls(wl_display* display);
    ~GammaControls();
    // How many entries per channel `output` takes (0: none; clients fail).
    void set_size(Output* output, uint32_t size);
    // A table to apply (red, green, blue ramps), or empty: the default again.
    Signal<Output*, const std::vector<uint16_t>&> set_gamma;

private:
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::map<Output*, uint32_t> sizes_;
    std::map<Output*, Weak<Resource>> controls_;
};

} // namespace atrium::wl
