/* cli_reply.c: see cli_reply.h. */

#include "cli_reply.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "am_config.h"

/* No TAG here: nothing in this module logs. Every line it produces is console
 * output a program is expected to parse, and a log line is not that. */

/* ----------------------------------------------------------------- replies */

/* Every command added for design 07 ends with exactly one of these lines, so
 * a program driving the console knows the command has finished and whether it
 * worked. They return 0 even for an error: a non-zero return makes the REPL
 * print a line of its own after ours, and then ours is not the last. */
int reply_ok(const char *fmt, ...)
{
    if (!fmt) {
        printf("ok\n");
        return 0;
    }
    char detail[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    printf("ok: %s\n", detail);
    return 0;
}

int reply_error(const char *fmt, ...)
{
    char detail[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    printf("error: %s\n", detail);
    return 0;
}

/* An esp_err_t as a reply: `ok` with the detail, or `error:` with its name. */
int reply_err(esp_err_t err, const char *ok_detail)
{
    if (err != ESP_OK) return reply_error("%s", esp_err_to_name(err));
    return reply_ok("%s", ok_detail);
}

/* ----------------------------------------------------------------- parsing */

const char *tx_name(int ch)
{
    static const char *const names[] = { "A", "B", "C", "D" };
    return (ch >= 0 && ch < 4) ? names[ch] : "?";
}

/* `a`/`b` in either case, or the chain index. -1 if it names no transmitter. */
int parse_tx(const char *s)
{
    if (!s || s[0] == '\0' || s[1] != '\0') return -1;
    int ch = -1;
    const char c = (char)tolower((unsigned char)s[0]);
    if (c >= 'a' && c <= 'z') ch = c - 'a';
    if (c >= '0' && c <= '9') ch = c - '0';
    return (ch >= 0 && ch < RF_CHAINS) ? ch : -1;
}

/* Whole-string decimal integer within [lo, hi]. Trailing junk is a failure:
 * "12x" is a typo, not twelve. */
bool parse_int(const char *s, long lo, long hi, int *out)
{
    if (!s || s[0] == '\0') return false;
    char *end = NULL;
    errno = 0;
    const long v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

/* on/off/1/0, and yes/no/true/false because people type them. */
bool parse_onoff(const char *s, bool *out)
{
    if (!s) return false;
    if (!strcasecmp(s, "on") || !strcmp(s, "1") || !strcasecmp(s, "yes") ||
        !strcasecmp(s, "true")) {
        *out = true;
        return true;
    }
    if (!strcasecmp(s, "off") || !strcmp(s, "0") || !strcasecmp(s, "no") ||
        !strcasecmp(s, "false")) {
        *out = false;
        return true;
    }
    return false;
}
