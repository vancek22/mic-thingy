/* Per-application presets: when a listed program is running, its preset is applied; when it
 * ends, the previous preset returns. This file only knows which programs run and how to match
 * the rules; the GUI does the preset switching. */
#ifndef VF_APPWATCH_H
#define VF_APPWATCH_H

#define VF_MAX_APP_RULES 8
#define VF_APP_NAME 64

typedef struct VfAppRule {
    char exe[VF_APP_NAME];       /* process name, e.g. "game.exe" (case-insensitive) */
    char preset[VF_APP_NAME];    /* preset key or user preset name */
} VfAppRule;

/* Index of the FIRST rule (list order = priority) whose program is in `running`, or -1. */
int vf_appwatch_match(const VfAppRule *rules, int n_rules, char (*running)[VF_APP_NAME], int n_running);

/* Names of the running processes (lower case, unique), at most `max`. Windows: toolhelp snapshot,
 * Linux: /proc/<pid>/comm. Returns the count. */
int vf_appwatch_list(char (*out)[VF_APP_NAME], int max);

/* Search for the program picker: indices into `list` of the names that contain `needle` (case-insensitive,
 * blanks around the needle ignored; empty needle = all), sorted alphabetically. At most `max`; returns the count. */
int vf_appwatch_filter(char (*list)[VF_APP_NAME], int n, const char *needle, int *idx, int max);

#endif
