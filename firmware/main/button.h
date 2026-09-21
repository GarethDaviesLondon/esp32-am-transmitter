/* button.h: the BOOT button, and the factory reset it triggers.
 *
 * The board has two buttons and only one of them can be read by firmware.
 * RESET is wired to the chip's enable pin, so nothing here will ever see it.
 * BOOT is GPIO 0, a strapping pin whose held-through-a-reset meaning belongs
 * to the ROM's download mode, which is why the gesture is a long press while
 * the board is RUNNING rather than anything at power-on. Design 07 section 7
 * is the contract.
 *
 * Held for AMTX_FACTORY_HOLD_MS, the board erases NVS and restarts. Released
 * early, nothing happens: the erase is the last thing before the restart, so
 * there is no half-reset state to recover from.
 */
#ifndef AMTX_BUTTON_H
#define AMTX_BUTTON_H

#include <stdbool.h>

#include "esp_err.h"

/* Configures GPIO 0 and starts the task that watches it. Safe to call before
 * the network is up; it touches nothing but the pin and, eventually, NVS. */
esp_err_t button_start(void);

/* True while BOOT is held. Reported as `sys.btn` in the state document, which
 * is the only way to tell a board whose button does not work from a person not
 * pressing it hard enough. */
bool button_pressed(void);

#endif /* AMTX_BUTTON_H */
