// SPDX-License-Identifier: GPL-2.0-only
//
// fake_power: stands in for the unit's power manager (powermgr, svp.svc.power) in
// test_desktop.sh. Serves SVP_PowerService on 127.0.0.1:<port> (GetPowerState answers the last
// published state, with sub-state 1 for PS_USER_ON when --temp-on) and publishes
// SVP_PowerListener on 127.0.0.1:<pub-port>, as powermgr does on 30202. What to publish comes
// from --cmd FILE: every line appended to it ("<state> <a> <b>") is published once as
// OnPowerStateChanged(state, a, b).
//
//   fake_power [--port N] [--pub-port N] --cmd FILE [--temp-on]   (defaults 30201, 30202)

#include "power_idl.hpp"

#include <RCF/RCF.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <pthread.h>

namespace {

std::atomic<int> gState(SVPPower::PS_USER_ON);
bool gTempOn = false;

class FakePower {
public:
    int GetPowerState(SVPPower::PowerState& state, unsigned char& sub) {
        state = static_cast<SVPPower::PowerState>(gState.load());
        sub = (gTempOn && state == SVPPower::PS_USER_ON) ? 1 : 0;
        std::printf("GetPowerState -> %d, %u\n", (int)state, (unsigned)sub);
        std::fflush(stdout);
        return 1;
    }
};

} // namespace

int main(int argc, char** argv) {
    int port = kPowerServicePort, pubPort = kPowerListenerPort;
    std::string cmd;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--pub-port") && i + 1 < argc) pubPort = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cmd") && i + 1 < argc) cmd = argv[++i];
        else if (!std::strcmp(argv[i], "--temp-on")) gTempOn = true;
    }
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    RCF::RcfInitDeinit rcfInit;
    FakePower power;
    RCF::RcfServer server(RCF::TcpEndpoint("127.0.0.1", port));
    server.bind<SVP_PowerService>(power);
    server.start();
    RCF::RcfServer pubServer(RCF::TcpEndpoint("127.0.0.1", pubPort));
    pubServer.start();
    boost::shared_ptr<RCF::Publisher<SVP_PowerListener> > pub = pubServer.createPublisher<SVP_PowerListener>();

    std::atomic<bool> stop(false);
    std::thread reader([&] {
        long offset = 0;
        std::string partial;
        while (!stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            FILE* f = cmd.empty() ? nullptr : std::fopen(cmd.c_str(), "r");
            if (!f) continue;
            std::fseek(f, offset, SEEK_SET);
            int c;
            while ((c = std::fgetc(f)) != EOF) {
                ++offset;
                if (c != '\n') {
                    partial += static_cast<char>(c);
                    continue;
                }
                int s = 0, a = 0, b = 0;
                if (std::sscanf(partial.c_str(), "%d %d %d", &s, &a, &b) >= 1) {
                    gState = s;
                    pub->publish().OnPowerStateChanged(static_cast<SVPPower::PowerState>(s),
                                                       static_cast<unsigned char>(a), static_cast<unsigned char>(b));
                    std::printf("published %s (%d, %d, %d)\n", SVPPower::name(s), s, a, b);
                    std::fflush(stdout);
                }
                partial.clear();
            }
            std::fclose(f);
        }
    });
    std::printf("fake_power listening on 127.0.0.1:%d, publishing on %d\n", port, pubPort);
    std::fflush(stdout);
    int sig = 0;
    sigwait(&sigs, &sig);
    stop = true;
    reader.join();
    return 0;
}
