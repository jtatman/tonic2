// DPF metadata for the Tonic Poly example plugin (the "poly" demo synth from examples/demo_synths.hpp).
#pragma once

#define DISTRHO_PLUGIN_BRAND   "tonic2"
#define DISTRHO_PLUGIN_NAME    "Tonic Poly"
#define DISTRHO_PLUGIN_URI     "https://github.com/jtatman/tonic2#poly"
#define DISTRHO_PLUGIN_CLAP_ID "io.github.jtatman.tonic2.poly"

#define DISTRHO_PLUGIN_BRAND_ID  Tnc2
#define DISTRHO_PLUGIN_UNIQUE_ID TnPl

// The build sets TONIC2_HAS_UI=0 when the UI is disabled (e.g. headless Linux without X11/GL headers).
#ifndef TONIC2_HAS_UI
#define TONIC2_HAS_UI 1
#endif
#define DISTRHO_PLUGIN_HAS_UI          TONIC2_HAS_UI
#define DISTRHO_UI_USE_NANOVG          1
#define DISTRHO_UI_USER_RESIZABLE      0
#define DISTRHO_UI_DEFAULT_WIDTH       720
#define DISTRHO_UI_DEFAULT_HEIGHT      380
#define DISTRHO_PLUGIN_IS_RT_SAFE      1
#define DISTRHO_PLUGIN_IS_SYNTH        1
#define DISTRHO_PLUGIN_NUM_INPUTS      0
#define DISTRHO_PLUGIN_NUM_OUTPUTS     2
#define DISTRHO_PLUGIN_WANT_MIDI_INPUT 1
#define DISTRHO_PLUGIN_LV2_CATEGORY    "lv2:InstrumentPlugin"
#define DISTRHO_PLUGIN_VST3_CATEGORIES "Instrument|Synth"
#define DISTRHO_PLUGIN_CLAP_FEATURES   "instrument", "synthesizer", "stereo"
