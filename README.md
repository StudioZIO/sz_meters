# sz_meters

Audio measurement for C++ and JUCE: **BS.1770-4 / EBU R128 loudness with real gating**, an **inter-sample true peak** meter, and **AES17 RMS, crest factor and correlation**.

The DSP depends on nothing but the C++ standard library. JUCE is optional — the module wrapper is there so a JUCE project can `juce_add_module` it and move on.

```cpp
#include <sz_meters/sz_meters.h>

sz::LoudnessAnalyser loudness;
sz::TruePeakMeter    truePeak;

loudness.prepare (sampleRate);
truePeak.prepare (sampleRate);

for (int i = 0; i < numSamples; ++i)
{
    loudness.processSample (left[i], right[i]);
    truePeak.process       (left[i], right[i]);
}

const float lufs = loudness.getIntegrated();   // gated, per BS.1770-4
const float lra  = loudness.getRange();        // EBU Tech 3342

float tpL = 0.0f, tpR = 0.0f;
truePeak.readAndClear (tpL, tpR);              // max since the last read
```

## Why this exists

Loudness code is easy to write and hard to write *correctly*, and the difference is invisible until someone checks. The two mistakes that show up most often:

- **An ungated average is not integrated loudness.** BS.1770-4 discards blocks below −70 LUFS (absolute gate) and then blocks below the programme mean − 10 LU (relative gate). Skip the gates and a track with quiet passages reads several LU low. `LoudnessAnalyser` implements both, and there is a [test](tests/test_main.cpp) that fails if either is removed.
- **A sample peak is not a true peak.** The waveform between two samples can exceed both of them. `TruePeakMeter` upsamples 4× and measures the reconstruction; a sine at ⅟₄ the sample rate with a 45° phase offset reads −3.01 dBFS on a sample meter and 0 dBTP here.

## What it measures

| Class | Reports |
| --- | --- |
| `LoudnessAnalyser` | Momentary (400 ms), short-term (3 s), integrated (gated), loudness range (EBU Tech 3342) |
| `TruePeakMeter` | Inter-sample peak per channel, 4× oversampled, max-since-read |
| `SignalMeters` | Sample peak, AES17 RMS (300 ms), crest factor, phase correlation |

## Verified, and to what tolerance

`tests/test_main.cpp` builds with any C++17 compiler and runs 30 checks. Values below are what it actually prints, not what it aims for:

| Check | Result |
| --- | --- |
| EBU Tech 3341 case 1 — 1 kHz stereo sine at −23 dBFS | −23.000 LUFS |
| EBU Tech 3341 case 2 — the same at −33 dBFS | −33.000 LUFS |
| Same tone at 44.1 / 88.2 / 96 kHz | −23.000 LUFS at each |
| Absolute gate — 10 s of digital silence | stays at −70 LUFS |
| Relative gate — 3 s at −23 dBFS, then 7 s at −50 dBFS | −23.2 LUFS (an ungated mean reads −28.9) |
| True peak — sine at fs/4, 45° phase | sample peak −3.01 dBFS, true peak −0.06 dBTP |
| True peak — settled constant | unity gain, exact |
| AES17 — full-scale sine | RMS 0.00 dB, crest factor 3.011 dB |

## What this is not

Stated here rather than discovered later:

- **The true peak filter is not the tabulated BS.1770-4 filter.** It is a 4-phase, 16-tap-per-phase Blackman-Harris windowed sinc. The structure and length match Annex 2; the stopband ripple does not. On programme material it agrees within a few hundredths of a dB, and at fs/4 — half of Nyquist, where a short kernel rolls off — it reads 0.06 dB low. **Do not describe it as certified.**
- **Stereo only.** Both classes take an L/R pair. There is no 5.1 channel weighting.
- **No thread safety.** Feed and read from the audio thread; getting the number to a UI is the host program's job.
- **Loudness is computed on the audio thread**, once per 100 ms sub-block — one pass over a 751-bin histogram, roughly 1500 multiply-accumulates. That is deliberate, but it is not zero.

An abrupt edge in the signal — silence to a steady level, or a hard cut — genuinely overshoots between samples, and the true peak meter reports about 13 % above the sample value there. That is the meter working, not an artefact. Read once to discard it if you are measuring a steady state.

## Installing

**JUCE (CMake):**

```cmake
juce_add_module(path/to/sz_meters/sz_meters)
target_link_libraries(YourPlugin PRIVATE sz_meters)
```

**Anything else:** copy the three headers out of `sz_meters/` and include them. There is nothing to link.

**Running the tests:**

```sh
cmake -B build && cmake --build build && ./build/sz_meters_tests
# or, without CMake:
c++ -std=c++17 -O2 tests/test_main.cpp -o sz_meters_tests && ./sz_meters_tests
```

## Developer communication

Use the repository's [GitHub Issues](https://github.com/StudioZIO/sz_meters/issues) for reproducible bugs, measurement discrepancies, portability reports and test-case contributions. Please include the commit or release, compiler and standard-library version, sample rate, channel layout, a minimal signal or fixture, the expected result and the observed result. Keep audio files and other private project material out of public issues; reduce a report to a small reproducible case instead.

This is a focused developer channel for the `sz_meters` library. It is not a support queue for StudioZIO plug-in installation or DAW troubleshooting. For those requests, use the [StudioZIO support repository](https://github.com/StudioZIO/Support/issues). Do not describe this library as certified BS.1770-4 measurement software; the documented filter limitations above still apply.

## Origin

Extracted from the metering used in [StudioZIO Mastering Suite](https://studioziomasteringsuite.vercel.app/), where these classes drive the meters you can see on the product page. Published because a measurement you cannot inspect is a measurement you have to take on trust.

MIT licensed. Contributions welcome, particularly test cases from the rest of EBU Tech 3341.
