/* button.c: see button.h. */

#include "button.h"

#include "am_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rf/carrier.h"
#include "store/settings.h"

static const char *TAG = "button";

/* Pressed is LOW: the button shorts the pin to ground, and the pin is pulled
 * up. The internal pull-up is enabled as well as the board's own, because a
 * floating strapping pin reads as whatever the last thing to touch it left
 * behind, and a floating LOW here would erase the board. */
#define PRESSED 0

/* Polled rather than interrupt-driven. The event this watches is five seconds
 * long, nothing here is time-critical, and an interrupt on a strapping pin is
 * a way to be woken by noise. 50 ms also debounces for free: a contact that
 * bounces for a millisecond or two is never sampled mid-bounce twice. */
#define POLL_MS 50

static volatile bool s_pressed;

bool button_pressed(void)
{
    return s_pressed;
}

/* The countdown, on the console, once a second. A board with no display has
 * to say what it is about to do while there is still time to let go. */
static void announce(int held_ms)
{
    const int remaining = (AMTX_FACTORY_HOLD_MS - held_ms + 999) / 1000;
    if (remaining > 0) {
        ESP_LOGW(TAG, "hold to factory reset: %d ...", remaining);
    }
}

static void factory_reset_now(void)
{
    /* Carriers first, and not only for tidiness: on a board with no screen the
     * transmission stopping is the confirmation that the press was long
     * enough. Someone listening on a receiver hears it. */
    for (int ch = 0; ch < RF_CHAINS; ch++) carrier_set_enabled(ch, false);

    ESP_LOGW(TAG, "factory reset: erasing everything saved, then restarting");
    const esp_err_t err = settings_factory_reset();
    if (err != ESP_OK) {
        /* Say so and carry on running rather than restarting into the same
         * state and looking like the button did nothing. */
        ESP_LOGE(TAG, "factory reset failed: %s", esp_err_to_name(err));
        for (int ch = 0; ch < RF_CHAINS; ch++) carrier_set_enabled(ch, true);
        return;
    }
    /* settings_factory_reset() does not return on success. */
}

static void button_task(void *arg)
{
    (void)arg;

    int held_ms = 0;
    int announced_at = -1;

    for (;;) {
        const bool down = gpio_get_level(AMTX_FACTORY_BUTTON_GPIO) == PRESSED;
        s_pressed = down;

        if (!down) {
            if (held_ms >= 1000) {
                ESP_LOGI(TAG, "released after %d.%d s: nothing done",
                         held_ms / 1000, (held_ms % 1000) / 100);
            }
            held_ms = 0;
            announced_at = -1;
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }

        held_ms += POLL_MS;
        if (held_ms / 1000 != announced_at) {
            announced_at = held_ms / 1000;
            announce(held_ms);
        }

        if (held_ms >= AMTX_FACTORY_HOLD_MS) {
            factory_reset_now();
            /* Only reached if the erase failed. Wait for the button to come
             * back up so a still-held button does not retry every tick. */
            held_ms = 0;
            announced_at = -1;
            while (gpio_get_level(AMTX_FACTORY_BUTTON_GPIO) == PRESSED) {
                vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t button_start(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << AMTX_FACTORY_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio %d", AMTX_FACTORY_BUTTON_GPIO);

    /* Core 0, with the network: this must never share a core with the
     * modulation ISR's scheduling, and it has nothing to do with audio. Low
     * priority, because a five second press can afford to wait its turn. */
    if (xTaskCreatePinnedToCore(button_task, "button", 2560, NULL, 2, NULL,
                                TASK_CORE_NET) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BOOT button on GPIO %d: hold %d s to factory reset",
             AMTX_FACTORY_BUTTON_GPIO, AMTX_FACTORY_HOLD_MS / 1000);
    return ESP_OK;
}
