#ifndef VF_CONSOLE_H
#define VF_CONSOLE_H

void vf_console_init(void);        /* UTF-8 output, raw non-blocking keyboard */
void vf_console_restore(void);
int  vf_console_getkey(void);      /* -1 when no key is waiting */
void vf_sleep_ms(int ms);

#endif
