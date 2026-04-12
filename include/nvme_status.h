#ifndef NVME_STATUS_H
#define NVME_STATUS_H

/*
 * nvme_status.h — unified NVMe CQE status → AmigaOS io_Error mapping.
 *
 * The CQE status word (§4.6) packs:
 *     bit  0      phase tag (not part of status code)
 *     bits 1–8    SC  — Status Code
 *     bits 9–11   SCT — Status Code Type
 *     bits 12–13  CRD — Command Retry Delay
 *     bit  14     M   — More (further info in log page)
 *     bit  15     DNR — Do Not Retry
 *
 * Consumers of this helper pass the raw status word (phase bit already
 * masked off is fine, but not required — we re-extract SC/SCT anyway).
 *
 * Return value is the canonical io_Error to set on the completing
 * IORequest.  0 means success.
 */

#include <exec/types.h>
#include <exec/io.h>

/* Return an io_Error code for an NVMe CQE status word. */
LONG NVMe_StatusToIOErr(UWORD status_word);

/* Human-readable diagnostic string for a CQE status.  Never NULL.
 * Intended for debug logging only; not stable across NVMe revisions. */
const char *NVMe_StatusDescribe(UWORD status_word);

#endif /* NVME_STATUS_H */
