/* www_page.h: the single-page UI, as a C string literal.
 *
 * Kept out of webui.c so the handler code stays readable. HTML attributes use
 * single quotes throughout so nothing here needs escaping; JavaScript strings
 * do the same. Moving this to a gzipped embedded asset is TD-02.
 *
 * The page itself is in three files, because one 670-line string literal is
 * not something anyone can find their way around:
 *
 *   www_style.h    WWW_STYLE    doctype, head, the whole stylesheet
 *   www_body.h     WWW_BODY     the markup, <body> to just before <script>
 *   www_script.h   WWW_SCRIPT   the script, and the close of the document
 *
 * They concatenate below. Adjacent string literals are joined by the compiler,
 * so what is served is byte for byte what the one literal used to be; the
 * split is a move, not a rewrite, and the check that it stayed one is to
 * extract the string from both versions and diff them.
 *
 * Every control reports back. The board has no display and no buttons, so an
 * action that produces no visible change on the page is indistinguishable from
 * a dead button: press feedback plus an acknowledgement is not decoration
 * here, it is the only channel the operator has. The Network tab matters most
 * -- saving credentials reboots the board and drops the portal underneath the
 * browser, which reads as a failure unless the page says otherwise.
 *
 * Two transmitters. Rather than doubling every control, the page carries one
 * A/B selector and the Stations, Transmitter and Discover tabs act on the
 * selected one. Now playing and Diagnostics show both at once, because what is
 * on air is the question you want answered without clicking anything, and the
 * header carries both carriers for the same reason.
 *
 * The layout wants a wide screen. On a phone held upright a banner asks for it
 * to be turned sideways (design 07 section 6); it is a banner rather than a
 * wall, because the page still works upright, just cramped.
 */
#ifndef AMTX_WWW_PAGE_H
#define AMTX_WWW_PAGE_H

#include "www_body.h"
#include "www_script.h"
#include "www_style.h"

static const char WWW_PAGE[] = WWW_STYLE WWW_BODY WWW_SCRIPT;

#endif /* AMTX_WWW_PAGE_H */
