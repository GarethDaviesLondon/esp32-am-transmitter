/* webui.c: see webui.h. */

#include "webui.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "am_config.h"
#include "app.h"
#include "audio/decoder.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/discover.h"
#include "net/wifi_conn.h"
#include "net/www_page.h"
#include "state_doc.h"
#include "store/settings.h"

static const char *TAG = "webui";

#define BODY_MAX 512

static httpd_handle_t s_server;

/* ------------------------------------------------------------- plumbing */

/* Read a JSON body into a cJSON tree. Returns NULL on anything malformed;
 * callers treat that as "no fields supplied" rather than an error, because a
 * handler that 400s on an empty body is a handler that breaks the moment the
 * page sends {} for a toggle. */
static cJSON *read_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= BODY_MAX) return NULL;

    char buf[BODY_MAX];
    int received = 0;
    while (received < req->content_len) {
        const int n = httpd_req_recv(req, buf + received,
                                     (size_t)(req->content_len - received));
        if (n <= 0) return NULL;
        received += n;
    }
    buf[received] = '\0';
    return cJSON_Parse(buf);
}

static int json_int(const cJSON *root, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static bool json_bool(const cJSON *root, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (cJSON_IsNumber(item)) return item->valueint != 0;
    return fallback;
}

static const char *json_str(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *doc)
{
    char *text = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (!text) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static esp_err_t send_ok(httpd_req_t *req, esp_err_t result)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "ok", result == ESP_OK);
    if (result != ESP_OK) {
        cJSON_AddStringToObject(doc, "error", esp_err_to_name(result));
    }
    return send_json(req, doc);
}

/* -------------------------------------------------------------- handlers */

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, WWW_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t state_get(httpd_req_t *req)
{
    cJSON *doc = state_doc_build();
    if (!doc) return httpd_resp_send_500(req);
    return send_json(req, doc);
}

/* Which transmitter a request is aimed at. Absent means chain 0, so a client
 * written against the single-carrier API keeps working. */
static int json_chain(const cJSON *root)
{
    const int ch = json_int(root, "ch", 0);
    return (ch >= 0 && ch < RF_CHAINS) ? ch : 0;
}

static esp_err_t station_play_post(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (body) {
        const int ch = json_chain(body);
        const char *url = json_str(body, "url");
        if (url && url[0]) {
            err = app_play_url(ch, url);
        } else {
            err = app_play_index(ch, json_int(body, "index", -1));
        }
        cJSON_Delete(body);
    }
    return send_ok(req, err);
}

static esp_err_t station_add_post(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (body) {
        const char *url = json_str(body, "url");
        const char *name = json_str(body, "name");
        if (url) err = settings_station_add(name ? name : "", url);
        cJSON_Delete(body);
    }
    return send_ok(req, err);
}

/* Sets, or with a negative index clears, the station the board comes up on.
 * The index is remembered, not the name, so settings_station_move() and
 * settings_station_delete() carry it through a reordering. */
static esp_err_t station_default_post(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (body) {
        err = settings_set_default_station(json_chain(body),
                                           json_int(body, "index", -1));
        cJSON_Delete(body);
    }
    return send_ok(req, err);
}

/* Per-station level trim. Applied live if that station is the one currently
 * playing on the named chain. */
static esp_err_t station_gain_post(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (body) {
        err = app_set_station_gain(json_chain(body),
                                   json_int(body, "index", -1),
                                   json_int(body, "gain", 100));
        cJSON_Delete(body);
    }
    return send_ok(req, err);
}

static esp_err_t station_delete_post(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (body) {
        err = settings_station_delete(json_int(body, "index", -1));
        cJSON_Delete(body);
    }
    return send_ok(req, err);
}

static esp_err_t station_move_post(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (body) {
        err = settings_station_move(json_int(body, "from", -1),
                                    json_int(body, "to", -1));
        cJSON_Delete(body);
    }
    return send_ok(req, err);
}

static esp_err_t stop_post(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    const int ch = body ? json_chain(body) : 0;
    cJSON_Delete(body);
    app_stop(ch);
    return send_ok(req, ESP_OK);
}

static esp_err_t rf_post(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    if (!body) return send_ok(req, ESP_ERR_INVALID_ARG);

    /* Start from what is live, so a partial body only changes what it names. */
    const int ch = json_chain(body);

    rf_settings_t rf;
    app_get_rf(ch, &rf);
    rf.rf_on = json_bool(body, "on", rf.rf_on);
    rf.carrier_hz = (uint32_t)json_int(body, "hz", (int)rf.carrier_hz);
    rf.depth_pct = json_int(body, "depth", rf.depth_pct);
    rf.lpf_hz = json_int(body, "lpf", rf.lpf_hz);
    rf.gain_pct = json_int(body, "gain", rf.gain_pct);
    rf.level_pct = json_int(body, "level", rf.level_pct);
    rf.agc_on = json_bool(body, "agc", rf.agc_on);
    rf.agc_target_pct = json_int(body, "agct", rf.agc_target_pct);
    rf.fade_mode = json_int(body, "fdm", rf.fade_mode);
    rf.fade_rate_mhz = json_int(body, "fdr", rf.fade_rate_mhz);
    rf.fade_depth_pct = json_int(body, "fdd", rf.fade_depth_pct);
    cJSON_Delete(body);

    return send_ok(req, app_set_rf(ch, &rf));
}

static esp_err_t tone_post(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    if (!body) return send_ok(req, ESP_ERR_INVALID_ARG);

    decoder_set_tone(json_chain(body), json_bool(body, "on", false), json_int(body, "hz", 1000));
    cJSON_Delete(body);
    return send_ok(req, ESP_OK);
}

/* Decode %XX and + in place. httpd_query_key_value() hands back the raw
 * encoded value, and a search for "drum & bass" arrives as "drum+%26+bass". */
static void url_decode(char *s)
{
    char *w = s;
    for (const char *r = s; *r != '\0'; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && isxdigit((unsigned char)r[1]) &&
                   isxdigit((unsigned char)r[2])) {
            const char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* Blocking: a directory search is an HTTPS round trip of a second or two, and
 * it runs on the httpd task. The page disables its own Search button for the
 * duration rather than letting the operator queue several.
 *
 * The result array is heap-allocated, not a local: twenty discover_entry_t is
 * about 5 kB and the httpd task has a 6 kB stack. */
static esp_err_t discover_get(httpd_req_t *req)
{
    char query[256] = { 0 };
    char q[160] = { 0 };
    char by[16] = { 0 };

    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK) {
        httpd_query_key_value(query, "q", q, sizeof q);
        httpd_query_key_value(query, "by", by, sizeof by);
        url_decode(q);
        url_decode(by);
    }

    if (q[0] == '\0') {
        cJSON *doc = cJSON_CreateObject();
        cJSON_AddStringToObject(doc, "error", "nothing to search for");
        return send_json(req, doc);
    }

    discover_entry_t *found = calloc(DISCOVER_RESULTS_MAX, sizeof *found);
    if (!found) {
        cJSON *doc = cJSON_CreateObject();
        cJSON_AddStringToObject(doc, "error", "out of memory");
        return send_json(req, doc);
    }

    const int n = discover_search(discover_by_from_string(by), q, found,
                                  DISCOVER_RESULTS_MAX);
    if (n < 0) {
        cJSON *doc = cJSON_CreateObject();
        cJSON_AddStringToObject(doc, "error", discover_last_error());
        free(found);
        return send_json(req, doc);
    }

    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", found[i].name);
        cJSON_AddStringToObject(e, "url", found[i].url);
        cJSON_AddStringToObject(e, "country", found[i].country);
        cJSON_AddStringToObject(e, "codec", found[i].codec);
        cJSON_AddNumberToObject(e, "bitrate", found[i].bitrate);
        cJSON_AddNumberToObject(e, "votes", found[i].votes);
        cJSON_AddItemToArray(list, e);
    }
    free(found);
    return send_json(req, list);
}

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    static wifi_scan_entry_t found[24];
    const int n = wifi_conn_scan(found, 24);

    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", found[i].ssid);
        cJSON_AddNumberToObject(e, "rssi", found[i].rssi);
        cJSON_AddBoolToObject(e, "secure", found[i].secure);
        cJSON_AddItemToArray(list, e);
    }
    return send_json(req, list);
}

static void reboot_task(void *arg)
{
    (void)arg;
    /* Long enough for the response to leave, short enough that the operator
     * does not think the button missed. */
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

static esp_err_t wifi_add_post(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    bool joined_from_portal = false;

    if (body) {
        const char *ssid = json_str(body, "ssid");
        const char *pass = json_str(body, "pass");
        if (ssid && ssid[0]) {
            err = settings_wifi_add(ssid, pass ? pass : "");
            joined_from_portal = !wifi_conn_is_online();
        }
        cJSON_Delete(body);
    }

    const esp_err_t sent = send_ok(req, err);

    /* From the setup portal there is no way back: the AP has to come down for
     * the STA to come up, which drops this very connection. Reboot into the
     * normal path rather than trying to hand the socket over. */
    if (err == ESP_OK && joined_from_portal) {
        ESP_LOGI(TAG, "credential saved from the portal, restarting to join");
        xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    }
    return sent;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    const esp_err_t sent = send_ok(req, ESP_OK);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return sent;
}

/* Design 07 section 3. The mDNS name changes at once; the page is told the
 * router's list catches up after a reboot. */
static void factory_reset_task(void *arg)
{
    (void)arg;
    /* The reply has to reach the browser first: after this the board restarts
     * and the page it came from is talking to a factory-fresh transmitter on
     * a different network, or to nothing at all. */
    vTaskDelay(pdMS_TO_TICKS(600));
    ESP_LOGW(TAG, "factory reset from the page");
    settings_factory_reset();
    vTaskDelete(NULL);
}

/* Design 07 section 7. `confirm` is required in the body for the same reason
 * the console wants a word: this is not a button anyone should hit by
 * accident, and the page puts a dialog in front of it as well. */
static esp_err_t factory_reset_post(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    const bool confirmed = body && json_bool(body, "confirm", false);
    cJSON_Delete(body);

    if (!confirmed) return send_ok(req, ESP_ERR_INVALID_ARG);

    const esp_err_t sent = send_ok(req, ESP_OK);
    xTaskCreate(factory_reset_task, "factory", 2560, NULL, 5, NULL);
    return sent;
}

static esp_err_t hostname_post(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (body) {
        err = settings_set_hostname(json_str(body, "host"));
        cJSON_Delete(body);
    }
    if (err == ESP_OK) wifi_conn_apply_hostname();
    return send_ok(req, err);
}

static void web_off_task(void *arg)
{
    (void)arg;
    /* The reply has to reach the browser first, and httpd_stop() cannot be
     * called from one of the server's own handlers. */
    vTaskDelay(pdMS_TO_TICKS(600));
    webui_stop();
    ESP_LOGW(TAG, "web interface turned off from the page: only the serial "
                  "console can turn it back on ('web on')");
    vTaskDelete(NULL);
}

/* Design 07 section 4. Only turns the web off: a request that reaches this
 * handler is proof the web is already on, and there is no way back from the
 * page once it is off. */
static esp_err_t web_post(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    const bool on = body ? json_bool(body, "on", true) : true;
    cJSON_Delete(body);

    if (on) return send_ok(req, ESP_OK);

    const esp_err_t err = settings_set_web_enabled(false);
    const esp_err_t sent = send_ok(req, err);
    if (err == ESP_OK) xTaskCreate(web_off_task, "weboff", 2560, NULL, 5, NULL);
    return sent;
}

/* Every OS probes a different URL to decide whether it is behind a captive
 * portal. Redirecting anything unrecognised to the page covers all of them.
 * The Location is relative on purpose: an absolute 192.168.4.1 would be wrong
 * once the board is on the home network, and a stray URL there should land on
 * the UI rather than somewhere unreachable. */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------------- API */

esp_err_t webui_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    cfg.core_id = TASK_CORE_NET;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd");

    static const httpd_uri_t routes[] = {
        { .uri = "/",                    .method = HTTP_GET,  .handler = root_get },
        { .uri = "/api/state",           .method = HTTP_GET,  .handler = state_get },
        { .uri = "/api/wifi/scan",       .method = HTTP_GET,  .handler = wifi_scan_get },
        { .uri = "/api/discover",        .method = HTTP_GET,  .handler = discover_get },
        { .uri = "/api/station/play",    .method = HTTP_POST, .handler = station_play_post },
        { .uri = "/api/station/add",     .method = HTTP_POST, .handler = station_add_post },
        { .uri = "/api/station/delete",  .method = HTTP_POST, .handler = station_delete_post },
        { .uri = "/api/station/default", .method = HTTP_POST, .handler = station_default_post },
        { .uri = "/api/station/gain",    .method = HTTP_POST, .handler = station_gain_post },
        { .uri = "/api/station/move",    .method = HTTP_POST, .handler = station_move_post },
        { .uri = "/api/stop",            .method = HTTP_POST, .handler = stop_post },
        { .uri = "/api/rf",              .method = HTTP_POST, .handler = rf_post },
        { .uri = "/api/tone",            .method = HTTP_POST, .handler = tone_post },
        { .uri = "/api/wifi/add",        .method = HTTP_POST, .handler = wifi_add_post },
        { .uri = "/api/reboot",          .method = HTTP_POST, .handler = reboot_post },
        { .uri = "/api/hostname",        .method = HTTP_POST, .handler = hostname_post },
        { .uri = "/api/web",             .method = HTTP_POST, .handler = web_post },
        { .uri = "/api/factory/reset",   .method = HTTP_POST, .handler = factory_reset_post },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]),
                            TAG, "route %s", routes[i].uri);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found);

    /* Captive-portal detection hammers this server: a phone or a browser sitting
     * on the AMTX-Setup AP probes /generate_204, /hotspot-detect.html or
     * /canonical.html every couple of seconds until it sees real internet, and
     * every one of those logs a warning from httpd_uri. They are all answered
     * correctly by not_found() above, which 302s them to the portal page, so the
     * warnings are noise that buries the log the operator actually needs. The
     * route table is fixed and single-page, so nothing useful is being hidden. */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);

    wifi_conn_advertise_http(true);
    ESP_LOGI(TAG, "web interface up on port %d", cfg.server_port);
    return ESP_OK;
}

void webui_stop(void)
{
    wifi_conn_advertise_http(false);
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "web interface stopped");
}

bool webui_running(void)
{
    return s_server != NULL;
}
