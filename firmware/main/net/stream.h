/* stream.h: the network half of the audio path.
 *
 * Runs on core 0 alongside Wi-Fi and lwIP. Pulls bytes from an HTTP
 * shoutcast/icecast endpoint and drops them into a 64 KB ring buffer, which
 * is the shock absorber between a network that delivers in bursts and a
 * decoder that wants a steady trickle. About four seconds at 128 kbit/s.
 *
 * The decoder never touches the socket and the stream task never touches the
 * DSP: the ring buffer is the whole interface between them.
 */
#ifndef AMTX_STREAM_H
#define AMTX_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "am_config.h"
#include "esp_err.h"

typedef enum {
    STREAM_IDLE = 0,
    STREAM_CONNECTING,
    STREAM_PLAYING,
    STREAM_RETRYING,
    STREAM_FAILED,
} stream_state_t;

typedef struct {
    stream_state_t state;
    uint32_t buffered;        /* bytes waiting in the ring buffer */
    uint32_t capacity;
    uint32_t bytes_total;     /* since the current connection opened */
    uint32_t reconnects;
    int http_status;
    char url[192];
    char error[64];
} stream_stats_t;

/* Creates a ring buffer and a stream task for every chain. Each task idles
 * until stream_play() is called for its chain. The buffers go to PSRAM when
 * the board has it: two of them is 128 kB, which internal DRAM cannot spare
 * alongside Wi-Fi and TLS. */
esp_err_t stream_init(void);

/* Connect and start filling one chain. Replaces any stream already playing on
 * that chain; returns as soon as the request is queued, not when audio
 * arrives. Chains are independent: playing on one does not disturb the other. */
esp_err_t stream_play(int ch, const char *url);

/* Disconnect one chain and empty its buffer. */
void stream_stop(int ch);

/* Consumer side, called from the decode task on core 1. Returns a pointer to
 * up to `max` contiguous bytes, or NULL on timeout; the caller MUST pass it
 * back to stream_release() before the next call. */
const uint8_t *stream_acquire(int ch, size_t max, uint32_t timeout_ms,
                              size_t *got);
void stream_release(int ch, const uint8_t *chunk);

void stream_get_stats(int ch, stream_stats_t *out);
bool stream_is_playing(int ch);

#endif /* AMTX_STREAM_H */
