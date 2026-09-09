#pragma once
/* A 40-line test runner. No framework: the interesting part of these tests is
 * the fakes and the assertions, and a dependency that has to be downloaded
 * before CI can run the first test would be the opposite of the point. */
#include <math.h>
#include <stdio.h>
#include <string.h>

extern int         t_checks, t_fails;
extern const char *t_current;

#define CHECK(cond)                                                            \
    do {                                                                       \
        t_checks++;                                                            \
        if (!(cond)) {                                                         \
            t_fails++;                                                         \
            fprintf(stderr, "  FAIL  %s:%d\n    %s\n    (in %s)\n",            \
                    __FILE__, __LINE__, #cond, t_current);                     \
        }                                                                      \
    } while (0)

/* Floating point with an explicit tolerance -- the code under test computes in
 * float and divides by three, so exact comparison would be a lie. */
#define CHECK_F(got, want, tol)                                                \
    do {                                                                       \
        t_checks++;                                                            \
        double g_ = (double)(got), w_ = (double)(want);                        \
        if (!(fabs(g_ - w_) <= (double)(tol))) {                               \
            t_fails++;                                                         \
            fprintf(stderr, "  FAIL  %s:%d\n    %s = %.4f, erwartet %.4f "     \
                            "(+-%g)\n    (in %s)\n",                           \
                    __FILE__, __LINE__, #got, g_, w_, (double)(tol),           \
                    t_current);                                                \
        }                                                                      \
    } while (0)

#define CHECK_I(got, want)                                                     \
    do {                                                                       \
        t_checks++;                                                            \
        long long g_ = (long long)(got), w_ = (long long)(want);               \
        if (g_ != w_) {                                                        \
            t_fails++;                                                         \
            fprintf(stderr, "  FAIL  %s:%d\n    %s = %lld, erwartet %lld\n"    \
                            "    (in %s)\n",                                   \
                    __FILE__, __LINE__, #got, g_, w_, t_current);              \
        }                                                                      \
    } while (0)

#define RUN(fn)                                                                \
    do {                                                                       \
        t_current = #fn;                                                       \
        fn();                                                                  \
    } while (0)

int t_report(const char *suite);
