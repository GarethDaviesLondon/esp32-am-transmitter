/* main.c: bring-up order, and nothing else.
 *
 * The order matters more than it looks:
 *
 *   1. NVS, so everything below can read its saved settings.
 *   2. Wi-Fi, which blocks until a network joins or the setup portal is up.
 *   3. The web interface, on whichever of the two is running, unless the
 *      operator has turned it off. Then there is no portal either, and the
 *      serial console, started before step 2, is the way in (design 07 §4).
 *   4. The audio and RF stack, started from a task pinned to core 1. The
 *      modulation ISR is allocated on the core that registers it, and the
 *      sample FIFO's lockless contract assumes the decode task and the ISR
 *      share a core, so this cannot be done from app_main (core 0).
 */

#include "am_config.h"
#include "app.h"
#include "button.h"
#include "cli/console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/webui.h"
#include "net/wifi_conn.h"
#include "store/settings.h"

static const char *TAG = "amtx";

static void audio_startup_task(void *arg)
{
    (void)arg;

    const esp_err_t err = app_init();
    if (err != ESP_OK) {
        /* The web UI is already up on core 0, so the operator can still see
         * the board and read the log rather than facing a silent brick. */
        ESP_LOGE(TAG, "audio and RF stack failed to start: %s",
                 esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 AM Transmitter");
    ESP_LOGI(TAG, "carrier GPIO %d, %d Hz nominal, %d Hz modulation rate",
             RF_CARRIER_GPIO, RF_CARRIER_HZ, AM_SAMPLE_RATE_HZ);
    ESP_LOGW(TAG, "this radiates in a licensed broadcast band: keep the field "
                  "inside the room and fit the output filter (see "
                  "docs/design/03_hardware.md)");

    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(wifi_conn_init());

    /* Before wifi_conn_start(), which blocks: the console is the one way in
     * when the network is the thing that is broken, so it must not wait on
     * the network coming up. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(console_start());

    /* Before it too, and for the same reason. A board whose saved network no
     * longer exists spends 15 seconds per credential in wifi_conn_start(), and
     * the button that rescues it should work during that wait rather than
     * after it (design 07 §7). */
    ESP_ERROR_CHECK_WITHOUT_ABORT(button_start());

    /* Blocks: walks the saved networks, then falls back to the setup AP if
     * the web interface is on to serve it. */
    const bool web = settings_web_enabled();
    wifi_conn_start(web);

    if (web) {
        ESP_ERROR_CHECK(webui_start());
    } else {
        ESP_LOGW(TAG, "web interface is off: use the serial console, and "
                      "'web on' to bring the page back");
    }

    xTaskCreatePinnedToCore(audio_startup_task, "audiostart", 4096, NULL, 5,
                            NULL, TASK_CORE_AUDIO);
}
