#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    char *u = str_upper("hello");
    char *t = str_trim("   spaced out   ");
    printf("upper=%s trim=[%s] a-count=%zu\n", u, t, str_count("banana", 'a'));
    free(u);
    free(t);
    return 0;
}
