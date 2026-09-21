/* wifi_conn.c: see wifi_conn.h. */

#include "wifi_conn.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "am_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "store/settings.h"

static const char *TAG = "wifi";

#define BIT_GOT_IP    BIT0
#define BIT_JOIN_FAIL BIT1

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

static volatile wifi_conn_state_t s_state = WIFI_MODE_OFF;
static char s_ssid[33];
static char s_ip[16] = "0.0.0.0";
static volatile uint32_t s_disconnects;
static volatile bool s_want_reconnect;

/* A first join gets this many attempts inside wifi_conn_try()'s timeout when
 * the failure is one a retry can fix. A router that misses one authentication
 * frame answers the next; giving up on the first was KI-09. */
#define JOIN_ATTEMPTS 4
#define JOIN_RETRY_US 1000000

static volatile uint8_t s_last_reason;
static volatile int s_join_attempt;

static bool s_mdns_up;
static bool s_http_advert;

/* ------------------------------------------------------------- reasons */

const char *wifi_conn_reason_text(uint8_t reason)
{
    switch (reason) {
    case 0:                                   return "no failure recorded";
    case WIFI_REASON_AUTH_EXPIRE:             return "router did not answer authentication: MAC filter, or weak signal";
    case WIFI_REASON_ASSOC_EXPIRE:            return "router did not answer association: weak signal";
    case WIFI_REASON_ASSOC_TOOMANY:           return "router has too many devices connected";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:       return "handshake timed out: wrong password";
    case WIFI_REASON_MIC_FAILURE:             return "key check failed: wrong password";
    case WIFI_REASON_802_1X_AUTH_FAILED:      return "enterprise (802.1X) network: not supported";
    case WIFI_REASON_BEACON_TIMEOUT:          return "lost the router's beacons: weak signal";
    case WIFI_REASON_NO_AP_FOUND:             return "no network with that name: check spelling and case";
    case WIFI_REASON_AUTH_FAIL:               return "authentication rejected: wrong password or blocked device";
    case WIFI_REASON_ASSOC_FAIL:              return "association rejected by the router";
    case WIFI_REASON_CONNECTION_FAIL:         return "connection failed";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
                                              return "network security not supported (WPA3-only, or open when a password was given)";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
                                              return "network signal too weak";
    default:                                  return "see esp_wifi_types_generic.h";
    }
}

/* Failures a second try can fix: a lost frame, a missed probe response. A
 * wrong password or an unsupported security mode fails the same way every
 * time, so those end the join at once rather than costing the full timeout. */
static bool join_reason_is_transient(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_EXPIRE:
    case WIFI_REASON_BEACON_TIMEOUT:
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
        return true;
    default:
        return false;
    }
}

uint8_t wifi_conn_last_reason(void)
{
    return s_last_reason;
}

/* ---------------------------------------------------------------- events */

static esp_timer_handle_t s_retry_timer;

static void on_retry_timer(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

/* ------------------------------------------------------------- heartbeat */

static esp_timer_handle_t s_beat_timer;

/* One line, every AMTX_WIFI_HEARTBEAT_S seconds, answering the only question
 * that matters while setting the board up. Deliberately logged whether or not
 * anything has changed: a periodic line that stops appearing is itself a
 * diagnostic, and a stream of DNS failures is not evidence either way about
 * whether the radio is actually associated.
 *
 * Runs on the esp_timer task, so it must not block. Everything it reads is
 * either a file-static or a cached driver value. */
static void on_heartbeat(void *arg)
{
    (void)arg;

    switch (s_state) {
    case WIFI_MODE_JOINED: {
        wifi_ap_record_t ap;
        const bool have_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        ESP_LOGI(TAG,
                 "wifi up: connected to \"%s\", ip %s, rssi %d dBm, drops %" PRIu32,
                 s_ssid, s_ip, have_ap ? ap.rssi : 0, s_disconnects);
        break;
    }
    case WIFI_MODE_JOINING:
        ESP_LOGW(TAG, "wifi down: still trying to join \"%s\", no ip yet", s_ssid);
        break;

    case WIFI_MODE_PORTAL: {
        esp_netif_ip_info_t info = { 0 };
        if (s_ap_netif) esp_netif_get_ip_info(s_ap_netif, &info);
        ESP_LOGW(TAG,
                 "wifi down: no network joined, " AMTX_AP_SSID
                 " setup portal is up at " IPSTR " - join it to configure",
                 IP2STR(&info.ip));
        break;
    }
    default:
        ESP_LOGW(TAG, "wifi down: radio idle, no network joined, drops %" PRIu32,
                 s_disconnects);
        break;
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = data;
        s_disconnects++;
        snprintf(s_ip, sizeof s_ip, "0.0.0.0");

        if (s_want_reconnect) {
            /* Already joined once: this is a drop, so keep trying forever
             * rather than falling back to the portal and taking the radio off
             * the air. The retry is deferred to a one-shot timer: sleeping
             * here would sleep the default event loop task, and every other
             * event in the system queues behind it. */
            ESP_LOGW(TAG, "disconnected (reason %d), retrying in 2 s",
                     e->reason);
            s_state = WIFI_MODE_JOINING;
            esp_timer_start_once(s_retry_timer, 2000000);
        } else {
            s_last_reason = e->reason;
            if (join_reason_is_transient(e->reason) && s_join_attempt < JOIN_ATTEMPTS) {
                ESP_LOGW(TAG, "join attempt %d of %d failed (reason %d: %s), retrying",
                         s_join_attempt, JOIN_ATTEMPTS, e->reason,
                         wifi_conn_reason_text(e->reason));
                s_join_attempt++;
                esp_timer_start_once(s_retry_timer, JOIN_RETRY_US);
            } else {
                ESP_LOGW(TAG, "join failed (reason %d: %s)", e->reason,
                         wifi_conn_reason_text(e->reason));
                xEventGroupSetBits(s_events, BIT_JOIN_FAIL);
            }
        }
        break;
    }

    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;

    const ip_event_got_ip_t *e = data;
    snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
    s_state = WIFI_MODE_JOINED;
    s_want_reconnect = true;
    ESP_LOGI(TAG, "joined %s, IP %s", s_ssid, s_ip);
    xEventGroupSetBits(s_events, BIT_GOT_IP);
}

/* ------------------------------------------------------- captive DNS ---- */

/* Answers every A query with the SoftAP's own address, which is what makes a
 * phone pop the setup page up by itself. Deliberately minimal: it parses just
 * enough of the query to echo it back, and it only ever runs while the portal
 * is up. */
static void dns_portal_task(void *arg)
{
    (void)arg;

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "captive DNS socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        ESP_LOGE(TAG, "captive DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[256];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof from;
        const int n = (int)recvfrom(sock, buf, sizeof buf, 0,
                                    (struct sockaddr *)&from, &from_len);
        /* Need at least a header and one question, and room to append the
         * 16-byte answer record. */
        if (n < 12 + 5 || n + 16 > (int)sizeof buf) continue;

        buf[2] |= 0x80;          /* QR: this is a response */
        buf[3] = 0x00;           /* no error, not authoritative */
        buf[6] = 0; buf[7] = 1;  /* one answer */
        buf[8] = 0; buf[9] = 0;  /* no authority records */
        buf[10] = 0; buf[11] = 0;

        int p = n;
        buf[p++] = 0xC0; buf[p++] = 0x0C;         /* pointer to the question */
        buf[p++] = 0x00; buf[p++] = 0x01;         /* type A */
        buf[p++] = 0x00; buf[p++] = 0x01;         /* class IN */
        buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 30;   /* TTL */
        buf[p++] = 0x00; buf[p++] = 0x04;         /* 4 bytes of address */
        buf[p++] = 192; buf[p++] = 168; buf[p++] = 4; buf[p++] = 1;

        sendto(sock, buf, (size_t)p, 0, (struct sockaddr *)&from, from_len);
    }
}

/* ------------------------------------------------------------- portal ---- */

static void start_portal(void)
{
    ESP_LOGW(TAG, "no network joined: bringing up the %s setup portal",
             AMTX_AP_SSID);

    esp_wifi_stop();
    /* APSTA, not AP: a scan needs the station interface, and the whole point
     * of the portal is to let someone pick a network from a list. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid, "%s", AMTX_AP_SSID);
    ap.ap.ssid_len = (uint8_t)strlen(AMTX_AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    /* Open on purpose: there is no screen to show a password on, and the
     * portal is only up when the board cannot reach a network anyway. */
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    snprintf(s_ip, sizeof s_ip, "192.168.4.1");
    snprintf(s_ssid, sizeof s_ssid, "%s", AMTX_AP_SSID);
    s_state = WIFI_MODE_PORTAL;

    xTaskCreatePinnedToCore(dns_portal_task, "dnsportal", 3072, NULL, 4, NULL,
                            TASK_CORE_NET);
}

/* --------------------------------------------------------------- mDNS ---- */

/* So the operator can find the board without hunting for its address; there
 * is no display to read one off. Started on the first successful join from
 * any path, the boot walk or the console's `wifi join`, since a board that
 * joined from the console is as hard to find as one that joined at boot. Runs
 * on the joining task, never the event loop. */
static void mdns_up(void)
{
    if (s_mdns_up) return;
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS did not start: use the IP address");
        return;
    }
    s_mdns_up = true;
    mdns_hostname_set(settings_hostname());
    mdns_instance_name_set("ESP32-S3 AM Transmitter");
    wifi_conn_advertise_http(s_http_advert);
    ESP_LOGI(TAG, "reachable at %s%s.local", s_http_advert ? "http://" : "",
             settings_hostname());
}

void wifi_conn_advertise_http(bool on)
{
    s_http_advert = on;
    if (!s_mdns_up) return;

    const bool there = mdns_service_exists("_http", "_tcp", NULL);
    if (on && !there) {
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else if (!on && there) {
        mdns_service_remove("_http", "_tcp");
    }
}

void wifi_conn_apply_hostname(void)
{
    const char *host = settings_hostname();
    /* The DHCP client sends this with its next request, which is the next
     * lease or reconnect, so the router's device list lags until then. */
    esp_netif_set_hostname(s_sta_netif, host);
    if (s_mdns_up) mdns_hostname_set(host);
    ESP_LOGI(TAG, "hostname %s", host);
}

/* ------------------------------------------------------------------- API */

esp_err_t wifi_conn_init(void)
{
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    esp_netif_set_hostname(s_sta_netif, settings_hostname());

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL));

    /* Keep the radio out of power-save: the modulator does not care, but a
     * stream that stalls every beacon interval does. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    const esp_timer_create_args_t retry = {
        .callback = on_retry_timer,
        .name = "wifiretry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry, &s_retry_timer));

    if (AMTX_WIFI_HEARTBEAT_S > 0) {
        const esp_timer_create_args_t beat = {
            .callback = on_heartbeat,
            .name = "wifibeat",
        };
        ESP_ERROR_CHECK(esp_timer_create(&beat, &s_beat_timer));
        /* Started here rather than after a successful join, so the "not
         * connected" case reports itself too. That is the case the operator
         * needs told about. */
        ESP_ERROR_CHECK(esp_timer_start_periodic(
            s_beat_timer, (uint64_t)AMTX_WIFI_HEARTBEAT_S * 1000000ULL));
    }
    return ESP_OK;
}

esp_err_t wifi_conn_try(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "trying %s", ssid);

    s_want_reconnect = false;
    s_last_reason = 0;
    s_join_attempt = 1;
    (void)esp_timer_stop(s_retry_timer);   /* not running is fine */
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_JOIN_FAIL);

    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta = { 0 };
    snprintf((char *)sta.sta.ssid, sizeof sta.sta.ssid, "%s", ssid);
    snprintf((char *)sta.sta.password, sizeof sta.sta.password, "%s",
             pass ? pass : "");
    sta.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA_PSK
                                                   : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    snprintf(s_ssid, sizeof s_ssid, "%s", ssid);
    s_state = WIFI_MODE_JOINING;

    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    const EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_GOT_IP | BIT_JOIN_FAIL, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    /* A retry still pending must not reconnect behind the caller's back. */
    (void)esp_timer_stop(s_retry_timer);   /* not running is fine */
    if (bits & BIT_GOT_IP) {
        mdns_up();
        return ESP_OK;
    }

    s_state = WIFI_MODE_OFF;
    return (bits & BIT_JOIN_FAIL) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

void wifi_conn_start(bool portal)
{
    const int n = settings_wifi_count();
    for (int i = 0; i < n; i++) {
        char ssid[33], pass[65];
        if (!settings_wifi_get(i, ssid, sizeof ssid, pass, sizeof pass)) continue;
        if (wifi_conn_try(ssid, pass, 15000) == ESP_OK) return;
    }

    if (portal) {
        start_portal();
        return;
    }
    ESP_LOGW(TAG, "no network joined, and the web interface is off so there is "
                  "no setup portal: join one from this console with "
                  "'wifi join <ssid> <password>'");
}

int wifi_conn_scan(wifi_scan_entry_t *out, int max)
{
    /* Scanning while joined drops the stream for a moment; that is the
     * bargain the operator makes by opening the network page. */
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) return 0;

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) return 0;
    if (found > 32) found = 32;

    wifi_ap_record_t *recs = calloc(found, sizeof *recs);
    if (!recs) return 0;
    esp_wifi_scan_get_ap_records(&found, recs);

    int n = 0;
    for (int i = 0; i < found && n < max; i++) {
        if (recs[i].ssid[0] == '\0') continue;

        /* Keep the strongest sighting of each SSID; mesh networks show the
         * same name several times. */
        bool dup = false;
        for (int k = 0; k < n; k++) {
            if (strcmp(out[k].ssid, (const char *)recs[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        snprintf(out[n].ssid, sizeof out[n].ssid, "%s", (const char *)recs[i].ssid);
        out[n].rssi = recs[i].rssi;
        out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        n++;
    }
    free(recs);
    return n;
}

void wifi_conn_get_status(wifi_conn_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->state = s_state;
    snprintf(out->ssid, sizeof out->ssid, "%s", s_ssid);
    snprintf(out->ip, sizeof out->ip, "%s", s_ip);
    out->disconnects = s_disconnects;

    wifi_ap_record_t ap;
    if (s_state == WIFI_MODE_JOINED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->rssi = ap.rssi;
    }
}

bool wifi_conn_is_online(void)
{
    return s_state == WIFI_MODE_JOINED;
}
