/* cmd_net.h: the console commands that decide how the board is reached.
 *
 * `wifi`, `hostname` and `web`. Its own module because these are the commands
 * that matter when the network is the thing that is broken, which is the whole
 * reason the console exists (console.h), and because `web off` leaves this
 * console as the only way in at all (design 07 section 4).
 *
 * The group publishes its command table rather than a register-me function, so
 * console.c keeps one registration loop with one error path and the help text
 * stays next to the command it describes.
 */
#ifndef AMTX_CMD_NET_H
#define AMTX_CMD_NET_H

#include <stddef.h>

#include "esp_console.h"

extern const esp_console_cmd_t cmd_net_table[];
extern const size_t cmd_net_count;

#endif /* AMTX_CMD_NET_H */
