/* cmd_system.h: the console commands that report on or restart the board.
 *
 * `status`, `state`, `log` and `reboot`. Its own module because these are the
 * commands about the board rather than about the radio or the network: they
 * are what a script polls and what a person reaches for when something looks
 * wrong, and none of them touches app.c.
 *
 * The group publishes its command table rather than a register-me function, so
 * console.c keeps one registration loop with one error path and the help text
 * stays next to the command it describes.
 */
#ifndef AMTX_CMD_SYSTEM_H
#define AMTX_CMD_SYSTEM_H

#include <stddef.h>

#include "esp_console.h"

extern const esp_console_cmd_t cmd_system_table[];
extern const size_t cmd_system_count;

/* Not static, unlike every other command function here, because `wifi status`
 * prints the `status` line and must print exactly that line. Calling this is
 * how it stays exactly that line: see cmd_net.c. */
int cmd_status(int argc, char **argv);

#endif /* AMTX_CMD_SYSTEM_H */
