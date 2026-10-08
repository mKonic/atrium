// atrium-login: the display manager. Run by atrium-login.service as root.
#include "daemon.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <unistd.h>

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/etc/atrium/login.conf";
    if (geteuid() != 0) {
        std::fprintf(stderr, "atrium-login runs as root (atrium-login.service)\n");
        return 1;
    }
    std::string text;
    if (std::ifstream in(path); in) {
        std::stringstream s;
        s << in.rdbuf();
        text = s.str();
    }
    return atrium::login::Daemon(atrium::login::parse_config(text)).run();
}
