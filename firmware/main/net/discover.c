/* discover.c: see discover.h. */

#include "discover.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "am_config.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "discover";

/* Radio-Browser asks callers to identify themselves so they can see which
 * clients are generating load. It is a community service run on donated
 * mirrors; a project-shaped User-Agent is the price of using it. */
#define DISCOVER_USER_AGENT "AMTX/1.0 (ESP32-S3 AM Transmitter)"

static char s_error[80] = "";

const char *discover_last_error(void) { return s_error; }

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof s_error, fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s", s_error);
}

discover_by_t discover_by_from_string(const char *s)
{
    if (!s) return DISCOVER_BY_NAME;
    if (strcmp(s, "tag") == 0 || strcmp(s, "genre") == 0) return DISCOVER_BY_TAG;
    if (strcmp(s, "country") == 0) return DISCOVER_BY_COUNTRY;
    return DISCOVER_BY_NAME;
}

/* Percent-encode everything outside the unreserved set. The query arrives
 * from a text box and goes into a URL, so this is not optional: a space or an
 * ampersand would otherwise change the request's shape. */
static void url_encode(const char *in, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    for (size_t i = 0; in[i] != '\0' && o + 4 < out_len; i++) {
        const unsigned char c = (unsigned char)in[i];
        const bool unreserved =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~';

        if (unreserved) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0f];
        }
    }
    out[o] = '\0';
}

/* Reads the whole reply into `buf`, which the caller has already allocated.
 *
 * The buffer is deliberately not allocated in here. By the time the response
 * headers arrive, mbedTLS holds tens of kB for the session and the parsed
 * certificate bundle, and the largest free block has shrunk accordingly: an
 * allocation attempted at that moment fails even though the total free heap
 * still looks ample. Claiming it before the connection opens is the difference
 * between this working and not, and it cost a flash cycle to find out.
 *
 * cJSON needs the complete document, so there is no streaming alternative
 * short of a different parser. */
static bool fetch_body(const char *url, char *buf, size_t cap)
{
    const esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 12000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = DISCOVER_USER_AGENT,
        /* The API is HTTPS and the mirrors redirect between themselves. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 3,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        set_error("client init failed");
        return false;
    }

    bool ok = false;
    size_t len = 0;

    const esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        set_error("connect failed: %s", esp_err_to_name(err));
        goto done;
    }

    esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        set_error("directory returned HTTP %d", status);
        goto done;
    }

    for (;;) {
        if (len + 1 >= cap) {
            /* Truncated JSON will not parse, so say what actually happened
             * rather than blaming the directory for malformed output. */
            set_error("too many results to hold in RAM; narrow the search");
            goto done;
        }

        const int n = esp_http_client_read(client, buf + len,
                                           (int)(cap - len - 1));
        if (n < 0) {
            set_error("read failed");
            goto done;
        }
        if (n == 0) break;      /* complete */
        len += (size_t)n;
    }

    buf[len] = '\0';
    ok = true;

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

/* Copies a cJSON string field, tolerating absent or null members. */
static void copy_str(const cJSON *obj, const char *key, char *out, size_t len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    out[0] = '\0';
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(out, len, "%s", item->valuestring);
    }
}

static int copy_int(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : 0;
}

int discover_search(discover_by_t by, const char *query,
                    discover_entry_t *out, int max)
{
    s_error[0] = '\0';

    if (!query || query[0] == '\0' || !out || max <= 0) {
        set_error("empty search");
        return -1;
    }

    const char *key = (by == DISCOVER_BY_TAG)       ? "tag"
                      : (by == DISCOVER_BY_COUNTRY) ? "country"
                                                    : "name";

    char encoded[192];
    url_encode(query, encoded, sizeof encoded);

    if (max > DISCOVER_RESULTS_MAX) max = DISCOVER_RESULTS_MAX;

    /* order=clickcount puts the stations people actually listen to first,
     * which matters far more than an alphabetical list on a short page.
     * hidebroken drops the ones the directory's own checker cannot reach. */
    char url[420];
    snprintf(url, sizeof url,
             "https://%s/json/stations/search"
             "?%s=%s&codec=MP3&hidebroken=true&is_https=false"
             "&order=clickcount&reverse=true&limit=%d",
             DISCOVER_API_HOST, key, encoded, max);

    ESP_LOGI(TAG, "search %s=\"%s\"", key, query);

    /* Claimed before the connection, while the heap is least fragmented. */
    char *body = malloc(DISCOVER_BODY_MAX);
    if (!body) {
        set_error("out of memory");
        return -1;
    }

    if (!fetch_body(url, body, DISCOVER_BODY_MAX)) {
        free(body);
        return -1;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        set_error("directory sent something that is not a list");
        return -1;
    }

    int n = 0;
    const cJSON *st = NULL;
    cJSON_ArrayForEach(st, root) {
        if (n >= max) break;
        if (!cJSON_IsObject(st)) continue;

        discover_entry_t *e = &out[n];
        memset(e, 0, sizeof *e);

        /* url_resolved is the stream itself; url is often a .pls or .m3u
         * playlist, which this firmware does not parse. Prefer the resolved
         * one and fall back only if the directory has not filled it in. */
        copy_str(st, "url_resolved", e->url, sizeof e->url);
        if (e->url[0] == '\0') copy_str(st, "url", e->url, sizeof e->url);
        if (e->url[0] == '\0') continue;

        copy_str(st, "name", e->name, sizeof e->name);
        if (e->name[0] == '\0') snprintf(e->name, sizeof e->name, "Unnamed");

        copy_str(st, "country", e->country, sizeof e->country);
        copy_str(st, "codec", e->codec, sizeof e->codec);
        e->bitrate = copy_int(st, "bitrate");
        e->votes = copy_int(st, "votes");
        n++;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "%d result(s)", n);
    return n;
}
