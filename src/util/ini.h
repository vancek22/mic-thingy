/* Minimal INI reader: [section], key = value, comments with ';' or '#'.
 * Keys and sections are case-insensitive; values are trimmed UTF-8 strings. */
#ifndef VF_INI_H
#define VF_INI_H

typedef struct VfIniEntry {
    char section[64];
    char key[64];
    char value[256];
} VfIniEntry;

typedef struct VfIni {
    VfIniEntry *e;
    int count, cap;
} VfIni;

int  vf_ini_load(VfIni *ini, const char *path);       /* 0 = ok, -1 = cannot open */
int  vf_ini_parse(VfIni *ini, const char *text);      /* from memory */
void vf_ini_free(VfIni *ini);
const char *vf_ini_get(const VfIni *ini, const char *section, const char *key);   /* NULL if absent */
int  vf_ini_get_int(const VfIni *ini, const char *section, const char *key, int def);
double vf_ini_get_double(const VfIni *ini, const char *section, const char *key, double def);

#endif
