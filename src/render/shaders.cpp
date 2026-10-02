#include "render/shaders.hpp"
#include "util/log.hpp"

#include "paths.hpp"

#include <sys/inotify.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace atrium::render {

// The built-in copies (shaders_embedded.cpp).
const char* embedded_shader(std::string_view name);

namespace {

namespace fs = std::filesystem;

std::string shader_directory() {
    if (const char* d = std::getenv("ATRIUM_SHADER_DIR"); d && *d)
        return d;
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && exe.string().starts_with(ATRIUM_SOURCE_DIR))
        return std::string(ATRIUM_SOURCE_DIR) + "/data/share/atrium/shaders";
    return std::string(ATRIUM_DATADIR) + "/shaders";
}

bool reload_wanted() {
#ifndef NDEBUG
    return true;
#else
    const char* v = std::getenv("ATRIUM_SHADER_RELOAD");
    return v && *v == '1';
#endif
}

// The #version line stays first; the defines go right after it.
std::string with_defines(const std::string& src, const std::string& defines) {
    if (defines.empty())
        return src;
    const size_t eol = src.find('\n');
    if (src.starts_with("#version") && eol != std::string::npos)
        return src.substr(0, eol + 1) + defines + src.substr(eol + 1);
    return defines + src;
}

GLuint compile(GLenum type, const std::string& src, std::string* error) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE) {
        GLint len = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(size_t(std::max(len, 1)), '\0');
        glGetShaderInfoLog(s, len, nullptr, log.data());
        if (error)
            *error = log.c_str();
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link(GLuint vert, GLuint frag, std::string* error) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);
    glDetachShader(prog, vert);
    glDetachShader(prog, frag);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok == GL_FALSE) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(size_t(std::max(len, 1)), '\0');
        glGetProgramInfoLog(prog, len, nullptr, log.data());
        if (error)
            *error = log.c_str();
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

Program make_program(GLuint id) {
    Program p;
    p.id = id;
    p.pos = id ? glGetAttribLocation(id, "pos") : -1;
    p.at = id ? glGetAttribLocation(id, "at") : -1;
    return p;
}

} // namespace

GLint Program::loc(const char* name) {
    // Keyed by the literal's address range: callers pass string literals.
    auto it = locs_.find(name);
    if (it != locs_.end())
        return it->second;
    GLint l = id ? glGetUniformLocation(id, name) : -1;
    locs_.emplace(name, l);
    return l;
}

void Program::set_mat3(const char* name, const float m[9]) {
    GLint l = loc(name);
    if (l < 0)
        return;
    // Row-major in, column-major for GLSL.
    const float t[9] = {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
    glUniformMatrix3fv(l, 1, GL_FALSE, t);
}

ShaderLibrary::ShaderLibrary(bool external_textures) : dir_(shader_directory()), external_(external_textures) {
    auto set = [this](Shader s, const char* frag, std::vector<std::string> defines) {
        entries_[size_t(s)] = Entry{frag, std::move(defines), {}};
    };
    set(Shader::Quad, "quad.frag", {"#define EFFECTS 0\n", "#define EFFECTS 1\n"});
    set(Shader::QuadRound, "quad_round.frag", {""});
    std::vector<std::string> tex;
    for (int effects = 0; effects < 2; ++effects)
        for (int source = 1; source <= 3; ++source)
            tex.push_back("#define SOURCE " + std::to_string(source) + "\n#define EFFECTS " +
                          std::to_string(effects) + "\n");
    for (int source = 1; source <= 3; ++source)
        tex.push_back("#define SOURCE " + std::to_string(source) + "\n#define EFFECTS 0\n#define MESH 1\n");
    set(Shader::Tex, "tex.frag", std::move(tex));
    set(Shader::BoxShadow, "box_shadow.frag", {""});
    set(Shader::Blur1, "blur1.frag", {""});
    set(Shader::Blur2, "blur2.frag", {""});
    set(Shader::BlurEffects, "blur_effects.frag", {""});
    set(Shader::Glass, "glass.frag", {""});
    set(Shader::GlassField, "glass_field.frag", {""});
    set(Shader::Output, "output.frag", {""});
    set(Shader::Screen, nullptr, {});

    if (reload_wanted()) {
        inotify_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_ >= 0 && inotify_add_watch(inotify_, dir_.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
            close(inotify_);
            inotify_ = -1;
        }
    }
}

ShaderLibrary::~ShaderLibrary() {
    for (Entry& e : entries_)
        for (Program& p : e.programs)
            if (p.id)
                glDeleteProgram(p.id);
    if (screen_.id)
        glDeleteProgram(screen_.id);
    if (inotify_ >= 0)
        close(inotify_);
}

std::string ShaderLibrary::source(const std::string& name, bool from_disk, int depth) {
    if (depth > 8)
        return {};
    std::string text;
    if (from_disk) {
        const fs::path path = name.starts_with('/') ? fs::path(name) : fs::path(dir_) / name;
        std::ifstream in(path);
        if (!in)
            return {};
        std::stringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    } else if (const char* e = embedded_shader(name)) {
        text = e;
    } else {
        return {};
    }
    // #include "file" (one per line), resolved from the same place.
    std::string out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t hash = line.find_first_not_of(" \t");
        if (hash != std::string::npos && line.compare(hash, 8, "#include") == 0) {
            const size_t a = line.find('"'), b = line.rfind('"');
            if (a != std::string::npos && b > a) {
                std::string inc = source(line.substr(a + 1, b - a - 1), from_disk && !name.starts_with('/'), depth + 1);
                if (inc.empty() && !from_disk)
                    return {};
                if (inc.empty())
                    inc = source(line.substr(a + 1, b - a - 1), false, depth + 1);
                // An included file's own #version line is dropped.
                if (inc.starts_with("#version")) {
                    const size_t eol = inc.find('\n');
                    inc = eol == std::string::npos ? std::string() : inc.substr(eol + 1);
                }
                out += inc;
                out += '\n';
                continue;
            }
        }
        out += line;
        out += '\n';
    }
    return out;
}

GLuint ShaderLibrary::build(const std::string& frag_name, const std::string& defines, bool from_disk,
                            std::string* error) {
    const std::string vsrc = source("common.vert", from_disk, 0);
    const std::string fsrc = source(frag_name, from_disk, 0);
    if (vsrc.empty() || fsrc.empty()) {
        if (error)
            *error = "missing file";
        return 0;
    }
    GLuint vert = compile(GL_VERTEX_SHADER, with_defines(vsrc, defines), error);
    if (!vert)
        return 0;
    GLuint frag = compile(GL_FRAGMENT_SHADER, with_defines(fsrc, defines), error);
    if (!frag) {
        glDeleteShader(vert);
        return 0;
    }
    GLuint prog = link(vert, frag, error);
    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}

bool ShaderLibrary::load() {
    bool ok = true;
    for (size_t i = 0; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        if (!e.frag)
            continue;
        std::vector<Program> programs;
        for (size_t v = 0; v < e.defines.size(); ++v) {
            // The external-texture variants need GL_OES_EGL_image_external_essl3.
            const bool external = Shader(i) == Shader::Tex && v % 3 == 2;
            if (external && !external_) {
                programs.push_back(make_program(0));
                continue;
            }
            std::string error;
            GLuint id = build(e.frag, e.defines[v], true, &error);
            if (!id) {
                alog(Log::Error, "Shader %s (variant %zu) from %s: %s; using the built-in one", e.frag, v,
                        dir_.c_str(), error.c_str());
                id = build(e.frag, e.defines[v], false, &error);
                if (!id) {
                    alog(Log::Error, "Built-in shader %s (variant %zu) doesn't compile: %s", e.frag, v,
                            error.c_str());
                    ok = false;
                }
            }
            programs.push_back(make_program(id));
        }
        for (Program& p : e.programs)
            if (p.id)
                glDeleteProgram(p.id);
        e.programs = std::move(programs);
    }
    return ok;
}

Program& ShaderLibrary::get(Shader s, int variant) {
    if (s == Shader::Screen)
        return screen_;
    Entry& e = entries_[size_t(s)];
    return e.programs[size_t(variant) < e.programs.size() ? variant : 0];
}

bool ShaderLibrary::poll_reload() {
    if (inotify_ < 0)
        return false;
    alignas(inotify_event) char buf[4096];
    bool changed = false;
    while (read(inotify_, buf, sizeof(buf)) > 0)
        changed = true;
    if (!changed)
        return false;
    alog(Log::Info, "Shaders changed on disk: rebuilding");
    load();
    return true;
}

bool ShaderLibrary::set_screen_shader(const std::string& path) {
    if (screen_.id)
        glDeleteProgram(screen_.id);
    screen_ = Program();
    if (path.empty())
        return true;
    std::string error;
    const std::string vsrc = source("common.vert", true, 0).empty() ? source("common.vert", false, 0)
                                                                     : source("common.vert", true, 0);
    const std::string fsrc = source(path, true, 0);
    if (fsrc.empty()) {
        alog(Log::Error, "Screen shader %s: can't read it", path.c_str());
        return false;
    }
    GLuint vert = compile(GL_VERTEX_SHADER, vsrc, &error);
    GLuint frag = vert ? compile(GL_FRAGMENT_SHADER, fsrc, &error) : 0;
    GLuint prog = frag ? link(vert, frag, &error) : 0;
    if (vert)
        glDeleteShader(vert);
    if (frag)
        glDeleteShader(frag);
    if (!prog) {
        alog(Log::Error, "Screen shader %s: %s", path.c_str(), error.c_str());
        return false;
    }
    screen_ = make_program(prog);
    return true;
}

} // namespace atrium::render
