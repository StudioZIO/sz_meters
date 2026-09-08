/*  StudioZIO Meters - verification against reference values.

    No test framework and no JUCE: this compiles with a bare C++17 compiler, so
    "does it build and is it right" is one command on any machine.

    Where a number comes from a published document the document is named. Where
    a tolerance is loose, the reason is stated - a tolerance with no reason is
    just a test that has been made to pass.
*/

#include "../sz_meters/sz_meters_LoudnessAnalyser.h"
#include "../sz_meters/sz_meters_SignalMeters.h"
#include "../sz_meters/sz_meters_TruePeakMeter.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
int failures = 0;
int checks   = 0;

constexpr double pi = 3.14159265358979323846;

void check (bool condition, const std::string& what, const std::string& detail = {})
{
    ++checks;
    if (condition)
    {
        std::printf ("  ok    %s%s%s\n", what.c_str(),
                     detail.empty() ? "" : "  ", detail.c_str());
    }
    else
    {
        ++failures;
        std::printf ("  FAIL  %s%s%s\n", what.c_str(),
                     detail.empty() ? "" : "  ", detail.c_str());
    }
}

void near (double actual, double expected, double tolerance, const std::string& what)
{
    char detail[128];
    std::snprintf (detail, sizeof (detail), "(got %.3f, expected %.3f +/- %.3f)",
                   actual, expected, tolerance);
    check (std::abs (actual - expected) <= tolerance, what, detail);
}

double dbToGain (double db) { return std::pow (10.0, db / 20.0); }

/** Feeds a steady stereo sine and returns the analyser afterwards. */
void feedSine (sz::LoudnessAnalyser& a, double freq, double db,
               double seconds, double sampleRate)
{
    const auto  n = static_cast<long long> (seconds * sampleRate);
    const double amp = dbToGain (db);

    for (long long i = 0; i < n; ++i)
    {
        const auto s = static_cast<float> (amp * std::sin (2.0 * pi * freq
                                                           * static_cast<double> (i) / sampleRate));
        a.processSample (s, s);
    }
}

// ---------------------------------------------------------------------------

void loudnessTests()
{
    std::printf ("\nLoudnessAnalyser\n");
    constexpr double fs = 48000.0;

    // EBU Tech 3341, test case 1: a 1 kHz sine at -23 dBFS on both channels
    // must read -23.0 LUFS. The document's own tolerance is +/- 0.1 LU.
    {
        sz::LoudnessAnalyser a;
        a.prepare (fs);
        feedSine (a, 1000.0, -23.0, 10.0, fs);
        near (a.getIntegrated(), -23.0, 0.1, "3341 case 1: -23 dBFS 1 kHz -> -23 LUFS");
        near (a.getShortTerm(),  -23.0, 0.1, "  short-term agrees");
        near (a.getMomentary(),  -23.0, 0.1, "  momentary agrees");
    }

    // EBU Tech 3341, test case 2: the same tone at -33 dBFS reads -33.0 LUFS.
    {
        sz::LoudnessAnalyser a;
        a.prepare (fs);
        feedSine (a, 1000.0, -33.0, 10.0, fs);
        near (a.getIntegrated(), -33.0, 0.1, "3341 case 2: -33 dBFS 1 kHz -> -33 LUFS");
    }

    // The measurement must not depend on the sample rate: the K-weighting
    // coefficients are derived per rate rather than tabulated for 48 kHz.
    for (double rate : { 44100.0, 88200.0, 96000.0 })
    {
        sz::LoudnessAnalyser a;
        a.prepare (rate);
        feedSine (a, 1000.0, -23.0, 10.0, rate);
        near (a.getIntegrated(), -23.0, 0.15,
              "rate independence at " + std::to_string (static_cast<int> (rate)) + " Hz");
    }

    // Absolute gate: digital silence must leave the programme measurement at
    // its floor rather than reporting a very quiet number.
    {
        sz::LoudnessAnalyser a;
        a.prepare (fs);
        for (int i = 0; i < static_cast<int> (10.0 * fs); ++i)
            a.processSample (0.0f, 0.0f);
        near (a.getIntegrated(), -70.0, 0.001, "absolute gate: silence stays at -70 LUFS");
    }

    // Relative gate. This is the test that separates a real BS.1770
    // implementation from a running average, so it is worth being explicit:
    // 3 s at -23 dBFS followed by 7 s at -50 dBFS. An ungated mean lands near
    // -28.9 LUFS. Gated, the quiet blocks fall below (mean - 10 LU) and are
    // discarded, so the answer must stay at the loud passage: -23.
    {
        sz::LoudnessAnalyser a;
        a.prepare (fs);
        feedSine (a, 1000.0, -23.0, 3.0, fs);
        feedSine (a, 1000.0, -50.0, 7.0, fs);
        near (a.getIntegrated(), -23.0, 0.3, "relative gate: quiet passage is discarded");
        check (a.getIntegrated() > -26.0,
               "relative gate: not an ungated mean (which would read about -28.9)");
    }

    // Integrated loudness is a programme measurement and must survive silence;
    // only an explicit reset clears it.
    {
        sz::LoudnessAnalyser a;
        a.prepare (fs);
        feedSine (a, 1000.0, -23.0, 5.0, fs);
        const double afterTone = a.getIntegrated();
        for (int i = 0; i < static_cast<int> (5.0 * fs); ++i)
            a.processSample (0.0f, 0.0f);

        // 0.3 LU rather than 0.0: the gating blocks overlap the moment the tone
        // stops, so a few blocks legitimately contain part tone and part
        // silence and are measured as the quieter programme they are. The
        // measurement is held - it does not decay towards -70 - but it is not
        // frozen either, and a test asserting it never moves would be asserting
        // the wrong thing.
        near (a.getIntegrated(), afterTone, 0.3, "integrated is held through silence");
        check (a.getIntegrated() > -24.0, "  and does not decay towards the floor");

        a.resetIntegrated();
        near (a.getIntegrated(), -70.0, 0.001, "resetIntegrated() clears it");
    }
}

// ---------------------------------------------------------------------------

void truePeakTests()
{
    std::printf ("\nTruePeakMeter\n");
    constexpr double fs = 48000.0;

    // A sine at fs/4 with a 45 degree phase offset lands every sample at
    // +/- A/sqrt(2) while the waveform itself reaches A. The sample peak
    // therefore reads 3.01 dB low, and that gap is the entire reason
    // inter-sample metering exists.
    {
        sz::TruePeakMeter m;
        m.prepare (fs);

        float samplePeak = 0.0f;
        for (int i = 0; i < 4096; ++i)
        {
            const auto s = static_cast<float> (std::sin (2.0 * pi * 0.25 * i + pi / 4.0));
            samplePeak = std::max (samplePeak, std::abs (s));
            m.process (s, s);
        }

        float l = 0.0f, r = 0.0f;
        m.readAndClear (l, r);

        const double samplePeakDb = 20.0 * std::log10 (samplePeak);
        const double truePeakDb   = 20.0 * std::log10 (l);

        char detail[160];
        std::snprintf (detail, sizeof (detail),
                       "(sample peak %.2f dBFS, true peak %.2f dBTP)",
                       samplePeakDb, truePeakDb);
        check (samplePeakDb < -2.9 && samplePeakDb > -3.1,
               "fs/4 at 45 deg: sample peak sits 3.01 dB low", detail);

        // Tolerance is 0.5 dB rather than 0.1: this is a 16-tap-per-phase
        // windowed sinc, not the tabulated BS.1770-4 filter, and fs/4 is half
        // of Nyquist where a short kernel rolls off measurably. The header
        // says the same thing; the test agrees with the header.
        near (truePeakDb, 0.0, 0.5, "  and the true peak recovers full scale");
        check (l > samplePeak, "  true peak exceeds sample peak");
    }

    // An abrupt edge - silence to a steady level, or a hard cut back to
    // silence - is not a quiet event between samples. The reconstructed
    // waveform overshoots it, and a true peak meter is supposed to see that
    // where a sample peak meter cannot. Measured here at about 13 % above the
    // sample value, consistent in both directions.
    //
    // This behaviour is why the tests below settle the meter before reading:
    // measure across an edge and you measure the edge.
    {
        sz::TruePeakMeter m;
        m.prepare (fs);
        for (int i = 0; i < 512; ++i)          // step from silence to 0.5
            m.process (0.5f, 0.5f);

        float l = 0.0f, r = 0.0f;
        m.readAndClear (l, r);

        char detail[128];
        std::snprintf (detail, sizeof (detail), "(sample value 0.500, true peak %.3f)", l);
        check (l > 0.5f && l < 0.65f, "a step edge overshoots between samples", detail);
    }

    // Unity DC gain: each polyphase branch is normalised, so a settled
    // constant must read its own value rather than the filter's incidental
    // gain. Read once to discard the step, then measure the steady state.
    {
        sz::TruePeakMeter m;
        m.prepare (fs);
        for (int i = 0; i < 512; ++i)
            m.process (0.5f, 0.5f);

        float l = 0.0f, r = 0.0f;
        m.readAndClear (l, r);                 // discard the entry transient

        for (int i = 0; i < 512; ++i)
            m.process (0.5f, 0.5f);
        m.readAndClear (l, r);
        near (l, 0.5, 0.001, "settled constant reads its own level (unity DC gain)");
    }

    // readAndClear() must actually clear, or a peak from minutes ago sticks to
    // the display. Read twice with no audio in between: the second read sees
    // nothing, with no edge involved to muddy the question.
    {
        sz::TruePeakMeter m;
        m.prepare (fs);
        for (int i = 0; i < 128; ++i)
            m.process (0.9f, 0.9f);

        float l = 0.0f, r = 0.0f;
        m.readAndClear (l, r);
        check (l > 0.8f, "first read sees the peak");

        m.readAndClear (l, r);
        near (l, 0.0, 0.0001, "second read is empty: the accumulator was cleared");
    }

    // Channels are independent, measured once settled.
    {
        sz::TruePeakMeter m;
        m.prepare (fs);
        for (int i = 0; i < 256; ++i)
            m.process (0.8f, 0.2f);

        float l = 0.0f, r = 0.0f;
        m.readAndClear (l, r);                 // discard the entry transient

        for (int i = 0; i < 256; ++i)
            m.process (0.8f, 0.2f);
        m.readAndClear (l, r);
        near (l, 0.8, 0.01, "left channel is measured on its own");
        near (r, 0.2, 0.01, "right channel is measured on its own");
    }
}

// ---------------------------------------------------------------------------

void signalMeterTests()
{
    std::printf ("\nSignalMeters\n");
    constexpr double fs = 48000.0;
    constexpr int    n  = 48000;

    // AES17: a full-scale sine reads 0 dB RMS, not -3.01 dB, and its crest
    // factor is the textbook 3.01 dB.
    //
    // The RMS is a 300 ms average, so it has to be given time to settle. One
    // second of tone reaches 96.4 % of the final value, which reads 0.16 dB
    // low - not an error, just an average that has not finished averaging.
    // Three seconds is 11 time constants and settles completely.
    {
        sz::SignalMeters m;
        m.prepare (fs);

        std::vector<float> buf (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            buf[static_cast<size_t> (i)] =
                static_cast<float> (std::sin (2.0 * pi * 1000.0 * i / fs));

        for (int pass = 0; pass < 3; ++pass)          // 3 seconds
            m.processBlock (buf.data(), buf.data(), n);

        near (m.getPeakDbL(), 0.0, 0.01, "full-scale sine: peak reads 0 dBFS");
        near (m.getRmsDbL(),  0.0, 0.05, "full-scale sine: AES17 RMS reads 0 dB");
        near (m.getCrestFactorDb(), 3.01, 0.05, "full-scale sine: crest factor 3.01 dB");
    }

    // The 300 ms time constant is a documented property, not an accident:
    // after exactly one time constant the accumulated power has reached
    // (1 - 1/e) = 0.632 of its final value, which is 10*log10(0.632) =
    // 1.99 dB low. (Not 4.34 dB - that is 10*log10(1/e), the amount still
    // missing, which is a different question and the easy one to answer by
    // mistake.)
    {
        sz::SignalMeters m;
        m.prepare (fs);

        const int samples = static_cast<int> (0.3 * fs);
        std::vector<float> buf (static_cast<size_t> (samples));
        for (int i = 0; i < samples; ++i)
            buf[static_cast<size_t> (i)] =
                static_cast<float> (std::sin (2.0 * pi * 1000.0 * i / fs));

        m.processBlock (buf.data(), buf.data(), samples);
        near (m.getRmsDbL(), -1.992, 0.1, "RMS integrates over 300 ms as documented");
    }

    // Correlation, the three cases a listener actually cares about.
    {
        sz::SignalMeters m;
        m.prepare (fs);

        std::vector<float> a (256), b (256);
        for (int i = 0; i < 256; ++i)
        {
            a[static_cast<size_t> (i)] = static_cast<float> (std::sin (2.0 * pi * i / 64.0));
            b[static_cast<size_t> (i)] = -a[static_cast<size_t> (i)];
        }

        m.processBlock (a.data(), a.data(), 256);
        near (m.getCorrelation(), 1.0, 0.001, "identical channels correlate at +1");

        m.processBlock (a.data(), b.data(), 256);
        near (m.getCorrelation(), -1.0, 0.001, "inverted channels correlate at -1");

        std::vector<float> silence (256, 0.0f);
        m.processBlock (silence.data(), silence.data(), 256);
        near (m.getCorrelation(), 1.0, 0.001, "silence reads +1, not noise");
    }

    // Peak is a peak, not a smoothed average: a single sample must show up.
    {
        sz::SignalMeters m;
        m.prepare (fs);

        std::vector<float> buf (1024, 0.0f);
        buf[500] = 0.5f;
        m.processBlock (buf.data(), buf.data(), 1024);
        near (m.getPeakDbL(), -6.0206, 0.01, "a lone sample is not averaged away");
    }
}
} // namespace

int main()
{
    std::printf ("sz_meters verification\n");

    loudnessTests();
    truePeakTests();
    signalMeterTests();

    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
