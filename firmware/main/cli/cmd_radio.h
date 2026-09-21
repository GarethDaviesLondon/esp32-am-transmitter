/* cmd_radio.h: the console commands that put something on air.
 *
 * `play`, `stop`, `station`, `rf`, `tone` and `discover`: everything that
 * chooses what a transmitter plays or how it modulates. Its own module because
 * it is the half of the console that talks to app.c and the RF chain, and
 * because `discover` brings a whole short-lived task with it (design 07
 * section 2) that has no business sitting next to `reboot`.
 *
 * The group publishes its command table rather than a register-me function, so
 * console.c keeps one registration loop with one error path and the help text
 * stays next to the command it describes.
 */
#ifndef AMTX_CMD_RADIO_H
#define AMTX_CMD_RADIO_H

#include <stddef.h>

#include "esp_console.h"

extern const esp_console_cmd_t cmd_radio_table[];
extern const size_t cmd_radio_count;

#endif /* AMTX_CMD_RADIO_H */
