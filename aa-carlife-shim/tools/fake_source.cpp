// SPDX-License-Identifier: GPL-2.0-only
//
// fake_source: stands in for the unit's SVP source service (sourcemgr, svp.svc.source) in
// test_desktop.sh. Serves SVP_SourceService on 127.0.0.1:<port>, takes the callback connection
// a client makes after RegisterClient, prints every call, and after each RequestSource calls
// the client back over that connection: cbSourceOp(source, PLAY); with --pause-ms N also
// PAUSE after N ms and PLAY again N ms later, as sourcemgr does when another source takes over
// and then gives back. With --no-play it never calls back PLAY, as the unit's sourcemgr did in
// step 3 (only PAUSE and STOP ever came).
//
// With --pub-port it also publishes SVP_SourceListener there (as sourcemgr does on 30206):
// after the PAUSE of source 34, (--taker N, SS_ACTIVED) as the source that took over, and with
// --restore, instead of the PLAY, (34, SS_RESTORED); each source then pauses only once.
// With --fail-status N the first N SetSourceStatus calls fail (an exception back to the
// client), as a busy or restarting sourcemgr's would.
//
//   fake_source [--port N] [--pause-ms N] [--no-play] [--pub-port N [--taker N] [--restore]]
//               [--fail-status N]   (default port 30205)

#include "source_idl.hpp"

#include <RCF/RCF.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <pthread.h>

namespace {

std::mutex gOut;
#define SAY(...) do { std::lock_guard<std::mutex> lk(gOut); std::printf(__VA_ARGS__); std::fflush(stdout); } while (0)

typedef RCF::Publisher<SVP_SourceListener> StatusPublisher;

class FakeSource {
public:
    FakeSource(int pauseMs, bool play) : mPauseMs(pauseMs), mPlay(play) {}

    void setPublisher(boost::shared_ptr<StatusPublisher> pub, int taker, bool restore) {
        mPub = pub;
        mTaker = taker;
        mRestore = restore;
    }

    void onCallbackConnection(RCF::RcfSessionPtr, RCF::ClientTransportAutoPtr transport) {
        std::lock_guard<std::mutex> lk(mMutex);
        mCallback.reset(new RcfClient<SVP_SourceCallback>(transport));
        SAY("callback connection created\n");
    }

    void RegisterClient(int pid) { SAY("RegisterClient(%d)\n", pid); }

    int RequestSource(int source, int client) {
        SAY("RequestSource(%d, %d)\n", source, client);
        bool cycle = mPauseMs > 0;
        if (mPub) {
            std::lock_guard<std::mutex> lk(mMutex);
            cycle = cycle && !mPaused[source & 63];
            mPaused[source & 63] = true;
        }
        // Call back from another thread, after this call has returned.
        std::thread([this, source, cycle] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (mPlay) callback(source, SVPSource::OP_PLAY);
            if (cycle) {
                std::this_thread::sleep_for(std::chrono::milliseconds(mPauseMs));
                callback(source, SVPSource::OP_PAUSE);
                if (source == SVPSource::SID_CARLIFE_MEDIA && mTaker > 0) publish(mTaker, SVPSource::SS_ACTIVED);
                std::this_thread::sleep_for(std::chrono::milliseconds(mPauseMs));
                if (source == SVPSource::SID_CARLIFE_MEDIA && mRestore) publish(source, SVPSource::SS_RESTORED);
                else if (mPlay) callback(source, SVPSource::OP_PLAY);
            }
        }).detach();
        return 1;
    }

    int ReleaseSource(int source, int x, int client) {
        SAY("ReleaseSource(%d, %d, %d)\n", source, x, client);
        return 1;
    }
    int ConfirmSource(int source) {
        SAY("ConfirmSource(%d)\n", source);
        return 1;
    }
    int SetSourceStatus(int source, SVPSource::SourceStatus st) {
        if (mFailStatus > 0) {
            --mFailStatus;
            SAY("SetSourceStatus(%d, %d) refused\n", source, (int)st);
            throw std::runtime_error("fake_source: SetSourceStatus refused");
        }
        SAY("SetSourceStatus(%d, %d)\n", source, (int)st);
        return 1;
    }
    void setFailStatus(int n) { mFailStatus = n; }
    SVPSource::SourceStatus GetSourceStatus(int source) {
        SAY("GetSourceStatus(%d)\n", source);
        return SVPSource::SS_VALID;
    }
    void SetIpodAttatchFilterFlag() { SAY("SetIpodAttatchFilterFlag()\n"); }
    void SetDeviceAttachFilterFlag(int a, int b) { SAY("SetDeviceAttachFilterFlag(%d, %d)\n", a, b); }
    int GetLastBGSource() { return 0; }

private:
    void publish(int source, SVPSource::SourceStatus st) {
        std::lock_guard<std::mutex> lk(mMutex);
        if (!mPub) return;
        mPub->publish().pubSourceStatus(source, st);
        SAY("pubSourceStatus(%d, %d)\n", source, (int)st);
    }

    void callback(int source, SVPSource::SourceOp op) {
        std::lock_guard<std::mutex> lk(mMutex);
        if (!mCallback) {
            SAY("cbSourceOp(%d, %d): no callback connection\n", source, (int)op);
            return;
        }
        try {
            int r = mCallback->cbSourceOp(source, op);
            SAY("cbSourceOp(%d, %d) -> %d\n", source, (int)op, r);
        } catch (const RCF::Exception& e) {
            SAY("cbSourceOp(%d, %d) failed: %s\n", source, (int)op, e.getErrorString().c_str());
        }
    }

    int mPauseMs;
    bool mPlay;
    std::atomic<int> mFailStatus{0};
    boost::shared_ptr<StatusPublisher> mPub;
    int mTaker = 0;
    bool mRestore = false;
    bool mPaused[64] = {};
    std::mutex mMutex;
    std::unique_ptr<RcfClient<SVP_SourceCallback> > mCallback;
};

} // namespace

int main(int argc, char** argv) {
    int port = kSourceServicePort, pauseMs = 0, pubPort = 0, taker = 0, failStatus = 0;
    bool play = true, restore = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--pause-ms") && i + 1 < argc) pauseMs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-play")) play = false;
        else if (!std::strcmp(argv[i], "--pub-port") && i + 1 < argc) pubPort = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--taker") && i + 1 < argc) taker = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--restore")) restore = true;
        else if (!std::strcmp(argv[i], "--fail-status") && i + 1 < argc) failStatus = std::atoi(argv[++i]);
    }
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    RCF::RcfInitDeinit rcfInit;
    FakeSource source(pauseMs, play);
    source.setFailStatus(failStatus);
    RCF::RcfServer server(RCF::TcpEndpoint("127.0.0.1", port));
    server.bind<SVP_SourceService>(source);
    server.setOnCallbackConnectionCreated(
        boost::bind(&FakeSource::onCallbackConnection, &source, _1, _2));
    server.start();
    std::unique_ptr<RCF::RcfServer> pubServer;
    if (pubPort > 0) {
        pubServer.reset(new RCF::RcfServer(RCF::TcpEndpoint("127.0.0.1", pubPort)));
        pubServer->start();
        source.setPublisher(pubServer->createPublisher<SVP_SourceListener>(), taker, restore);
    }
    SAY("fake_source listening on 127.0.0.1:%d\n", port);
    int sig = 0;
    sigwait(&sigs, &sig);
    return 0;
}
