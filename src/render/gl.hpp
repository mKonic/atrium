#pragma once
// EGL and GLES 3 with the extension entry points atrium uses.

#include "util/format_set.hpp"
#include "util/timeline.hpp"
#include "wlr.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

namespace atrium::render {

// Whether `ext` is a whole word in a space-separated extension list.
bool has_extension(const char* list, const char* ext);

// eglGetProcAddress into a typed pointer; false (and null) if it's missing.
template <typename F>
bool load_proc(F& out, const char* name) {
    out = reinterpret_cast<F>(eglGetProcAddress(name));
    return out != nullptr;
}

} // namespace atrium::render
