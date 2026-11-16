// SPDX-License-Identifier: GPL-3.0-or-later
//
// PaOut — plays one AA audio stream through PulseAudio's simple API (libpulse-simple), the way
// the unit's own CarLife service plays its voice prompts (CMediaPlayer::PlaybackPcmInit_PaSimple:
// pa_simple_new(NULL, ..., PA_STREAM_PLAYBACK, NULL, "sv-as-carlife-prompt", ...)). The system
// PulseAudio daemon owns hw:MIB2AuxSound and mixes such streams over whatever else plays.
//
// Why not GStreamer's pulsesink: the unit's libgstpulse was built against a different
// libgstaudio ("specified instance size for type 'GstPulseRingBuffer' is smaller than the
// parent type's"); starting it fails with CRITICALs, and the retry then blocked the bridge's
// main loop for good (step 3, run 9: video froze when the voice assistant spoke).
//
// libpulse-simple is loaded at run time (the desktop build has no PulseAudio to link against).
// Every PulseAudio call runs on the stream's own thread: push() only queues, and drops the
// oldest data beyond ~1 s, so a slow or absent daemon can never stall video. The stream closes
// after a second without data (AA stops sending when the phone's stream ends), so PulseAudio and
// the platform see it end, and reopens on the next data.
//
// The unit's audiomgr sets a CarLife source's volume (CL_NAVI: the navigation volume) only on
// the "sv-as-carlife-prompt" streams that exist when the source becomes active
// (SVPRmtAudio::ActiveSource -> SVPPaVolMgr::SetVolumeDirect; it does not watch for new
// streams), and PulseAudio's stream-restore cannot keep volumes across boots here (/var/lib/pulse
// is read-only), so a stream opened after the source was taken started at full volume (step 3:
// the assistant's first chime of each boot was very loud). As stock carlifeservice registers
// each stream it opens with audiomgr (RegisterAudio, which applies the active source's volume),
// every stream that opens is reported to the CarLife shim, which makes that call (notify.h).
//
// A held stream (`hold`, sink "pasimple-open") instead opens at once and stays open until the
// session ends, fed silence from half a second after each sound until the next, so it exists
// whenever a source becomes active. Not the default since the shim registers streams; kept as
// a fallback (step 3 option "heldprompt").
//
// Streams slower than 48 kHz by an
// integer factor (AA's 16 kHz speech and system sounds) are upsampled here first (resample.h):
// the unit's PulseAudio resamples with the low-quality "ffmpeg" method.
//
// The server holds AA_PA_BUFFER_MS of each stream (default 200). Every sound ends with one log
// line: its length against the time it took to come, the longest wait for its data and how many
// waits outlasted the buffer (likely underruns, heard as breaks), its peak, and how many samples
// the upsampler had to limit (they were cut off flat before, heard as crackle).
//
// pa_simple calls have no timeout, so a PulseAudio that is alive but not serving keeps the
// stream's thread in pa_simple_write for good. The destructor (the session end, on the main
// loop) therefore waits at most 2 s for the thread (the longest write in 51 car runs was
// 120 ms); after that it leaves the thread to finish on its own (detached, with the state it
// uses) and says so, rather than stopping the bridge with it.
#ifndef AA_BRIDGE_PA_OUT_H
#define AA_BRIDGE_PA_OUT_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mono.h"
#include "resample.h"

namespace aa {

class PaOut {
public:
    PaOut(std::uint32_t id, std::uint32_t rate, std::uint32_t channels, std::string stream_name,
          bool hold = false);
    ~PaOut();

    void push(const std::uint8_t *data, std::size_t n);

    // Asks the thread to close the stream and end, without waiting (the destructor waits): the
    // session end asks every stream first, so that their waits overlap.
    void request_stop();

private:
    // Everything the stream's thread uses, shared with it so that a thread stuck in PulseAudio
    // can outlive this object.
    struct State {
        std::uint32_t id, rate, channels;
        bool hold;
        std::uint32_t out_rate;
        std::unique_ptr<Upsampler> up;
        std::string name;
        std::mutex mutex;
        MonoCond cv;
        std::deque<std::vector<std::uint8_t>> queue;
        std::deque<std::uint64_t> arrived; // when each queued chunk came (mono_us)
        std::size_t queued = 0;
        std::uint64_t dropped = 0;
        bool stop = false;
        bool done = false; // the thread has returned
    };
    static void run(std::shared_ptr<State> st);

    std::shared_ptr<State> st_;
    std::thread thread_;
};

} // namespace aa

#endif // AA_BRIDGE_PA_OUT_H
