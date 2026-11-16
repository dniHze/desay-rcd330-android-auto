// SPDX-License-Identifier: GPL-3.0-or-later
//
// Upsampler — integer-ratio upsampling of interleaved S16 PCM with a polyphase windowed-sinc
// low-pass, for the PulseAudio streams (pa_out.h).
//
// The unit's PulseAudio resamples with "ffmpeg" (/etc/pulse/daemon.conf), libavcodec's old
// low-quality resampler. Android Auto's speech and system sounds come at 16 kHz and the sink
// runs at 48 kHz, so every assistant prompt went through it and sounded chirpy (step 3, run
// 14). Handing PulseAudio 48 kHz avoids its resampler altogether.
//
// Interpolation by L: each input sample yields L outputs, output phase p being
// sum_j h[p + j*L] * x[n - j] over the last kTaps inputs. h is a Blackman-windowed sinc with
// its cutoff just below the input Nyquist, scaled by L to keep unity gain.
//
// The new samples between the old ones can peak above them: loud audio whose samples sit near
// full scale comes out above it (a 4 kHz tone with every sample at full scale, by 3 dB). Those
// were cut off flat, which crackles. Now the top 2 dB is a soft knee (limit()): below -2 dBFS
// nothing changes, above it the level bends smoothly towards full scale (a sample at full scale
// comes out 0.4 dB lower; 3 dB above it, just under it). overs() counts the samples that would
// have been cut off.
#ifndef AA_BRIDGE_RESAMPLE_H
#define AA_BRIDGE_RESAMPLE_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aa {

class Upsampler {
public:
    static const int kTaps = 32; // per phase

    Upsampler(unsigned factor, unsigned channels)
        : L_(factor), ch_(channels), h_(factor * kTaps), hist_(channels * kTaps, 0.0f) {
        const double pi = 3.14159265358979323846;
        const int n = static_cast<int>(h_.size());
        const double fc = 0.45 / L_; // cycles per output sample: 0.9 x the input Nyquist
        for (int k = 0; k < n; ++k) {
            const double t = k - (n - 1) / 2.0;
            const double sinc = t == 0 ? 2 * fc : std::sin(2 * pi * fc * t) / (pi * t);
            const double w = 0.42 - 0.5 * std::cos(2 * pi * k / (n - 1)) + 0.08 * std::cos(4 * pi * k / (n - 1));
            h_[k] = static_cast<float>(L_ * sinc * w);
        }
    }

    unsigned factor() const { return L_; }

    // Forget the input history (as after silence).
    void reset() { std::fill(hist_.begin(), hist_.end(), 0.0f); }

    // Output samples above full scale (limited) since the last call.
    std::uint64_t overs() {
        const std::uint64_t n = overs_;
        overs_ = 0;
        return n;
    }

    // An output sample: below kKnee as it is, above it bent smoothly (equal slope at the knee)
    // towards full scale, which it never reaches.
    static long limit(float acc) {
        const float kKnee = 26028.0f; // -2 dBFS
        const float kRoom = 32767.0f - kKnee;
        float a = acc < 0 ? -acc : acc;
        if (a > kKnee) a = kKnee + kRoom * std::tanh((a - kKnee) / kRoom);
        const long v = static_cast<long>(a + 0.5f);
        return acc < 0 ? -v : v;
    }

    // Interleaved S16LE in, interleaved S16LE out (factor x as many frames).
    std::vector<std::uint8_t> process(const std::uint8_t *data, std::size_t bytes) {
        const std::size_t frames = bytes / (2 * ch_);
        std::vector<std::uint8_t> out(frames * L_ * ch_ * 2);
        std::size_t o = 0;
        for (std::size_t f = 0; f < frames; ++f) {
            for (unsigned c = 0; c < ch_; ++c) {
                // Newest sample first in this channel's history.
                float *x = &hist_[c * kTaps];
                for (int j = kTaps - 1; j > 0; --j) x[j] = x[j - 1];
                const std::size_t i = (f * ch_ + c) * 2;
                x[0] = static_cast<std::int16_t>(data[i] | (data[i + 1] << 8));
            }
            for (unsigned p = 0; p < L_; ++p) {
                for (unsigned c = 0; c < ch_; ++c) {
                    const float *x = &hist_[c * kTaps];
                    float acc = 0;
                    for (int j = 0; j < kTaps; ++j) acc += h_[p + j * L_] * x[j];
                    if (acc > 32767.0f || acc < -32768.0f) ++overs_;
                    const long v = limit(acc);
                    out[o++] = static_cast<std::uint8_t>(v & 0xff);
                    out[o++] = static_cast<std::uint8_t>((v >> 8) & 0xff);
                }
            }
        }
        return out;
    }

private:
    unsigned L_, ch_;
    std::vector<float> h_;
    std::vector<float> hist_;
    std::uint64_t overs_ = 0;
};

} // namespace aa

#endif // AA_BRIDGE_RESAMPLE_H
