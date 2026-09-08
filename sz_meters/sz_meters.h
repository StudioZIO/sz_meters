/*
BEGIN_JUCE_MODULE_DECLARATION

    ID:                 sz_meters
    vendor:             StudioZIO
    version:            1.0.0
    name:               StudioZIO Meters
    description:        BS.1770-4 / EBU R128 loudness, inter-sample true peak, AES17 RMS, crest factor and correlation.
    website:            https://studiozio.vercel.app/
    license:            MIT
    minimumCppStandard: 17

    dependencies:

END_JUCE_MODULE_DECLARATION
*/

#pragma once

/** StudioZIO Meters - the measurement side of an audio signal chain.

    The DSP in this module depends on nothing but the C++ standard library.
    JUCE is not required to use it: the three headers can be dropped into any
    project. The JUCE module wrapper exists so that a JUCE project can add it
    with juce_add_module and forget about it.

    Each class documents the convention it follows and, where it departs from a
    standard, says so in the header rather than in a changelog nobody reads.
*/

#include "sz_meters_LoudnessAnalyser.h"
#include "sz_meters_SignalMeters.h"
#include "sz_meters_TruePeakMeter.h"
