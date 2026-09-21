/* cli_reply.h: the console's reply convention, and the argument parsers every
 * command group shares.
 *
 * This is design 07 section 2 written as code, which is why it is its own
 * module rather than a few statics at the top of whichever file happened to
 * need them first. A program driving the console cannot see a command's return
 * code, only what it prints, so every command ends with exactly one line that
 * starts `ok` or `error:`. `tools/amtx-programmer/` parses those lines: a
 * change to the wording here is a change to that tool's input, and to the
 * tables in docs/design/07_console_and_access.md.
 *
 * The parsers are here for the same reason. `a`, `b`, `0`, `1` naming a
 * transmitter, and on/off/yes/no/true/false meaning a boolean, are part of the
 * documented command grammar; three command groups accepting three slightly
 * different versions of it is exactly the drift this module prevents.
 */
#ifndef AMTX_CLI_REPLY_H
#define AMTX_CLI_REPLY_H

#include <stdbool.h>

#include "esp_err.h"

/* ----------------------------------------------------------------- replies */

/* The last line a command prints. They return 0 even for an error: a non-zero
 * return makes the REPL print a line of its own after ours, and then ours is
 * not the last, which is what a program is watching for.
 *
 * reply_ok(NULL) prints a bare `ok`; otherwise `ok: <detail>`. The detail is
 * formatted into a 160-byte buffer and truncated beyond that.
 *
 * No printf format attribute on these: reply_ok(NULL) is the documented way to
 * print a bare `ok`, and a format attribute makes that call a warning. */
int reply_ok(const char *fmt, ...);
int reply_error(const char *fmt, ...);

/* An esp_err_t as a reply: `ok` with the detail, or `error:` with its name. */
int reply_err(esp_err_t err, const char *ok_detail);

/* ----------------------------------------------------------------- parsing */

/* "A".."D" for a chain index, "?" for anything outside the built chains. The
 * letter is what the replies and `station list` flags show. */
const char *tx_name(int ch);

/* `a`/`b` in either case, or the chain index. -1 if it names no transmitter. */
int parse_tx(const char *s);

/* Whole-string decimal integer within [lo, hi]. Trailing junk is a failure:
 * "12x" is a typo, not twelve. */
bool parse_int(const char *s, long lo, long hi, int *out);

/* on/off/1/0, and yes/no/true/false because people type them. */
bool parse_onoff(const char *s, bool *out);

#endif /* AMTX_CLI_REPLY_H */
