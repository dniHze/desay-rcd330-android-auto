// SPDX-License-Identifier: GPL-2.0-only
//
// fake_receiver: stands in for the AA receiver on aa-carlife-shim's control socket.
//
// Prints every line the shim sends. On "start" it answers with the states given on the
// command line (default: connected projecting), one per --step-ms; an argument "line:<text>"
// is sent as <text> instead ("line:mic 1"), and "wait:<ms>" only waits. On "stop" it answers
// "state disconnected". With --hello it is a protocol 2 receiver: "hello 2" first, and every
// "ping <n>" answered "pong <n>", the first --pongs N of them only (then it plays hung); with
// --comm NAME it takes that process name (the shim kills only the real receiver's name).
//
//   fake_receiver [--sock PATH] [--step-ms N] [--hello [--pongs N]] [--comm NAME]
//                 [state | line:<text> | wait:<ms> ...]
//
// Used by test_desktop.sh; also handy on the unit to watch what appconnect sends.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static bool sendLine(int fd, const std::string& line) {
    std::string out = line + "\n";
    return ::send(fd, out.data(), out.size(), MSG_NOSIGNAL) == (ssize_t)out.size();
}

int main(int argc, char** argv) {
    std::string path = "/tmp/run/aa-carlife.sock";
    int stepMs = 300, pongs = -1;
    bool hello = false;
    std::vector<std::string> states;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--sock") && i + 1 < argc) path = argv[++i];
        else if (!std::strcmp(argv[i], "--step-ms") && i + 1 < argc) stepMs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--hello")) hello = true;
        else if (!std::strcmp(argv[i], "--pongs") && i + 1 < argc) pongs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--comm") && i + 1 < argc) prctl(PR_SET_NAME, argv[++i], 0, 0, 0);
        else states.push_back(argv[i]);
    }
    if (states.empty()) { states.push_back("connected"); states.push_back("projecting"); }

    int fd = -1;
    for (int attempt = 0; attempt < 50; ++attempt) {
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr;
        std::memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
        if (::connect(fd, (sockaddr*)&addr, sizeof addr) == 0) break;
        ::close(fd);
        fd = -1;
        ::usleep(100 * 1000);
    }
    if (fd < 0) { std::fprintf(stderr, "fake_receiver: cannot connect to %s\n", path.c_str()); return 1; }
    std::printf("fake_receiver: connected to %s\n", path.c_str());
    std::fflush(stdout);
    if (hello) sendLine(fd, "hello 2");

    std::string buf;
    char tmp[512];
    for (;;) {
        ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            std::printf("fake_receiver: got '%s'\n", line.c_str());
            std::fflush(stdout);
            if (line == "start") {
                for (size_t k = 0; k < states.size(); ++k) {
                    ::usleep((useconds_t)stepMs * 1000);
                    if (states[k].compare(0, 5, "wait:") == 0) ::usleep((useconds_t)std::atoi(states[k].c_str() + 5) * 1000);
                    else if (states[k].compare(0, 5, "line:") == 0) sendLine(fd, states[k].substr(5));
                    else sendLine(fd, "state " + states[k]);
                }
            } else if (line == "stop") {
                sendLine(fd, "state disconnected");
            } else if (hello && line.compare(0, 5, "ping ") == 0) {
                if (pongs != 0) sendLine(fd, "pong " + line.substr(5));
                if (pongs > 0) --pongs; // -1: answers every ping
            }
        }
    }
    std::printf("fake_receiver: shim closed the connection\n");
    ::close(fd);
    return 0;
}
