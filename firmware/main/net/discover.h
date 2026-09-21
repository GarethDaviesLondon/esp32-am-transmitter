/* discover.h: station search against the Radio-Browser community directory.
 *
 * EH-03. The reference internet-radio project searches Radio-Browser by name,
 * genre and country and offers one-click save into the station list; this is
 * the same idea behind a `/api/discover` endpoint, with the results shaped so
 * they drop straight into `settings_station_add()`.
 *
 * Results are filtered to MP3 server-side. libhelix is the only decoder
 * compiled in, so an AAC or OGG station would save cleanly and then play
 * silence, which is a worse outcome than not offering it (EH-04).
 *
 * A search is a blocking HTTPS round trip of a second or two. Call it from a
 * task that can afford to wait -- the web handler does -- never from an ISR
 * and never while holding a lock the stream needs.
 */
#ifndef AMTX_DISCOVER_H
#define AMTX_DISCOVER_H

#include "am_config.h"
#include "esp_err.h"

typedef enum {
    DISCOVER_BY_NAME = 0,   /* station name substring */
    DISCOVER_BY_TAG,        /* genre, as Radio-Browser calls a tag */
    DISCOVER_BY_COUNTRY,
} discover_by_t;

/* Sized so an entry drops into station_t without truncating anything the
 * store would have kept. */
typedef struct {
    char name[STATION_NAME_MAX];
    char url[STATION_URL_MAX];
    char country[32];
    char codec[8];
    int  bitrate;           /* kbit/s, 0 if the directory does not say */
    int  votes;
} discover_entry_t;

/* Returns the number of entries written, or -1 on failure, in which case
 * discover_last_error() says why. `query` is plain text: this encodes it. */
int discover_search(discover_by_t by, const char *query,
                    discover_entry_t *out, int max);

discover_by_t discover_by_from_string(const char *s);

/* Valid until the next search. Never NULL. */
const char *discover_last_error(void);

#endif /* AMTX_DISCOVER_H */
