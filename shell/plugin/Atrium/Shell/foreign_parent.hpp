#pragma once
// xdg-foreign: a portal's dialog (a process of its own) made a child of the
// app window that asked, as xdg-desktop-portal-gtk and -kde do: the portal
// hands the app's "wayland:HANDLE" over in ATRIUM_PARENT_WINDOW, and every
// window this process shows imports it and takes it as its parent.

namespace atrium::shell {

void installForeignParent();

} // namespace atrium::shell
