# TFMX Playback Plugin
#
# Plays TFMX, Hippel COSO, and Future Composer music formats.
# Based on libtfmxaudiodecoder by Michael Schwendt.

cmake_minimum_required(VERSION 3.16)
project(playback_tfmx C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

set(TFMX_LIB_SOURCES
    src/tfmxaudiodecoder.cpp
    src/CRCLight.cpp
    src/Decoder.cpp
    src/DecoderProxy.cpp
    src/Dump.cpp
    src/Filter.cpp
    src/LamePaulaMixer.cpp
    src/LamePaulaVoice.cpp
    src/PaulaVoice.cpp
    src/Chris/ByChecksum.cpp
    src/Chris/Macro.cpp
    src/Chris/MergedFiles.cpp
    src/Chris/Modulation.cpp
    src/Chris/Pattern.cpp
    src/Chris/SamplesFile.cpp
    src/Chris/Sequencer.cpp
    src/Chris/Songs.cpp
    src/Chris/TFMXDecoder.cpp
    src/Jochen/Analyze.cpp
    src/Jochen/COSO.cpp
    src/Jochen/Envelope.cpp
    src/Jochen/FC.cpp
    src/Jochen/HippelDecoder.cpp
    src/Jochen/Instrument.cpp
    src/Jochen/MCMD.cpp
    src/Jochen/ModPack.cpp
    src/Jochen/Portamento.cpp
    src/Jochen/Probe.cpp
    src/Jochen/SMOD.cpp
    src/Jochen/TFMX7V.cpp
    src/Jochen/TFMX.cpp
    src/Jochen/TraitsByChecksum.cpp
    src/Jochen/Vibrato.cpp
)

add_library(tfmx_playback SHARED tfmx_plugin.c ${TFMX_LIB_SOURCES})
target_include_directories(tfmx_playback PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(tfmx_playback PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
rv_set_plugin_properties(tfmx_playback)
