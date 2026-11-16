// SPDX-License-Identifier: GPL-2.0-only
//
// fake_ilm: stands in for the unit's SVP iLM service (svp.svc.ilm) in test_desktop.sh. Serves
// SVP_iLMService on 127.0.0.1:<port> and prints every call, so a test can check what the shim
// asks for when appconnect calls Show(n). Every call answers 0.
//
//   fake_ilm [--port N]      (default 30005)

#include "ilm_idl.hpp"

#include <RCF/RCF.hpp>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pthread.h>

namespace {

class FakeIlm {
public:
    int WaitSurface(unsigned int id) {
        std::printf("WaitSurface(%u)\n", id);
        std::fflush(stdout);
        return 0;
    }
    int ShowSurface(unsigned int id, int show) {
        std::printf("ShowSurface(%u, %d)\n", id, show);
        std::fflush(stdout);
        return 0;
    }
    int SetSurfacePos(unsigned int id, unsigned int x, unsigned int y, unsigned int w, unsigned int h) {
        std::printf("SetSurfacePos(%u, %u, %u, %u, %u)\n", id, x, y, w, h);
        std::fflush(stdout);
        return 0;
    }
    int Screenshot(unsigned int id, const std::string& path) {
        std::printf("Screenshot(%u, %s)\n", id, path.c_str());
        std::fflush(stdout);
        return 0;
    }
};

} // namespace

int main(int argc, char** argv) {
    int port = kILMServicePort;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
    }
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    RCF::RcfInitDeinit rcfInit;
    FakeIlm ilm;
    RCF::RcfServer server(RCF::TcpEndpoint("127.0.0.1", port));
    server.bind<SVP_iLMService>(ilm);
    server.start();
    std::printf("fake_ilm listening on 127.0.0.1:%d\n", port);
    std::fflush(stdout);
    int sig = 0;
    sigwait(&sigs, &sig);
    return 0;
}
