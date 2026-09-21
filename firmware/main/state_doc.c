/* state_doc.c: see state_doc.h. */

#include "state_doc.h"

#include "am_config.h"
#include "app.h"
#include "button.h"
#include "audio/decoder.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "net/stream.h"
#include "net/wifi_conn.h"
#include "rf/carrier.h"
#include "store/settings.h"

static const char *stream_state_name(stream_state_t s)
{
    switch (s) {
    case STREAM_CONNECTING: return "connecting";
    case STREAM_PLAYING:    return "playing";
    case STREAM_RETRYING:   return "reconnecting";
    case STREAM_FAILED:     return "failed";
    default:                return "stopped";
    }
}

static const char *wifi_state_name(wifi_conn_state_t s)
{
    switch (s) {
    case WIFI_MODE_JOINING: return "joining";
    case WIFI_MODE_JOINED:  return "connected";
    case WIFI_MODE_PORTAL:  return "setup portal";
    default:                return "offline";
    }
}

/* One chain's slice of the state document. The page polls this once a second
 * and redraws, so everything the operator can see about a transmitter has to
 * be in here. */
static void add_chain(cJSON *arr, int ch)
{
    stream_stats_t ss;
    decoder_stats_t ds;
    carrier_stats_t cs;
    rf_settings_t rf;

    stream_get_stats(ch, &ss);
    decoder_get_stats(ch, &ds);
    carrier_get_stats(ch, &cs);
    app_get_rf(ch, &rf);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "ch", ch);

    const int current = app_current_index(ch);
    station_t st;
    const bool have_station = settings_station_get(current, &st);

    cJSON *play = cJSON_AddObjectToObject(doc, "play");
    cJSON_AddNumberToObject(play, "index", current);
    cJSON_AddStringToObject(play, "name",
                            have_station ? st.name
                                         : (ss.url[0] ? "Direct URL" : ""));
    cJSON_AddStringToObject(play, "url", ss.url);
    cJSON_AddStringToObject(play, "state",
                            decoder_tone_active(ch)
                                ? "test tone"
                                : stream_state_name(ss.state));

    cJSON *rfj = cJSON_AddObjectToObject(doc, "rf");
    cJSON_AddBoolToObject(rfj, "on", rf.rf_on);
    cJSON_AddNumberToObject(rfj, "hz", rf.carrier_hz);
    cJSON_AddNumberToObject(rfj, "depth", rf.depth_pct);
    cJSON_AddNumberToObject(rfj, "lpf", rf.lpf_hz);
    cJSON_AddNumberToObject(rfj, "gain", rf.gain_pct);
    cJSON_AddNumberToObject(rfj, "level", rf.level_pct);
    cJSON_AddNumberToObject(rfj, "levelnow", (int)(cs.level_now * 100.0f + 0.5f));
    cJSON_AddBoolToObject(rfj, "agc", rf.agc_on);
    cJSON_AddNumberToObject(rfj, "agct", rf.agc_target_pct);
    cJSON_AddNumberToObject(rfj, "fdm", rf.fade_mode);
    cJSON_AddNumberToObject(rfj, "fdr", rf.fade_rate_mhz);
    cJSON_AddNumberToObject(rfj, "fdd", rf.fade_depth_pct);
    cJSON_AddNumberToObject(rfj, "fifo", cs.fifo_count);
    cJSON_AddNumberToObject(rfj, "fifocap", cs.fifo_capacity);
    cJSON_AddNumberToObject(rfj, "underruns", cs.underruns);

    cJSON *audio = cJSON_AddObjectToObject(doc, "audio");
    cJSON_AddNumberToObject(audio, "peak", ds.peak);
    cJSON_AddNumberToObject(audio, "rate", ds.sample_rate);
    cJSON_AddNumberToObject(audio, "ch", ds.channels);
    cJSON_AddNumberToObject(audio, "kbps", ds.bitrate / 1000);
    cJSON_AddNumberToObject(audio, "frames", ds.frames);
    cJSON_AddNumberToObject(audio, "errors", ds.errors);
    cJSON_AddNumberToObject(audio, "resyncs", ds.resyncs);
    cJSON_AddNumberToObject(audio, "trim", ds.trim_ppm);
    cJSON_AddNumberToObject(audio, "agcdb", ds.agc_db);

    cJSON *stream = cJSON_AddObjectToObject(doc, "stream");
    cJSON_AddNumberToObject(stream, "buffered", ss.buffered);
    cJSON_AddNumberToObject(stream, "capacity", ss.capacity);
    cJSON_AddNumberToObject(stream, "status", ss.http_status);
    cJSON_AddNumberToObject(stream, "reconnects", ss.reconnects);
    cJSON_AddStringToObject(stream, "error", ss.error);

    cJSON_AddItemToArray(arr, doc);
}

cJSON *state_doc_build(void)
{
    wifi_conn_status_t ws;
    wifi_conn_get_status(&ws);

    cJSON *doc = cJSON_CreateObject();
    if (!doc) return NULL;

    cJSON *chains = cJSON_AddArrayToObject(doc, "chains");
    for (int ch = 0; ch < RF_CHAINS; ch++) add_chain(chains, ch);

    cJSON *net = cJSON_AddObjectToObject(doc, "net");
    cJSON_AddStringToObject(net, "state", wifi_state_name(ws.state));
    cJSON_AddStringToObject(net, "ssid", ws.ssid);
    cJSON_AddStringToObject(net, "ip", ws.ip);
    cJSON_AddNumberToObject(net, "rssi", ws.rssi);
    cJSON_AddNumberToObject(net, "drops", ws.disconnects);

    cJSON *sys = cJSON_AddObjectToObject(doc, "sys");
    cJSON_AddNumberToObject(sys, "heap", esp_get_free_heap_size());
    /* Internal RAM is the number that actually constrains anything. Total
     * free heap counts PSRAM and so reads as 8 MB no matter how close the
     * board is to failing a TLS handshake, which is exactly how the discovery
     * failure hid itself. */
    cJSON_AddNumberToObject(sys, "heapint",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(sys, "heapintmax",
                            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(sys, "uptime",
                            (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(sys, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(sys, "host", settings_hostname());
    /* True while the BOOT button is held. The only way to tell a board whose
     * button does not work from a person not pressing it hard enough, and the
     * only way this feature could be checked at all without hands on the
     * board. Design 07 section 7. */
    cJSON_AddBoolToObject(sys, "btn", button_pressed());
    /* The saved choice, which is also what is running: the console and the
     * page change both together. Always true when the page reads it. */
    cJSON_AddBoolToObject(sys, "web", settings_web_enabled());

    cJSON *list = cJSON_AddArrayToObject(doc, "stations");
    for (int i = 0; i < settings_station_count(); i++) {
        station_t s;
        if (!settings_station_get(i, &s)) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", s.name);
        cJSON_AddStringToObject(e, "url", s.url);
        cJSON_AddNumberToObject(e, "gain", s.gain_pct);
        /* One flag per chain: a station can be the power-on choice for
         * either transmitter, both, or neither. */
        cJSON *def = cJSON_AddArrayToObject(e, "def");
        for (int ch = 0; ch < RF_CHAINS; ch++) {
            cJSON_AddItemToArray(def,
                cJSON_CreateBool(i == settings_default_station(ch)));
        }
        cJSON_AddItemToArray(list, e);
    }

    return doc;
}
