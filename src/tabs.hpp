#pragma once
#include "tabs_core.hpp"

namespace atrium {

class View;

// Windows sharing one frame as tabs (Server::merge_tab and the rest, in
// tabs.cpp). Always two or more: down to one, it's a window again.
struct TabGroup : tabs::List<View*> {};

} // namespace atrium
