#include "server.hpp"
#include "settings.hpp"
#include "version.hpp"

#include <cstdio>
#include <cstdlib>
#include <getopt.h>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s [-s startup-command] [-c registry] [-g] [-d] [-v]\n"
        "  -s, --startup CMD   run CMD through /bin/sh once the session is up\n"
        "  -c, --registry FILE the registry database (default $XDG_CONFIG_HOME/atrium/registry.db)\n"
        "  -g, --greeter       the login screen, for greetd (atrium-greeter.toml)\n"
        "  -d, --debug         verbose logging\n"
        "  -v, --version       print the version and exit\n",
        argv0);
}

} // namespace

int main(int argc, char** argv) {
    const char* startup = nullptr;
    const char* registry_file = std::getenv("ATRIUM_REGISTRY");
    bool debug = false;
    bool greeter = false;

    static const option long_opts[] = {
        {"startup", required_argument, nullptr, 's'},
        {"registry", required_argument, nullptr, 'c'},
        {"debug", no_argument, nullptr, 'd'},
        {"greeter", no_argument, nullptr, 'g'},
        {"version", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    for (int c; (c = getopt_long(argc, argv, "s:c:dgvh", long_opts, nullptr)) != -1;) {
        switch (c) {
        case 's': startup = optarg; break;
        case 'c': registry_file = optarg; break;
        case 'd': debug = true; break;
        case 'g': greeter = true; break;
        case 'v': std::printf("atrium %s (build %d)\n", ATRIUM_VERSION, ATRIUM_BUILD); return 0;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }
    if (optind < argc) {
        usage(argv[0]);
        return 1;
    }

    wlr_log_init(debug ? WLR_DEBUG : WLR_INFO, nullptr);
    wlr_log(WLR_INFO, "atrium %s (build %d)", ATRIUM_VERSION, ATRIUM_BUILD);

    if (!std::getenv("XDG_RUNTIME_DIR")) {
        wlr_log(WLR_ERROR, "XDG_RUNTIME_DIR must be set");
        return 1;
    }

    // Inside another compositor (a window on a desktop, or a test box) that
    // compositor keeps Super for itself.
    const bool nested = std::getenv("WAYLAND_DISPLAY") || std::getenv("WAYLAND_SOCKET") ||
                        std::getenv("DISPLAY");

    {
        atrium::Config config = atrium::Config::defaults(nested);
        config.greeter = greeter;
        // The greeter's user keeps nothing: an in-memory registry.
        const std::filesystem::path registry = greeter          ? std::filesystem::path(":memory:")
                                               : registry_file ? std::filesystem::path(registry_file)
                                                               : atrium::Registry::default_file();
        atrium::Server server(std::move(config), nested, registry);
        server.run(startup);
    }
    return 0;
}
