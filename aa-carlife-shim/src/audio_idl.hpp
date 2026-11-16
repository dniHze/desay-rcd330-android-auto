// SPDX-License-Identifier: GPL-2.0-only
//
// The SVP audio service (svp/bin/audiomgr): volumes and mute. The shim only calls
// RegisterAudio, which applies the right volume to a PulseAudio stream that has just opened.
// Recovered from the stock client svp/lib/libaudio.so.1.0.0 (SVPAudioService_Client):
//
//   * service "ISVP_AudioService" on TCP 30203: the client ctor (0x42528) builds
//     RcfClient<ISVP_AudioService>(TcpEndpoint("0.0.0.0", 30203), ...).
//   * method IDs: the fnId each libaudio RcfClient<ISVP_AudioService>::M(CallOptions&, ...) stub
//     passes to FutureImplBase. RegisterAudio (0x5c86c) passes 14 and allocates
//     <int, std::string, unsigned int>. The methods before it are placeholders here: the shim
//     never calls them, and only the position matters for the ID.
//   * stock use: carlifeservice's CMediaPlayer::PlaybackPcmInit_PaSimple calls
//     SVP_GetAudioService()->RegisterAudio("sv-as-carlife-prompt", 0xffffffff) right after
//     pa_simple_new; libcarplayserver does the same for its streams.
//   * what it does (audiomgr SVPAudioService_Server::RegisterAudio 0x528d4 ->
//     SVPPaVolMgr::Register 0x9f768): for "sv-as-carlife-prompt" it sets the stream to the
//     navigation volume while CL_NAVI is active (SVPRmtAudio +136), to the speech-recognition
//     volume while an ASR-type CarLife source is (+144), else to the stream's own stored
//     volume; then SVPPaVolMgr::ControlVol on the PulseAudio sink inputs of that name. audiomgr
//     sets volumes on source activation too (SVPRmtAudio::ActiveSource), but only on streams
//     that exist then; it does not watch PulseAudio for new ones.
#ifndef AA_CARLIFE_SHIM_AUDIO_IDL_HPP
#define AA_CARLIFE_SHIM_AUDIO_IDL_HPP

#include <string>

#include <RCF/Idl.hpp>
#include <SF/string.hpp>

RCF_BEGIN(ISVP_AudioService, "ISVP_AudioService")
    RCF_METHOD_PLACEHOLDER()                                                    // 0 ActiveSource
    RCF_METHOD_PLACEHOLDER()                                                    // 1 IsSourceActived
    RCF_METHOD_PLACEHOLDER()                                                    // 2 ClearSource
    RCF_METHOD_PLACEHOLDER()                                                    // 3
    RCF_METHOD_PLACEHOLDER()                                                    // 4 GetVolume
    RCF_METHOD_PLACEHOLDER()                                                    // 5 SetMuteState
    RCF_METHOD_PLACEHOLDER()                                                    // 6 GetMuteState
    RCF_METHOD_PLACEHOLDER()                                                    // 7 SetSDVCMode
    RCF_METHOD_PLACEHOLDER()                                                    // 8 GetSDVCMode
    RCF_METHOD_PLACEHOLDER()                                                    // 9 SetAudioParams
    RCF_METHOD_PLACEHOLDER()                                                    // 10 GetAudioParams
    RCF_METHOD_PLACEHOLDER()                                                    // 11 SetVolume
    RCF_METHOD_PLACEHOLDER()                                                    // 12
    RCF_METHOD_PLACEHOLDER()                                                    // 13 SetVolumeAttri
    RCF_METHOD_R2(int, RegisterAudio, std::string, unsigned int)                // 14
RCF_END(ISVP_AudioService)

const int kAudioServicePort = 30203;
// RegisterAudio's second argument as stock passes it (every process).
const unsigned int kRegisterAllPids = 0xffffffffu;

#endif // AA_CARLIFE_SHIM_AUDIO_IDL_HPP
