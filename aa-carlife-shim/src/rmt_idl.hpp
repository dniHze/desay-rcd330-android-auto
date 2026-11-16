// SPDX-License-Identifier: GPL-2.0-only
//
// The SVP "remote" service: raw command frames to the MCU ("radio sw", /dev/mcu_spi), which owns
// the amplifier/DSP routing. Served by svp/bin/devicemgr (SVPRmtService_Server, registry
// svp.svc.device.remote); stock clients use svp/lib/libdevice.so (SVPRmtService_Client, RCF
// 2.2) through svp/lib/libremote.so (SVPRmtAdapter):
//
//   * service "SVP_RmtService" (libdevice 0x72294) on TCP 30101: the client builds
//     RcfClient<SVP_RmtService>(TcpEndpoint("0.0.0.0", 30101), "") (SVPRmtService_Client ctor
//     0x711f8); devicemgr serves RcfServer(TcpEndpoint("0.0.0.0", 30101)) (ctor 0x77018) with
//     bind<SVP_RmtService> and setOnCallbackConnectionCreated (InitInstance 0x771bc).
//     SVPRmtService_Client::InitInstance (0x71038) only waits: SVP_WaitService(
//     "svp.svc.device.remote", 5000).
//   * method IDs and types: libdevice RcfClient<SVP_RmtService> stubs (fnId to FutureImplBase,
//     AllocateClientParameters<>). SendMessage -> SVPRmtService_Server::SendMessage (devicemgr
//     0x76c94) -> SVP_RmtWrapper_SendMessage(data, size) (0xa1784) -> hal-remote write() to
//     /dev/mcu_spi; it returns 1 if every byte was written, else 0. No validation, no reply.
//   * callback "SVP_RmtCallback" (ID/types from devicemgr's RcfClient<SVP_RmtCallback> stub):
//     only for receiving MCU messages. SVPRmtService_Client::InitListener (0x71508) binds it
//     on its own RcfServer(TcpEndpoint(-1)), calls SetMessageFilter(mask) (ID 0) and
//     RCF::createCallbackConnection; it does nothing when the mask is 0. The mask has bit
//     1 << (frame[1] >> 3) per wanted group (libremote SVPRmtAdapter::ProcessMessage 0x18a0).
//     Sending needs neither.
//
// Frame layout (libremote SVPRmtAdapter::SendCommand(unsigned short cmd, SVPRmtParameter* p),
// 0x176c; the vector it hands to ISVPRmtService vtable +40 = SendMessage):
//     [0] (cmd >> 8) & 7     [1] cmd >> 8     [2] cmd & 0xff
//     [3] len + 1            [4] 0            [5 .. 5+len) data
// with len/data from SVPRmtParameter {vtable; uint8 len at +4; uint8 data[256] at +5}; with no
// parameter, [3] = 1 and the frame is 5 bytes. SVPRmtAdapter::Init(mask) (0x1484) is
// InitInstance + SetMessageFilter(mask) (local, vtable +36) + AddListener; with mask 0 (what
// CarPlay passes) no RCF call happens before the first SendMessage.
//
// Group 0x35 is audio: audiomgr's SVPRmtAudio sends 0x3501 SetVolume, 0x3504 mute, 0x3509 /
// 0x350a ActiveSource off / on {2, mcu_source, AudioSourceType}, 0x350e SDVC, 0x350f audio
// params, 0x3511 beep, 0x3512 SetMicMute {1, state} (BT telephone only: "Only allow mute mic
// when in bt phone"), 0x3513 fast-play mode, 0x3515 volume feedback. 0x3514 is sent only by
// CarPlay (libcarplayserver AudioStreamPrepare_Init 0x12c30), for telephony streams, before
// opening MIB2BtOptOut/In: data {0x00, rate >> 8, rate & 0xff} with the stream's sample rate,
// i.e. for 16 kHz the frame 05 35 14 04 00 00 3E 80. No log or table names it; inferred: the
// voice-call sample rate (8/16/24 kHz) for the MCU's audio DSP. There is no matching "off"
// command (AudioStreamStop sends nothing to the MCU), and CarPlay Siri streams do not send it.
#ifndef AA_CARLIFE_SHIM_RMT_IDL_HPP
#define AA_CARLIFE_SHIM_RMT_IDL_HPP

#include <vector>

#include <RCF/Idl.hpp>
#include <SF/vector.hpp>

RCF_BEGIN(SVP_RmtService, "SVP_RmtService")
    RCF_METHOD_V1(void, SetMessageFilter, unsigned int)                                    // 0
    RCF_METHOD_R1(int, SendMessage, std::vector<unsigned char>&)                           // 1
RCF_END(SVP_RmtService)

RCF_BEGIN(SVP_RmtCallback, "SVP_RmtCallback")
    RCF_METHOD_V1(void, ProcessMessage, std::vector<unsigned char>&)                       // 0
RCF_END(SVP_RmtCallback)

const int kRmtServicePort = 30101;

namespace SVPRmt {

const unsigned short CMD_AUDIO_VOICE_RATE = 0x3514;   // name inferred (see header)

// The frame SVPRmtAdapter::SendCommand(cmd, {len, data}) sends.
inline std::vector<unsigned char> MakeFrame(unsigned short cmd, const unsigned char* data,
                                            unsigned char len) {
    std::vector<unsigned char> f;
    f.push_back((unsigned char)((cmd >> 8) & 7));
    f.push_back((unsigned char)(cmd >> 8));
    f.push_back((unsigned char)(cmd & 0xff));
    f.push_back((unsigned char)(len + 1));
    f.push_back(0);
    if (data) f.insert(f.end(), data, data + len);
    return f;
}

} // namespace SVPRmt

#endif // AA_CARLIFE_SHIM_RMT_IDL_HPP
