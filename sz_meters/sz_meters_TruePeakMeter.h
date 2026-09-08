#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace sz
{
/** Inter-sample (true) peak meter.

    ITU-R BS.1770-4 Annex 2 estimates the true peak by upsampling at least 4x
    and taking the peak of the interpolated signal. This is a 4-phase,
    16-tap-per-phase Blackman-Harris windowed-sinc detector.

    STATED PLAINLY, because it is the sort of thing meters lie about: the
    coefficients are generated at prepare() time as a windowed sinc rather than
    being the coefficient table printed in Annex 2. The structure and length
    match the Recommendation; the exact stopband ripple does not. Measured
    against the tabulated filter this reads within a few hundredths of a dB on
    programme material, but it is not a bit-exact BS.1770-4 implementation and
    must not be described as certified.

    At sample rates >= 96 kHz the Recommendation permits 2x. This keeps 4x
    unconditionally, for a stable cost and one behaviour at every sample rate.

    Not thread safe, and not meant to be: feed it from the audio thread and read
    it from the audio thread. Getting a number to the UI is the caller's job.
*/
class TruePeakMeter
{
public:
    static constexpr int oversampleFactor = 4;
    static constexpr int tapsPerPhase     = 16;
    static constexpr int totalTaps        = oversampleFactor * tapsPerPhase;

    /** The kernel does not depend on the sample rate; the parameter exists so
        that this matches every other prepare() in a signal chain. */
    void prepare (double /*sampleRate*/)
    {
        buildKernel();
        reset();
    }

    void reset() noexcept
    {
        historyL.fill (0.0f);
        historyR.fill (0.0f);
        index = 0;
        peakL = 0.0f;
        peakR = 0.0f;
    }

    /** Feeds one stereo frame and updates the running maxima. */
    void process (float left, float right) noexcept
    {
        historyL[static_cast<size_t> (index)] = left;
        historyR[static_cast<size_t> (index)] = right;

        for (int phase = 0; phase < oversampleFactor; ++phase)
        {
            float accL = 0.0f;
            float accR = 0.0f;

            const float* coeffs = &kernel[static_cast<size_t> (phase * tapsPerPhase)];

            for (int tap = 0; tap < tapsPerPhase; ++tap)
            {
                const auto h = static_cast<size_t> ((index - tap + tapsPerPhase) % tapsPerPhase);
                accL += coeffs[tap] * historyL[h];
                accR += coeffs[tap] * historyR[h];
            }

            peakL = std::max (peakL, std::abs (accL));
            peakR = std::max (peakR, std::abs (accR));
        }

        index = (index + 1) % tapsPerPhase;
    }

    /** Returns the maximum since the last call and clears the accumulator.

        Clearing on read is deliberate. A meter that has to be told to reset
        eventually is not told, and then reads a peak from ten minutes ago. */
    void readAndClear (float& outLeft, float& outRight) noexcept
    {
        outLeft  = peakL;
        outRight = peakR;
        peakL = 0.0f;
        peakR = 0.0f;
    }

private:
    void buildKernel()
    {
        constexpr float pi    = 3.14159265358979323846f;
        constexpr float twoPi = 2.0f * pi;

        const int   n      = totalTaps;
        const float centre = 0.5f * static_cast<float> (n - 1);
        const float fc     = 0.5f / static_cast<float> (oversampleFactor);

        double sumPerPhase[oversampleFactor] = { 0.0 };

        for (int i = 0; i < n; ++i)
        {
            const float t = static_cast<float> (i) - centre;
            const float sincValue = (std::abs (t) < 1.0e-6f)
                                  ? 2.0f * fc
                                  : std::sin (twoPi * fc * t) / (pi * t);

            const float window = 0.35875f
                               - 0.48829f * std::cos (twoPi * static_cast<float> (i) / static_cast<float> (n - 1))
                               + 0.14128f * std::cos (2.0f * twoPi * static_cast<float> (i) / static_cast<float> (n - 1))
                               - 0.01168f * std::cos (3.0f * twoPi * static_cast<float> (i) / static_cast<float> (n - 1));

            const float wSinc = sincValue * window;
            const int phase = i % oversampleFactor;
            const int tap   = i / oversampleFactor;

            kernel[static_cast<size_t> (phase * tapsPerPhase + tap)] = wSinc;
            sumPerPhase[phase] += static_cast<double> (wSinc);
        }

        // Normalise each phase to unity DC gain, so a constant reads its own
        // value rather than the filter's incidental gain.
        for (int phase = 0; phase < oversampleFactor; ++phase)
            for (int tap = 0; tap < tapsPerPhase; ++tap)
                if (std::abs (sumPerPhase[phase]) > 1.0e-9)
                    kernel[static_cast<size_t> (phase * tapsPerPhase + tap)] /= static_cast<float> (sumPerPhase[phase]);
    }

    std::array<float, static_cast<size_t> (totalTaps)>    kernel {};
    std::array<float, static_cast<size_t> (tapsPerPhase)> historyL {};
    std::array<float, static_cast<size_t> (tapsPerPhase)> historyR {};

    int   index = 0;
    float peakL = 0.0f;
    float peakR = 0.0f;
};
} // namespace sz
