/* settings.c: see settings.h.
 *
 * What is left here after the split is the part that belongs to no one
 * namespace: mounting NVS, and the order the four caches are loaded in. The
 * rest is one file per namespace, matching the schema table in design 04
 * section 4:
 *
 *   settings_stations.c   `radio`  the station list and the two indices into it
 *   settings_wifi.c       `wifi`   saved network credentials
 *   settings_rf.c         `rf`     per-transmitter carrier and audio settings
 *   settings_sys.c        `sys`    hostname, and whether the web runs
 *
 * settings_internal.h carries what they share, including the carrier bracket
 * every NVS write in store/ must be wrapped in (KI-07). settings.h stays the
 * one public header, and the API in it did not change with the split.
 */

#include "settings_internal.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

/* Same tag as the other settings_*.c files: this is one subsystem to anyone
 * reading the log, and splitting the source should not split the log. */
static const char *TAG = "settings";

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* A partition left over from a different NVS version, or one with no
         * free page, cannot be opened at all. Erasing loses the stations and
         * the credentials, which is bad; refusing to boot is worse. */
        ESP_LOGW(TAG, "NVS partition unusable, erasing it");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    /* The order is the order the caches are read in, not a dependency: each
     * load reads its own namespace and none of them looks at another's. Kept
     * as it was so the log line below, and any log line a load prints, appear
     * in the same order they always have. */
    settings_stations_load();
    settings_wifi_load();
    settings_rf_load();
    settings_sys_load();

    /* Through the public API rather than the caches, so this file needs no
     * view into any of the four. */
    ESP_LOGI(TAG, "%d station(s), %d saved network(s), hostname %s, web %s",
             settings_station_count(), settings_wifi_count(),
             settings_hostname(), settings_web_enabled() ? "on" : "off");
    return ESP_OK;
}

esp_err_t settings_factory_reset(void)
{
    /* The same bracket every other write here uses, and for the same reason:
     * erasing a partition is a long flash operation with the cache off, and a
     * 20 kHz interrupt taken through it trips the watchdog (KI-07). This one
     * erases everything rather than one key, so the window is the longest the
     * firmware ever opens. */
    carrier_suspend();

    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK) {
        /* Deliberately not re-initialising NVS: every cache in this module
         * still holds what was just erased, and a running board with stale
         * caches over an empty partition is a worse state than a restart. */
        ESP_LOGW(TAG, "NVS erased, restarting");
    } else {
        ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(err));
        carrier_resume();
        return err;
    }

    /* Long enough for that line to leave the console before the reset. */
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ESP_OK;   /* not reached */
}
