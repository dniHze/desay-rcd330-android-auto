// SPDX-License-Identifier: GPL-2.0-only
//
// fake_can: stands in for the unit's CAN service (canservice, svp.svc.can) in test_desktop.sh.
// Serves SVP_CanService on 127.0.0.1:<port> and prints every GetCanCarplayInfo. The CarPlay
// info record says day (DayNight 0) until --night-after-ms, then night (DayNight 1). Every
// other method only exists because an RCF servant must implement the whole interface.
// With --pub-port it also publishes SVP_CanListener there, as canservice does on 30706: the
// vehicle speed (--speed-raw, 0.01 km/h) every 500 ms and whenever GetCanVehicleSpeed asks.
//
//   fake_can [--port N] [--night-after-ms N] [--pub-port N --speed-raw R]
//            (default port 30705; never night; no publisher)

#include "can_idl.hpp"

#include <RCF/RCF.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include <thread>

#include <pthread.h>

namespace {

using namespace SVPCanType;
typedef std::vector<unsigned char> Bytes;
typedef RCF::Publisher<SVP_CanListener> SpeedPublisher;

boost::shared_ptr<SpeedPublisher> gPub;
int gSpeedRaw = 0;
std::mutex gPubMutex; // the ticker and GetCanVehicleSpeed both publish; RCF's publisher is not thread-safe

void publishSpeed() {
    std::lock_guard<std::mutex> lk(gPubMutex);
    if (!gPub) return;
    gPub->publish().OnCanVehicleSpeedChanged(static_cast<unsigned short>(gSpeedRaw));
    static bool printed = false;
    if (!printed) std::printf("published speed %d\n", gSpeedRaw);
    printed = true;
    std::fflush(stdout);
}

class FakeCan {
public:
    explicit FakeCan(int nightAfterMs) : mNightAfterMs(nightAfterMs), mT0(std::chrono::steady_clock::now()) {}

    void GetCanCarplayInfo(SVPCanCarplayInfoType& info) {
        const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - mT0).count();
        info.DayNight = (mNightAfterMs >= 0 && ms >= mNightAfterMs) ? 1 : 0;
        info.TransmissionState = 1;
        info.RightHandDrive = 0;
        std::printf("GetCanCarplayInfo -> DayNight %u\n", (unsigned)info.DayNight);
        std::fflush(stdout);
    }

    void GetCanIgnitionMode() {}
    void GetCanIgnitionMode(SVPCanIgnModeType&) {}
    void GetCanVehicleSpeed() {
        std::printf("GetCanVehicleSpeed\n");
        std::fflush(stdout);
        publishSpeed();
    }
    int GetCanAudioLoweringPresent() { return 0; }
    void GetDescAdaptionBtSetting(SVPDescAdaptionBtSettingType&) {}
    void GetDescAdaptionVehicleCfg(SVPDescAdaptionVehicleConfigType&) {}
    void GetDescAdaptionStickerBits(unsigned short&) {}
    void GetDescCodingVehicleImage(SVPDescCodingVehicleImageType&) {}
    void GetDescCodingBtSetting(SVPDescCodingBtSettingType&) {}
    void GetDescCodingHmiSkin(unsigned char&) {}
    void GetDescCodingStartScreen(unsigned char&) {}
    void SetDescActuatorTestDone(SVPDescActuatorIdType, SVPDescActuatorCtrlType) {}
    void SetDescDatasetDownloadDone(SVPDescDatasetIdType) {}
    void SetDescRoutineTestDone(SVPDescRoutineIdType, SVPDescRoutineCtrlType, unsigned char) {}
    void SetDescMeasuredValue(SVPDescMeasuredIdType, Bytes&) {}
    void GetDescCarFuncAdaption(Bytes&) {}
    void SetBapScreenViewRequest(SVPBapScreenViewIdType, SVPBapScreenCtrlType) {}
    void SetBapScreenViewOptionRequest(SVPBapScreenViewIdType) {}
    void SetBapScreenVauleRequest(SVPBapScreenViewIdType) {}
    void SetBapPhoneDataInfo(SVPBapPhoneDataInfoType, Bytes&) {}
    void GetBapStatusBarVal() {}
    void SetBapScreenItemVal(SVPBapScreenSetIdType, unsigned char) {}
    void SetBapScreenItemParams(SVPBapScreenSetParamIdType, Bytes&) {}
    void GetBapClockFormat(SVPBapClockFormatType&) {}
    void SetBapWakeupFlag(SVPBapWakeupFlagType, SVPBapWakeupStateType) {}
    void SetBapScreenOffTimer(SVPBapScreenOffFuncType, SVPBapScreenOffTimerType) {}
    void GetBapRvcOpsState(SVPBapRvcOpsStateType&) {}
    int SendCanServiceMessage(unsigned short, Bytes&) { return 0; }

private:
    int mNightAfterMs;
    std::chrono::steady_clock::time_point mT0;
};

} // namespace

int main(int argc, char** argv) {
    int port = kCanServicePort, nightAfterMs = -1, pubPort = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--night-after-ms") && i + 1 < argc) nightAfterMs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--pub-port") && i + 1 < argc) pubPort = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--speed-raw") && i + 1 < argc) gSpeedRaw = std::atoi(argv[++i]);
    }
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    RCF::RcfInitDeinit rcfInit;
    FakeCan can(nightAfterMs);
    RCF::RcfServer server(RCF::TcpEndpoint("127.0.0.1", port));
    server.bind<SVP_CanService>(can);
    server.start();
    std::unique_ptr<RCF::RcfServer> pubServer;
    std::atomic<bool> stop(false);
    std::thread ticker;
    if (pubPort > 0) {
        pubServer.reset(new RCF::RcfServer(RCF::TcpEndpoint("127.0.0.1", pubPort)));
        pubServer->start();
        gPub = pubServer->createPublisher<SVP_CanListener>();
        ticker = std::thread([&stop] {
            while (!stop) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                publishSpeed();
            }
        });
    }
    std::printf("fake_can listening on 127.0.0.1:%d\n", port);
    std::fflush(stdout);
    int sig = 0;
    sigwait(&sigs, &sig);
    stop = true;
    if (ticker.joinable()) ticker.join();
    return 0;
}
