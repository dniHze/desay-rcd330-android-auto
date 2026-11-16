// Checks the bridge's 16 -> 48 kHz upsampler (src/resample.h): a tone must come out at the
// same level, and the images of a 3x upsampler (16 kHz +/- f) far below it. Those images are
// what a poor resampler leaves audible as a "chirp" over speech.
#include "resample.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// Power of a frequency in mono S16LE (Goertzel), in dBFS.
static double level_db(const std::vector<std::uint8_t> &pcm, double rate, double f, std::size_t skip) {
    const std::size_t n = pcm.size() / 2;
    const double w = 2 * 3.14159265358979323846 * f / rate, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (std::size_t i = skip; i < n; ++i) {
        const double x = static_cast<std::int16_t>(pcm[2 * i] | (pcm[2 * i + 1] << 8)) / 32768.0;
        const double s0 = x + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    const double amp = 2 * std::sqrt(p) / (n - skip);
    return 20 * std::log10(amp + 1e-12);
}

static std::vector<std::uint8_t> tone(double f, double rate, double secs) {
    std::vector<std::uint8_t> v;
    for (int i = 0; i < rate * secs; ++i) {
        const int s = static_cast<int>(std::lround(16384 * std::sin(2 * 3.14159265358979323846 * f * i / rate)));
        v.push_back(s & 0xff);
        v.push_back((s >> 8) & 0xff);
    }
    return v;
}

int main() {
    int fails = 0;
    const double in = 16000, out = 48000;
    for (double f : {440.0, 1000.0, 3000.0, 5000.0, 6000.0}) {
        aa::Upsampler up(3, 1);
        std::vector<std::uint8_t> x = tone(f, in, 1.0), y;
        // Fed in 20 ms chunks, as the bridge gets it.
        for (std::size_t i = 0; i < x.size(); i += 640) {
            std::vector<std::uint8_t> part = up.process(&x[i], std::min<std::size_t>(640, x.size() - i));
            y.insert(y.end(), part.begin(), part.end());
        }
        const double ref = level_db(x, in, f, 0);
        const double sig = level_db(y, out, f, 1000);
        const double img = std::max(level_db(y, out, in - f, 1000), level_db(y, out, in + f, 1000));
        const bool ok = std::fabs(sig - ref) < 1.0 && sig - img > 60;
        std::printf("%s: %5.0f Hz  in %.1f dB, out %.1f dB, worst image %.1f dB below\n",
                    ok ? "PASS" : "FAIL", f, ref, sig, sig - img);
        if (!ok) ++fails;
    }
    // Loud audio: every sample of a 4 kHz tone at full scale, the wave between them 3 dB above.
    // It used to be cut off flat (half the output at +/-full scale); now it bends below it.
    {
        std::vector<std::uint8_t> x;
        for (int i = 0; i < 16000; ++i) {
            const int s = static_cast<int>(std::lround(32767 * std::sqrt(2.0) *
                                                       std::sin(2 * 3.14159265358979323846 * 4000 * i / 16000 + 0.785398)));
            x.push_back(s & 0xff);
            x.push_back((s >> 8) & 0xff);
        }
        aa::Upsampler up(3, 1);
        const std::vector<std::uint8_t> y = up.process(x.data(), x.size());
        std::size_t flat = 0;
        int top = 0;
        for (std::size_t i = 0; i < y.size(); i += 2) {
            const int s = static_cast<std::int16_t>(y[i] | (y[i + 1] << 8));
            if (s >= 32767 || s <= -32768) ++flat;
            top = std::max(top, std::abs(s));
        }
        const std::uint64_t overs = up.overs();
        const bool ok = flat == 0 && overs > 0 && up.overs() == 0 && top > 32000;
        std::printf("%s: full-scale 4 kHz: %llu samples over full scale limited, none cut off flat (top %d)\n",
                    ok ? "PASS" : "FAIL", (unsigned long long) overs, top);
        if (!ok) ++fails;
    }
    // The knee: unchanged below -2 dBFS, monotonic and below full scale above it.
    {
        bool ok = aa::Upsampler::limit(26000.0f) == 26000 && aa::Upsampler::limit(-12345.0f) == -12345;
        long prev = 0;
        for (float a = 26000; a < 46341; a += 50) {
            const long v = aa::Upsampler::limit(a);
            ok = ok && v >= prev && v <= 32767 && aa::Upsampler::limit(-a) == -v;
            prev = v;
        }
        std::printf("%s: soft knee: unchanged below -2 dBFS, rising and below full scale up to +3 dB\n", ok ? "PASS" : "FAIL");
        if (!ok) ++fails;
    }
    std::printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
