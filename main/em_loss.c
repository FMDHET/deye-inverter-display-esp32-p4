#include "em_loss.h"

#include <math.h>
#include <string.h>

#define MIN_S      10        /* a bin enters the fit from 10 s on (deye.html FIT_S) */
#define W_CAP_S    3600      /* weight cap: one long bin must not decide alone       */

typedef struct { double p, loss, w; } pt_t;

/* Gauss with partial pivoting, m <= 3. */
static bool solve(double A[3][3], double y[3], int m, double x[3])
{
    double M[3][4];
    for (int i = 0; i < m; i++) { for (int j = 0; j < m; j++) M[i][j] = A[i][j]; M[i][m] = y[i]; }
    for (int c = 0; c < m; c++) {
        int piv = c;
        for (int r = c + 1; r < m; r++) if (fabs(M[r][c]) > fabs(M[piv][c])) piv = r;
        if (fabs(M[piv][c]) < 1e-12) return false;
        if (piv != c) for (int k = 0; k <= m; k++) { double t = M[c][k]; M[c][k] = M[piv][k]; M[piv][k] = t; }
        for (int r = 0; r < m; r++) if (r != c) {
            double f = M[r][c] / M[c][c];
            for (int k = c; k <= m; k++) M[r][k] -= f * M[c][k];
        }
    }
    for (int i = 0; i < m; i++) x[i] = M[i][m] / M[i][i];
    return true;
}

/* Weighted least squares over the powers in `terms` (subset of 0,1,2). */
static bool wls(const pt_t *pts, int n, const int *terms, int nt, double out[3])
{
    double A[3][3] = { { 0 } }, y[3] = { 0 }, c[3];
    for (int k = 0; k < n; k++) {
        double f[3];
        for (int i = 0; i < nt; i++) f[i] = pow(pts[k].p, terms[i]);
        for (int i = 0; i < nt; i++) {
            y[i] += pts[k].w * f[i] * pts[k].loss;
            for (int j = 0; j < nt; j++) A[i][j] += pts[k].w * f[i] * f[j];
        }
    }
    if (!solve(A, y, nt, c)) return false;
    out[0] = out[1] = out[2] = 0;
    for (int i = 0; i < nt; i++) out[terms[i]] = c[i];
    return true;
}

bool em_loss_fit(const em_loss_bin_t *bins, int n, bool dis, float out[3])
{
    pt_t pts[64];
    int m = 0;
    double pmin = INFINITY, pmax = 0;
    for (int i = 0; i < n && m < 64; i++) {
        const em_loss_bin_t *b = &bins[i];
        if (b->s < MIN_S || !(b->in_wh > 0)) continue;
        double pin = b->in_wh * 3600.0 / b->s, pout = b->out_wh * 3600.0 / b->s;
        double p = (dis ? pout : pin) / 1000.0;
        if (!(p > 0)) continue;
        pts[m].p = p;
        pts[m].loss = (pin - pout) / 1000.0;
        pts[m].w = b->s < W_CAP_S ? b->s : W_CAP_S;
        if (p < pmin) pmin = p;
        if (p > pmax) pmax = p;
        m++;
    }
    if (m < 3 || pmax / pmin < 3.0) return false;

    double k[3];
    static const int T012[] = { 0, 1, 2 }, T01[] = { 0, 1 }, T12[] = { 1, 2 }, T1[] = { 1 };
    bool ok = wls(pts, m, T012, 3, k);
    if (ok && k[2] < 0) ok = wls(pts, m, T01, 2, k);
    if (ok && k[0] < 0) ok = k[2] > 0 ? wls(pts, m, T12, 2, k) : wls(pts, m, T1, 1, k);
    if (!ok || k[0] < 0 || k[2] < 0 || !isfinite(k[0]) || !isfinite(k[1]) || !isfinite(k[2])) return false;
    for (int i = 0; i < 3; i++) out[i] = (float)k[i];
    return true;
}
