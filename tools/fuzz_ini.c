/* Fuzzer for the INI parser (src/util/ini.c), which reads settings.ini, presets, profiles and imported text.
 *
 *   fuzz-ini [--iterations N] [--seed S] [--max-len L] [--corpus file]... [--save-crash file]
 *
 * Self-contained mutation fuzzer (no libFuzzer needed, runs with MinGW): starts from built-in seeds plus any
 * --corpus files and mutates them (bit flips, byte/range insert, delete, duplicate, splice, interesting tokens,
 * long lines, NUL / CR / BOM bytes). Every input is parsed from a heap block of EXACTLY its length + 1, so a read
 * past the terminator is caught by AddressSanitizer / page heap when those are on.
 * Properties checked on every input:
 *   - no crash or hang, no leak (vf_ini_free after each parse);
 *   - 0 <= count <= 4096 (the cap) and count <= cap;
 *   - every section / key / value is NUL terminated inside its array, has no line break, and keys and values
 *     have no blanks at either end;
 *   - vf_ini_get finds every entry and returns the LAST one with the same section and key (case-insensitive);
 *   - vf_ini_get_int / vf_ini_get_double never crash;
 *   - round trip: writing the entries back out and parsing again gives the same entries (skipped when a field
 *     filled its array, because truncation may end in a blank).
 * Exit code 0 = nothing found, 1 = a property was violated (the input is printed in hex, optionally saved).
 *
 * libFuzzer: compile with -DVF_LIBFUZZER -fsanitize=fuzzer to get LLVMFuzzerTestOneInput instead of main. */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/ini.h"

#define MAX_ENTRIES 4096

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static size_t span(const char *s, size_t n)         /* strnlen */
{
    size_t l = 0;
    while (l < n && s[l]) l++;
    return l;
}

/* returns 0 when all properties hold, otherwise fills `why` */
static int check_text(const char *text, char *why, size_t whyn)
{
    VfIni a;
    vf_ini_parse(&a, text);
    int bad = 0, full = 0;
    if (a.count < 0 || a.count > MAX_ENTRIES || a.count > a.cap) {
        snprintf(why, whyn, "count %d cap %d", a.count, a.cap);
        vf_ini_free(&a);
        return 1;
    }
    for (int i = 0; i < a.count && !bad; i++) {
        const VfIniEntry *e = &a.e[i];
        const struct { const char *s; size_t n; const char *what; int trimmed; } f[3] = {
            { e->section, sizeof e->section, "section", 0 }, { e->key, sizeof e->key, "key", 1 },
            { e->value, sizeof e->value, "value", 1 } };
        for (int k = 0; k < 3 && !bad; k++) {
            const size_t l = span(f[k].s, f[k].n);
            if (l == f[k].n) { snprintf(why, whyn, "entry %d %s not terminated", i, f[k].what); bad = 1; break; }
            if (l + 1 == f[k].n) full = 1;
            if (strpbrk(f[k].s, "\r\n")) { snprintf(why, whyn, "entry %d %s has a line break", i, f[k].what); bad = 1; }
            else if (f[k].trimmed && l && (isspace((unsigned char)f[k].s[0]) || isspace((unsigned char)f[k].s[l - 1])) && l + 1 < f[k].n)
                { snprintf(why, whyn, "entry %d %s has blanks at an end", i, f[k].what); bad = 1; }
        }
        if (bad) break;
        const char *v = vf_ini_get(&a, e->section, e->key);
        if (!v) { snprintf(why, whyn, "entry %d [%s] %s not found", i, e->section, e->key); bad = 1; break; }
        int last = -1;
        for (int j = a.count - 1; j >= 0; j--)
            if (ieq(a.e[j].section, e->section) && ieq(a.e[j].key, e->key)) { last = j; break; }
        if (v != a.e[last].value) { snprintf(why, whyn, "entry %d: get returned an earlier duplicate", i); bad = 1; break; }
        (void)vf_ini_get_int(&a, e->section, e->key, 7);
        (void)vf_ini_get_double(&a, e->section, e->key, 0.5);
    }
    if (!bad && vf_ini_get(&a, "no such section", "no such key") != NULL) { snprintf(why, whyn, "found a missing key"); bad = 1; }

    /* round trip */
    if (!bad && !full && a.count > 0) {
        size_t cap = (size_t)a.count * (64 + 64 + 256 + 16) + 16, used = 0;
        char *out = (char *)malloc(cap);
        if (out) {
            for (int i = 0; i < a.count; i++)
                used += (size_t)snprintf(out + used, cap - used, "[%s]\n%s=%s\n", a.e[i].section, a.e[i].key, a.e[i].value);
            VfIni b;
            vf_ini_parse(&b, out);
            /* entries with an empty or '['/';'/'#'-leading key or a section containing ']' do not survive: compare
             * only what the writer can express */
            int j = 0;
            for (int i = 0; i < a.count && !bad; i++) {
                const char c = a.e[i].key[0];
                if (c == '[' || c == ';' || c == '#' || strchr(a.e[i].section, ']')) continue;
                if (j >= b.count || strcmp(a.e[i].section, b.e[j].section) || strcmp(a.e[i].key, b.e[j].key) ||
                    strcmp(a.e[i].value, b.e[j].value)) {
                    snprintf(why, whyn, "round trip differs at entry %d ([%s] %s)", i, a.e[i].section, a.e[i].key);
                    bad = 1;
                }
                j++;
            }
            vf_ini_free(&b);
            free(out);
        }
    }
    vf_ini_free(&a);
    return bad;
}

static int check_input(const unsigned char *data, size_t len, char *why, size_t whyn)
{
    char *text = (char *)malloc(len + 1);                   /* exact size: an overread hits the guard */
    if (!text) return 0;
    memcpy(text, data, len);
    text[len] = '\0';
    const int bad = check_text(text, why, whyn);
    free(text);
    return bad;
}

#ifdef VF_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char why[160];
    if (check_input(data, size, why, sizeof why)) abort();
    return 0;
}
#else

/* ---------------------------------------------------------------- mutation engine */
static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static size_t below(size_t n) { return n ? (size_t)(rnd() % n) : 0; }

static const char *k_tokens[] = {
    "[", "]", "[audio]", "[fx.rnnoise]", "=", " = ", ";", "#", "\n", "\r\n", "\r", "\t", "\xEF\xBB\xBF", "key=", "enabled = 1",
    "99999999999999999999", "-1", "1e999", "nan", "inf", "0x7fffffff", "[[", "]]", "= =", "\n[x]\n", "a=b\nc=d\n",
};
static const char *k_seeds[] = {
    "[audio]\ninput = Microphone\noutput = Speakers\nmode = vmic\nbypass = 0\n\n[app]\nlanguage = en\npreset = default\n"
    "[fx.rnnoise]\nenabled = 1\nwet = 0.95\n[fx.gate]\nhold = 250\n",
    "\xEF\xBB\xBF; comment\n# another\n[window]\nx = -5\ny = 99999999999\n",
    "[apps]\nrule1 = game.exe|noisy\nrule2 = |x\n",
    "k=v\n[s]\nk=v\n[S]\nK=w\n",
    "",
};

typedef struct { unsigned char *d; size_t n, cap; } Buf;

static void buf_reserve(Buf *b, size_t n)
{
    if (n <= b->cap) return;
    size_t c = b->cap ? b->cap : 256;
    while (c < n) c *= 2;
    b->d = (unsigned char *)realloc(b->d, c);
    if (!b->d) { fprintf(stderr, "out of memory\n"); exit(2); }
    b->cap = c;
}

static void insert(Buf *b, size_t at, const void *p, size_t n, size_t max_len)
{
    if (b->n + n > max_len) return;
    buf_reserve(b, b->n + n + 1);
    memmove(b->d + at + n, b->d + at, b->n - at);
    memcpy(b->d + at, p, n);
    b->n += n;
}

static void mutate(Buf *b, const Buf *corpus, int ncorp, size_t max_len)
{
    const int rounds = 1 + (int)below(4);
    for (int r = 0; r < rounds; r++) {
        switch (below(10)) {
        case 0: if (b->n) b->d[below(b->n)] ^= (unsigned char)(1u << below(8)); break;          /* bit flip */
        case 1: { unsigned char c = (unsigned char)rnd(); insert(b, below(b->n + 1), &c, 1, max_len); break; }
        case 2: if (b->n) { size_t at = below(b->n), l = 1 + below(16); if (at + l > b->n) l = b->n - at;
                            memmove(b->d + at, b->d + at + l, b->n - at - l); b->n -= l; } break;   /* delete range */
        case 3: if (b->n) { size_t at = below(b->n), l = 1 + below(64); if (at + l > b->n) l = b->n - at;
                            unsigned char *tmp = (unsigned char *)malloc(l); memcpy(tmp, b->d + at, l);
                            insert(b, below(b->n + 1), tmp, l, max_len); free(tmp); } break;      /* duplicate range */
        case 4: { const char *t = k_tokens[below(sizeof k_tokens / sizeof k_tokens[0])];
                  insert(b, below(b->n + 1), t, strlen(t), max_len); break; }
        case 5: { size_t l = 100 + below(600); unsigned char *tmp = (unsigned char *)malloc(l);       /* very long line */
                  memset(tmp, "abc =;[]"[below(8)], l); insert(b, below(b->n + 1), tmp, l, max_len); free(tmp); break; }
        case 6: { const unsigned char z = (unsigned char)(below(3) == 0 ? 0 : below(3) == 0 ? '\r' : '\n');
                  if (b->n) { b->d[below(b->n)] = z; }
                  break; }
        case 7: {                                                                                     /* splice */
                  const Buf *o = &corpus[below((size_t)ncorp)];
                  if (o->n) { size_t at = below(o->n), l = 1 + below(o->n - at); insert(b, below(b->n + 1), o->d + at, l, max_len); }
                  break;
                }
        case 8: if (b->n) b->d[below(b->n)] = (unsigned char)(rnd() & 0xFF); break;
        default: if (b->n > 1) { size_t i = below(b->n), j = below(b->n); unsigned char t = b->d[i]; b->d[i] = b->d[j]; b->d[j] = t; } break;
        }
    }
}

static void print_hex(const unsigned char *d, size_t n)
{
    for (size_t i = 0; i < n; i++) printf("%02x%s", d[i], (i + 1) % 32 == 0 ? "\n" : " ");
    printf("\n");
}

static int load_file(const char *path, Buf *b)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (1 << 20)) { fclose(f); return -1; }
    buf_reserve(b, (size_t)n + 1);
    b->n = fread(b->d, 1, (size_t)n, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    long iterations = 200000;
    size_t max_len = 8192;
    const char *crash_file = NULL;
    Buf corpus[64];
    int ncorp = 0;
    memset(corpus, 0, sizeof corpus);
    for (size_t i = 0; i < sizeof k_seeds / sizeof k_seeds[0] && ncorp < 64; i++) {
        buf_reserve(&corpus[ncorp], strlen(k_seeds[i]) + 1);
        corpus[ncorp].n = strlen(k_seeds[i]);
        memcpy(corpus[ncorp].d, k_seeds[i], corpus[ncorp].n);
        ncorp++;
    }
    for (int i = 1; i < argc; i++) {
        const int more = i + 1 < argc;
        if (!strcmp(argv[i], "--iterations") && more) iterations = atol(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && more) rs = (uint64_t)strtoull(argv[++i], NULL, 10) * 2654435761ull + 88172645463325252ull;
        else if (!strcmp(argv[i], "--max-len") && more) max_len = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--save-crash") && more) crash_file = argv[++i];
        else if (!strcmp(argv[i], "--corpus") && more && ncorp < 64) {
            if (load_file(argv[++i], &corpus[ncorp]) == 0) ncorp++;
            else { fprintf(stderr, "cannot read %s\n", argv[i]); return 2; }
        } else { fprintf(stderr, "usage: fuzz-ini [--iterations N] [--seed S] [--max-len L] [--corpus file]... [--save-crash file]\n"); return 2; }
    }
    if (!rs) rs = 1;

    Buf cur = { 0 };
    char why[200];
    long total_entries = 0;
    for (long it = 0; it < iterations; it++) {
        const Buf *base = &corpus[below((size_t)ncorp)];
        cur.n = 0;
        buf_reserve(&cur, base->n + 1);
        memcpy(cur.d, base->d, base->n);
        cur.n = base->n;
        mutate(&cur, corpus, ncorp, max_len);
        if (check_input(cur.d, cur.n, why, sizeof why)) {
            printf("FAILED at iteration %ld: %s\ninput (%zu bytes):\n", it, why, cur.n);
            print_hex(cur.d, cur.n);
            if (crash_file) { FILE *f = fopen(crash_file, "wb"); if (f) { fwrite(cur.d, 1, cur.n, f); fclose(f); } }
            return 1;
        }
        total_entries += 1;
        /* feed some inputs back so the corpus drifts towards deeper structure */
        if (below(64) == 0 && cur.n > 0) {
            Buf *slot = &corpus[ncorp < 64 ? ncorp++ : (int)below(64)];
            buf_reserve(slot, cur.n + 1);
            memcpy(slot->d, cur.d, cur.n);
            slot->n = cur.n;
        }
    }
    printf("fuzz-ini: %ld inputs, no property violated (seed state %llu)\n", total_entries, (unsigned long long)rs);
    return 0;
}
#endif
