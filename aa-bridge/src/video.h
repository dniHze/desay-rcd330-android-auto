// SPDX-License-Identifier: GPL-3.0-or-later
//
// VideoOut — one session's video: the pipeline (pipeline.h) and what keeps it going.
//
// * The pipeline is built at the first video frame, not at KIND_VIDEO_START: the receiver's
//   first session after the phone switches to accessory mode often dies right after service
//   discovery (step 3 runs 42-47), and each such session used to create an IVA-HD decoder and
//   a sink for nothing, then log a drain error.
// * A video error (a bus ERROR, a pipeline that does not start, or a stall: the decoder or sink
//   stops putting frames out while frames keep coming in) stops the pipeline, and video only:
//   the feed and the audio go on. The pipeline is rebuilt at the next keyframe (an IDR, or the
//   parameter sets that precede one), with the last parameter sets seen put ahead of it if the
//   keyframe comes without them. If no keyframe comes within 5 s it is rebuilt anyway, and the
//   decoder picks up at the phone's next keyframe. At most 3 rebuilds a minute; past that the
//   next waits for the oldest to be a minute old. A rebuilt sink makes its ILM surface anew,
//   hidden: once its first frame is up the shim is told ("video", notify.h) to show it again.
// * At the session end the pipeline is drained (EOS, up to 2 s; it normally takes
//   milliseconds) if it has had frames, then stopped.
#ifndef AA_BRIDGE_VIDEO_H
#define AA_BRIDGE_VIDEO_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "pipeline.h"

namespace aa {

class VideoOut {
public:
    VideoOut(std::string decoder, std::string sink) : decoder_(std::move(decoder)), sink_(std::move(sink)) {}
    ~VideoOut() { pipeline_.stop(); }

    // KIND_VIDEO_START: the codec the frames will use. Nothing is built yet.
    void set_codec(Codec codec);

    // One KIND_VIDEO record (an Annex-B access unit).
    void push(const std::uint8_t *data, std::size_t n);

    // Bus errors and stalls; cheap, call from the main loop.
    void poll();

    // The session is over: drain (if `drain` and frames went in), then stop.
    void finish(bool drain);

    unsigned errors() const { return errors_; }

private:
    bool build();
    void fail(const std::string &why);
    bool may_rebuild(std::uint64_t now);
    void remember_parameter_sets(const std::uint8_t *data, std::size_t n);

    const std::string decoder_, sink_;
    Codec codec_ = Codec::H264;
    Pipeline pipeline_;
    bool built_once_ = false;           // a build has been tried this session
    bool announce_ = false;             // a rebuilt pipeline's first frame is still to be reported
    std::uint64_t down_since_ = 0;      // when the last error stopped the pipeline
    unsigned errors_ = 0;               // video errors this session
    std::deque<std::uint64_t> rebuilds_; // when the rebuilds of the last minute were tried
    std::uint64_t waiting_logged_ = 0;  // when "waiting for a rebuild slot" was last logged
    std::map<unsigned, std::vector<std::uint8_t>> params_; // NAL type -> last parameter set seen
    unsigned rejected_ = 0;             // pushes appsrc refused since the last line about it
    std::uint64_t rejected_logged_ = 0;
};

} // namespace aa

#endif // AA_BRIDGE_VIDEO_H
