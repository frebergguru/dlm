/* libdlm — native archive.org extractor (internal). */
#ifndef DLM_ARCHIVEORG_H
#define DLM_ARCHIVEORG_H

#include "dlm/extract.h"

/* Resolve an archive.org item URL into download tasks. */
int dlm_extract_archiveorg(const char *url, dlm_extract_result *out);

#endif /* DLM_ARCHIVEORG_H */
