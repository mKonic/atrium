#include "server.hpp"
#include "version.hpp"

#include <cstdio>
#include <cstdlib>
#include <getopt.h>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s [-s startup-command] [-d] [-v]\n"
        "  -s, --startup CMD   run CMD through /bin/sh once the session is up\n"
        "  -d, --debug         verbose logging\n"
        "  -v, --version       print the version and exit\n",
        argv0);
}

} // namespace

int main(int argc, char** argv) {
    const char* startup = nullptr;
    bool debug = false;

    static const option long_opts[] = {
        {"startup", required_argument, nullptr, 's'},
        {"debug", no_argument, nullptr, 'd'},
        {"version", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    for (int c; (c = getopt_long(argc, argv, "s:dvh", long_opts, nullptr)) != -1;) {
        switch (c) {
        case 's': startup = optarg; break;
        case 'd': debug = true; break;
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
        atrium::Server server(atrium::Config::defaults(nested), nested);
        server.run(startup);
    }
    return 0;
}
