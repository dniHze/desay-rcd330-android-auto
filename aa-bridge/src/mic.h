// SPDX-License-Identifier: GPL-3.0-or-later
//
// MicCapture — captures the car microphone and feeds it to the AA phone as the voice uplink.
//
// The LIVI receiver (livi-aa-headless) binds the mic socket (AA_MIC_SOCK, default
// /tmp/aa.mic.sock) only while the phone has requested the microphone, and relays whatever
// it reads there into the session, which forwards it to the phone over ch MIC_INPUT. The
// wire on that socket is feedproto KIND_MIC records: [u32 len][u8 kind=4][u32 id][u64 ts][PCM].
//
// The source is "alsa:<device>": libasound directly (snd_pcm_readi at rate/channels), as the
// unit's own CarLife service records (the unit's GStreamer alsasrc does not negotiate, so there
// is no GStreamer capture path).
//
// A background thread retries connect() until the socket exists (i.e. until the phone asks
// for the mic), captures while connected, and closes the device on disconnect so ALSA is only
// held while the phone actually wants the mic. AA's default mic format is 16 kHz mono S16LE.
// Each session logs the bytes captured and the loudest sample, so a source that only gives
// silence shows.
#ifndef AA_BRIDGE_MIC_H
#define AA_BRIDGE_MIC_H

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace aa {

class MicCapture {
public:
    MicCapture(std::string sock, std::string src, std::uint32_t rate, std::uint32_t channels)
        : sock_(std::move(sock)), src_(std::move(src)), rate_(rate), channels_(channels) {}
    ~MicCapture() { stop(); }

    bool enabled() const { return src_ != "none"; }

    void start();
    void stop();

private:
    void run();                                           // connect -> capture -> reconnect loop
    bool capture_alsa(int fd, const std::string &device); // one connected session; false = stop

    std::string sock_;
    std::string src_;
    std::uint32_t rate_;
    std::uint32_t channels_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace aa

#endif // AA_BRIDGE_MIC_H
