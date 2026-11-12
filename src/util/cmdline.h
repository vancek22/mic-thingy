/* Text command interpreter for the console front-end (and tests). */
#ifndef VF_CMDLINE_H
#define VF_CMDLINE_H

#include <stdio.h>
#include "dsp/pipeline.h"

/* Executes one command line, printing the response to `out`.
 * Returns 1 if the user asked to quit, otherwise 0. Control thread only. */
int vf_cmd_execute(Pipeline *p, const char *line, FILE *out);

void vf_cmd_help(FILE *out);
void vf_cmd_show(const Pipeline *p, FILE *out);

#endif
