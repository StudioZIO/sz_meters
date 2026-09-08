#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sz
{
/** ITU-R BS.1770-4 / EBU R128 loudness analyser.

    Implements the parts that partial implementations usually leave out, which
    are also the parts that decide whether the number is right:

      * K-weighting   - high shelf + RLB high pass, coefficients derived for the
                        running sample rate (the libebur128 formulation, which
                        reproduces the tabulated 48 kHz values).
      * Gating        - 400 ms blocks at 75 % overlap, one gating block per
                        100 ms step.
      * Absolute gate - blocks below -70 LUFS are discarded.
      * Relative gate - threshold = (ungated mean loudness) - 10 LU.
      * Integrated    - energy mean of the surviving blocks. HELD during
                        silence: the histogram is only cleared by an explicit
                        reset() or resetIntegrated().
      * LRA           - 10th to 95th percentile of short-term blocks, gated at
                        -20 LU relative, per EBU Tech 3342.

    An ungated running average is not integrated loudness, and the difference
    is large on real programme material - it is the whole reason the gates are
    in the Recommendation.

    Loudness is recomputed once per 100 ms sub-block, on the audio thread. That
    is one pass over a 751-bin histogram with a precomputed power table, about
    1500 multiply-accumulates per 100 ms.

    Verified against EBU Tech 3341 case 1 (see tests/): a 1 kHz stereo sine at
    -23 dBFS reads -23 LUFS.
*/
class LoudnessAnalyser
{
public:
    static constexpr int   histogramBins  = 751;      // -70.0 .. +5.0 LUFS, 0.1 dB
    static constexpr float histogramMinDb = -70.0f;
    static constexpr float histogramStep  = 0.1f;

    /** Centre of histogram bin `i`, in LUFS. Deliberately float, and
        deliberately computed in this order: recomputing in double would shift
        every bin centre by ~1e-7 dB and perturb a meter calibrated against
        reference tones. */
    [[nodiscard]] static constexpr float binCentreDb (int i) noexcept
    {
        return histogramMinDb + histogramStep * static_cast<float> (i);
    }

    static constexpr int momentaryBlocks = 4;        // 400 ms
    static constexpr int shortTermBlocks = 30;       // 3 s

    void prepare (double newSampleRate)
    {
        sampleRate     = newSampleRate;
        subBlockLength = std::max (1, static_cast<int> (std::round (newSampleRate * 0.1)));

        designKWeighting();

        subBlockPower.assign (static_cast<size_t> (shortTermBlocks), 0.0);
        integratedHistogram.assign (static_cast<size_t> (histogramBins), 0);
        shortTermHistogram.assign (static_cast<size_t> (histogramBins), 0);

        binPower.assign (static_cast<size_t> (histogramBins), 0.0);
        for (int i = 0; i < histogramBins; ++i)
        {
            const double loudness = binCentreDb (i);
            binPower[static_cast<size_t> (i)] = std::pow (10.0, (loudness + 0.691) / 10.0);
        }

        reset();
    }

    /** Clears filter state and the accumulated programme measurement. */
    void reset() noexcept
    {
        shelfL = shelfR = rlbL = rlbR = BiquadState {};

        std::fill (subBlockPower.begin(), subBlockPower.end(), 0.0);
        std::fill (integratedHistogram.begin(), integratedHistogram.end(), 0);
        std::fill (shortTermHistogram.begin(), shortTermHistogram.end(), 0);

        accumulator      = 0.0;
        accumulatedCount = 0;
        blocksWritten    = 0;
        writeIndex       = 0;

        momentary  = -70.0f;
        shortTerm  = -70.0f;
        integrated = -70.0f;
        range      = 0.0f;
    }

    /** Resets only the programme measurement (integrated + LRA), leaving the
        momentary and short-term windows running. This is what a "reset
        loudness" button should call - clearing the filter state as well makes
        the momentary reading jump. */
    void resetIntegrated() noexcept
    {
        std::fill (integratedHistogram.begin(), integratedHistogram.end(), 0);
        std::fill (shortTermHistogram.begin(), shortTermHistogram.end(), 0);
        integrated = -70.0f;
        range      = 0.0f;
    }

    void processSample (float left, float right) noexcept
    {
        const float zL = filterSample (left,  shelfL, rlbL);
        const float zR = filterSample (right, shelfR, rlbR);

        accumulator += static_cast<double> (zL) * zL
                     + static_cast<double> (zR) * zR;

        if (++accumulatedCount >= subBlockLength)
        {
            pushSubBlock (accumulator / accumulatedCount);
            accumulator      = 0.0;
            accumulatedCount = 0;
        }
    }

    [[nodiscard]] float getMomentary()  const noexcept { return momentary;  }
    [[nodiscard]] float getShortTerm()  const noexcept { return shortTerm;  }
    [[nodiscard]] float getIntegrated() const noexcept { return integrated; }
    [[nodiscard]] float getRange()      const noexcept { return range;      }

private:
    struct BiquadState
    {
        float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    };

    struct Coeffs
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    };

    [[nodiscard]] float filterSample (float x, BiquadState& s1, BiquadState& s2) const noexcept
    {
        const float stage1 = shelfCoeffs.b0 * x
                           + shelfCoeffs.b1 * s1.x1 + shelfCoeffs.b2 * s1.x2
                           - shelfCoeffs.a1 * s1.y1 - shelfCoeffs.a2 * s1.y2;
        s1.x2 = s1.x1; s1.x1 = x;
        s1.y2 = s1.y1; s1.y1 = stage1;

        const float stage2 = rlbCoeffs.b0 * stage1
                           + rlbCoeffs.b1 * s2.x1 + rlbCoeffs.b2 * s2.x2
                           - rlbCoeffs.a1 * s2.y1 - rlbCoeffs.a2 * s2.y2;
        s2.x2 = s2.x1; s2.x1 = stage1;
        s2.y2 = s2.y1; s2.y1 = stage2;

        return stage2;
    }

    void designKWeighting()
    {
        constexpr double pi = 3.14159265358979323846;

        // --- Stage 1: high shelf, +4 dB above ~1.5 kHz -----------------------
        {
            constexpr double G  = 3.999843853973347;
            constexpr double Q  = 0.7071752369554193;
            constexpr double fc = 1681.974450955533;

            const double K  = std::tan (pi * fc / sampleRate);
            const double Vh = std::pow (10.0, G / 20.0);
            const double Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;

            shelfCoeffs.b0 = static_cast<float> ((Vh + Vb * K / Q + K * K) / a0);
            shelfCoeffs.b1 = static_cast<float> (2.0 * (K * K - Vh) / a0);
            shelfCoeffs.b2 = static_cast<float> ((Vh - Vb * K / Q + K * K) / a0);
            shelfCoeffs.a1 = static_cast<float> (2.0 * (K * K - 1.0) / a0);
            shelfCoeffs.a2 = static_cast<float> ((1.0 - K / Q + K * K) / a0);
        }

        // --- Stage 2: RLB high pass, ~38 Hz ---------------------------------
        {
            constexpr double Q  = 0.5003270373238773;
            constexpr double fc = 38.13547087602444;

            const double K  = std::tan (pi * fc / sampleRate);
            const double a0 = 1.0 + K / Q + K * K;

            rlbCoeffs.b0 =  1.0f;
            rlbCoeffs.b1 = -2.0f;
            rlbCoeffs.b2 =  1.0f;
            rlbCoeffs.a1 = static_cast<float> (2.0 * (K * K - 1.0) / a0);
            rlbCoeffs.a2 = static_cast<float> ((1.0 - K / Q + K * K) / a0);
        }
    }

    void pushSubBlock (double meanSquare) noexcept
    {
        subBlockPower[static_cast<size_t> (writeIndex)] = meanSquare;
        writeIndex = (writeIndex + 1) % shortTermBlocks;
        ++blocksWritten;

        momentary = averageLoudness (momentaryBlocks);
        shortTerm = averageLoudness (shortTermBlocks);

        // A complete 400 ms gating block exists from the 4th sub-block onward.
        if (blocksWritten >= momentaryBlocks)
        {
            addToHistogram (integratedHistogram, momentary);
            integrated = gatedLoudness (integratedHistogram, -10.0);
        }

        if (blocksWritten >= shortTermBlocks)
        {
            addToHistogram (shortTermHistogram, shortTerm);
            range = computeRange();
        }
    }

    /** Mean power over the most recent `count` sub-blocks, expressed in LUFS. */
    [[nodiscard]] float averageLoudness (int count) const noexcept
    {
        const int available = std::min (count, static_cast<int> (blocksWritten));

        if (available <= 0)
            return -70.0f;

        double sum = 0.0;

        for (int i = 0; i < available; ++i)
        {
            const int idx = (writeIndex - 1 - i + shortTermBlocks) % shortTermBlocks;
            sum += subBlockPower[static_cast<size_t> (idx)];
        }

        const double mean = sum / available;

        if (mean <= 0.0)
            return -70.0f;

        return static_cast<float> (std::max (-70.0, -0.691 + 10.0 * std::log10 (mean)));
    }

    static void addToHistogram (std::vector<int64_t>& histogram, float loudness) noexcept
    {
        if (loudness <= histogramMinDb)      // absolute gate
            return;

        const int raw = static_cast<int> ((loudness - histogramMinDb) / histogramStep + 0.5f);
        const int bin = std::min (histogramBins - 1, std::max (0, raw));
        ++histogram[static_cast<size_t> (bin)];
    }

    /** Two-pass gated mean: the ungated mean sets the relative threshold, then
        the mean is recomputed over the blocks above it. */
    [[nodiscard]] float gatedLoudness (const std::vector<int64_t>& histogram,
                                       double relativeOffsetLu) const noexcept
    {
        double  sumPower = 0.0;
        int64_t count    = 0;

        for (int i = 0; i < histogramBins; ++i)
        {
            const int64_t n = histogram[static_cast<size_t> (i)];

            if (n > 0)
            {
                sumPower += binPower[static_cast<size_t> (i)] * static_cast<double> (n);
                count    += n;
            }
        }

        if (count == 0)
            return -70.0f;

        const double ungated   = -0.691 + 10.0 * std::log10 (sumPower / static_cast<double> (count));
        const double threshold = ungated + relativeOffsetLu;

        double  gatedPower = 0.0;
        int64_t gatedCount = 0;

        for (int i = 0; i < histogramBins; ++i)
        {
            const int64_t n = histogram[static_cast<size_t> (i)];

            if (n > 0 && binCentreDb (i) > threshold)
            {
                gatedPower += binPower[static_cast<size_t> (i)] * static_cast<double> (n);
                gatedCount += n;
            }
        }

        if (gatedCount == 0)
            return static_cast<float> (ungated);

        return static_cast<float> (-0.691 + 10.0 * std::log10 (gatedPower / static_cast<double> (gatedCount)));
    }

    /** EBU Tech 3342 loudness range: 10th to 95th percentile of short-term
        blocks that survive a -20 LU relative gate. */
    [[nodiscard]] float computeRange() const noexcept
    {
        double  sumPower = 0.0;
        int64_t count    = 0;

        for (int i = 0; i < histogramBins; ++i)
        {
            const int64_t n = shortTermHistogram[static_cast<size_t> (i)];

            if (n > 0)
            {
                sumPower += binPower[static_cast<size_t> (i)] * static_cast<double> (n);
                count    += n;
            }
        }

        if (count == 0)
            return 0.0f;

        const double threshold = -0.691 + 10.0 * std::log10 (sumPower / static_cast<double> (count)) - 20.0;

        int64_t gatedTotal = 0;

        for (int i = 0; i < histogramBins; ++i)
            if (binCentreDb (i) > threshold)
                gatedTotal += shortTermHistogram[static_cast<size_t> (i)];

        if (gatedTotal < 2)
            return 0.0f;

        const auto lowTarget  = static_cast<int64_t> (0.10 * static_cast<double> (gatedTotal));
        const auto highTarget = static_cast<int64_t> (0.95 * static_cast<double> (gatedTotal));

        int64_t running = 0;
        float   low  = histogramMinDb;
        float   high = histogramMinDb;
        bool    lowFound = false;

        for (int i = 0; i < histogramBins; ++i)
        {
            if (binCentreDb (i) <= threshold)
                continue;

            running += shortTermHistogram[static_cast<size_t> (i)];

            if (! lowFound && running >= lowTarget)
            {
                low = binCentreDb (i);
                lowFound = true;
            }

            if (running >= highTarget)
            {
                high = binCentreDb (i);
                break;
            }
        }

        return std::max (0.0f, high - low);
    }

    double sampleRate     = 48000.0;
    int    subBlockLength = 4800;

    Coeffs      shelfCoeffs, rlbCoeffs;
    BiquadState shelfL, shelfR, rlbL, rlbR;

    std::vector<double>  subBlockPower;
    std::vector<double>  binPower;
    std::vector<int64_t> integratedHistogram;
    std::vector<int64_t> shortTermHistogram;

    double  accumulator      = 0.0;
    int     accumulatedCount = 0;
    int64_t blocksWritten    = 0;
    int     writeIndex       = 0;

    float momentary  = -70.0f;
    float shortTerm  = -70.0f;
    float integrated = -70.0f;
    float range      = 0.0f;
};
} // namespace sz
