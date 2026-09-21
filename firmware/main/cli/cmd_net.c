/* cmd_net.c: see cmd_net.h. */

#include "cmd_net.h"

#include <stdio.h>
#include <string.h>

#include "am_config.h"
#include "cli_reply.h"
#include "cmd_system.h"
#include "esp_err.h"
#include "net/webui.h"
#include "net/wifi_conn.h"
#include "store/settings.h"

/* No TAG here: every line this module prints is a command's reply or the
 * output before it, and design 07 fixes both. */

/* -------------------------------------------------------------------- wifi */

/* These predate design 07 and printed no ok / error: line, so a program had to
 * guess when a join had finished and whether it worked. They now end with one,
 * like every other command, and keep the human-readable lines before it. */
static int wifi_usage(void)
{
    return reply_error("usage: wifi status | scan | join <ssid> [pass] | "
                       "save <ssid> [pass] | list | forget");
}

static int wifi_join(const char *ssid, const char *pass)
{
    printf("joining \"%s\" ...\n", ssid);

    /* Blocks for up to 15 s, retrying failures a retry can fix. The reason is
     * printed here rather than left to the log: a monitor that splits log
     * lines from the console shows them in a different pane. */
    const esp_err_t err = wifi_conn_try(ssid, pass, 15000);
    if (err != ESP_OK) {
        if (settings_web_enabled()) {
            printf("run 'reboot' to bring the " AMTX_AP_SSID " portal back\n");
        }
        const uint8_t reason = wifi_conn_last_reason();
        if (reason != 0) {
            return reply_error("join failed: reason %u, %s", (unsigned)reason,
                               wifi_conn_reason_text(reason));
        }
        return reply_error("join failed: %s, no answer at all", esp_err_to_name(err));
    }

    wifi_conn_status_t st;
    wifi_conn_get_status(&st);
    printf("joined \"%s\", ip %s, rssi %d dBm\n", st.ssid, st.ip, st.rssi);

    /* Joined but not saved is a failure worth reporting as one: it works now
     * and silently stops working at the next power cut. */
    const esp_err_t saved = settings_wifi_add(ssid, pass);
    if (saved != ESP_OK) {
        return reply_error("joined, but the credential was not saved: %s",
                           esp_err_to_name(saved));
    }
    return reply_ok("joined %s, ip %s, saved (%d stored)", st.ssid, st.ip,
                    settings_wifi_count());
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) return wifi_usage();

    if (strcmp(argv[1], "status") == 0 && argc == 2) {
        /* Deliberately the same function the `status` command runs, not a
         * second line that looks like it: design 07 section 1 says there is
         * one code path per answer, and this is the smallest example of it. */
        cmd_status(0, NULL);
        return reply_ok(NULL);
    }

    if (strcmp(argv[1], "scan") == 0 && argc == 2) {
        wifi_scan_entry_t found[24];
        const int n = wifi_conn_scan(found, 24);
        for (int i = 0; i < n; i++) {
            printf("%2d  %-32s %4d dBm  %s\n", i, found[i].ssid, found[i].rssi,
                   found[i].secure ? "secured" : "open");
        }
        return reply_ok("%d network(s) found", n > 0 ? n : 0);
    }

    if (strcmp(argv[1], "list") == 0 && argc == 2) {
        const int n = settings_wifi_count();
        for (int i = 0; i < n; i++) {
            char ssid[33];
            if (settings_wifi_get(i, ssid, sizeof ssid, NULL, 0)) {
                printf("%2d  %s\n", i, ssid);
            }
        }
        return reply_ok("%d saved", n);
    }

    if (strcmp(argv[1], "forget") == 0 && argc == 2) {
        int removed = 0;
        while (settings_wifi_count() > 0) {
            const esp_err_t err = settings_wifi_delete(0);
            if (err != ESP_OK) {
                return reply_error("deleted %d, then %s with %d left", removed,
                                   esp_err_to_name(err), settings_wifi_count());
            }
            removed++;
        }
        return reply_ok("deleted %d credential(s)", removed);
    }

    if ((strcmp(argv[1], "join") == 0 || strcmp(argv[1], "save") == 0) &&
        (argc == 3 || argc == 4)) {
        const char *ssid = argv[2];
        const char *pass = (argc > 3) ? argv[3] : "";

        if (strcmp(argv[1], "save") == 0) {
            const esp_err_t err = settings_wifi_add(ssid, pass);
            if (err != ESP_OK) return reply_error("not saved: %s", esp_err_to_name(err));
            return reply_ok("saved %s (%d stored), tried at the next boot", ssid,
                            settings_wifi_count());
        }
        return wifi_join(ssid, pass);
    }

    return wifi_usage();
}

/* ---------------------------------------------------------------- hostname */

static int cmd_hostname(int argc, char **argv)
{
    if (argc == 1) {
        printf("hostname=%s\n", settings_hostname());
        return reply_ok(NULL);
    }
    if (argc != 2) return reply_error("usage: hostname [name]");

    const esp_err_t err = settings_set_hostname(argv[1]);
    if (err == ESP_ERR_INVALID_ARG) {
        return reply_error("1 to %d letters, digits and hyphens, not starting or "
                           "ending with a hyphen", HOSTNAME_MAX - 1);
    }
    if (err != ESP_OK) return reply_error("%s", esp_err_to_name(err));

    wifi_conn_apply_hostname();
    const char *host = settings_hostname();
    return reply_ok("hostname %s: %s.local now, the router's list after a reboot",
                    host, host);
}

/* --------------------------------------------------------------------- web */

static int cmd_web(int argc, char **argv)
{
    if (argc == 1) {
        printf("web=%s\n", webui_running() ? "on" : "off");
        return reply_ok(NULL);
    }

    bool on;
    if (argc != 2 || !parse_onoff(argv[1], &on)) return reply_error("usage: web [on|off]");

    const esp_err_t err = settings_set_web_enabled(on);
    if (err != ESP_OK) return reply_error("not saved: %s", esp_err_to_name(err));

    if (!on) {
        webui_stop();
        return reply_ok("web interface off, and no setup portal after a reboot; "
                        "only this console can turn it back on");
    }

    const esp_err_t started = webui_start();
    if (started != ESP_OK) return reply_error("saved, but it did not start: %s",
                                              esp_err_to_name(started));
    return reply_ok("web interface on at http://%s.local", settings_hostname());
}

/* ------------------------------------------------------------------- table */

/* Design 07 sections 2 to 4 are the reference for these three. The help
 * strings are what `help` prints, so they are as much a contract as the
 * replies are. */
const esp_console_cmd_t cmd_net_table[] = {
    { .command = "wifi",     .help = "wifi status|scan|join|save|list|forget",
      .func = cmd_wifi },
    { .command = "hostname", .help = "hostname [name]: the name.local this board answers to",
      .func = cmd_hostname },
    { .command = "web",      .help = "web [on|off]: the web interface, saved",
      .func = cmd_web },
};

const size_t cmd_net_count = sizeof cmd_net_table / sizeof cmd_net_table[0];
