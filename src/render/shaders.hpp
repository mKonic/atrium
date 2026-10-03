#pragma once
// The renderer's GLSL, loaded at run time: every program is a vertex and a
// fragment file from the shader directory, with `#include "file"` resolved
// and a variant's `#define`s put after the #version line. Each variant is
// compiled at startup; one that fails falls back to the copy built into
// atrium, so a broken file on disk never costs the picture. Debug builds
// (and ATRIUM_SHADER_RELOAD=1) watch the directory and rebuild what changed.
//
// References: Hyprland's ShaderLoader and KWin's glshadermanager.

#include "render/gl.hpp"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace atrium::render {

class Program {
public:
    GLuint id = 0;
    GLint pos = -1;  // the `pos` attribute
    GLint at = -1;   // the mesh variants' `at` attribute

    // A uniform's location, looked up once (-1 when the program has none).
    GLint loc(const char* name);

    void set(const char* name, int v) { if (GLint l = loc(name); l >= 0) glUniform1i(l, v); }
    void set(const char* name, float v) { if (GLint l = loc(name); l >= 0) glUniform1f(l, v); }
    void set(const char* name, float a, float b) { if (GLint l = loc(name); l >= 0) glUniform2f(l, a, b); }
    void set(const char* name, float a, float b, float c, float d) {
        if (GLint l = loc(name); l >= 0)
            glUniform4f(l, a, b, c, d);
    }
    // 3x3, row-major (wlroots' convention): transposed for GLSL, for
    // `matrix * v`.
    void set_mat3(const char* name, const float m[9]);
    // As is, for `v * matrix` (the vertex shader's proj and tex_proj).
    void set_mat3_raw(const char* name, const float m[9]) {
        if (GLint l = loc(name); l >= 0)
            glUniformMatrix3fv(l, 1, GL_FALSE, m);
    }

private:
    std::unordered_map<std::string_view, GLint> locs_;
};

enum class Shader {
    Quad,        // flat colour; variant 1: a rounded cut-out
    QuadRound,   // flat colour, rounded corners
    Tex,         // variant: source (0 RGBA, 1 RGBX, 2 external) + 3 * effects; 6 + source: a mesh
    BoxShadow,
    Blur1,       // dual Kawase down
    Blur2,       // dual Kawase up
    BlurEffects, // brightness, contrast, saturation, noise
    Glass,
    GlassField,
    Motion,      // a moving window's layer, blurred along its way
    Output,      // the HDR/colour output pass
    Screen,      // the user's screen shader over the whole output
    Count,
};

class ShaderLibrary {
public:
    ShaderLibrary(bool external_textures);
    ~ShaderLibrary();

    // Compiles every program. False only when not even the built-in
    // sources compile.
    bool load();

    Program& get(Shader s, int variant = 0);

    // Rebuilds from disk if a file changed (debug builds, or
    // ATRIUM_SHADER_RELOAD=1); true if anything was rebuilt.
    bool poll_reload();

    // The user's screen shader: a fragment file over the whole frame, or
    // empty for none. False (and none) if it doesn't compile.
    bool set_screen_shader(const std::string& path);
    bool has_screen_shader() const { return screen_.id != 0; }

    // Where the files are read from.
    const std::string& directory() const { return dir_; }

private:
    struct Entry {
        const char* frag;
        std::vector<std::string> defines;  // one list per variant
        std::vector<Program> programs;
    };

    GLuint build(const std::string& frag_name, const std::string& defines, bool from_disk, std::string* error);
    std::string source(const std::string& name, bool from_disk, int depth);

    std::string dir_;
    bool external_ = false;
    std::array<Entry, size_t(Shader::Count)> entries_{};
    Program screen_;
    int inotify_ = -1;
};

} // namespace atrium::render
