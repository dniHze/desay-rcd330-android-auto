// SPDX-License-Identifier: GPL-2.0-only
//
// fake_bt: stands in for the unit's Bluetooth service (btservice, svp.svc.bluetooth) in
// test_desktop.sh. Serves SVP_BtService on 127.0.0.1:<port> and prints every
// SetECNRAudioPath (the car mic's route). GetAttribute(3), the local Bluetooth address, answers
// as btservice does: "xx:xx:xx:xx:xx:xx", all zeros while Bluetooth is not up (without --addr).
// The other methods only exist because an RCF servant must implement the whole interface.
// Pairing (bt_idl.hpp): --paired lists the paired phones (GetDeviceNum / GetDevice); with
// --pub-port it publishes SVP_BtListener there, as btservice does on 30316, and with --phone it
// plays that phone bonding: --confirm-after-ms after SetAutoAcceptConnect(1),
// OnPairingComfirmRequest(123456, the phone's 6 address bytes); on AcceptSspRequest(1) the
// phone is paired: OnPairListChanged, then OnPairStatusChanged(0, its address in lowercase).
// With --pin it plays legacy PIN pairing instead: OnPairingPinCodeRequest, and
// AcceptPinCodeRequest(pin) pairs the phone. With --rebond the phone (in --paired as well) has
// forgotten the car: it bonds anyway, --confirm-after-ms after the paired list is read.
// With --stranger ADDR another device starts bonding too, 300 ms before the phone: its confirm
// request must be left to the Bluetooth app.
// Music link: --a2dp ADDR holds A2DP (GetAvpConnectAddr); Disconnect / Connect are printed, and
// with --a2dp-back-ms N the link comes back N ms after a Disconnect, once (OnConnectStatusChanged).
// Phone link: --hfp ADDR holds HFP (GetHfpConnectAddr); SetCapability is printed, and with
// --hfp-drop-ms N the link goes N ms after the first SetCapability(10, 1), once
// (OnDisconnectStatusChanged).
//
//   fake_bt [--port N] [--addr xx:xx:xx:xx:xx:xx] [--paired a,b,...]
//           [--pub-port N [--noise] [--phone AA:BB:.. --confirm-after-ms N [--pin] [--rebond]]]
//           (default port 30315; --noise: OnMusicPlaytimeChanged every 100 ms, an event the
//           pairing loop does not use)

#include "bt_idl.hpp"

#include <RCF/RCF.hpp>

#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

namespace {

using namespace SVPBtType;

typedef RCF::Publisher<SVP_BtListener> EventPublisher;

std::string lowerAddr(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

class FakeBt {
public:
    explicit FakeBt(const std::string& addr) : mAddr(addr) {}

    void setRebond(bool on) { mRebond = on; }
    void setStranger(const std::string& addr) { mStranger = addr; }

    void setPairing(std::vector<std::string> paired, boost::shared_ptr<EventPublisher> pub, std::string phone,
                    int confirmAfterMs, bool pin) {
        mPaired = paired;
        mPub = pub;
        mPhone = phone;
        mConfirmAfterMs = confirmAfterMs;
        mPinMode = pin;
    }

    int GetDeviceNum(BT_DEVICE_TYPE type) {
        int n;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            n = type == BT_DEVICE_PAIRED ? (int)mPaired.size() : 0;
        }
        std::printf("GetDeviceNum(%d) -> %d\n", (int)type, n);
        std::fflush(stdout);
        if (mRebond) startBonding();
        return n;
    }
    int GetDevice(int i, SVPBtDevice& d, BT_DEVICE_TYPE type) {
        std::lock_guard<std::mutex> lk(mMutex);
        if (type != BT_DEVICE_PAIRED || i < 0 || i >= (int)mPaired.size()) return 0;
        d.name = "phone " + std::to_string(i);
        d.address = lowerAddr(mPaired[i]);
        return 1;
    }
    void SetAutoAcceptConnect(int on) {
        std::printf("SetAutoAcceptConnect(%d)\n", on);
        std::fflush(stdout);
        mAutoAccept = on;
        if (on) startBonding();
    }

    // The phone connects and bonds: SSP numeric comparison (or a PIN request).
    void startBonding() {
        if (mPub && !mPhone.empty() && !mBonding) {
            mBonding = true;
            std::thread([this] {
                std::this_thread::sleep_for(std::chrono::milliseconds(mConfirmAfterMs));
                if (!mStranger.empty()) {
                    const std::string other = rawAddr(mStranger);
                    publish([&](EventPublisher& p) { p.publish().OnPairingComfirmRequest(654321u, other); });
                    std::printf("published PairingComfirmRequest(654321) from %s\n", mStranger.c_str());
                    std::fflush(stdout);
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }
                const std::string raw = rawAddr(mPhone);
                if (mPinMode) {
                    publish([&](EventPublisher& p) { p.publish().OnPairingPinCodeRequest(0u, raw, std::string("phone")); });
                    std::printf("published PairingPinCodeRequest\n");
                } else {
                    publish([&](EventPublisher& p) { p.publish().OnPairingComfirmRequest(123456u, raw); });
                    std::printf("published PairingComfirmRequest(123456)\n");
                }
                std::fflush(stdout);
            }).detach();
        }
    }
    int GetAutoAcceptConnect() { return mAutoAccept; }

    int GetHfpConnectAddr(std::string& addr) {
        std::lock_guard<std::mutex> lk(mMutex);
        addr = mHfp;
        return mHfp.empty() ? 0 : 1;
    }
    void setHfp(const std::string& addr, int dropMs) {
        mHfp = addr;
        mHfpDropMs = dropMs;
    }

    int GetAvpConnectAddr(std::string& addr) {
        addr = mA2dp;
        std::printf("GetAvpConnectAddr -> %s\n", addr.c_str());
        std::fflush(stdout);
        return 1;
    }
    void Disconnect(BT_SERVICE svc) {
        std::printf("Disconnect(%d)\n", (int)svc);
        std::fflush(stdout);
        if ((int)svc == 2 && mA2dpBackMs > 0 && !mA2dp.empty()) {
            const int ms = mA2dpBackMs;
            mA2dpBackMs = 0; // once
            std::thread([this, ms] {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                const std::string a = lowerAddr(mA2dp);
                publish([&](EventPublisher& p) { p.publish().OnConnectStatusChanged(BT_PROFILE_CONNECT(2), 1, a); });
                std::printf("published ConnectStatusChanged(2, 1, %s)\n", a.c_str());
                std::fflush(stdout);
            }).detach();
        }
    }
    void Connect(const std::string& addr, BT_SERVICE svc) {
        std::printf("Connect(%s, %d)\n", addr.c_str(), (int)svc);
        std::fflush(stdout);
    }
    void setA2dp(const std::string& addr, int backMs) {
        mA2dp = addr;
        mA2dpBackMs = backMs;
    }
    int SetAutoAcceptPairing(int on) {
        std::printf("SetAutoAcceptPairing(%d)\n", on);
        std::fflush(stdout);
        return 1;
    }
    void startNoise() {
        std::thread([this] {
            for (unsigned t = 0;; ++t) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                publish([&](EventPublisher& p) { p.publish().OnMusicPlaytimeChanged(t); });
            }
        }).detach();
    }

    void AcceptPinCodeRequest(const std::string& pin) {
        std::printf("AcceptPinCodeRequest(%s)\n", pin.c_str());
        std::fflush(stdout);
        if (pin.empty() || !mPub || mPhone.empty()) return;
        paired();
    }
    void AcceptSspRequest(int accept) {
        std::printf("AcceptSspRequest(%d)\n", accept);
        std::fflush(stdout);
        if (accept != 1 || !mPub || mPhone.empty()) return;
        paired();
    }

private:
    // An address as OnPairingComfirmRequest carries it: 6 raw bytes, cut at the first 0.
    static std::string rawAddr(const std::string& addr) {
        std::string raw;
        unsigned v[6] = {0};
        std::sscanf(addr.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
        for (unsigned b : v) {
            if (!b) break; // btservice cuts the raw address at the first 0 byte
            raw += static_cast<char>(b);
        }
        return raw;
    }

    void paired() {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mPaired.push_back(mPhone);
        }
        std::thread([this] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            publish([](EventPublisher& p) { p.publish().OnPairListChanged(); });
            const std::string a = lowerAddr(mPhone);
            publish([&](EventPublisher& p) { p.publish().OnPairStatusChanged(PAIR_SUCCESS, a); });
            std::printf("published PairStatusChanged(0, %s)\n", a.c_str());
            std::fflush(stdout);
        }).detach();
    }

public:

    int GetAttribute(Attribute attr, std::string& value) {
        std::printf("GetAttribute(%d)\n", (int)attr);
        std::fflush(stdout);
        if ((int)attr != 3) return 0;
        value = mAddr;
        return 1;
    }

    void SetECNRAudioPath(ECNR_AUDIO_PATH path) {
        std::printf("SetECNRAudioPath(%d)\n", (int)path);
        std::fflush(stdout);
    }

    void OpenBt(int) {}
    void ShutdownBt(int) {}
    BT_STATUS GetBtStatus() { return BT_STATUS(0); }
    int GetLastBtEnable() { return 1; }
    unsigned int GetBtVersion() { return 0; }
    int SetAttribute(Attribute, const std::string&) { return 0; }
    int GetCapability(Capability) { return 0; }
    int SetCapability(Capability cap, int on) {
        std::printf("SetCapability(%d, %d)\n", (int)cap, on);
        std::fflush(stdout);
        if ((int)cap == 10 && on && mHfpDropMs > 0) {
            const int ms = mHfpDropMs;
            mHfpDropMs = 0; // once
            std::thread([this, ms] {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                {
                    std::lock_guard<std::mutex> lk(mMutex);
                    mHfp.clear();
                }
                publish([](EventPublisher& p) { p.publish().OnDisconnectStatusChanged(BT_PROFILE_CONNECT(1), 1); });
                std::printf("published DisconnectStatusChanged(1, 1)\n");
                std::fflush(stdout);
            }).detach();
        }
        return 1;
    }
    void ConfigTrace(unsigned short) {}
    int SetECNRParams(std::vector<unsigned char>&) { return 0; }
    void SetECNRParamsMode(ECNR_PARAMS_MODE) {}
    ECNR_PARAMS_MODE GetECNRParamsMode() { return ECNR_PARAMS_MODE(1); }

private:
    template <class F> void publish(F f) {
        std::lock_guard<std::mutex> lk(mPubMutex); // RCF's publisher is not thread-safe
        if (mPub) f(*mPub);
    }

    std::string mAddr;
    std::mutex mMutex, mPubMutex;
    std::vector<std::string> mPaired;
    boost::shared_ptr<EventPublisher> mPub;
    std::string mPhone;
    int mConfirmAfterMs = 500;
    int mAutoAccept = 0;
    bool mBonding = false;
    bool mPinMode = false;
    bool mRebond = false;
    std::string mStranger;
    std::string mA2dp;
    int mA2dpBackMs = 0;
    std::string mHfp;
    int mHfpDropMs = 0;
};

} // namespace

int main(int argc, char** argv) {
    int port = kBtServicePort, pubPort = 0, confirmAfterMs = 500;
    bool pin = false, noise = false, rebond = false;
    std::string a2dp, hfp;
    int a2dpBackMs = 0, hfpDropMs = 0;
    std::string addr = "00:00:00:00:00:00", phone, stranger;
    std::vector<std::string> paired;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--addr") && i + 1 < argc) addr = argv[++i];
        else if (!std::strcmp(argv[i], "--pub-port") && i + 1 < argc) pubPort = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--phone") && i + 1 < argc) phone = argv[++i];
        else if (!std::strcmp(argv[i], "--confirm-after-ms") && i + 1 < argc) confirmAfterMs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--pin")) pin = true;
        else if (!std::strcmp(argv[i], "--noise")) noise = true;
        else if (!std::strcmp(argv[i], "--rebond")) rebond = true;
        else if (!std::strcmp(argv[i], "--stranger") && i + 1 < argc) stranger = argv[++i];
        else if (!std::strcmp(argv[i], "--a2dp") && i + 1 < argc) a2dp = argv[++i];
        else if (!std::strcmp(argv[i], "--a2dp-back-ms") && i + 1 < argc) a2dpBackMs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--hfp") && i + 1 < argc) hfp = argv[++i];
        else if (!std::strcmp(argv[i], "--hfp-drop-ms") && i + 1 < argc) hfpDropMs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--paired") && i + 1 < argc) {
            std::string list = argv[++i];
            size_t p = 0;
            while (p <= list.size()) {
                size_t c = list.find(',', p);
                if (c == std::string::npos) c = list.size();
                if (c > p) paired.push_back(list.substr(p, c - p));
                p = c + 1;
            }
        }
    }
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    RCF::RcfInitDeinit rcfInit;
    FakeBt bt(addr);
    RCF::RcfServer server(RCF::TcpEndpoint("127.0.0.1", port));
    server.bind<SVP_BtService>(bt);
    server.start();
    std::unique_ptr<RCF::RcfServer> pubServer;
    boost::shared_ptr<EventPublisher> pub;
    if (pubPort > 0) {
        pubServer.reset(new RCF::RcfServer(RCF::TcpEndpoint("127.0.0.1", pubPort)));
        pubServer->start();
        pub = pubServer->createPublisher<SVP_BtListener>();
    }
    bt.setPairing(paired, pub, phone, confirmAfterMs, pin);
    bt.setRebond(rebond);
    bt.setStranger(stranger);
    bt.setA2dp(a2dp, a2dpBackMs);
    bt.setHfp(hfp, hfpDropMs);
    if (noise && pub) bt.startNoise();
    std::printf("fake_bt listening on 127.0.0.1:%d\n", port);
    std::fflush(stdout);
    int sig = 0;
    sigwait(&sigs, &sig);
    return 0;
}
