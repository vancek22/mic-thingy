/* "Check for updates": asks GitHub for the newest release of this project - only when the user clicks.
 * One HTTPS GET to api.github.com (nothing is sent besides the generic User-Agent "mic-thingy"); the answer is
 * reduced to a version tag, and the page that gets opened is built from that tag after validation, never taken from
 * the server's text. Windows: WinHTTP. Linux: curl (when installed) and xdg-open. */
#ifndef VF_UPDATE_H
#define VF_UPDATE_H

#include <stdatomic.h>
#include <stddef.h>
#include "util/thread.h"

enum { VF_UPD_IDLE, VF_UPD_CHECKING, VF_UPD_UP_TO_DATE, VF_UPD_NEWER, VF_UPD_NO_RELEASE, VF_UPD_ERROR };

typedef struct VfUpdate {
    _Atomic int state;
    char tag[40];                     /* newest release tag; valid once state is UP_TO_DATE / NEWER */
    VfThread th;
} VfUpdate;

void vf_update_init(VfUpdate *u);
void vf_update_check(VfUpdate *u);                    /* starts a background check unless one is running */
int  vf_update_state(VfUpdate *u);
void vf_update_open_page(const VfUpdate *u);          /* release page of the tag, or the releases list */
void vf_update_free(VfUpdate *u);

/* pure helpers (tested) */
int  vf_version_cmp(const char *a, const char *b);   /* <0, 0, >0 by numeric parts; "v" prefix and suffixes ignored; -2 if either has no digits */
int  vf_update_parse(const char *json, char *tag, size_t n);   /* tag_name of a release JSON; 0 = ok, -1 = missing / unsafe */
int  vf_update_page_url(const char *tag, char *out, size_t n); /* validated release page URL, 0 = ok */

#endif
