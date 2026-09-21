/* wifi_conn.h: joining a network, and the SoftAP fallback when that fails.
 *
 * The board has no display and no buttons, so the setup portal is the only
 * way in on a fresh device: if no saved network can be joined, it brings up
 * an open access point called AMTX-Setup with a DNS responder that points
 * every lookup at itself, so a phone opens the setup page as a captive
 * portal. The same web server serves both the portal and the normal UI.
 */
#ifndef AMTX_WIFI_CONN_H
#define AMTX_WIFI_CONN_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WIFI_MODE_OFF = 0,
    WIFI_MODE_JOINING,
    WIFI_MODE_JOINED,
    WIFI_MODE_PORTAL,      /* SoftAP up, waiting for someone to configure it */
} wifi_conn_state_t;

typedef struct {
    wifi_conn_state_t state;
    char ssid[33];
    char ip[16];
    int8_t rssi;
    uint32_t disconnects;
} wifi_conn_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_scan_entry_t;

/* Brings up netif, the event loop and the Wi-Fi driver. Does not connect. */
esp_err_t wifi_conn_init(void);

/* Walks the saved credential list, newest first, giving each a few seconds.
 * If none joins, brings up the setup portal when `portal` is true, and stays
 * off the network when it is false: the portal is a web page, so with the web
 * interface turned off there is nothing for it to serve. Blocks until one of
 * those has happened. */
void wifi_conn_start(bool portal);

/* Try one network now, retrying failures a retry can fix, for up to
 * timeout_ms. ESP_OK once an IP is held; ESP_FAIL when the join failed for a
 * reason (see wifi_conn_last_reason()); ESP_ERR_TIMEOUT if nothing came back. */
esp_err_t wifi_conn_try(const char *ssid, const char *pass, uint32_t timeout_ms);

/* The 802.11 / ESP-IDF reason code of the last failed join attempt, 0 if none. */
uint8_t wifi_conn_last_reason(void);

/* A short operator-facing explanation of a reason code. Never NULL. */
const char *wifi_conn_reason_text(uint8_t reason);

/* Scan for networks. Returns how many entries were written. */
int wifi_conn_scan(wifi_scan_entry_t *out, int max);

void wifi_conn_get_status(wifi_conn_status_t *out);
bool wifi_conn_is_online(void);

/* Applies settings_hostname() now: the mDNS name at once, the DHCP hostname
 * from the next lease. Call after settings_set_hostname(). */
void wifi_conn_apply_hostname(void);

/* Whether mDNS advertises the web interface as _http._tcp. Kept in step with
 * the web server, so a board with the web off does not advertise a service
 * that is not there. Safe before mDNS is up: it is applied when mDNS starts. */
void wifi_conn_advertise_http(bool on);

#endif /* AMTX_WIFI_CONN_H */
