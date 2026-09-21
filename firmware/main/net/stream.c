/* stream.c: see stream.h. */

#include "stream.h"

#include <stdio.h>
#include <string.h>

#include "am_config.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "stream";

/* Shoutcast v1 servers answer a non-browser client with "ICY 200 OK" instead
 * of a status line esp_http_client can parse, and the connection then looks
 * like a protocol error. Presenting as a browser gets a normal HTTP response
 * from every server tried. See KI-05. */
#define STREAM_USER_AGENT \
    "Mozilla/5.0 (compatible; AMTX/1.0; +ESP32-S3 AM Transmitter)"

#define READ_CHUNK 1460          /* one TCP segment */

/* One of these per chain. Everything that used to be a file static lives here
 * instead, so a second transmitter is a second element rather than a second
 * copy of the file. */
typedef struct {
    int ch;
    RingbufHandle_t rb;
    SemaphoreHandle_t lock;      /* guards url and epoch */
    TaskHandle_t task;

    char url[192];
    volatile bool want_play;
    volatile uint32_t epoch;     /* bumped on every play/stop request */

    volatile stream_state_t state;
    volatile uint32_t bytes_total;
    volatile uint32_t reconnects;
    volatile int http_status;
    char error[64];
} stream_ctx_t;

static stream_ctx_t s_ctx[RF_CHAINS];

static inline bool chain_ok(int ch) { return ch >= 0 && ch < RF_CHAINS; }

/* ------------------------------------------------------------ ring buffer */

static uint32_t rb_buffered(const stream_ctx_t *c)
{
    if (!c->rb) return 0;
    UBaseType_t free_bytes = 0;
    vRingbufferGetInfo(c->rb, &free_bytes, NULL, NULL, NULL, NULL);
    return (uint32_t)(STREAM_RINGBUF_BYTES - free_bytes);
}

static void rb_flush(stream_ctx_t *c)
{
    if (!c->rb) return;
    for (;;) {
        size_t sz = 0;
        void *p = xRingbufferReceiveUpTo(c->rb, &sz, 0, STREAM_RINGBUF_BYTES);
        if (!p) break;
        vRingbufferReturnItem(c->rb, p);
    }
}

/* --------------------------------------------------------------- the task */

static void set_error(stream_ctx_t *c, const char *msg)
{
    snprintf(c->error, sizeof c->error, "%s", msg ? msg : "");
}

/* Opens the URL, following 3xx by hand, and leaves the client sitting on a
 * response whose status is returned.
 *
 * esp_http_client only follows redirects inside esp_http_client_perform().
 * This path uses open()/fetch_headers() instead, because a radio stream is
 * unbounded and perform() wants to buffer a whole response, and on that path
 * .max_redirection_count in the config is inert: the 302 is simply handed back
 * as the status. That is a trap worth naming, because the config field reads
 * exactly as though it were doing the job. See LL-04.
 *
 * esp_http_client_set_redirection() applies the Location header the client has
 * already parsed, so the sequence is close, redirect, open again. Most station
 * URLs need at least one pass: CDN hand-offs, load balancers, and the http to
 * https promotion that a directory entry so often turns out to be. */
static esp_err_t open_following_redirects(int ch,
                                          esp_http_client_handle_t client,
                                          int *status_out)
{
    for (int hop = 0; hop <= STREAM_REDIRECT_MAX; hop++) {
        const esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) return err;

        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        *status_out = status;

        if (status < 300 || status >= 400) return ESP_OK;   /* done, good or bad */

        if (hop == STREAM_REDIRECT_MAX) {
            ESP_LOGW(TAG, "[%d] gave up after %d redirects", ch,
                     STREAM_REDIRECT_MAX);
            return ESP_OK;      /* caller reports the 3xx as the failure */
        }

        ESP_LOGI(TAG, "[%d] HTTP %d, following redirect", ch, status);
        esp_http_client_close(client);
        const esp_err_t rerr = esp_http_client_set_redirection(client);
        if (rerr != ESP_OK) {
            ESP_LOGW(TAG, "[%d] no usable Location header: %s", ch,
                     esp_err_to_name(rerr));
            return ESP_OK;      /* leave *status_out as the 3xx */
        }
    }
    return ESP_OK;
}

/* One connection, from open to disconnect. Returns when the stream ends, the
 * epoch changes (a new request arrived), or an error occurs. */
static void run_one_connection(stream_ctx_t *c, uint32_t epoch, const char *url)
{
    const esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = STREAM_USER_AGENT,
        .keep_alive_enable = true,
        /* A redirect from a plain-http station URL lands on https more often
         * than not now, so the bundle is needed for the hop even when the URL
         * the operator saved was http. If the handshake cannot be afforded --
         * mbedTLS wants tens of kB and the ring buffer has already taken 64 --
         * open() fails and the normal retry/backoff covers it. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* Station URLs redirect constantly: playlists, load balancers, CDN
         * hand-offs. Followed by hand in open_following_redirects() above --
         * see the comment there for why this field alone does nothing. */
        .max_redirection_count = STREAM_REDIRECT_MAX,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        set_error(c, "client init failed");
        c->state = STREAM_FAILED;
        return;
    }

    /* Not requesting ICY metadata: it is interleaved into the audio bytes and
     * every block has to be stripped before the decoder sees them. Worth
     * doing eventually (EH-08), not worth doing first. */
    esp_http_client_set_header(client, "Icy-MetaData", "0");

    int status = 0;
    esp_err_t err = open_following_redirects(c->ch, client, &status);
    if (err != ESP_OK) {
        set_error(c, esp_err_to_name(err));
        ESP_LOGW(TAG, "[%d] open failed: %s", c->ch, esp_err_to_name(err));
        goto done;
    }

    c->http_status = status;
    if (c->http_status < 200 || c->http_status >= 300) {
        snprintf(c->error, sizeof c->error, "HTTP %d", c->http_status);
        ESP_LOGW(TAG, "[%d] %s", c->ch, c->error);
        goto done;
    }

    ESP_LOGI(TAG, "[%d] connected, HTTP %d", c->ch, c->http_status);
    c->state = STREAM_PLAYING;
    c->bytes_total = 0;
    set_error(c, "");

    /* Per chain, and no longer static: two tasks run this concurrently. */
    uint8_t buf[READ_CHUNK];
    while (c->want_play && c->epoch == epoch) {
        const int n = esp_http_client_read(client, (char *)buf, sizeof buf);
        if (n < 0) {
            set_error(c, "read error");
            break;
        }
        if (n == 0) {
            /* The far end closed, or the socket timed out with nothing to
             * give. Either way this connection is finished. */
            if (esp_http_client_is_complete_data_received(client)) {
                set_error(c, "stream ended");
            } else {
                set_error(c, "read timeout");
            }
            break;
        }

        c->bytes_total += (uint32_t)n;

        /* Block until there is room. Back-pressure belongs here, on core 0,
         * where blocking costs nothing: the decoder is what must never wait.
         * A full buffer for a whole second means the decoder has stopped, so
         * drop the chunk rather than deadlock the task. */
        if (xRingbufferSend(c->rb, buf, (size_t)n, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "[%d] ring buffer full, dropped %d bytes", c->ch, n);
        }
    }

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static void stream_task(void *arg)
{
    stream_ctx_t *c = arg;
    int backoff_ms = 500;

    for (;;) {
        if (!c->want_play) {
            c->state = STREAM_IDLE;
            vTaskDelay(pdMS_TO_TICKS(100));
            backoff_ms = 500;
            continue;
        }

        char url[sizeof c->url];
        uint32_t epoch;
        xSemaphoreTake(c->lock, portMAX_DELAY);
        snprintf(url, sizeof url, "%s", c->url);
        epoch = c->epoch;
        xSemaphoreGive(c->lock);

        if (url[0] == '\0') {
            c->want_play = false;
            continue;
        }

        c->state = STREAM_CONNECTING;
        ESP_LOGI(TAG, "[%d] connecting to %s", c->ch, url);
        run_one_connection(c, epoch, url);

        if (!c->want_play || c->epoch != epoch) {
            continue;    /* superseded, not a failure */
        }

        /* Exponential backoff to 8 s. A station that is down stays down for
         * a while, and hammering it is both rude and pointless. */
        c->state = STREAM_RETRYING;
        c->reconnects++;
        ESP_LOGW(TAG, "[%d] reconnecting in %d ms (%s)", c->ch, backoff_ms,
                 c->error);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = backoff_ms < 8000 ? backoff_ms * 2 : 8000;
    }
}

/* ------------------------------------------------------------------- API */

esp_err_t stream_init(void)
{
    for (int ch = 0; ch < RF_CHAINS; ch++) {
        stream_ctx_t *c = &s_ctx[ch];
        if (c->rb) continue;
        c->ch = ch;

        /* PSRAM if the board has it. Two 64 kB buffers is most of the
         * internal DRAM that Wi-Fi and the TLS stack want, and neither buffer
         * is ever touched from an ISR, so PSRAM's latency is irrelevant. */
        c->rb = xRingbufferCreateWithCaps(STREAM_RINGBUF_BYTES,
                                          RINGBUF_TYPE_BYTEBUF,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!c->rb) {
            ESP_LOGW(TAG, "[%d] no PSRAM, using internal DRAM for the buffer",
                     ch);
            c->rb = xRingbufferCreate(STREAM_RINGBUF_BYTES, RINGBUF_TYPE_BYTEBUF);
        }
        if (!c->rb) return ESP_ERR_NO_MEM;

        c->lock = xSemaphoreCreateMutex();
        if (!c->lock) return ESP_ERR_NO_MEM;

        char name[12];
        snprintf(name, sizeof name, "stream%d", ch);
        if (xTaskCreatePinnedToCore(stream_task, name, TASK_STACK_STREAM, c,
                                    TASK_PRIO_STREAM, &c->task,
                                    TASK_CORE_NET) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t stream_play(int ch, const char *url)
{
    if (!chain_ok(ch)) return ESP_ERR_INVALID_ARG;
    if (!url || url[0] == '\0') return ESP_ERR_INVALID_ARG;

    stream_ctx_t *c = &s_ctx[ch];
    xSemaphoreTake(c->lock, portMAX_DELAY);
    snprintf(c->url, sizeof c->url, "%s", url);
    c->epoch++;
    xSemaphoreGive(c->lock);

    rb_flush(c);
    c->http_status = 0;
    set_error(c, "");
    c->want_play = true;
    return ESP_OK;
}

void stream_stop(int ch)
{
    if (!chain_ok(ch)) return;
    stream_ctx_t *c = &s_ctx[ch];

    xSemaphoreTake(c->lock, portMAX_DELAY);
    c->epoch++;
    c->url[0] = '\0';
    xSemaphoreGive(c->lock);

    c->want_play = false;
    rb_flush(c);
}

const uint8_t *stream_acquire(int ch, size_t max, uint32_t timeout_ms,
                              size_t *got)
{
    *got = 0;
    if (!chain_ok(ch) || !s_ctx[ch].rb) return NULL;

    size_t sz = 0;
    void *p = xRingbufferReceiveUpTo(s_ctx[ch].rb, &sz,
                                     pdMS_TO_TICKS(timeout_ms), max);
    if (!p) return NULL;
    *got = sz;
    return (const uint8_t *)p;
}

void stream_release(int ch, const uint8_t *chunk)
{
    if (chain_ok(ch) && chunk && s_ctx[ch].rb) {
        vRingbufferReturnItem(s_ctx[ch].rb, (void *)chunk);
    }
}

void stream_get_stats(int ch, stream_stats_t *out)
{
    memset(out, 0, sizeof *out);
    if (!chain_ok(ch)) return;
    stream_ctx_t *c = &s_ctx[ch];

    out->state = c->state;
    out->buffered = rb_buffered(c);
    out->capacity = STREAM_RINGBUF_BYTES;
    out->bytes_total = c->bytes_total;
    out->reconnects = c->reconnects;
    out->http_status = c->http_status;

    if (c->lock) {
        xSemaphoreTake(c->lock, portMAX_DELAY);
        snprintf(out->url, sizeof out->url, "%s", c->url);
        xSemaphoreGive(c->lock);
    }
    snprintf(out->error, sizeof out->error, "%s", c->error);
}

bool stream_is_playing(int ch)
{
    return chain_ok(ch) && s_ctx[ch].state == STREAM_PLAYING;
}
