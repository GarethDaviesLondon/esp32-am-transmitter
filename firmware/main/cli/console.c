/* console.c: see console.h.
 *
 * What is left here after the split is the REPL itself: the device the console
 * runs on, the line length, and the one loop that registers every command. The
 * commands live in three modules grouped by what they act on:
 *
 *   cli_reply.{c,h}   the ok / error: convention and the shared parsers
 *   cmd_radio.c       play, stop, station, rf, tone, discover
 *   cmd_net.c         wifi, hostname, web
 *   cmd_system.c      status, state, log, reboot
 *
 * Each group publishes a table rather than a register-me function. That keeps
 * one registration loop with one error path here, keeps each command's help
 * string next to the command it describes, and still lets this file show the
 * whole command set at a glance. `help` prints in alphabetical order whatever
 * the registration order is -- esp_console keeps its list sorted -- so the
 * grouping cannot change what a program driving the console sees.
 */

#include "console.h"

#include <stddef.h>

#include "cmd_net.h"
#include "cmd_radio.h"
#include "cmd_system.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "cli";

/* Long enough for `station add` with a full-length name and URL, both quoted. */
#define CLI_LINE_MAX 320

/* ------------------------------------------------------------------- start */

esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;

    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "amtx>";
    rc.max_cmdline_length = CLI_LINE_MAX;
    /* 4 kB, and it stays 4 kB: `discover` is the one command that needs more,
     * and it borrows a task of its own rather than making every console
     * session hold the extra internal RAM for ever (design 07 section 2,
     * LL-07). See cmd_radio.c. */
    rc.task_stack_size = 4096;

    esp_console_dev_usb_serial_jtag_config_t dev =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&dev, &rc, &repl),
                        TAG, "repl");

    ESP_ERROR_CHECK(esp_console_register_help_command());

    /* Design 07 section 2 is the reference for every command in these tables,
     * and for the ok / error: line the newer ones end with. Not static: the
     * counts live in other translation units, so they are not constants a
     * static initialiser could use. */
    const struct {
        const esp_console_cmd_t *cmds;
        size_t count;
    } groups[] = {
        { cmd_system_table, cmd_system_count },
        { cmd_radio_table,  cmd_radio_count },
        { cmd_net_table,    cmd_net_count },
    };

    for (size_t g = 0; g < sizeof groups / sizeof groups[0]; g++) {
        for (size_t i = 0; i < groups[g].count; i++) {
            ESP_RETURN_ON_ERROR(esp_console_cmd_register(&groups[g].cmds[i]), TAG,
                                "cmd %s", groups[g].cmds[i].command);
        }
    }

    ESP_RETURN_ON_ERROR(esp_console_start_repl(repl), TAG, "start");
    ESP_LOGI(TAG, "console ready: type 'help'. 'log off' stops this scrolling");
    return ESP_OK;
}
