#pragma once
// Every Wayland global atrium serves, on its own protocol layer (src/wl).
// The server makes them in setup() and lets them go in teardown().
#include "wl/capture.hpp"
#include "wl/color.hpp"
#include "wl/compositor.hpp"
#include "wl/data_device.hpp"
#include "wl/desktop.hpp"
#include "wl/dmabuf.hpp"
#include "wl/ime.hpp"
#include "wl/input_ext.hpp"
#include "wl/layer_shell.hpp"
#include "wl/misc.hpp"
#include "wl/output.hpp"
#include "wl/seat.hpp"
#include "wl/selection.hpp"
#include "wl/session_lock.hpp"
#include "wl/shm.hpp"
#include "wl/surface_ext.hpp"
#include "wl/tablet.hpp"
#include "wl/timing.hpp"
#include "wl/xdg_extras.hpp"
#include "wl/xdg_shell.hpp"
#include "wl/xwayland_shell.hpp"

#include <memory>

namespace atrium {

struct Protocols {
    // Buffers and surfaces.
    std::unique_ptr<wl::Shm> shm;
    std::unique_ptr<wl::LinuxDmabuf> dmabuf;
    std::unique_ptr<wl::LegacyDrm> drm;
    std::unique_ptr<wl::Syncobj> syncobj;
    std::unique_ptr<wl::Compositor> compositor;
    std::unique_ptr<wl::Viewporter> viewporter;
    std::unique_ptr<wl::SinglePixelBuffers> single_pixel;
    std::unique_ptr<wl::FractionalScales> fractional_scales;
    std::unique_ptr<wl::SurfaceHints> surface_hints;  // alpha, content type, tearing
    std::unique_ptr<wl::Presentation> presentation;
    std::unique_ptr<wl::Fifo> fifo;
    std::unique_ptr<wl::CommitTiming> commit_timing;
    std::unique_ptr<wl::ColorManagement> color;

    // Input.
    std::unique_ptr<wl::Seat> seat;
    std::unique_ptr<wl::DataDevices> data;
    std::unique_ptr<wl::PrimarySelection> primary;
    std::unique_ptr<wl::DataControl> data_control;
    std::unique_ptr<wl::RelativePointers> relative_pointers;
    std::unique_ptr<wl::PointerConstraints> pointer_constraints;
    std::unique_ptr<wl::PointerGestures> pointer_gestures;
    std::unique_ptr<wl::PointerWarps> pointer_warps;
    std::unique_ptr<wl::ShortcutInhibitors> shortcut_inhibitors;
    std::unique_ptr<wl::CursorShapes> cursor_shapes;
    std::unique_ptr<wl::IdleInhibitors> idle_inhibitors;
    std::unique_ptr<wl::IdleNotifier> idle_notifier;
    std::unique_ptr<wl::TextInputs> text_inputs;
    std::unique_ptr<wl::InputMethods> input_methods;
    std::unique_ptr<wl::VirtualInputs> virtual_inputs;
    std::unique_ptr<wl::Tablets> tablets;

    // Windows.
    std::unique_ptr<wl::Shell> xdg;
    std::unique_ptr<wl::PipShell> pip;
    std::unique_ptr<wl::Decorations> decorations;
    std::unique_ptr<wl::Dialogs> dialogs;
    std::unique_ptr<wl::Activation> activation;
    std::unique_ptr<wl::ToplevelTags> tags;
    std::unique_ptr<wl::XdgForeign> foreign;
    std::unique_ptr<wl::LayerShell> layer_shell;
    std::unique_ptr<wl::SessionLockManager> session_lock;
    std::unique_ptr<wl::XwaylandShell> xwayland_shell;

    // The desktop.
    std::unique_ptr<wl::XdgOutputs> xdg_outputs;
    std::unique_ptr<wl::OutputManagement> output_management;
    std::unique_ptr<wl::OutputPower> output_power;
    std::unique_ptr<wl::GammaControls> gamma;
    std::unique_ptr<wl::ForeignToplevels> toplevels;
    std::unique_ptr<wl::Workspaces> workspaces;
    std::unique_ptr<wl::Capture> capture;
    std::unique_ptr<wl::GlobalShortcuts> global_shortcuts;
    std::unique_ptr<wl::SecurityContexts> security;
};

} // namespace atrium
