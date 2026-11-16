// SPDX-License-Identifier: GPL-2.0-only
//
// cl_probe: RCF client for the CarLife service slot (svp.svc.carlife).
//
// Two jobs:
//   1. The go/no-go test on the unit: against the STOCK carlifeservice, prove that our
//      statically linked RCF build talks to the device's RCF 2.2 (same runtime version, 12),
//      and record the real callback sequence while CarLife connects.
//   2. Exercising aa-carlife-shim, on the desktop or on the unit.
//
// Only getters run by default. Commands that change state need --allow-write; do not use
// them against the stock service.

#include "carlife_idl.hpp"

#include <RCF/RCF.hpp>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <pthread.h>
#include <time.h>
#include <unistd.h>

namespace {

using namespace SVPCarLife;

double nowS() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

// Prints every callback the publisher sends. Method names must match ISVP_CarLifeListener.
class ListenerPrinter {
public:
    explicit ListenerPrinter(double t0) : mT0(t0) {}

    void OnAppStatusChange(S_CL_APP_STATUS s)                 { line("0 OnAppStatusChange(%d)", (int)s); }
    void OnCarLifeConnectStatusChange(S_CL_CONNECT_STATUS s, S_CL_CONNECT_ERROR e)
                                                              { line("1 OnCarLifeConnectStatusChange(%d, %d)", (int)s, (int)e); }
    void OnConnectingStatusPercentage(int p)                  { line("2 OnConnectingStatusPercentage(%d)", p); }
    void OnPhoneStatusChange(S_CL_PHONE_STATUS s)             { line("3 OnPhoneStatusChange(%d)", (int)s); }
    void OnTelephoneStatusChange(S_CL_TELEPHONE_STATUS s)     { line("4 OnTelephoneStatusChange(%d)", (int)s); }
    void OnScreenStatusChange(S_CL_SCREEN_STATUS s)           { line("5 OnScreenStatusChange(%d)", (int)s); }
    void OnConnectException(S_CL_EXCEPTION_TYPE t)            { line("6 OnConnectException(%d)", (int)t); }
    void OnGoToDeskTop()                                      { line("7 OnGoToDeskTop()"); }
    void OnModuleStatusChange(unsigned int m, unsigned int s) { line("8 OnModuleStatusChange(%u, %u)", m, s); }
    void OnBtCallingAndPauseCarlifeConnect(int c)             { line("9 OnBtCallingAndPauseCarlifeConnect(%d)", c); }

private:
    void line(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char msg[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        std::printf("[%8.3f] %s\n", nowS() - mT0, msg);
        std::fflush(stdout);
    }
    double mT0;
};

struct Options {
    std::string host;
    int svcPort;
    int pubPort;
    unsigned int timeoutMs;
    bool allowWrite;
};

void usage() {
    std::fprintf(stderr,
        "usage: cl_probe [options] <command> [args]\n"
        "options:\n"
        "  --host H            service host (default 127.0.0.1)\n"
        "  --svc-port P        ISVP_CarLifeService port (default 30407)\n"
        "  --pub-port P        ISVP_CarLifeListener publisher port (default 30408)\n"
        "  --timeout-ms N      connect and call timeout (default 3000)\n"
        "  --allow-write       permit the state-changing commands below\n"
        "read-only commands:\n"
        "  status              GetCarLifeConnectStatus (id 8). The go/no-go call.\n"
        "  ro                  every getter: ids 8, 10, 11, 12 (modules 1..6), 15, 16\n"
        "  listen [seconds]    subscribe to the publisher and print callbacks (0 = until ^C)\n"
        "state-changing commands (need --allow-write; not for the stock service):\n"
        "  connect | disconnect | clear | fg\n"
        "  touch <down|up|move> <x> <y> | rotate <code> <value>\n"
        "  play <n> | pause <n> | show <n> | startapp <n> | source <n> | modcmd <m> <c>\n"
        "exit status: 0 ok, 1 RCF error, 2 usage\n");
}

void configure(RcfClient<ISVP_CarLifeService>& c, const Options& o) {
    c.getClientStub().setConnectTimeoutMs(o.timeoutMs);
    c.getClientStub().setRemoteCallTimeoutMs(o.timeoutMs);
}

// RCF calls return RCF::FutureImpl<T>; assigning to a T makes the (synchronous) call.
int runGetters(RcfClient<ISVP_CarLifeService>& c, bool all) {
    S_CL_CONNECT_STATUS st = c.GetCarLifeConnectStatus();
    std::printf("GetCarLifeConnectStatus() = %d\n", (int)st);
    if (!all) return 0;
    int blockBt = c.NeedBlockBTPopup();
    std::printf("NeedBlockBTPopup() = %d\n", blockBt);
    int switchFg = c.NeedSwitchCarLifeAppToFG();
    std::printf("NeedSwitchCarLifeAppToFG() = %d\n", switchFg);
    for (unsigned int m = 1; m <= 6; ++m) {
        unsigned int ms = c.GetModuleStatus(m);
        std::printf("GetModuleStatus(%u) = %u\n", m, ms);
    }
    std::string bt = c.GetConnectCarlifePhoneBtAddress();
    std::printf("GetConnectCarlifePhoneBtAddress() = \"%s\"\n", bt.c_str());
    int ios = c.ConnectCarlifePhoneIsiOS();
    std::printf("ConnectCarlifePhoneIsiOS() = %d\n", ios);
    return 0;
}

int runListen(const Options& o, int seconds) {
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    ListenerPrinter printer(nowS());
    // Same as the stock subscriber (SVPCarLifeService_Client ctor): a TCP transport that
    // does not listen, needed to host the subscription connection.
    RCF::RcfServer subServer(RCF::TcpEndpoint(-1));
    subServer.start();
    RCF::SubscriptionParms parms;
    parms.setPublisherEndpoint(RCF::TcpEndpoint(o.host, o.pubPort));
    RCF::SubscriptionPtr sub = subServer.createSubscription<ISVP_CarLifeListener>(printer, parms);
    std::printf("subscribed to %s:%d (topic ISVP_CarLifeListener)\n", o.host.c_str(), o.pubPort);
    std::fflush(stdout);

    if (seconds > 0) {
        struct timespec ts;
        ts.tv_sec = seconds;
        ts.tv_nsec = 0;
        sigtimedwait(&sigs, nullptr, &ts);
    } else {
        int sig = 0;
        sigwait(&sigs, &sig);
    }
    bool stillConnected = sub->isConnected();
    sub->close();
    subServer.stop();
    std::printf("done (subscription %s at the end)\n", stillConnected ? "connected" : "lost");
    return 0;
}

S_CL_TOUCH_EVENT parseTouch(const char* s, bool& ok) {
    ok = true;
    if (!std::strcmp(s, "down")) return CL_TOUCH_DOWN;
    if (!std::strcmp(s, "up"))   return CL_TOUCH_UP;
    if (!std::strcmp(s, "move")) return CL_TOUCH_MOVE;
    ok = false;
    return CL_TOUCH_DOWN;
}

int runWrite(RcfClient<ISVP_CarLifeService>& c, int argc, char** argv) {
    const std::string cmd = argv[0];
    int r;
    if (cmd == "connect"    && argc == 1) { r = c.CarLifeConnection(); std::printf("CarLifeConnection() = %d\n", r); return 0; }
    if (cmd == "disconnect" && argc == 1) { r = c.CarLifeDisConnection(); std::printf("CarLifeDisConnection() = %d\n", r); return 0; }
    if (cmd == "clear"      && argc == 1) { c.ClearCarLifeStatus(); std::printf("ClearCarLifeStatus()\n"); return 0; }
    if (cmd == "fg"         && argc == 1) { c.GoToForeGround(); std::printf("GoToForeGround()\n"); return 0; }
    if (cmd == "touch" && argc == 4) {
        bool ok;
        S_CL_TOUCH_EVENT ev = parseTouch(argv[1], ok);
        if (!ok) return 2;
        c.SingleTouchInfo((unsigned)std::strtoul(argv[2], nullptr, 0),
                          (unsigned)std::strtoul(argv[3], nullptr, 0), ev);
        std::printf("SingleTouchInfo(%s, %s, %s)\n", argv[2], argv[3], argv[1]);
        return 0;
    }
    if (cmd == "rotate" && argc == 3) {
        c.RotateKeyEvent((unsigned)std::strtoul(argv[1], nullptr, 0), std::atoi(argv[2]));
        std::printf("RotateKeyEvent(%s, %s)\n", argv[1], argv[2]);
        return 0;
    }
    if (argc == 2) {
        int n = std::atoi(argv[1]);
        if (cmd == "play")     { r = c.Play(n);     std::printf("Play(%d) = %d\n", n, r); return 0; }
        if (cmd == "pause")    { r = c.Pause(n);    std::printf("Pause(%d) = %d\n", n, r); return 0; }
        if (cmd == "show")     { r = c.Show(n);     std::printf("Show(%d) = %d\n", n, r); return 0; }
        if (cmd == "startapp") { r = c.StartApp(n); std::printf("StartApp(%d) = %d\n", n, r); return 0; }
        if (cmd == "source")   { c.SetSourceAvailable(n); std::printf("SetSourceAvailable(%d)\n", n); return 0; }
    }
    if (cmd == "modcmd" && argc == 3) {
        c.SendModuleControlCmd((unsigned)std::strtoul(argv[1], nullptr, 0),
                               (unsigned)std::strtoul(argv[2], nullptr, 0));
        std::printf("SendModuleControlCmd(%s, %s)\n", argv[1], argv[2]);
        return 0;
    }
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    o.host = "127.0.0.1";
    o.svcPort = kCarLifeServicePort;
    o.pubPort = kCarLifePublisherPort;
    o.timeoutMs = 3000;
    o.allowWrite = false;

    int i = 1;
    for (; i < argc && argv[i][0] == '-'; ++i) {
        const char* a = argv[i];
        bool hasVal = i + 1 < argc;
        if (!std::strcmp(a, "--host") && hasVal)            o.host = argv[++i];
        else if (!std::strcmp(a, "--svc-port") && hasVal)   o.svcPort = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--pub-port") && hasVal)   o.pubPort = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--timeout-ms") && hasVal) o.timeoutMs = (unsigned)std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--allow-write"))          o.allowWrite = true;
        else { usage(); return 2; }
    }
    if (i >= argc) { usage(); return 2; }
    const std::string cmd = argv[i];

    RCF::RcfInitDeinit rcfInit;
    try {
        if (cmd == "listen") {
            int seconds = (i + 1 < argc) ? std::atoi(argv[i + 1]) : 0;
            return runListen(o, seconds);
        }

        RcfClient<ISVP_CarLifeService> client(RCF::TcpEndpoint(o.host, o.svcPort));
        configure(client, o);
        if (cmd == "status") return runGetters(client, false);
        if (cmd == "ro")     return runGetters(client, true);

        if (!o.allowWrite) {
            std::fprintf(stderr, "'%s' changes state; pass --allow-write (not against the stock service)\n",
                         cmd.c_str());
            return 2;
        }
        int rc = runWrite(client, argc - i, argv + i);
        if (rc == 2) usage();
        return rc;
    } catch (const RCF::Exception& e) {
        std::fprintf(stderr, "RCF error %d: %s\n", e.getErrorId(), e.getErrorString().c_str());
        return 1;
    }
}
