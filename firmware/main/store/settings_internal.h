/* settings_internal.h: what the four settings_*.c files share, and nothing a
 * caller outside store/ should ever see.
 *
 * settings.h is the public header and its API does not change; this one exists
 * only because the store was split one file per NVS namespace, the way design
 * 04 section 4 already documents the schema. What is shared between those files
 * is small and all of it is a rule rather than a convenience: the namespace
 * names, the key-numbering helpers, and above all the carrier bracket below.
 *
 * ==========================================================================
 * THE CARRIER BRACKET (KI-07). Every NVS write in store/ must be wrapped:
 *
 *     carrier_suspend();
 *     ... nvs_open / nvs_set_* / nvs_commit / nvs_close ...
 *     carrier_resume();
 *
 * A flash erase disables the instruction cache on both cores. A 20 kHz
 * interrupt taken through that window fetches from cache-disabled flash and
 * trips the interrupt watchdog before the write lands. That is why a
 * credential saved from the setup portal never survived the reboot: the board
 * reset mid-write. The bracket is not an optimisation and not a nicety; a new
 * write path without it reintroduces the bug.
 * ==========================================================================
 */
#ifndef AMTX_SETTINGS_INTERNAL_H
#define AMTX_SETTINGS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "am_config.h"
#include "nvs.h"
#include "settings.h"

/* For carrier_suspend()/carrier_resume(). Included here rather than in each
 * settings_*.c so the bracket rule above and the declarations it needs arrive
 * together. */
#include "rf/carrier.h"

/* The four namespaces, one per settings_*.c. NVS keys are 15 characters or
 * fewer; the schema table in docs/design/04_web_and_storage.md is the
 * contract, and a new key is a change to that table in the same commit. */
#define NS_RADIO "radio"
#define NS_WIFI  "wifi"
#define NS_RF    "rf"
#define NS_SYS   "sys"

static inline bool chain_ok(int ch) { return ch >= 0 && ch < RF_CHAINS; }

/* THE CHAIN KEY RULE. "last" and "def" for chain 0, "last1" and "def1" for
 * chain 1. Chain 0 keeps the original unsuffixed NVS keys so a board upgraded
 * from a single-carrier build finds its settings where it left them; chain 1
 * and above take a numeric suffix. This function is the one place that rule
 * lives, and design 04 section 4 says so by name. */
static inline void chain_key(char *out, size_t len, const char *base, int ch)
{
    if (ch == 0) {
        snprintf(out, len, "%s", base);
    } else {
        snprintf(out, len, "%s%d", base, ch);
    }
}

/* An indexed key, "<prefix><i>", for the list-shaped namespaces: stations are
 * "na0"/"ur0"/"tr0" upward, credentials "s0"/"p0".
 *
 * nvs_put_str_at() drops the write error. What saves it is the read side: a
 * load only counts an entry whose name key actually came back, so a short list
 * loads short rather than loading garbage. It still means a failed save is
 * silent until the next boot, which is not what the house rule on esp_err_t
 * asks for. */
static inline void nvs_put_str_at(nvs_handle_t h, const char *prefix, int i,
                                  const char *value)
{
    char key[16];
    snprintf(key, sizeof key, "%s%d", prefix, i);
    nvs_set_str(h, key, value);
}

static inline bool nvs_get_str_at(nvs_handle_t h, const char *prefix, int i,
                                  char *out, size_t out_len)
{
    char key[16];
    snprintf(key, sizeof key, "%s%d", prefix, i);
    size_t len = out_len;
    return nvs_get_str(h, key, out, &len) == ESP_OK;
}

/* ------------------------------------------------- load order, see settings.c
 *
 * One per namespace, called once from settings_init() in this order. Each
 * fills its own file's RAM cache from NVS and falls back to the compiled-in
 * defaults; none of them depends on another having run. */
void settings_stations_load(void);
void settings_wifi_load(void);
void settings_rf_load(void);
void settings_sys_load(void);

#endif /* AMTX_SETTINGS_INTERNAL_H */
