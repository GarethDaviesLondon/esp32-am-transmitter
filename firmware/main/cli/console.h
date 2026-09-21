/* console.h: a command line on the USB serial console.
 *
 * The board has no display and no buttons, and the web UI is unreachable
 * exactly when the thing that needs debugging is the network. This is the back
 * door: it runs whether or not Wi-Fi is up, and it can join a network directly
 * so a credential can be tested without going through the portal at all.
 *
 * Logging scrolls by default, because a silent board looks like a dead one.
 * `log off` stops it so the console can be driven by a script and polled with
 * `status`, which prints one parseable key=value line and nothing else.
 * Logging is on again after every reboot: the quiet mode is a debugging tool,
 * not a setting worth persisting.
 */
#ifndef AMTX_CONSOLE_H
#define AMTX_CONSOLE_H

#include "esp_err.h"

/* Starts the REPL task. Call after wifi_conn_init(), so the wifi commands
 * have a driver to talk to, and before wifi_conn_start(), so the console is
 * already answering while the join attempts are blocking app_main. */
esp_err_t console_start(void);

#endif /* AMTX_CONSOLE_H */
