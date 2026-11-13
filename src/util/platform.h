/* Small OS layer for settings: UTF-8 paths, config directory, directory listing,
 * start-with-system. Control thread only. */
#ifndef VF_PLATFORM_H
#define VF_PLATFORM_H

#include <stddef.h>
#include <stdio.h>

#define VF_PATH_MAX 1024

/* fopen() taking a UTF-8 path on every platform. */
FILE *vf_fopen(const char *path, const char *mode);

/* Call first thing in main(): Windows - drop the current directory from the DLL search path. */
void vf_harden_process(void);
int   vf_mkdir_p(const char *path);                  /* 0 = exists/created */
int   vf_remove(const char *path);                   /* 0 = ok */
int   vf_replace_file(const char *tmp, const char *dst);   /* atomic-ish rename over dst */

/* Windows: %APPDATA%\mic thingy   Linux: $XDG_CONFIG_HOME/mic-thingy or ~/.config/mic-thingy.
 * Created if missing. The VF_CONFIG_DIR environment variable overrides it (tests). */
int vf_config_dir(char *out, size_t n);

/* <Documents>/mic thingy (recordings, exported presets); created if missing. Falls back to the config dir. */
int vf_documents_dir(char *out, size_t n);

/* 1 while the session is locked / the secure desktop is shown (Windows); always 0 elsewhere. */
int vf_screen_locked(void);

/* Calls fn(name, user) for every regular file in dir ending with `ext` (e.g. ".ini");
 * `name` is the file name without the extension. Returns the number of files. */
int vf_list_files(const char *dir, const char *ext, void (*fn)(const char *name, void *user), void *user);

/* Start with the system (Windows: HKCU Run key "mic thingy"; Linux: ~/.config/autostart/mic-thingy.desktop).
 * The command line is this executable + "--autostart --minimized". */
int vf_autostart_get(void);                          /* 1 = enabled */
int vf_autostart_set(int enable);                    /* 0 = ok */
/* Same, with an explicit entry name (used by tests so the real entry is untouched). */
int vf_autostart_get_named(const char *name);
int vf_autostart_set_named(const char *name, int enable);
/* Replaces an autostart entry left by the old name ("VoiceFilter") with the current one,
 * pointing at this executable. Returns 1 if a migration happened. */
int vf_autostart_migrate(void);

int vf_executable_path(char *out, size_t n);         /* UTF-8, 0 = ok */

#endif
