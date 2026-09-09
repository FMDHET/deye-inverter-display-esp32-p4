#include "harness.h"

int         t_checks = 0;
int         t_fails  = 0;
const char *t_current = "(kein Test)";

int t_report(const char *suite)
{
    if (t_fails == 0) {
        printf("  %-28s %3d Prüfungen, alle bestanden\n", suite, t_checks);
        return 0;
    }
    printf("  %-28s %3d Prüfungen, %d FEHLGESCHLAGEN\n", suite, t_checks, t_fails);
    return 1;
}
