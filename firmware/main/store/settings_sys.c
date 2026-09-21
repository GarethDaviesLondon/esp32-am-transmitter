/* settings_sys.c: the `sys` NVS namespace.
 *
 * The hostname the board answers to, and whether the web interface runs at
 * all. Its own file because `sys` is one of the four namespaces in design 04
 * section 4, and because these two are the only settings that change how the
 * board is reached rather than what it transmits: design 07 sections 3 and 4
 * are the contract for both.
 *
 * Both keys are absent on a fresh board, and absent means the default:
 * AMTX_HOSTNAME, and the web on.
 */

#include "settings_internal.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"

/* Same tag as the other settings_*.c files: see settings_stations.c. */
static const char *TAG = "settings";

static char s_hostname[HOSTNAME_MAX] = AMTX_HOSTNAME;
static bool s_web_on = true;

/* Checks a hostname against the rules in settings.h and writes it lower case
 * into `out`, a HOSTNAME_MAX buffer. Leaves `out` untouched and returns false
 * if it breaks them. The rules are RFC 1123's for one label, less the length:
 * mDNS allows 63, but a name that long is no easier to type into a browser. */
static bool hostname_normalise(const char *name, char *out)
{
    const size_t n = name ? strlen(name) : 0;
    if (n == 0 || n >= HOSTNAME_MAX) return false;
    if (name[0] == '-' || name[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && c != '-') return false;
    }

    for (size_t i = 0; i <= n; i++) out[i] = (char)tolower((unsigned char)name[i]);
    return true;
}

/* ------------------------------------------------------------- persistence */

void settings_sys_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_SYS, NVS_READONLY, &h) != ESP_OK) return;

    char host[HOSTNAME_MAX];
    size_t len = sizeof host;
    /* Re-validated on load: a name that would not be accepted today is not
     * one to hand to mDNS, so it falls back to the default instead. */
    if (nvs_get_str(h, "host", host, &len) == ESP_OK &&
        !hostname_normalise(host, s_hostname)) {
        ESP_LOGW(TAG, "saved hostname \"%s\" is not valid, using %s", host,
                 AMTX_HOSTNAME);
    }

    uint8_t b;
    if (nvs_get_u8(h, "web", &b) == ESP_OK) s_web_on = (b != 0);
    nvs_close(h);
}

/* Writes one key in the sys namespace, a string or a u8. Returns the NVS error
 * rather than swallowing it: a hostname or web setting that silently fails to
 * persist comes back wrong after the next power cut. */
static esp_err_t sys_save(const char *key, const char *str, uint8_t u8)
{
    carrier_suspend();                  /* KI-07, settings_internal.h */

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_SYS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = str ? nvs_set_str(h, key, str) : nvs_set_u8(h, key, u8);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }

    carrier_resume();
    return err;
}

/* --------------------------------------------------------------------- API */

const char *settings_hostname(void) { return s_hostname; }

esp_err_t settings_set_hostname(const char *name)
{
    char next[HOSTNAME_MAX];
    if (!hostname_normalise(name, next)) return ESP_ERR_INVALID_ARG;
    if (strcmp(next, s_hostname) == 0) return ESP_OK;

    snprintf(s_hostname, sizeof s_hostname, "%s", next);
    return sys_save("host", s_hostname, 0);
}

bool settings_web_enabled(void) { return s_web_on; }

esp_err_t settings_set_web_enabled(bool on)
{
    if (on == s_web_on) return ESP_OK;
    s_web_on = on;
    return sys_save("web", NULL, on ? 1 : 0);
}
