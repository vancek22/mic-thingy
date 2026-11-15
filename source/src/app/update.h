/* Updates. "Check for updates" asks GitHub for the newest release of this project - only when the user clicks (one
 * HTTPS GET to api.github.com, nothing is sent besides the generic User-Agent "mic-thingy"); the answer is reduced
 * to a version tag that is validated before it is used anywhere.
 *
 * "Update now" (only after a check found a newer tag) replaces the running program in place, with no browser and no
 * zip to unpack: it downloads the program file of that release (the one file, mic-thingy.exe / mic-thingy-linux, from
 * github.com/<this repo>/releases/download/<tag>/ - the URL is built from the validated tag only) next to the running
 * one as <exe>.new, checks it against the SHA-256 published with the release (<file>.sha256) and the file type, swaps
 * it in (the old file is kept as <exe>.old until the next start) and then offers to restart. Settings live in the
 * config folder and are untouched. If anything fails the old program is left as it was and the release page can still
 * be opened. The checksum comes from the same release as the file: it protects against a damaged or truncated
 * download, not against a compromised GitHub account - HTTPS to github.com is the trust anchor.
 * Windows: WinHTTP. Linux: curl (when installed) and xdg-open. */
#ifndef VF_UPDATE_H
#define VF_UPDATE_H

#include <stdatomic.h>
#include <stddef.h>
#include "util/thread.h"

enum {
    VF_UPD_IDLE, VF_UPD_CHECKING, VF_UPD_UP_TO_DATE, VF_UPD_NEWER, VF_UPD_NO_RELEASE, VF_UPD_ERROR,
    VF_UPD_DOWNLOADING,           /* "Update now" is running (progress in got / total) */
    VF_UPD_READY,                 /* new program is in place, restart to use it */
    VF_UPD_NO_PACKAGE,            /* that release has no stand-alone program file (older releases): use the page */
    VF_UPD_FAIL                   /* download / check / replace failed, the old program is untouched */
};

typedef struct VfUpdate {
    _Atomic int state;
    char tag[40];                     /* newest release tag; valid once state is UP_TO_DATE / NEWER and after */
    _Atomic long got, total;          /* bytes of the program file so far / expected (0 = unknown) */
    VfThread th;
} VfUpdate;

void vf_update_init(VfUpdate *u);
void vf_update_check(VfUpdate *u);                    /* starts a background check unless one is running */
void vf_update_install(VfUpdate *u);                  /* state NEWER -> DOWNLOADING -> READY / NO_PACKAGE / FAIL */
int  vf_update_state(VfUpdate *u);
void vf_update_open_page(const VfUpdate *u);          /* release page of the tag, or the releases list */
int  vf_update_restart(void);                         /* starts the new program (it waits for this process to exit); the caller then quits. 0 = ok */
void vf_update_cleanup(void);                         /* at start: removes <exe>.old and unfinished downloads */
void vf_update_free(VfUpdate *u);

/* pure helpers (tested) */
int  vf_version_cmp(const char *a, const char *b);   /* <0, 0, >0 by numeric parts; "v" prefix and suffixes ignored; -2 if either has no digits */
int  vf_update_parse(const char *json, char *tag, size_t n);   /* tag_name of a release JSON; 0 = ok, -1 = missing / unsafe */
int  vf_update_page_url(const char *tag, char *out, size_t n); /* validated release page URL, 0 = ok */
const char *vf_update_asset_name(void);               /* "mic-thingy.exe" / "mic-thingy-linux" */
int  vf_update_asset_path(const char *tag, const char *asset, char *out, size_t n);   /* /<repo>/releases/download/<tag>/<asset>; 0 = ok */
int  vf_update_parse_sha(const char *text, char hex[65]);      /* first 64-hex-digit word (sha256sum format), lower-cased; 0 = ok */
int  vf_update_verify(const char *path, const char *want_hex); /* size sane, program file type, SHA-256 equal; 0 = ok */

#endif
