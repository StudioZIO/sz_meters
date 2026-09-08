#pragma once

#include <algorithm>
#include <cmath>

namespace sz
{
/** Sample peak, AES17 RMS, crest factor and phase correlation.

    Three conventions worth knowing about, because meters disagree on all
    three and the disagreement is usually undocumented:

      * Peak is a genuine peak - max |x| over the block, not a rectified
        average with a fast attack. A smoothed "peak" reads low exactly when
        it matters.
      * RMS uses the AES17 sine convention (+3.01 dB), so a full-scale sine
        reads 0 dB and lines up with the peak reading rather than sitting
        3 dB below it.
      * Correlation is a normalised dot product over every sample in the
        block, not over the decimated points a display happens to draw.

    Near digital silence the correlation denominator collapses, so silence is
    reported as +1 (fully correlated) rather than as whatever the noise floor
    divides into.
*/
class SignalMeters
{
public:
    void prepare (double newSampleRate)
    {
        rmsCoeff = onePoleCoeff (300.0f, newSampleRate);
        reset();
    }

    void reset() noexcept
    {
        rmsSquaredL = 0.0f;
        rmsSquaredR = 0.0f;
        peakL = peakR = 0.0f;
        correlation = 1.0f;
    }

    void processBlock (const float* left, const float* right, int numSamples) noexcept
    {
        float blockPeakL = 0.0f;
        float blockPeakR = 0.0f;

        double sumLL = 0.0;
        double sumRR = 0.0;
        double sumLR = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const float l = left[i];
            const float r = right[i];

            blockPeakL = std::max (blockPeakL, std::abs (l));
            blockPeakR = std::max (blockPeakR, std::abs (r));

            sumLL += static_cast<double> (l) * l;
            sumRR += static_cast<double> (r) * r;
            sumLR += static_cast<double> (l) * r;

            rmsSquaredL = rmsCoeff * rmsSquaredL + (1.0f - rmsCoeff) * (l * l);
            rmsSquaredR = rmsCoeff * rmsSquaredR + (1.0f - rmsCoeff) * (r * r);
        }

        peakL = blockPeakL;
        peakR = blockPeakR;

        const double denominator = std::sqrt (sumLL * sumRR);

        correlation = (denominator > 1.0e-12)
                    ? static_cast<float> (std::max (-1.0, std::min (1.0, sumLR / denominator)))
                    : 1.0f;
    }

    [[nodiscard]] float getPeakDbL() const noexcept { return gainToDb (peakL); }
    [[nodiscard]] float getPeakDbR() const noexcept { return gainToDb (peakR); }

    /** AES17: a 0 dBFS sine reads 0 dB, not -3.01 dB. */
    [[nodiscard]] float getRmsDbL() const noexcept
    {
        return 10.0f * std::log10 (std::max (rmsSquaredL, minGain)) + 3.0103f;
    }

    [[nodiscard]] float getRmsDbR() const noexcept
    {
        return 10.0f * std::log10 (std::max (rmsSquaredR, minGain)) + 3.0103f;
    }

    /** Peak minus RMS, corrected back out of the AES17 convention so that a
        sine reads its textbook 3.01 dB crest factor. */
    [[nodiscard]] float getCrestFactorDb() const noexcept
    {
        const float peakDb = std::max (getPeakDbL(), getPeakDbR());
        const float rmsDb  = std::max (getRmsDbL(),  getRmsDbR());
        return std::max (0.0f, peakDb - rmsDb + 3.0103f);
    }

    [[nodiscard]] float getCorrelation() const noexcept { return correlation; }

    /** -180 dB. The floor for log conversions, so silence reads as a very
        small number rather than as -inf or NaN. */
    static constexpr float minGain = 1.0e-9f;

    [[nodiscard]] static float gainToDb (float gain) noexcept
    {
        return 20.0f * std::log10 (std::max (std::abs (gain), minGain));
    }

    /** One-pole coefficient for a time constant in milliseconds.

        Assumes one application per sample. Feeding this to a smoother that
        runs once per block stretches the time constant by the block length -
        which is how gain-reduction meters end up thousands of times too slow
        and read a fraction of a dB where several dB is happening. */
    [[nodiscard]] static float onePoleCoeff (float timeMs, double sampleRate) noexcept
    {
        if (timeMs <= 0.0f)
            return 0.0f;

        return std::exp (-1.0f / (0.001f * timeMs * static_cast<float> (sampleRate)));
    }

private:
    float rmsCoeff    = 0.999f;
    float rmsSquaredL = 0.0f;
    float rmsSquaredR = 0.0f;
    float peakL = 0.0f, peakR = 0.0f;
    float correlation = 1.0f;
};
} // namespace sz
