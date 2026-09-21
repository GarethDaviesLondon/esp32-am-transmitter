/* settings_wifi.c: the `wifi` NVS namespace.
 *
 * The saved network credentials, most recently chosen first. Its own file
 * because `wifi` is one of the four namespaces in design 04 section 4, and
 * because this is the one place in the firmware that holds a password: keeping
 * it in a file of its own makes it obvious where that is, and that nothing
 * else in store/ reads it.
 *
 * Nothing here ever prints a password, and none of it belongs in a commit:
 * a credential reaches the board through the setup portal, the console, or
 * menuconfig into a gitignored sdkconfig.
 */

#include "settings_internal.h"

#include <string.h>

#include "esp_log.h"

/* Same tag as the other settings_*.c files: see settings_stations.c. */
static const char *TAG = "settings";

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_cred_t;

static wifi_cred_t s_creds[WIFI_CRED_MAX];
static int s_cred_count;

/* ------------------------------------------------------------- persistence */

static void wifi_save(void)
{
    carrier_suspend();                  /* KI-07, settings_internal.h */

    nvs_handle_t h;
    if (nvs_open(NS_WIFI, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "n", s_cred_count);
        for (int i = 0; i < s_cred_count; i++) {
            nvs_put_str_at(h, "s", i, s_creds[i].ssid);
            nvs_put_str_at(h, "p", i, s_creds[i].pass);
        }
        nvs_commit(h);
        nvs_close(h);
    }

    carrier_resume();
}

void settings_wifi_load(void)
{
    nvs_handle_t h;
    s_cred_count = 0;

    if (nvs_open(NS_WIFI, NVS_READONLY, &h) == ESP_OK) {
        int32_t n = 0;
        nvs_get_i32(h, "n", &n);
        if (n < 0) n = 0;
        if (n > WIFI_CRED_MAX) n = WIFI_CRED_MAX;

        for (int i = 0; i < n; i++) {
            wifi_cred_t *c = &s_creds[s_cred_count];
            memset(c, 0, sizeof *c);
            if (nvs_get_str_at(h, "s", i, c->ssid, sizeof c->ssid)) {
                nvs_get_str_at(h, "p", i, c->pass, sizeof c->pass);
                s_cred_count++;
            }
        }
        nvs_close(h);
    }

    /* A credential compiled in via menuconfig is a convenience for bench
     * work, not a substitute for the setup portal. It is only used if NVS
     * holds nothing. */
    if (s_cred_count == 0 && WIFI_SSID[0] != '\0') {
        snprintf(s_creds[0].ssid, sizeof s_creds[0].ssid, "%s", WIFI_SSID);
        snprintf(s_creds[0].pass, sizeof s_creds[0].pass, "%s", WIFI_PASS);
        s_cred_count = 1;
        ESP_LOGI(TAG, "using the credential compiled in via menuconfig");
    }
}

/* --------------------------------------------------------------------- API */

int settings_wifi_count(void) { return s_cred_count; }

bool settings_wifi_get(int idx, char *ssid, size_t ssid_len,
                       char *pass, size_t pass_len)
{
    if (idx < 0 || idx >= s_cred_count) return false;
    if (ssid) snprintf(ssid, ssid_len, "%s", s_creds[idx].ssid);
    if (pass) snprintf(pass, pass_len, "%s", s_creds[idx].pass);
    return true;
}

esp_err_t settings_wifi_add(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!pass) pass = "";

    /* Known SSID: update the password in place and promote it. */
    int found = -1;
    for (int i = 0; i < s_cred_count; i++) {
        if (strcmp(s_creds[i].ssid, ssid) == 0) { found = i; break; }
    }
    if (found < 0) {
        if (s_cred_count >= WIFI_CRED_MAX) {
            /* Drop the least recently chosen, at the end of the list. */
            s_cred_count = WIFI_CRED_MAX - 1;
        }
        found = s_cred_count++;
    }

    memset(&s_creds[found], 0, sizeof s_creds[found]);
    snprintf(s_creds[found].ssid, sizeof s_creds[found].ssid, "%s", ssid);
    snprintf(s_creds[found].pass, sizeof s_creds[found].pass, "%s", pass);

    /* Front of the list: the operator just picked this one, so try it first
     * next boot. */
    const wifi_cred_t chosen = s_creds[found];
    for (int i = found; i > 0; i--) s_creds[i] = s_creds[i - 1];
    s_creds[0] = chosen;

    wifi_save();
    return ESP_OK;
}

esp_err_t settings_wifi_delete(int idx)
{
    if (idx < 0 || idx >= s_cred_count) return ESP_ERR_INVALID_ARG;
    for (int i = idx; i < s_cred_count - 1; i++) s_creds[i] = s_creds[i + 1];
    s_cred_count--;
    wifi_save();
    return ESP_OK;
}
