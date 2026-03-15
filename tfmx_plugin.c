///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TFMX Playback Plugin
//
// Implements RVPlaybackPlugin interface for TFMX, Hippel COSO, and Future Composer music formats.
// Based on libtfmxaudiodecoder by Michael Schwendt.
//
// Supported formats:
//   TFMX: .tfx, .tfm, .mdat, .tfmx
//   Hippel COSO: .hip, .hipc, .hip7, .mcmd
//   Future Composer: .fc, .fc3, .fc4, .fc13, .fc14, .smod
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef nullptr
#define nullptr ((void*)0)
#endif

#include <retrovert/io.h>
#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>
#include <retrovert/settings.h>

#include "src/tfmxaudiodecoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define SAMPLE_RATE 48000
#define CHANNELS 2
#define BITS_PER_SAMPLE 16

RV_PLUGIN_USE_IO_API();
RV_PLUGIN_USE_LOG_API();
RV_PLUGIN_USE_METADATA_API();

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct TfmxReplayerData {
    void* decoder;
    bool scope_enabled;
} TfmxReplayerData;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const char* tfmx_supported_extensions(void) {
    return "tfx,tfm,mdat,tfmx,hip,hipc,hip7,mcmd,fc,fc3,fc4,fc13,fc14,smod";
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void* tfmx_create(const RVService* service_api) {
    TfmxReplayerData* data = (TfmxReplayerData*)malloc(sizeof(TfmxReplayerData));
    if (data == nullptr) {
        return nullptr;
    }

    memset(data, 0, sizeof(TfmxReplayerData));

    data->decoder = tfmxdec_new();
    if (data->decoder == nullptr) {
        free(data);
        return nullptr;
    }

    return data;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int tfmx_destroy(void* user_data) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;

    if (data != nullptr) {
        if (data->decoder != nullptr) {
            tfmxdec_delete(data->decoder);
        }
        free(data);
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVProbeResult tfmx_probe_can_play(uint8_t* data, uint64_t data_size, const char* url, uint64_t total_size) {
    (void)url;
    (void)total_size;

    // Create a temporary decoder for probing
    void* decoder = tfmxdec_new();
    if (decoder == nullptr) {
        return RVProbeResult_Unsupported;
    }

    int result = tfmxdec_detect(decoder, data, (uint32_t)data_size);
    tfmxdec_delete(decoder);

    if (result) {
        return RVProbeResult_Supported;
    }

    return RVProbeResult_Unsupported;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int tfmx_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;
    RVIoReadUrlResult read_res;

    if ((read_res = rv_io_read_url_to_memory(url)).data == nullptr) {
        rv_error("TFMX: Failed to load %s to memory", url);
        return -1;
    }

    // Set path for multi-file format support (TFMX mdat/smpl pairs)
    tfmxdec_set_path(data->decoder, url);

    // Initialize the decoder with the file data
    if (!tfmxdec_init(data->decoder, read_res.data, (uint32_t)read_res.data_size, (int)subsong)) {
        rv_error("TFMX: Failed to initialize decoder for %s", url);
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    // Initialize the mixer: 48kHz, 16-bit, stereo, signed zero, 75% stereo separation
    tfmxdec_mixer_init(data->decoder, SAMPLE_RATE, BITS_PER_SAMPLE, CHANNELS, 0, 75);

    // Build pattern display for visualization
    if (!tfmxdec_build_pattern_display(data->decoder)) {
        rv_error("TFMX: Failed to build pattern display for %s", url);
    } else {
        int num_tracks = tfmxdec_get_pattern_display_num_tracks(data->decoder);
        uint32_t max_rows = 0;
        for (int i = 0; i < num_tracks; i++) {
            uint32_t rows = tfmxdec_get_pattern_display_track_rows(data->decoder, i);
            if (rows > max_rows)
                max_rows = rows;
        }
        rv_info("TFMX: Pattern display built - %d tracks, %u max rows", num_tracks, max_rows);
    }

    rv_io_free_url_to_memory(read_res.data);

    const char* format_name = tfmxdec_format_name(data->decoder);
    rv_info("TFMX: Opened %s (format: %s)", url, format_name ? format_name : "unknown");

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void tfmx_close(void* user_data) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;

    // Reinitialize decoder to clear state (library doesn't have explicit close)
    // The decoder will be reused for the next file
    (void)data;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVReadInfo tfmx_read_data(void* user_data, RVReadData dest) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;

    // Calculate how many S16 stereo frames fit in the output buffer
    uint32_t max_frames = dest.channels_output_max_bytes_size / (sizeof(int16_t) * CHANNELS);

    // Fill the output buffer directly with S16 samples
    uint32_t bytes_to_fill = max_frames * CHANNELS * sizeof(int16_t);
    tfmxdec_buffer_fill(data->decoder, (int16_t*)dest.channels_output, bytes_to_fill);

    // Check if song has ended
    int song_end = tfmxdec_song_end(data->decoder);

    RVAudioFormat format = { RVAudioStreamFormat_S16, CHANNELS, SAMPLE_RATE };
    RVReadStatus status = song_end ? RVReadStatus_Finished : RVReadStatus_Ok;

    return (RVReadInfo) { format, (uint16_t)max_frames, status};
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int64_t tfmx_seek(void* user_data, int64_t ms) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;

    tfmxdec_seek(data->decoder, (int32_t)ms);

    return ms;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int tfmx_metadata(const char* url, const RVService* service_api) {
    (void)service_api;
    RVIoReadUrlResult read_res;

    if ((read_res = rv_io_read_url_to_memory(url)).data == nullptr) {
        rv_error("TFMX: Failed to load %s for metadata", url);
        return -1;
    }

    void* decoder = tfmxdec_new();
    if (decoder == nullptr) {
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    tfmxdec_set_path(decoder, url);

    if (!tfmxdec_init(decoder, read_res.data, (uint32_t)read_res.data_size, 0)) {
        tfmxdec_delete(decoder);
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    RVMetadataId index = rv_metadata_create_url(url);

    // Get metadata from decoder
    const char* title = tfmxdec_get_title(decoder);
    const char* name = tfmxdec_get_name(decoder);
    const char* artist = tfmxdec_get_artist(decoder);
    const char* format_name = tfmxdec_format_name(decoder);

    // Use title if available, otherwise use name (constructed from filename)
    if (title != nullptr && title[0] != '\0') {
        rv_metadata_set_tag(index, RV_METADATA_TITLE_TAG, title);
    } else if (name != nullptr && name[0] != '\0') {
        rv_metadata_set_tag(index, RV_METADATA_TITLE_TAG, name);
    }

    if (artist != nullptr && artist[0] != '\0') {
        rv_metadata_set_tag(index, RV_METADATA_ARTIST_TAG, artist);
    }

    if (format_name != nullptr) {
        rv_metadata_set_tag(index, RV_METADATA_SONGTYPE_TAG, format_name);
    }

    // Get duration in milliseconds and convert to seconds
    uint32_t duration_ms = tfmxdec_duration(decoder);
    rv_metadata_set_tag_f64(index, RV_METADATA_LENGTH_TAG, (double)duration_ms / 1000.0);

    // Handle subsongs
    int num_songs = tfmxdec_songs(decoder);
    if (num_songs > 1) {
        for (int i = 0; i < num_songs; i++) {
            // Reinit for each subsong to get its duration
            tfmxdec_reinit(decoder, i);
            uint32_t subsong_duration_ms = tfmxdec_duration(decoder);
            rv_metadata_add_subsong(index, i, "", (float)subsong_duration_ms / 1000.0f);
        }
    }

    tfmxdec_delete(decoder);
    rv_io_free_url_to_memory(read_res.data);

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void tfmx_event(void* user_data, uint8_t* event_data, uint64_t len) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;

    if (data->decoder == nullptr || event_data == nullptr || len < 8) {
        return;
    }

    // Get number of voices and their volumes for VU meters
    int num_voices = tfmxdec_voices(data->decoder);

    // Clear event data
    memset(event_data, 0, 8);

    // Fill in voice volumes (up to 4 voices for visualization)
    int voices_to_report = num_voices < 4 ? num_voices : 4;
    for (int i = 0; i < voices_to_report; i++) {
        unsigned short vol = tfmxdec_get_voice_volume(data->decoder, (unsigned int)i);
        // Scale 0-100 to 0-255
        event_data[3 - i] = (uint8_t)((vol * 255) / 100);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void tfmx_static_init(const RVService* service_api) {
    rv_init_log_api(service_api);
    rv_init_io_api(service_api);
    rv_init_metadata_api(service_api);

    rv_info("TFMX plugin initialized (libtfmxaudiodecoder)");
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Channel-based visualization callbacks

static int tfmx_get_tracker_info(void* user_data, RVTrackerInfo* info) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;

    if (data == nullptr || data->decoder == nullptr || info == nullptr) {
        return -1;
    }

    memset(info, 0, sizeof(RVTrackerInfo));

    int num_tracks = tfmxdec_get_pattern_display_num_tracks(data->decoder);
    if (num_tracks <= 0) {
        return -1;
    }

    info->num_channels = (uint8_t)num_tracks;
    info->channels_synchronized = 0; // TFMX tracks are NOT synchronized - each has its own position

    uint32_t max_rows = 0;
    uint32_t max_current_row = 0;
    uint32_t current_tick = tfmxdec_get_pattern_tick(data->decoder);

    // Calculate row positions for each track
    // TFMX tracks are asynchronous - each has different row density per tick.
    uint32_t min_current_row = UINT32_MAX;
    for (int i = 0; i < num_tracks && i < RV_MAX_CHANNELS; i++) {
        uint32_t rows = tfmxdec_get_pattern_display_track_rows(data->decoder, i);
        int current = tfmxdec_find_pattern_display_row_for_tick(data->decoder, i, current_tick);
        if (current < 0)
            current = 0;

        info->channels[i].num_rows = rows;
        info->channels[i].current_row = (uint32_t)current;
        snprintf(info->channels[i].name, sizeof(info->channels[i].name), "Track %d", i + 1);

        if (rows > max_rows) {
            max_rows = rows;
        }
        if ((uint32_t)current > max_current_row) {
            max_current_row = (uint32_t)current;
        }
        if ((uint32_t)current < min_current_row) {
            min_current_row = (uint32_t)current;
        }
    }

    info->total_rows = max_rows;
    // Use minimum row for scrolling so no track scrolls off the top
    info->current_row = (min_current_row != UINT32_MAX) ? min_current_row : 0;
    info->rows_per_beat = 4; // Typical for TFMX
    info->rows_per_measure = 16;

    // Build song name from title and artist
    const char* title = tfmxdec_get_title(data->decoder);
    const char* name = tfmxdec_get_name(data->decoder);
    const char* artist = tfmxdec_get_artist(data->decoder);

    // Use title if available, otherwise use name (from filename)
    const char* display_title = (title != nullptr && title[0] != '\0') ? title : name;

    if (display_title != nullptr && display_title[0] != '\0') {
        if (artist != nullptr && artist[0] != '\0') {
            // Format as "Title - Artist"
            snprintf(info->song_name, sizeof(info->song_name), "%s - %s", display_title, artist);
        } else {
            strncpy(info->song_name, display_title, sizeof(info->song_name) - 1);
        }
        info->song_name[sizeof(info->song_name) - 1] = '\0';
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int tfmx_get_pattern_cell(void* user_data, int pattern, int row, int channel, RVPatternCell* cell) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;
    (void)pattern; // TFMX uses single pattern display, pattern ignored

    if (data == nullptr || data->decoder == nullptr || cell == nullptr) {
        return -1;
    }

    uint8_t type, note, macro, volume;
    int8_t detune;
    uint8_t dest_channel;
    uint16_t wait;
    uint32_t tick;

    if (!tfmxdec_get_pattern_display_row(data->decoder, channel, (uint32_t)row, &type, &note, &macro, &volume, &detune,
                                         &dest_channel, &wait, &tick)) {
        return -1;
    }

    if (type == 0) {
        // Note row: show note, macro, and wait (if embedded) or detune
        cell->note = note;
        cell->instrument = macro;
        cell->volume = volume;
        if (wait > 0) {
            // Note has embedded wait - show as W## effect
            cell->effect = 'W';
            cell->effect_param = (uint8_t)wait;
        } else {
            cell->effect = 0;
            cell->effect_param = (uint8_t)detune;
        }
        cell->dest_channel = dest_channel;
    } else {
        // Command row: show command in effect column only
        cell->note = 0;
        cell->instrument = 0;
        cell->volume = 0;
        if (note == 0xF3) {
            // WAIT command - display as Wxx for consistency with embedded waits
            cell->effect = 'W';
            cell->effect_param = macro;
        } else {
            cell->effect = note;
            cell->effect_param = macro;
        }
        cell->dest_channel = dest_channel;
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int tfmx_get_pattern_num_rows(void* user_data, int pattern) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;
    (void)pattern; // TFMX uses single pattern display

    if (data == nullptr || data->decoder == nullptr) {
        return 0;
    }

    // Return max rows across all tracks
    int num_tracks = tfmxdec_get_pattern_display_num_tracks(data->decoder);
    uint32_t max_rows = 0;
    for (int i = 0; i < num_tracks; i++) {
        uint32_t rows = tfmxdec_get_pattern_display_track_rows(data->decoder, i);
        if (rows > max_rows) {
            max_rows = rows;
        }
    }

    return (int)max_rows;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Scope capture callback

static uint32_t tfmx_get_scope_data(void* user_data, int channel, float* buffer, uint32_t num_samples) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;
    if (data == nullptr || data->decoder == nullptr || buffer == nullptr) {
        return 0;
    }

    // Auto-enable scope capture on first call
    if (!data->scope_enabled) {
        tfmxdec_enable_scope_capture(data->decoder, 1);
        data->scope_enabled = true;
    }

    return tfmxdec_get_scope_data(data->decoder, channel, buffer, num_samples);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t tfmx_get_scope_channel_names(void* user_data, const char** names, uint32_t max_channels) {
    (void)user_data;
    static const char* s_names[] = { "Voice 0", "Voice 1", "Voice 2", "Voice 3" };
    uint32_t count = 4;
    if (count > max_channels)
        count = max_channels;
    for (uint32_t i = 0; i < count; i++)
        names[i] = s_names[i];
    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVPlaybackPlugin g_tfmx_plugin = {
    RV_PLAYBACK_PLUGIN_API_VERSION,
    "tfmx",
    "0.1.0",
    "libtfmxaudiodecoder 1.0.1",
    tfmx_probe_can_play,
    tfmx_supported_extensions,
    tfmx_create,
    tfmx_destroy,
    tfmx_event,
    tfmx_open,
    tfmx_close,
    tfmx_read_data,
    tfmx_seek,
    tfmx_metadata,
    tfmx_static_init,
    nullptr, // settings_updated
    // Tracker visualization API
    tfmx_get_tracker_info,
    tfmx_get_pattern_cell,
    tfmx_get_pattern_num_rows,
    // Scope capture API
    tfmx_get_scope_data,
    nullptr, // static_destroy
    tfmx_get_scope_channel_names,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_tfmx_plugin;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
