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
// Visualization API (per-channel, non-synchronized)

#define TFMX_COLUMN_COUNT 5
#define TFMX_CMD_WAIT 0xF3

static void tfmx_set_cell(RVPatternCell* cell, uint32_t raw, const char* text) {
    cell->raw = raw;
    memset(cell->text, 0, sizeof(cell->text));
    if (text != nullptr) {
        strncpy((char*)cell->text, text, sizeof(cell->text) - 1);
    }
}

static void tfmx_note_name(uint8_t note, char* out, size_t out_size) {
    static const char* names[12] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };
    snprintf(out, out_size, "%s%d", names[note % 12], note / 12);
}

// Render the TFMX_COLUMN_COUNT cells (Note, Inst, Vol, Eff, Prm) for one (track, row).
// Each cell's `raw` is the consistent numeric value and `text` is the rendered string, so
// TFMX's mixed effect encoding (a letter for WAIT, a raw byte otherwise) is standardized at
// the boundary: 'W' lives in `text`, the command byte in `raw`. The destination voice
// (old dest_channel routing) is packed into the Note cell's raw high byte for colouring.
static void tfmx_fill_row(void* decoder, int track, uint32_t row, RVPatternCell* out) {
    uint8_t type = 0, note = 0, macro = 0, volume = 0, dest = 0;
    int8_t detune = 0;
    uint16_t wait = 0;
    uint32_t tick = 0;
    char buf[8];

    if (!tfmxdec_get_pattern_display_row(decoder, track, row, &type, &note, &macro, &volume, &detune, &dest, &wait,
                                         &tick)) {
        for (int c = 0; c < TFMX_COLUMN_COUNT; c++) {
            tfmx_set_cell(&out[c], 0, "");
        }
        return;
    }

    uint32_t routing = (uint32_t)dest << 8;

    if (type == 0) {
        tfmx_note_name(note, buf, sizeof(buf));
        tfmx_set_cell(&out[0], (uint32_t)note | routing, buf);

        if (macro != 0) {
            snprintf(buf, sizeof(buf), "%02X", macro);
            tfmx_set_cell(&out[1], macro, buf);
        } else {
            tfmx_set_cell(&out[1], 0, "..");
        }

        if (volume != 0) {
            snprintf(buf, sizeof(buf), "%02X", volume);
            tfmx_set_cell(&out[2], volume, buf);
        } else {
            tfmx_set_cell(&out[2], 0, "..");
        }

        if (wait != 0) {
            tfmx_set_cell(&out[3], TFMX_CMD_WAIT, "W");
            snprintf(buf, sizeof(buf), "%02X", (unsigned)(wait & 0xFF));
            tfmx_set_cell(&out[4], wait, buf);
        } else if (detune != 0) {
            tfmx_set_cell(&out[3], 0, "..");
            snprintf(buf, sizeof(buf), "%02X", (uint8_t)detune);
            tfmx_set_cell(&out[4], (uint8_t)detune, buf);
        } else {
            tfmx_set_cell(&out[3], 0, "..");
            tfmx_set_cell(&out[4], 0, "..");
        }
    } else {
        // Command row (TFMX emits WAIT here); `note` holds the command byte, not a pitch.
        tfmx_set_cell(&out[0], routing, "---");
        tfmx_set_cell(&out[1], 0, "..");
        tfmx_set_cell(&out[2], 0, "..");
        if (note == TFMX_CMD_WAIT) {
            tfmx_set_cell(&out[3], note, "W");
        } else {
            snprintf(buf, sizeof(buf), "%02X", note);
            tfmx_set_cell(&out[3], note, buf);
        }
        snprintf(buf, sizeof(buf), "%02X", macro);
        tfmx_set_cell(&out[4], macro, buf);
    }
}

static bool tfmx_get_structure(void* user_data, RVVizInfo* out) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;
    if (data == nullptr || data->decoder == nullptr || out == nullptr) {
        return false;
    }
    int num_tracks = tfmxdec_get_pattern_display_num_tracks(data->decoder);
    if (num_tracks <= 0) {
        return false;
    }
    int num_voices = tfmxdec_voices(data->decoder);
    out->caps = RVVizCaps_PatternCells | RVVizCaps_Scope;
    out->scroll_mode = RVScrollMode_PerChannel;
    out->pattern_channel_count = (uint32_t)num_tracks;
    out->scope_channel_count = num_voices > 0 ? (uint32_t)num_voices : 0;
    out->column_count = TFMX_COLUMN_COUNT;
    return true;
}

static uint32_t tfmx_get_columns(void* user_data, RVColumnDesc* out, uint32_t cap) {
    (void)user_data;
    static const struct {
        const char* label;
        uint8_t width;
        RVColumnKind kind;
    } cols[TFMX_COLUMN_COUNT] = {
        { "Note", 3, RVColumnKind_Note }, { "Inst", 2, RVColumnKind_Instrument }, { "Vol", 2, RVColumnKind_Volume },
        { "Eff", 2, RVColumnKind_Effect }, { "Prm", 2, RVColumnKind_Param },
    };
    uint32_t n = cap < TFMX_COLUMN_COUNT ? cap : TFMX_COLUMN_COUNT;
    for (uint32_t i = 0; i < n; i++) {
        memset(out[i].label, 0, sizeof(out[i].label));
        strncpy((char*)out[i].label, cols[i].label, sizeof(out[i].label) - 1);
        out[i].char_width = cols[i].width;
        out[i].kind = cols[i].kind;
    }
    return n;
}

static uint32_t tfmx_get_pattern_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;
    if (data == nullptr || data->decoder == nullptr) {
        return 0;
    }
    int num_tracks = tfmxdec_get_pattern_display_num_tracks(data->decoder);
    if (num_tracks < 0) {
        num_tracks = 0;
    }
    uint32_t n = (uint32_t)num_tracks < cap ? (uint32_t)num_tracks : cap;
    for (uint32_t i = 0; i < n; i++) {
        memset(out[i].name, 0, sizeof(out[i].name));
        snprintf((char*)out[i].name, sizeof(out[i].name), "Track %u", i + 1);
        out[i].scope_width = 0;
    }
    return n;
}

static uint32_t tfmx_get_scope_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;
    if (data == nullptr || data->decoder == nullptr) {
        return 0;
    }
    int num_voices = tfmxdec_voices(data->decoder);
    if (num_voices < 0) {
        num_voices = 0;
    }
    uint32_t n = (uint32_t)num_voices < cap ? (uint32_t)num_voices : cap;
    for (uint32_t i = 0; i < n; i++) {
        memset(out[i].name, 0, sizeof(out[i].name));
        snprintf((char*)out[i].name, sizeof(out[i].name), "Voice %u", i + 1);
        out[i].scope_width = 1; // mono per voice
    }
    return n;
}

static bool tfmx_get_position(void* user_data, RVTrackerPosition* out) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;
    if (data == nullptr || data->decoder == nullptr || out == nullptr) {
        return false;
    }
    int num_tracks = tfmxdec_get_pattern_display_num_tracks(data->decoder);
    uint32_t max_rows = 0;
    for (int i = 0; i < num_tracks; i++) {
        uint32_t rows = tfmxdec_get_pattern_display_track_rows(data->decoder, i);
        if (rows > max_rows) {
            max_rows = rows;
        }
    }
    out->order = 0;
    out->pattern = 0;
    out->row = 0;
    out->window_lo = 0;
    out->window_hi = max_rows; // per-channel playheads come from get_channel_rows
    return true;
}

static uint32_t tfmx_get_channel_rows(void* user_data, uint32_t* out, uint32_t cap) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;
    if (data == nullptr || data->decoder == nullptr || out == nullptr) {
        return 0;
    }
    int num_tracks = tfmxdec_get_pattern_display_num_tracks(data->decoder);
    if (num_tracks < 0) {
        num_tracks = 0;
    }
    uint32_t tick = tfmxdec_get_pattern_tick(data->decoder);
    uint32_t n = (uint32_t)num_tracks < cap ? (uint32_t)num_tracks : cap;
    for (uint32_t i = 0; i < n; i++) {
        int r = tfmxdec_find_pattern_display_row_for_tick(data->decoder, (int)i, tick);
        out[i] = r < 0 ? 0 : (uint32_t)r;
    }
    return n;
}

static uint32_t tfmx_get_cells(void* user_data, int32_t channel, uint32_t row_lo, uint32_t row_hi, RVPatternCell* out,
                               uint32_t cap) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;
    if (data == nullptr || data->decoder == nullptr || out == nullptr) {
        return 0;
    }
    int num_tracks = tfmxdec_get_pattern_display_num_tracks(data->decoder);
    if (num_tracks <= 0) {
        return 0;
    }

    int ch_start = channel < 0 ? 0 : channel;
    int ch_end = channel < 0 ? num_tracks : channel + 1;
    if (ch_start >= num_tracks) {
        return 0;
    }
    if (ch_end > num_tracks) {
        ch_end = num_tracks;
    }

    // Rectangular row-major grid (row -> channel -> column). Rows past a short track's
    // end yield empty cells so the host can index a fixed stride across channels.
    uint32_t written = 0;
    for (uint32_t row = row_lo; row < row_hi; row++) {
        for (int ch = ch_start; ch < ch_end; ch++) {
            if (written + TFMX_COLUMN_COUNT > cap) {
                return written;
            }
            tfmx_fill_row(data->decoder, ch, row, &out[written]);
            written += TFMX_COLUMN_COUNT;
        }
    }
    return written;
}

static void tfmx_set_scope_enabled(void* user_data, bool on) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;
    if (data == nullptr || data->decoder == nullptr) {
        return;
    }
    tfmxdec_enable_scope_capture(data->decoder, on ? 1 : 0);
    data->scope_enabled = on;
}

static uint32_t tfmx_get_scope_samples(void* user_data, int32_t channel, float* out, uint32_t cap) {
    TfmxReplayerData* data = (TfmxReplayerData*)user_data;
    if (data == nullptr || data->decoder == nullptr || out == nullptr) {
        return 0;
    }
    if (!data->scope_enabled) {
        tfmxdec_enable_scope_capture(data->decoder, 1);
        data->scope_enabled = true;
    }
    return tfmxdec_get_scope_data(data->decoder, (int)channel, out, cap);
}

static uint32_t tfmx_get_vu(void* user_data, float* out, uint32_t cap) {
    (void)user_data;
    (void)out;
    (void)cap;
    return 0;
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
    nullptr, // static_destroy
    tfmx_get_structure,
    tfmx_get_columns,
    tfmx_get_pattern_channels,
    tfmx_get_scope_channels,
    tfmx_get_position,
    tfmx_get_channel_rows,
    tfmx_get_cells,
    tfmx_set_scope_enabled,
    tfmx_get_scope_samples,
    tfmx_get_vu,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_tfmx_plugin;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
