/* interrupt — Ctrl+C (SIGINT) handling for the interactive REPL. The first press
 * during a turn sets a flag that the inference and agent loops poll, so they stop
 * the current generation and return to the prompt instead of killing the process.
 * A second press before the flag is cleared restores the default handler and
 * re-raises, so a genuinely stuck process can still be force-killed. Pure and tiny;
 * the flag is a single volatile sig_atomic_t. */
#ifndef ANACHRON_INTERRUPT_H
#define ANACHRON_INTERRUPT_H

#ifdef __cplusplus
extern "C" {
#endif

void interrupt_install(void);   /* install the SIGINT handler (call once at startup) */
int  interrupt_pending(void);   /* nonzero if Ctrl+C was pressed since the last clear */
void interrupt_clear(void);     /* reset the flag (call before each turn) */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ANACHRON_INTERRUPT_H */
