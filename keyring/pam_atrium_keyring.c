// pam_atrium_keyring: the login password, for atrium-keyring to open the
// login keyring with (as pam_kwallet and pam_gnome_keyring do for theirs).
//
//   auth     optional  pam_atrium_keyring.so   keeps the password typed
//   session  optional  pam_atrium_keyring.so   after pam_systemd: hands it over
//
// At session open a child, as the user, serves it once on
// $XDG_RUNTIME_DIR/atrium-keyring.login (only that user can connect; the
// peer is checked too) and goes; two minutes unclaimed and it goes anyway.
// What it sends: the length (4 bytes, native order), then the password.

#define _GNU_SOURCE
#define PAM_SM_AUTH
#define PAM_SM_SESSION

#include <security/pam_ext.h>
#include <security/pam_modules.h>

#include <errno.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#define DATA "atrium_keyring_password"

static void forget(pam_handle_t* pamh, void* data, int status) {
    (void)pamh;
    (void)status;
    if (data) {
        explicit_bzero(data, strlen(data));
        free(data);
    }
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t* pamh, int flags, int argc, const char** argv) {
    (void)flags;
    (void)argc;
    (void)argv;
    const void* tok = NULL;
    if (pam_get_item(pamh, PAM_AUTHTOK, &tok) != PAM_SUCCESS || !tok)
        return PAM_IGNORE;
    char* copy = strdup(tok);
    if (!copy || pam_set_data(pamh, DATA, copy, forget) != PAM_SUCCESS) {
        forget(pamh, copy, 0);
        return PAM_IGNORE;
    }
    return PAM_IGNORE;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t* pamh, int flags, int argc, const char** argv) {
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_IGNORE;
}

static int write_all(int fd, const void* buf, size_t len) {
    const char* p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

// The child, as the user: serve it once, then go.
static void serve(const char* path, uid_t uid, const char* password) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof addr.sun_path)
        return;
    strcpy(addr.sun_path, path);
    unlink(path);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    const mode_t old = umask(077);
    const int bound = bind(fd, (struct sockaddr*)&addr, sizeof addr);
    umask(old);
    if (bound != 0 || listen(fd, 1) != 0)
        return;
    const int deadline = 120 * 1000;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    while (poll(&p, 1, deadline) > 0) {
        const int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0)
            continue;
        struct ucred cred;
        socklen_t len = sizeof cred;
        if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.uid == uid) {
            const uint32_t n = (uint32_t)strlen(password);
            write_all(c, &n, sizeof n);
            write_all(c, password, n);
            close(c);
            break;
        }
        close(c);
    }
    unlink(path);
}

PAM_EXTERN int pam_sm_open_session(pam_handle_t* pamh, int flags, int argc, const char** argv) {
    (void)flags;
    (void)argc;
    (void)argv;
    const void* data = NULL;
    if (pam_get_data(pamh, DATA, &data) != PAM_SUCCESS || !data)
        return PAM_IGNORE;
    const char* user = NULL;
    if (pam_get_user(pamh, &user, NULL) != PAM_SUCCESS || !user)
        return PAM_IGNORE;
    const struct passwd* pw = getpwnam(user);
    const char* runtime = pam_getenv(pamh, "XDG_RUNTIME_DIR");
    if (!pw || !runtime) {
        pam_syslog(pamh, LOG_NOTICE, "no runtime dir for %s: the keyring will ask for its password", user);
        return PAM_IGNORE;
    }
    char path[256];
    if (snprintf(path, sizeof path, "%s/atrium-keyring.login", runtime) >= (int)sizeof path)
        return PAM_IGNORE;
    const uid_t uid = pw->pw_uid;
    const gid_t gid = pw->pw_gid;

    // Twice forked: nothing for the caller to wait on.
    const pid_t child = fork();
    if (child < 0)
        return PAM_IGNORE;
    if (child == 0) {
        if (fork() != 0)
            _exit(0);
        setsid();
        signal(SIGHUP, SIG_IGN);
        if (setgroups(0, NULL) != 0 || setgid(gid) != 0 || setuid(uid) != 0 || getuid() != uid)
            _exit(1);
        serve(path, uid, data);
        _exit(0);
    }
    waitpid(child, NULL, 0);
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_close_session(pam_handle_t* pamh, int flags, int argc, const char** argv) {
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_IGNORE;
}
