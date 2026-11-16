// SPDX-License-Identifier: GPL-2.0-only
//
// fake_syslog: stands in for the unit's syslogd (busybox, /dev/log) in test_desktop.sh. Binds a
// Unix datagram socket at PATH and prints each datagram it receives as one line. With
// --stall-ms N it reads nothing for the first N ms (its queue fills, as a busy syslogd's does),
// and with --stall it never reads: the shim's log sink must drop lines then, never wait.
// With --send TEXT it is a sender instead: one datagram TEXT to PATH (as the bridge sends
// "video" to the shim's notify socket), then it exits (0 when it was sent).
//
//   fake_syslog PATH [--stall-ms N | --stall]
//   fake_syslog PATH --send TEXT
//
// Stops on SIGTERM / SIGINT.

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t gStop = 0;
void onSignal(int) { gStop = 1; }
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fake_syslog PATH [--stall-ms N | --stall]\n");
        return 2;
    }
    const char* path = argv[1];
    long stallMs = 0;
    const char* text = nullptr;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--stall-ms") && i + 1 < argc) stallMs = std::atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--stall")) stallMs = -1;
        else if (!std::strcmp(argv[i], "--send") && i + 1 < argc) text = argv[++i];
    }
    if (text) {
        const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
        sockaddr_un a;
        std::memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        std::strncpy(a.sun_path, path, sizeof a.sun_path - 1);
        const bool ok = fd >= 0 && ::sendto(fd, text, std::strlen(text), 0, (sockaddr*)&a, sizeof a) >= 0;
        if (!ok) std::fprintf(stderr, "fake_syslog: sending to %s: %s\n", path, std::strerror(errno));
        return ok ? 0 : 1;
    }
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);

    const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    sockaddr_un a;
    std::memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path, sizeof a.sun_path - 1);
    ::unlink(path);
    if (fd < 0 || ::bind(fd, (sockaddr*)&a, sizeof a) != 0) {
        std::fprintf(stderr, "fake_syslog: %s: %s\n", path, std::strerror(errno));
        return 1;
    }
    std::printf("fake_syslog: listening on %s\n", path);
    std::fflush(stdout);

    if (stallMs < 0) {
        while (!gStop) ::pause();
    } else if (stallMs > 0) {
        long left = stallMs;
        while (!gStop && left > 0) {
            ::usleep(10 * 1000);
            left -= 10;
        }
    }
    char buf[2048];
    while (!gStop) {
        pollfd p;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        if (::poll(&p, 1, 200) <= 0) continue;
        const ssize_t n = ::recv(fd, buf, sizeof buf - 1, 0);
        if (n <= 0) continue;
        buf[n] = '\0';
        std::printf("%s\n", buf);
        std::fflush(stdout);
    }
    ::unlink(path);
    return 0;
}
