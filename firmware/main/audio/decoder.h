/* decoder.h: the audio half of the path, on core 1.
 *
 * Reads MP3 bytes from the stream ring buffer, decodes them with libhelix,
 * runs the DSP chain, and hands 20 kHz samples to the modulator. It always
 * produces output at exactly AM_SAMPLE_RATE_HZ, whatever is happening
 * upstream: decoded audio when a station is playing, a test tone in tone
 * mode, silence otherwise. A carrier fed silence is an unmodulated carrier,
 * which is what a receiver should hear between stations, rather than the
 * underrun decay.
 */
#ifndef AMTX_DECODER_H
#define AMTX_DECODER_H

#include <stdbool.h>
#include <stdint.h>

#include "am_config.h"
#include "esp_err.h"

typedef struct {
    int sample_rate;          /* as reported by the last decoded frame */
    int channels;
    int bitrate;
    uint32_t frames;          /* decoded successfully */
    uint32_t errors;          /* frames the decoder rejected */
    uint32_t resyncs;         /* times we had to hunt for a sync word */
    int32_t trim_ppm;         /* what the fill controller is asking for */
    float agc_db;             /* what the AGC is currently adding */
    float peak;               /* 0..1, last block, for the level meter */
} decoder_stats_t;

/* Creates one decode task per chain, all pinned to core 1. Must be called
 * after carrier_init(), which the tasks feed.
 *
 * Core 1 is required rather than convenient: each sample FIFO is a lockless
 * single-producer, single-consumer queue whose contract assumes the producing
 * task and the consuming ISR share a core. */
esp_err_t decoder_init(void);

/* Both take effect at the next block; the filter is redesigned in place. */
void decoder_set_lpf(int ch, int cutoff_hz);
void decoder_set_gain(int ch, int percent);

/* The loudness leveller. `target_pct` is the average level it aims for, as a
 * percentage of full scale. Off leaves the fixed gain and the limiter alone,
 * which is what you want if you care about dynamic range more than about
 * every station sounding the same. */
void decoder_set_agc(int ch, bool on, int target_pct);

/* Bring-up aid: replaces the programme with a steady tone at the modulator's
 * own rate, so a scope on the RF pin shows a clean envelope with the network
 * and the decoder taken out of the picture. */
void decoder_set_tone(int ch, bool on, int tone_hz);
bool decoder_tone_active(int ch);

void decoder_get_stats(int ch, decoder_stats_t *out);

#endif /* AMTX_DECODER_H */
