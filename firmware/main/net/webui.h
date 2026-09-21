/* webui.h: the control surface.
 *
 * There is no display and no buttons on this board, so this is it: the whole
 * user interface is one page served from flash plus a small JSON API. The
 * same server answers on the home network and inside the setup portal; the
 * page notices which it is talking to from /api/state and shows the setup
 * form instead of the radio when the board has not joined a network yet.
 *
 * It can be turned off, leaving the serial console as the only way in
 * (design 07 section 4), so nothing may exist only here: the console reaches
 * every function the page does.
 *
 * Handlers do no work themselves. They parse, call app.h, and serialise the
 * result, so that a front panel added later (EH-02) drives the same code.
 */
#ifndef AMTX_WEBUI_H
#define AMTX_WEBUI_H

#include <stdbool.h>

#include "esp_err.h"

/* Starts the server and the mDNS advert for it. Does nothing if it is already
 * running. Whether it should run at all is settings_web_enabled(); callers
 * check that, so the console can start it on request. */
esp_err_t webui_start(void);

/* Stops the server and withdraws the advert. Must not be called from one of
 * the server's own handlers. */
void webui_stop(void);

bool webui_running(void);

#endif /* AMTX_WEBUI_H */
