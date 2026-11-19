// DPF metadata for the Tonic Probe example plugin (test plugin: output = parameter "level").
#pragma once

#define DISTRHO_PLUGIN_BRAND   "tonic2"
#define DISTRHO_PLUGIN_NAME    "Tonic Probe"
#define DISTRHO_PLUGIN_URI     "https://github.com/jtatman/tonic2#probe"
#define DISTRHO_PLUGIN_CLAP_ID "io.github.jtatman.tonic2.probe"

#define DISTRHO_PLUGIN_BRAND_ID  Tnc2
#define DISTRHO_PLUGIN_UNIQUE_ID TnPr

#define DISTRHO_PLUGIN_HAS_UI          0
#define DISTRHO_PLUGIN_IS_RT_SAFE      1
#define DISTRHO_PLUGIN_IS_SYNTH        1
#define DISTRHO_PLUGIN_NUM_INPUTS      0
#define DISTRHO_PLUGIN_NUM_OUTPUTS     2
#define DISTRHO_PLUGIN_WANT_MIDI_INPUT 1
#define DISTRHO_PLUGIN_LV2_CATEGORY    "lv2:InstrumentPlugin"
#define DISTRHO_PLUGIN_VST3_CATEGORIES "Instrument|Synth"
#define DISTRHO_PLUGIN_CLAP_FEATURES   "instrument", "synthesizer", "stereo"
