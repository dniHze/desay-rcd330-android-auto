// SPDX-License-Identifier: GPL-2.0-only
//
// The SVP source service (svp/bin/sourcemgr, registry svp.svc.source): which audio source owns
// the speakers and the media keys. Recovered from the stock client svp/lib/libsource.so
// (SVPSourceService_Client) and checked against sourcemgr's server methods:
//
//   * service "SVP_SourceService" on TCP 30205. Method IDs: the fnId each stub passes to
//     FutureImplBase (1, 2 and 4 are inlined into SVPSourceService_Client::RequestSource /
//     ReleaseSource / SetSourceStatus); types: libsource's RCF::ClientParameters<> instances.
//     Argument meanings from SVPSourceService_Server: RequestSource(source, client) passes
//     source to SVPSourceMgr::RequestSource and (source, client) to SVPSourceClient::Active;
//     ReleaseSource(source, client, restore) (server 0x3ba98: SVPSourceMgr::ReleaseSource(
//     source, restore), SVPSourceClient::Deactive(source, client); libsource sends
//     (source, getpid(), 1)); RegisterClient stores the client id (the pid) in the RCF session.
//   * listener "SVP_SourceListener": RCF publish/subscribe. sourcemgr runs a second RcfServer
//     on 127.0.0.1:30206 (0x3c818) with createPublisher<SVP_SourceListener>() (0x3c9ac);
//     libsource subscribes with setPublisherEndpoint(TcpEndpoint("127.0.0.1", 30206))
//     (SVPSourceService_Client::InitListener 0x3f72c). pubSourceStatus(source, status) goes out
//     for every source: SS_ACTIVED from SVPSourceMgr::ActiveSource, SS_DEACTIVED from
//     DeactiveSource, SS_RESTORED when a source is to resume after an interruption
//     (HandleSourceRestore 0x36078), SS_INBACKGROUND when a requested background source waits
//     behind a foreground one, plus every SetSourceStatus.
//   * sourcemgr never calls back PLAY: all of its cbSourceOp calls pass PAUSE or STOP. When an
//     interruption ends it publishes (source, SS_RESTORED) and the owner requests the source
//     again (RequestSource + ConfirmSource, which unmutes it). On PAUSE, it has already
//     published (source, SS_DEACTIVED); (new source, SS_ACTIVED) follows the PAUSE reply.
//   * callback "SVP_SourceCallback": a client binds it on its own RcfServer, calls
//     RegisterClient(getpid()) and turns that connection into a callback connection
//     (RCF::createCallbackConnection); sourcemgr then calls cbSourceOp(source, op) over it.
//     Its one method's ID and types are from sourcemgr's RcfClient<SVP_SourceCallback> stub.
//
// Stock carlifeservice: at session start SetSourceStatus(34..37, SS_ATTACHED) then
// (…, SS_VALID); at the end (…, SS_DETACHED); on Show(1) RequestSource(SID_CARLIFE_MEDIA) and,
// if that returns 1, ConfirmSource. Its stub answers cbSourceOp PLAY with 1 and pauses CarLife
// playback on PAUSE and STOP.
#ifndef AA_CARLIFE_SHIM_SOURCE_IDL_HPP
#define AA_CARLIFE_SHIM_SOURCE_IDL_HPP

#include <RCF/Idl.hpp>

namespace SVPSource {

// Ordinals as libsource's SVPSource::ToString(SourceStatus) table lists them; the stock calls
// agree (1 attached, 2 valid, 6 detached).
enum SourceStatus {
    SS_NULL       = 0,
    SS_ATTACHED   = 1,
    SS_VALID      = 2,
    SS_INVALID    = 3,
    SS_ACTIVED    = 4,
    SS_DEACTIVED  = 5,
    SS_DETACHED   = 6,
    SS_RESTORED   = 7,      // sourcemgr's jump table at 0x3bce4 (7.. as named there)
    SS_INBACKGROUND = 8,
    SS_MAX_       = 0x7fffffff
};

// From SVPSourceService_Client::cbSourceOp's dispatch to the ISVPSourceStub vtable
// (0 -> Play, 1 -> Pause, 2 -> Stop).
enum SourceOp {
    OP_PLAY   = 0,
    OP_PAUSE  = 1,
    OP_STOP   = 2,
    OP_MAX_   = 0x7fffffff
};

// libsource's SVPSource::ToString(SourceID) table.
const int SID_CARLIFE_MEDIA  = 34;
const int SID_CARLIFE_NAVI   = 35;
const int SID_CARLIFE_PROMPT = 36;
const int SID_CARLIFE_ASR    = 37;

} // namespace SVPSource

RCF_BEGIN(SVP_SourceService, "SVP_SourceService")
    RCF_METHOD_V1(void, RegisterClient, int)                                  // 0 (client pid)
    RCF_METHOD_R2(int, RequestSource, int, int)                               // 1 (source, client)
    RCF_METHOD_R3(int, ReleaseSource, int, int, int)                          // 2 (source, client, restore)
    RCF_METHOD_R1(int, ConfirmSource, int)                                    // 3
    RCF_METHOD_R2(int, SetSourceStatus, int, SVPSource::SourceStatus)         // 4
    RCF_METHOD_R1(SVPSource::SourceStatus, GetSourceStatus, int)              // 5
    RCF_METHOD_V0(void, SetIpodAttatchFilterFlag)                             // 6
    RCF_METHOD_V2(void, SetDeviceAttachFilterFlag, int, int)                  // 7
    RCF_METHOD_R0(int, GetLastBGSource)                                       // 8
RCF_END(SVP_SourceService)

RCF_BEGIN(SVP_SourceCallback, "SVP_SourceCallback")
    RCF_METHOD_R2(int, cbSourceOp, int, SVPSource::SourceOp)                  // 0
RCF_END(SVP_SourceCallback)

const int kSourceServicePort = 30205;

// Its one method: sourcemgr's RcfClient<SVP_SourceListener> stub (0x466ec): fnId 0,
// "pubSourceStatus", <Void, int, SourceStatus>.
RCF_BEGIN(SVP_SourceListener, "SVP_SourceListener")
    RCF_METHOD_V2(void, pubSourceStatus, int, SVPSource::SourceStatus)       // 0
RCF_END(SVP_SourceListener)

const int kSourceListenerPort = 30206;

#endif // AA_CARLIFE_SHIM_SOURCE_IDL_HPP
