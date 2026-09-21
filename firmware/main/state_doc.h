/* state_doc.h: the whole board state as one JSON document.
 *
 * The web page polls it as /api/state and the console prints it for `state`.
 * It is built in one place so the two cannot drift: a program driving the
 * console (tools/amtx-programmer/) reads exactly what the page reads, and
 * keeps working when the web interface is turned off. Design 04 section 3 and
 * design 07 section 1 are the contract for its shape.
 */
#ifndef AMTX_STATE_DOC_H
#define AMTX_STATE_DOC_H

#include "cJSON.h"

/* A new document the caller owns and must cJSON_Delete(). NULL only if the
 * allocation failed. */
cJSON *state_doc_build(void);

#endif /* AMTX_STATE_DOC_H */
