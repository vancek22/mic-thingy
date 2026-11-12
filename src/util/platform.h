/* Small OS layer for settings: UTF-8 paths, config directory, directory listing,
 * start-with-system. Control thread only. */
#ifndef VF_PLATFORM_H
#define VF_PLATFORM_H

#include <stddef.h>
#include <stdio.h>

#define VF_PATH_MAX 1024

/* fopen() taking a UTF-8 path on every platform. */
FILE *vf_fopen(const char *path, const char *mode);
int   vf_mkdir_p(const char *path);                  /* 0 = exists/created */
int   vf_remove(const char *path);                   /* 0 = ok */
int   vf_replace_file(const char *tmp, const char *dst);   /* atomic-ish rename over dst */

/* Windows: %APPDATA%\VoiceFilter   Linux: $XDG_CONFIG_HOME/voicefilter or ~/.config/voicefilter.
 * Created if missing. The VF_CONFIG_DIR environment variable overrides it (tests). */
int vf_config_dir(char *out, size_t n);

/* Calls fn(name, user) for every regular file in dir ending with `ext` (e.g. ".ini");
 * `name` is the file name without the extension. Returns the number of files. */
int vf_list_files(const char *dir, const char *ext, void (*fn)(const char *name, void *user), void *user);

/* Start with the system (Windows: HKCU Run key "VoiceFilter"; Linux: ~/.config/autostart/voicefilter.desktop).
 * The command line is this executable + "--autostart --minimized". */
int vf_autostart_get(void);                          /* 1 = enabled */
int vf_autostart_set(int enable);                    /* 0 = ok */
/* Same, with an explicit entry name (used by tests so the real entry is untouched). */
int vf_autostart_get_named(const char *name);
int vf_autostart_set_named(const char *name, int enable);

int vf_executable_path(char *out, size_t n);         /* UTF-8, 0 = ok */

#endif
