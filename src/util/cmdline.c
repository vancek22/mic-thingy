#include "util/cmdline.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOK 6

void vf_cmd_help(FILE *out)
{
    fprintf(out,
        "Commands (press ':' to type one):\n"
        "  show                          list effects, parameters and values\n"
        "  set <fx> <param> <value>      e.g. set comp ratio 4   (names may be abbreviated)\n"
        "  bypass <fx|all> [on|off]      toggle bypass of one effect or the whole chain\n"
        "  reset [fx]                    restore default parameter values\n"
        "  help                          this text\n"
        "  quit                          exit\n"
        "Keys: b = bypass all, 1..6 = toggle effect, : = command, h = help, q = quit\n");
}

void vf_cmd_show(const Pipeline *p, FILE *out)
{
    fprintf(out, "chain: %s\n", atomic_load(&p->bypass_all) ? "BYPASSED" : "active");
    for (int i = 0; i < p->count; i++) {
        const Fx *fx = p->fx[i];
        fprintf(out, " [%d] %-9s %s\n", i + 1, fx->name,
                atomic_load(&fx->bypass) ? "BYPASS" : "on");
        for (int j = 0; j < fx->n_params; j++) {
            const FxParamInfo *pi = &fx->params[j];
            fprintf(out, "       %-10s = %g %s   (%g .. %g, default %g)\n", pi->name,
                    (double)pipeline_get_param(p, i, j), pi->unit,
                    (double)pi->min, (double)pi->max, (double)pi->def);
        }
    }
}

static int is_on(const char *s)  { return !strcmp(s, "on")  || !strcmp(s, "1") || !strcmp(s, "yes"); }
static int is_off(const char *s) { return !strcmp(s, "off") || !strcmp(s, "0") || !strcmp(s, "no"); }

static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

int vf_cmd_execute(Pipeline *p, const char *line, FILE *out)
{
    char buf[256];
    strncpy(buf, line, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';

    char *tok[MAX_TOK];
    int nt = 0;
    for (char *t = strtok(buf, " \t\r\n"); t && nt < MAX_TOK; t = strtok(NULL, " \t\r\n")) {
        lower(t);
        tok[nt++] = t;
    }
    if (nt == 0) return 0;

    if (!strcmp(tok[0], "quit") || !strcmp(tok[0], "q") || !strcmp(tok[0], "exit")) return 1;
    if (!strcmp(tok[0], "help") || !strcmp(tok[0], "?")) { vf_cmd_help(out); return 0; }
    if (!strcmp(tok[0], "show") || !strcmp(tok[0], "ls")) { vf_cmd_show(p, out); return 0; }

    if (!strcmp(tok[0], "set")) {
        if (nt != 4) { fprintf(out, "usage: set <fx> <param> <value>\n"); return 0; }
        int fi = pipeline_find_fx(p, tok[1]);
        if (fi < 0) { fprintf(out, "unknown effect '%s' (try: show)\n", tok[1]); return 0; }
        int id = pipeline_find_param(p, fi, tok[2]);
        if (id < 0) { fprintf(out, "unknown parameter '%s' of %s\n", tok[2], p->fx[fi]->name); return 0; }
        char *end;
        float v = strtof(tok[3], &end);
        if (end == tok[3] || *end != '\0') { fprintf(out, "'%s' is not a number\n", tok[3]); return 0; }
        pipeline_set_param(p, fi, id, v);
        const FxParamInfo *pi = &p->fx[fi]->params[id];
        fprintf(out, "%s.%s = %g %s\n", p->fx[fi]->name, pi->name,
                (double)pipeline_get_param(p, fi, id), pi->unit);
        return 0;
    }

    if (!strcmp(tok[0], "bypass") || !strcmp(tok[0], "byp")) {
        if (nt < 2) { fprintf(out, "usage: bypass <fx|all> [on|off]\n"); return 0; }
        int want = -1;                                   /* -1 = toggle */
        if (nt >= 3) want = is_on(tok[2]) ? 1 : (is_off(tok[2]) ? 0 : -2);
        if (want == -2) { fprintf(out, "use on or off\n"); return 0; }
        if (!strcmp(tok[1], "all")) {
            int v = want < 0 ? !atomic_load(&p->bypass_all) : want;
            atomic_store(&p->bypass_all, v);
            fprintf(out, "chain %s\n", v ? "BYPASSED" : "active");
            return 0;
        }
        int fi = pipeline_find_fx(p, tok[1]);
        if (fi < 0) { fprintf(out, "unknown effect '%s'\n", tok[1]); return 0; }
        int v = want < 0 ? !atomic_load(&p->fx[fi]->bypass) : want;
        pipeline_set_bypass(p, fi, v);
        fprintf(out, "%s %s\n", p->fx[fi]->name, v ? "BYPASS" : "on");
        return 0;
    }

    if (!strcmp(tok[0], "reset")) {
        int lo = 0, hi = p->count - 1;
        if (nt >= 2) {
            int fi = pipeline_find_fx(p, tok[1]);
            if (fi < 0) { fprintf(out, "unknown effect '%s'\n", tok[1]); return 0; }
            lo = hi = fi;
        }
        for (int i = lo; i <= hi; i++)
            for (int j = 0; j < p->fx[i]->n_params; j++)
                pipeline_set_param(p, i, j, p->fx[i]->params[j].def);
        fprintf(out, "defaults restored\n");
        return 0;
    }

    fprintf(out, "unknown command '%s' (try: help)\n", tok[0]);
    return 0;
}
