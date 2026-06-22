#include "interrupt.h"

#include <signal.h>

static volatile sig_atomic_t g_flag = 0;

static void on_sigint(int sig) {
    (void)sig;
    if (g_flag) {
        /* Second press while still pending: hand back to the default handler so a
         * truly stuck process can still be killed. */
        signal(SIGINT, SIG_DFL);
        raise(SIGINT);
        return;
    }
    g_flag = 1;
    /* Re-arm under System V semantics, where delivery resets the handler. */
    signal(SIGINT, on_sigint);
}

void interrupt_install(void) { signal(SIGINT, on_sigint); }
int  interrupt_pending(void) { return g_flag != 0; }
void interrupt_clear(void)   { g_flag = 0; }
