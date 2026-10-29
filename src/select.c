/*
 * select.c - Auswahlmaske (8 Bit): Polygon/Lasso, Rechteck, Ellipse
 *
 * Kantenglaettung: 4 Unterzeilen je Pixelzeile, horizontal exakte
 * Abdeckung in 1/256 Pixel. Fuellregel gerade/ungerade.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "apx.h"

#define SUBS 4

typedef struct { float ymin, ymax, x, slope; } Edge;

static void isort(float *a, LONG n)
{
    LONG i, j;
    for (i = 1; i < n; i++) {
        float v = a[i];
        for (j = i - 1; j >= 0 && a[j] > v; j--)
            a[j + 1] = a[j];
        a[j + 1] = v;
    }
}

static int edge_cmp(const void *a, const void *b)
{
    float d = ((const Edge *)a)->ymin - ((const Edge *)b)->ymin;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

static void combine_row(UBYTE *s, const UBYTE *c, LONG n, LONG mode)
{
    LONG i;
    switch (mode) {
    case SEL_NEW:
    case SEL_ADD:
        for (i = 0; i < n; i++) {
            ULONG a = s[i], b = c[i];
            s[i] = (UBYTE)(a + b - ((a * b * 257 + 0x8080) >> 16));
        }
        break;
    case SEL_SUB:
        for (i = 0; i < n; i++)
            s[i] = (UBYTE)((s[i] * (255 - c[i]) * 257 + 0x8080) >> 16);
        break;
    case SEL_ISECT:
        for (i = 0; i < n; i++)
            s[i] = (UBYTE)((s[i] * c[i] * 257 + 0x8080) >> 16);
        break;
    }
}

static void clear_region(LONG x0, LONG y0, LONG x1, LONG y1)
{
    LONG y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    if (x0 >= x1)
        return;
    for (y = y0; y < y1; y++)
        memset(doc->sel + y * doc->w + x0, 0, x1 - x0);
}

/* Vor dem Kombinieren: NEU loescht die alte Auswahl, SCHNITTMENGE alles
 * ausserhalb der neuen Box. Liefert FALSE, wenn die Box leer ist. */
static BOOL prepare_mode(LONG mode, LONG bx0, LONG by0, LONG bx1, LONG by1)
{
    if (mode == SEL_NEW && doc->hassel)
        clear_region(doc->selx0, doc->sely0, doc->selx1, doc->sely1);
    if (mode == SEL_ISECT && doc->hassel) {
        clear_region(doc->selx0, doc->sely0, doc->selx1, by0);
        clear_region(doc->selx0, by1, doc->selx1, doc->sely1);
        clear_region(doc->selx0, by0, bx0, by1);
        clear_region(bx1, by0, doc->selx1, by1);
    }
    if (bx0 >= bx1 || by0 >= by1) {
        if (mode == SEL_ISECT)
            clear_region(0, 0, doc->w, doc->h);
        return FALSE;
    }
    return TRUE;
}

void sel_polygon(const float *xs, const float *ys, LONG n, LONG mode)
{
    Edge *edges;
    LONG *act;
    float *ix;
    UWORD *acc;
    UBYTE *cov;
    LONG ne = 0, i, y, bx0, by0, bx1, by1, w, next = 0, nact = 0;
    float fx0 = 1e9f, fy0 = 1e9f, fx1 = -1e9f, fy1 = -1e9f;

    if (n < 3)
        return;
    for (i = 0; i < n; i++) {
        if (xs[i] < fx0) fx0 = xs[i];
        if (xs[i] > fx1) fx1 = xs[i];
        if (ys[i] < fy0) fy0 = ys[i];
        if (ys[i] > fy1) fy1 = ys[i];
    }
    bx0 = (LONG)floorf(fx0); by0 = (LONG)floorf(fy0);
    bx1 = (LONG)ceilf(fx1);  by1 = (LONG)ceilf(fy1);
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > doc->w) bx1 = doc->w;
    if (by1 > doc->h) by1 = doc->h;

    if (!prepare_mode(mode, bx0, by0, bx1, by1)) {
        sel_update_bounds();
        return;
    }
    w = bx1 - bx0;

    edges = AllocVec(n * sizeof(Edge), MEMF_ANY);
    act = AllocVec(n * sizeof(LONG), MEMF_ANY);
    ix = AllocVec(n * sizeof(float), MEMF_ANY);
    acc = AllocVec(w * sizeof(UWORD), MEMF_ANY);
    cov = AllocVec(w, MEMF_ANY);
    if (!edges || !act || !ix || !acc || !cov)
        goto out;

    for (i = 0; i < n; i++) {
        float x0 = xs[i], y0 = ys[i];
        float x1 = xs[(i + 1) % n], y1 = ys[(i + 1) % n];
        if (y0 == y1)
            continue;
        if (y0 > y1) {
            float t;
            t = x0; x0 = x1; x1 = t;
            t = y0; y0 = y1; y1 = t;
        }
        edges[ne].ymin = y0;
        edges[ne].ymax = y1;
        edges[ne].slope = (x1 - x0) / (y1 - y0);
        edges[ne].x = x0;
        ne++;
    }
    qsort(edges, ne, sizeof(Edge), edge_cmp);

    for (y = by0; y < by1; y++) {
        LONG k;
        memset(acc, 0, w * sizeof(UWORD));
        for (k = 0; k < SUBS; k++) {
            float sy = y + (k + 0.5f) / SUBS;
            LONG j, m = 0, ni = 0;
            while (next < ne && edges[next].ymin <= sy)
                act[nact++] = next++;
            for (j = 0; j < nact; j++) {
                Edge *e = &edges[act[j]];
                if (e->ymax <= sy)
                    continue;           /* abgelaufen */
                act[m++] = act[j];
                if (e->ymin <= sy)
                    ix[ni++] = e->x + (sy - e->ymin) * e->slope;
            }
            nact = m;
            isort(ix, ni);
            for (j = 0; j + 1 < ni; j += 2) {
                LONG xa = (LONG)((ix[j] - bx0) * 256.0f);
                LONG xb = (LONG)((ix[j + 1] - bx0) * 256.0f);
                LONG pa, pb, p;
                if (xa < 0) xa = 0;
                if (xb > w * 256) xb = w * 256;
                if (xa >= xb)
                    continue;
                pa = xa >> 8;
                pb = xb >> 8;
                if (pa == pb) {
                    acc[pa] += xb - xa;
                    continue;
                }
                acc[pa] += 256 - (xa & 255);
                for (p = pa + 1; p < pb; p++)
                    acc[p] += 256;
                if (pb < w)
                    acc[pb] += xb & 255;
            }
        }
        for (i = 0; i < w; i++) {
            ULONG v = acc[i] >> 2;      /* 4 * 256 -> 0..256 */
            cov[i] = v > 255 ? 255 : (UBYTE)v;
        }
        combine_row(doc->sel + y * doc->w + bx0, cov, w, mode);
    }
out:
    if (edges) FreeVec(edges);
    if (act) FreeVec(act);
    if (ix) FreeVec(ix);
    if (acc) FreeVec(acc);
    if (cov) FreeVec(cov);
    sel_update_bounds();
}

void sel_rect(float x0, float y0, float x1, float y1, LONG mode)
{
    float xs[4], ys[4], t;
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
    /* Rechteck rastet auf Pixel ein */
    x0 = floorf(x0 + 0.5f); y0 = floorf(y0 + 0.5f);
    x1 = floorf(x1 + 0.5f); y1 = floorf(y1 + 0.5f);
    xs[0] = x0; ys[0] = y0;
    xs[1] = x1; ys[1] = y0;
    xs[2] = x1; ys[2] = y1;
    xs[3] = x0; ys[3] = y1;
    sel_polygon(xs, ys, 4, mode);
}

#define ELL_N 256
static float ell_x[ELL_N], ell_y[ELL_N];

void sel_ellipse(float x0, float y0, float x1, float y1, LONG mode)
{
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float rx = fabsf(x1 - x0) * 0.5f, ry = fabsf(y1 - y0) * 0.5f;
    LONG i;
    for (i = 0; i < ELL_N; i++) {
        float a = i * (6.2831853f / ELL_N);
        ell_x[i] = cx + rx * cosf(a);
        ell_y[i] = cy + ry * sinf(a);
    }
    sel_polygon(ell_x, ell_y, ELL_N, mode);
}

void sel_all(void)
{
    memset(doc->sel, 255, doc->w * doc->h);
    sel_update_bounds();
}

void sel_none(void)
{
    if (doc->hassel)
        clear_region(doc->selx0, doc->sely0, doc->selx1, doc->sely1);
    doc->hassel = FALSE;
}

void sel_invert(void)
{
    ULONG *p = (ULONG *)doc->sel;
    LONG i, n = doc->w * doc->h;
    for (i = 0; i < n / 4; i++)
        p[i] = ~p[i];
    for (i = (n / 4) * 4; i < n; i++)
        doc->sel[i] = 255 - doc->sel[i];
    sel_update_bounds();
}

void sel_update_bounds(void)
{
    LONG x, y, w = doc->w;
    LONG x0 = w, y0 = doc->h, x1 = 0, y1 = 0;
    for (y = 0; y < doc->h; y++) {
        const UBYTE *s = doc->sel + y * w;
        const ULONG *l = (const ULONG *)s;
        LONG first = -1, last = -1;
        /* schnell: 4 Bytes auf einmal pruefen */
        for (x = 0; x < w / 4; x++)
            if (l[x]) {
                first = x * 4;
                break;
            }
        if (first < 0) {
            for (x = (w / 4) * 4; x < w; x++)
                if (s[x]) { first = x; break; }
            if (first < 0)
                continue;
        }
        for (x = w - 1; x >= first; x--)
            if (s[x]) { last = x; break; }
        while (!s[first])
            first++;
        if (first < x0) x0 = first;
        if (last + 1 > x1) x1 = last + 1;
        if (y < y0) y0 = y;
        y1 = y + 1;
    }
    if (x1 > x0 && y1 > y0) {
        doc->hassel = TRUE;
        doc->selx0 = x0; doc->sely0 = y0;
        doc->selx1 = x1; doc->sely1 = y1;
    } else {
        doc->hassel = FALSE;
        doc->selx0 = doc->sely0 = doc->selx1 = doc->sely1 = 0;
    }
}

/* ---- Zauberstab ---- */

static BOOL wand_match(ULONG p, ULONG ref, LONG tol)
{
    LONG da = (LONG)(p >> 24) - (LONG)(ref >> 24);
    LONG dr = (LONG)((p >> 16) & 0xFF) - (LONG)((ref >> 16) & 0xFF);
    LONG dg = (LONG)((p >> 8) & 0xFF) - (LONG)((ref >> 8) & 0xFF);
    LONG db = (LONG)(p & 0xFF) - (LONG)(ref & 0xFF);
    if (!(p >> 24) && !(ref >> 24))
        return TRUE;            /* transparent ist transparent */
    if (da < 0) da = -da;
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    return da <= tol && dr <= tol && dg <= tol && db <= tol;
}

/* Box-Unschaerfe (Radius r) eines Bereichs einer w-breiten Bytemaske */
void mask_blur(UBYTE *m, LONG stride, LONG x0, LONG y0, LONG x1, LONG y1, LONG r)
{
    LONG w = x1 - x0, h = y1 - y0, n = (w > h ? w : h), x, y, i;
    UBYTE *tmp;
    LONG div = 2 * r + 1;
    if (r < 1 || w <= 0 || h <= 0 || !(tmp = AllocVec(n, MEMF_ANY)))
        return;
    /* waagerecht */
    for (y = y0; y < y1; y++) {
        UBYTE *row = m + y * stride + x0;
        LONG sum = 0;
        for (i = -r; i <= r; i++)
            sum += (i >= 0 && i < w) ? row[i] : 0;
        for (x = 0; x < w; x++) {
            tmp[x] = (UBYTE)((sum + r) / div);
            sum -= (x - r >= 0) ? row[x - r] : 0;
            sum += (x + r + 1 < w) ? row[x + r + 1] : 0;
        }
        CopyMem(tmp, row, w);
    }
    /* senkrecht */
    for (x = x0; x < x1; x++) {
        UBYTE *col = m + x;
        LONG sum = 0;
        for (i = -r; i <= r; i++)
            sum += (y0 + i >= y0 && y0 + i < y1) ? col[(y0 + i) * stride] : 0;
        for (y = 0; y < h; y++) {
            tmp[y] = (UBYTE)((sum + r) / div);
            sum -= (y - r >= 0) ? col[(y0 + y - r) * stride] : 0;
            sum += (y + r + 1 < h) ? col[(y0 + y + r + 1) * stride] : 0;
        }
        for (y = 0; y < h; y++)
            col[(y0 + y) * stride] = tmp[y];
    }
    FreeVec(tmp);
}

static void combine_mask(const UBYTE *m, LONG bx0, LONG by0, LONG bx1, LONG by1, LONG mode)
{
    LONG y;
    if (prepare_mode(mode, bx0, by0, bx1, by1))
        for (y = by0; y < by1; y++)
            combine_row(doc->sel + y * doc->w + bx0, m + y * doc->w + bx0, bx1 - bx0, mode);
    sel_update_bounds();
}

static Tile **wm_arr;

static ULONG arr_get(LONG x, LONG y)
{
    Tile *t = wm_arr[(y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT)];
    return t ? t->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)] : 0;
}

/* Flaeche aehnlicher Farbe ab (sx, sy) in arr (Ebene oder Maske) als
 * Maske in Dokumentgroesse; bb = Umriss x0, y0, x1, y1 (leer: x1 <= x0) */
UBYTE *wand_mask(Tile **arr, LONG sx, LONG sy, LONG tol, BOOL contiguous, BOOL antialias, LONG *bb)
{
    LONG w = doc->w, h = doc->h, x, y;
    LONG bx0 = w, by0 = h, bx1 = 0, by1 = 0;
    UBYTE *m;
    ULONG ref;

    bb[0] = bb[1] = bb[2] = bb[3] = 0;
    if (!(m = AllocVec(w * h, MEMF_ANY | MEMF_CLEAR)))
        return NULL;
    if (sx < 0 || sy < 0 || sx >= w || sy >= h)
        return m;
    wm_arr = arr;
    ref = arr_get(sx, sy);

    if (!contiguous) {
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                if (wand_match(arr_get(x, y), ref, tol)) {
                    m[y * w + x] = 255;
                    if (x < bx0) bx0 = x;
                    if (x >= bx1) bx1 = x + 1;
                    if (y < by0) by0 = y;
                    by1 = y + 1;
                }
    } else {
        /* Scanline-Flood-Fill mit eigenem Stapel */
        LONG *stack, sp = 0, max = 65536;
        if (!(stack = AllocVec(max * 2 * sizeof(LONG), MEMF_ANY))) {
            FreeVec(m);
            return NULL;
        }
        stack[sp++] = sx;
        stack[sp++] = sy;
        while (sp) {
            LONG xl, xr, yy, d;
            y = stack[--sp];
            x = stack[--sp];
            if (m[y * w + x] || !wand_match(arr_get(x, y), ref, tol))
                continue;
            xl = x;
            while (xl > 0 && !m[y * w + xl - 1] && wand_match(arr_get(xl - 1, y), ref, tol))
                xl--;
            xr = x;
            while (xr < w - 1 && !m[y * w + xr + 1] && wand_match(arr_get(xr + 1, y), ref, tol))
                xr++;
            memset(m + y * w + xl, 255, xr - xl + 1);
            if (xl < bx0) bx0 = xl;
            if (xr + 1 > bx1) bx1 = xr + 1;
            if (y < by0) by0 = y;
            if (y + 1 > by1) by1 = y + 1;
            /* Nachbarzeilen: je Lauf passender Pixel einen Startpunkt */
            for (d = -1; d <= 1; d += 2) {
                BOOL in = FALSE;
                yy = y + d;
                if (yy < 0 || yy >= h)
                    continue;
                for (x = xl; x <= xr; x++) {
                    BOOL ok = !m[yy * w + x] && wand_match(arr_get(x, yy), ref, tol);
                    if (ok && !in) {
                        if (sp + 2 > max * 2) {
                            LONG *ns = AllocVec(max * 4 * sizeof(LONG), MEMF_ANY);
                            if (!ns)
                                break;
                            CopyMem(stack, ns, sp * sizeof(LONG));
                            FreeVec(stack);
                            stack = ns;
                            max *= 2;
                        }
                        stack[sp++] = x;
                        stack[sp++] = yy;
                    }
                    in = ok;
                }
            }
        }
        FreeVec(stack);
    }

    if (bx1 > bx0) {
        if (antialias) {
            bx0 = bx0 > 0 ? bx0 - 1 : 0;
            by0 = by0 > 0 ? by0 - 1 : 0;
            bx1 = bx1 < w ? bx1 + 1 : w;
            by1 = by1 < h ? by1 + 1 : h;
            mask_blur(m, w, bx0, by0, bx1, by1, 1);
        }
        bb[0] = bx0; bb[1] = by0; bb[2] = bx1; bb[3] = by1;
    }
    return m;
}

void sel_wand(LONG sx, LONG sy, LONG tol, BOOL contiguous, BOOL antialias, LONG mode)
{
    LONG bb[4];
    UBYTE *m;
    if (sx < 0 || sy < 0 || sx >= doc->w || sy >= doc->h)
        return;
    if (!(m = wand_mask(doc->layers[doc->active]->tiles, sx, sy, tol, contiguous, antialias, bb)))
        return;
    combine_mask(m, bb[0], bb[1], bb[2], bb[3], mode);
    FreeVec(m);
}

/* ---- Auswahl veraendern ---- */

void sel_feather(LONG r)
{
    LONG x0, y0, x1, y1, b, k;
    if (!doc->hassel || r < 1)
        return;
    /* drei Box-Durchgaenge ~ Gauss mit sigma ~ r/2 */
    b = (r + 1) / 2;
    x0 = doc->selx0 - 3 * b - 1; y0 = doc->sely0 - 3 * b - 1;
    x1 = doc->selx1 + 3 * b + 1; y1 = doc->sely1 + 3 * b + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    for (k = 0; k < 3; k++)
        mask_blur(doc->sel, doc->w, x0, y0, x1, y1, b);
    sel_update_bounds();
}

/* Chamfer-3-4-Distanz zur naechsten Zelle mit seed(...)==TRUE */
static UWORD *chamfer(LONG x0, LONG y0, LONG w, LONG h, BOOL inside_seed)
{
    UWORD *d = AllocVec(w * h * sizeof(UWORD), MEMF_ANY);
    LONG x, y;
    if (!d)
        return NULL;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            LONG gx = x0 + x, gy = y0 + y;
            BOOL sel = gx >= 0 && gy >= 0 && gx < doc->w && gy < doc->h &&
                       doc->sel[gy * doc->w + gx] >= 128;
            d[y * w + x] = (sel == inside_seed) ? 0 : 0xFFFF;
        }
#define RELAX(cond, off, c) if (cond) { ULONG v = d[i + (off)] + (c); if (v < d[i]) d[i] = (UWORD)v; }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            LONG i = y * w + x;
            RELAX(x > 0, -1, 3)
            RELAX(y > 0, -w, 3)
            RELAX(x > 0 && y > 0, -w - 1, 4)
            RELAX(x < w - 1 && y > 0, -w + 1, 4)
        }
    for (y = h - 1; y >= 0; y--)
        for (x = w - 1; x >= 0; x--) {
            LONG i = y * w + x;
            RELAX(x < w - 1, 1, 3)
            RELAX(y < h - 1, w, 3)
            RELAX(x < w - 1 && y < h - 1, w + 1, 4)
            RELAX(x > 0 && y < h - 1, w - 1, 4)
        }
#undef RELAX
    return d;
}

void sel_grow(LONG r)
{
    LONG x0, y0, w, h, x, y;
    UWORD *d;
    if (!doc->hassel || r < 1)
        return;
    x0 = doc->selx0 - r - 1; y0 = doc->sely0 - r - 1;
    w = doc->selx1 + r + 1 - x0; h = doc->sely1 + r + 1 - y0;
    if (!(d = chamfer(x0, y0, w, h, TRUE)))
        return;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            LONG gx = x0 + x, gy = y0 + y;
            if (gx < 0 || gy < 0 || gx >= doc->w || gy >= doc->h)
                continue;
            if (d[y * w + x] <= 3 * r)
                doc->sel[gy * doc->w + gx] = 255;
        }
    FreeVec(d);
    sel_update_bounds();
}

void sel_shrink(LONG r)
{
    LONG x0, y0, w, h, x, y;
    UWORD *d;
    if (!doc->hassel || r < 1)
        return;
    x0 = doc->selx0 - 1; y0 = doc->sely0 - 1;
    w = doc->selx1 + 1 - x0; h = doc->sely1 + 1 - y0;
    if (!(d = chamfer(x0, y0, w, h, FALSE)))
        return;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            LONG gx = x0 + x, gy = y0 + y;
            if (gx < 0 || gy < 0 || gx >= doc->w || gy >= doc->h)
                continue;
            if (d[y * w + x] <= 3 * r)
                doc->sel[gy * doc->w + gx] = 0;
        }
    FreeVec(d);
    sel_update_bounds();
}

/* Select > Modify > Border: Band der Breite w um die Auswahlkante (je halb innen und aussen),
 * weich auslaufend wie in Photoshop */
void sel_border(LONG wd)
{
    LONG x0, y0, w, h, x, y, half = (wd + 1) / 2;
    UWORD *din, *dout;
    UBYTE *band;
    if (!doc->hassel || wd < 1)
        return;
    x0 = doc->selx0 - half - 2; y0 = doc->sely0 - half - 2;
    w = doc->selx1 + half + 2 - x0; h = doc->sely1 + half + 2 - y0;
    din = chamfer(x0, y0, w, h, FALSE);     /* innen: Abstand zur Kante */
    dout = chamfer(x0, y0, w, h, TRUE);     /* aussen: Abstand zur Auswahl */
    band = AllocVec(w * h, MEMF_ANY);
    if (din && dout && band) {
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                LONG i = y * w + x, gx = x0 + x, gy = y0 + y;
                BOOL in = gx >= 0 && gy >= 0 && gx < doc->w && gy < doc->h &&
                          doc->sel[gy * doc->w + gx] >= 128;
                LONG d = in ? din[i] : dout[i];     /* 3 = ein Pixel */
                band[i] = d <= 3 * half ? 255 : 0;
            }
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                LONG gx = x0 + x, gy = y0 + y;
                if (gx >= 0 && gy >= 0 && gx < doc->w && gy < doc->h)
                    doc->sel[gy * doc->w + gx] = band[y * w + x];
            }
        /* weiche Kanten */
        {
            LONG bx0 = x0 < 0 ? 0 : x0, by0 = y0 < 0 ? 0 : y0;
            LONG bx1 = x0 + w > doc->w ? doc->w : x0 + w, by1 = y0 + h > doc->h ? doc->h : y0 + h;
            mask_blur(doc->sel, doc->w, bx0, by0, bx1, by1, 1);
        }
    }
    if (din) FreeVec(din);
    if (dout) FreeVec(dout);
    if (band) FreeVec(band);
    sel_update_bounds();
}

/* Select > Modify > Smooth: Mehrheit im (2r+1)^2-Fenster entscheidet (Zacken, Inseln weg) */
void sel_smooth(LONG r)
{
    LONG x0, y0, x1, y1, w, h, x, y;
    ULONG *sum;
    if (!doc->hassel || r < 1)
        return;
    x0 = doc->selx0 - r; y0 = doc->sely0 - r;
    x1 = doc->selx1 + r; y1 = doc->sely1 + r;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    w = x1 - x0;
    h = y1 - y0;
    /* Summentabelle (w+1)*(h+1) der gesetzten Pixel */
    if (!(sum = AllocVec((w + 1) * (h + 1) * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR)))
        return;
    for (y = 0; y < h; y++) {
        ULONG row = 0;
        const UBYTE *s = doc->sel + (y0 + y) * doc->w + x0;
        for (x = 0; x < w; x++) {
            row += s[x] >= 128;
            sum[(y + 1) * (w + 1) + x + 1] = sum[y * (w + 1) + x + 1] + row;
        }
    }
    for (y = 0; y < h; y++) {
        UBYTE *s = doc->sel + (y0 + y) * doc->w + x0;
        LONG ya = y - r < 0 ? 0 : y - r, yb = y + r + 1 > h ? h : y + r + 1;
        for (x = 0; x < w; x++) {
            LONG xa = x - r < 0 ? 0 : x - r, xb = x + r + 1 > w ? w : x + r + 1;
            /* Fenster am Bildrand wird nicht verkleinert gewertet: ausserhalb zaehlt als leer */
            ULONG n = sum[yb * (w + 1) + xb] - sum[ya * (w + 1) + xb] - sum[yb * (w + 1) + xa] + sum[ya * (w + 1) + xa];
            ULONG area = (2 * r + 1) * (2 * r + 1);
            s[x] = 2 * n > area ? 255 : 0;
        }
    }
    FreeVec(sum);
    sel_update_bounds();
}

/* ---- Schnellauswahl: mit dem Pinsel ueber das Objekt malen ---- */

static BOOL qs_sub;
static UBYTE *qs_m;
static LONG *qs_stack;
static LONG qs_max;

void sel_quick_begin(LONG mode)
{
    qs_sub = mode == SEL_SUB;
    if (mode == SEL_NEW)
        sel_none();
}

static ULONG flat_rgb(LONG x, LONG y)
{
    ULONG p = doc->flat[y * doc->w + x], a = p >> 24;
    if (a == 255 || !a)
        return p & 0xFFFFFF;
    /* vormultipliziert -> ungefaehr straight (fuer den Farbvergleich reicht das) */
    return ((((p >> 16) & 0xFF) * 255 / a) << 16) | ((((p >> 8) & 0xFF) * 255 / a) << 8) | ((p & 0xFF) * 255 / a);
}

void sel_quick_dab(float cx, float cy, LONG size)
{
    LONG r = size / 2 > 1 ? size / 2 : 1, R = r * 4 > 40 ? r * 4 : 40;
    LONG rc = r * 3 / 5 > 1 ? r * 3 / 5 : 1;        /* Kern fuer die Farbmessung */
    LONG x0 = (LONG)cx - R, y0 = (LONG)cy - R, x1 = (LONG)cx + R + 1, y1 = (LONG)cy + R + 1;
    LONG ww, hh, x, y, n = 0, sp = 0;
    float mr = 0, mg = 0, mb = 0, vr = 0, tol;
    doc_process_dirty();
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    ww = x1 - x0;
    hh = y1 - y0;
    if (!qs_m || qs_max < ww * hh) {
        if (qs_m) FreeVec(qs_m);
        if (qs_stack) FreeVec(qs_stack);
        qs_max = ww * hh;
        qs_m = AllocVec(qs_max, MEMF_ANY);
        qs_stack = AllocVec(qs_max * 2 * sizeof(LONG), MEMF_ANY);
        if (!qs_m || !qs_stack) {
            qs_max = 0;
            return;
        }
    }
    memset(qs_m, 0, ww * hh);
    /* Farbe unter dem Pinsel: Mittel und Streuung */
    for (y = (LONG)cy - rc; y <= (LONG)cy + rc; y++)
        for (x = (LONG)cx - rc; x <= (LONG)cx + rc; x++) {
            ULONG c;
            if (x < 0 || y < 0 || x >= doc->w || y >= doc->h ||
                (x - cx) * (x - cx) + (y - cy) * (y - cy) > rc * rc)
                continue;
            c = flat_rgb(x, y);
            mr += (c >> 16) & 0xFF; mg += (c >> 8) & 0xFF; mb += c & 0xFF;
            n++;
        }
    if (!n)
        return;
    mr /= n; mg /= n; mb /= n;
    for (y = (LONG)cy - rc; y <= (LONG)cy + rc; y++)
        for (x = (LONG)cx - rc; x <= (LONG)cx + rc; x++) {
            ULONG c;
            float d;
            if (x < 0 || y < 0 || x >= doc->w || y >= doc->h ||
                (x - cx) * (x - cx) + (y - cy) * (y - cy) > rc * rc)
                continue;
            c = flat_rgb(x, y);
            d = fabsf(((c >> 16) & 0xFF) - mr) + fabsf(((c >> 8) & 0xFF) - mg) + fabsf((c & 0xFF) - mb);
            vr += d;
        }
    /* Toleranz: Streuung im Kern, gedeckelt */
    tol = vr / n * 1.5f + 24.0f;
    if (tol > 90.0f)
        tol = 90.0f;
    /* Startpunkte: der Kern (nur passende Farben) */
    for (y = (LONG)cy - rc; y <= (LONG)cy + rc; y++)
        for (x = (LONG)cx - rc; x <= (LONG)cx + rc; x++)
            if (x >= x0 && y >= y0 && x < x1 && y < y1 &&
                (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rc * rc) {
                ULONG c = flat_rgb(x, y);
                if (fabsf(((c >> 16) & 0xFF) - mr) + fabsf(((c >> 8) & 0xFF) - mg) + fabsf((c & 0xFF) - mb) > tol)
                    continue;
                qs_m[(y - y0) * ww + (x - x0)] = 255;
                qs_stack[sp++] = x;
                qs_stack[sp++] = y;
            }
    /* wachsen, solange die Farbe passt */
    while (sp) {
        LONG k;
        static const BYTE nb[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
        y = qs_stack[--sp];
        x = qs_stack[--sp];
        ULONG cc = flat_rgb(x, y);
        for (k = 0; k < 4; k++) {
            LONG nx = x + nb[k][0], ny = y + nb[k][1], e;
            ULONG c;
            float d;
            if (nx < x0 || ny < y0 || nx >= x1 || ny >= y1 || qs_m[(ny - y0) * ww + (nx - x0)] ||
                (nx - cx) * (nx - cx) + (ny - cy) * (ny - cy) > R * R)
                continue;           /* rundes Fenster */
            c = flat_rgb(nx, ny);
            d = fabsf(((c >> 16) & 0xFF) - mr) + fabsf(((c >> 8) & 0xFF) - mg) + fabsf((c & 0xFF) - mb);
            if (d > tol)
                continue;
            /* an starken Kanten nicht weiter (Einrasten) */
            e = abs((LONG)((c >> 16) & 0xFF) - (LONG)((cc >> 16) & 0xFF)) +
                abs((LONG)((c >> 8) & 0xFF) - (LONG)((cc >> 8) & 0xFF)) +
                abs((LONG)(c & 0xFF) - (LONG)(cc & 0xFF));
            if (e > 60)
                continue;
            qs_m[(ny - y0) * ww + (nx - x0)] = 255;
            if (sp + 2 <= qs_max * 2) {
                qs_stack[sp++] = nx;
                qs_stack[sp++] = ny;
            }
        }
    }
    /* in die Auswahl uebernehmen */
    for (y = 0; y < hh; y++) {
        UBYTE *s = doc->sel + (y0 + y) * doc->w + x0, *m = qs_m + y * ww;
        for (x = 0; x < ww; x++)
            if (m[x])
                s[x] = qs_sub ? 0 : 255;
    }
    sel_update_bounds();
}

/* ---- Select > Color Range ---- */

/* weiche Auswahl nach Farbabstand zum Bild (Komposit) */
void sel_color_range(ULONG rgb, LONG fuzz, BOOL invert)
{
    LONG n = doc->w * doc->h, i, r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    LONG half = fuzz / 2;
    doc_process_dirty();
    for (i = 0; i < n; i++) {
        ULONG p = flat_rgb(i % doc->w, i / doc->w);
        LONG dr = (LONG)((p >> 16) & 0xFF) - r, dg = (LONG)((p >> 8) & 0xFF) - g, db = (LONG)(p & 0xFF) - b, d, v;
        if (dr < 0) dr = -dr;
        if (dg < 0) dg = -dg;
        if (db < 0) db = -db;
        d = dr > dg ? dr : dg;
        if (db > d) d = db;
        /* bis halbe Toleranz voll, dann linear auslaufend */
        v = d <= half ? 255 : d >= fuzz ? 0 : (fuzz - d) * 255 / (fuzz - half);
        doc->sel[i] = (UBYTE)(invert ? 255 - v : v);
    }
    sel_update_bounds();
}

/* ---- Select > Grow / Similar ---- */

/* Farbtabelle 32x32x32 der ausgewaehlten Farben, um tol erweitert */
static UBYTE *gs_table(LONG tol)
{
    UBYTE *t = AllocVec(32768, MEMF_ANY | MEMF_CLEAR), *u;
    LONG i, n = doc->w * doc->h, rad = (tol + 7) / 8, ax, k;
    if (!t)
        return NULL;
    for (i = 0; i < n; i++)
        if (doc->sel[i] >= 128) {
            ULONG p = flat_rgb(i % doc->w, i / doc->w);
            t[((p >> 9) & 0x7C00) | ((p >> 6) & 0x3E0) | ((p >> 3) & 0x1F)] = 1;
        }
    if (!rad || !(u = AllocVec(32768, MEMF_ANY)))
        return t;
    /* je Achse um rad Faecher ausdehnen */
    for (ax = 0; ax < 3; ax++) {
        LONG st = ax == 0 ? 1 : ax == 1 ? 32 : 1024;
        for (i = 0; i < 32768; i++) {
            LONG c = (i / st) & 31, v = 0;
            for (k = -rad; k <= rad && !v; k++)
                if (c + k >= 0 && c + k < 32)
                    v = t[i + k * st];
            u[i] = (UBYTE)v;
        }
        CopyMem(u, t, 32768);
    }
    FreeVec(u);
    return t;
}

static BOOL gs_match(const UBYTE *t, LONG i)
{
    ULONG p = flat_rgb(i % doc->w, i / doc->w);
    return t[((p >> 9) & 0x7C00) | ((p >> 6) & 0x3E0) | ((p >> 3) & 0x1F)];
}

BOOL sel_grow_similar(BOOL similar, LONG tol)
{
    LONG w = doc->w, n = w * doc->h, i, qh = 0, qt = 0;
    UBYTE *t;
    LONG *q;
    if (!doc->hassel)
        return TRUE;
    doc_process_dirty();
    if (!(t = gs_table(tol)))
        return FALSE;
    if (similar) {
        for (i = 0; i < n; i++)
            if (doc->sel[i] < 255 && gs_match(t, i))
                doc->sel[i] = 255;
    } else {
        /* Grow: von der Auswahl aus in passende Nachbarn wachsen */
        if (!(q = AllocVec(n * sizeof(LONG), MEMF_ANY))) {
            FreeVec(t);
            return FALSE;
        }
        for (i = 0; i < n; i++)
            if (doc->sel[i] >= 128) {
                doc->sel[i] = 255;
                q[qt++] = i;
            }
        while (qh < qt) {
            LONG p = q[qh++], x = p % w, d;
            for (d = 0; d < 4; d++) {
                LONG j = d == 0 ? (x > 0 ? p - 1 : -1) : d == 1 ? (x < w - 1 ? p + 1 : -1) :
                         d == 2 ? p - w : p + w;
                if (j < 0 || j >= n || doc->sel[j] == 255 || !gs_match(t, j))
                    continue;
                doc->sel[j] = 255;
                q[qt++] = j;
            }
        }
        FreeVec(q);
    }
    FreeVec(t);
    sel_update_bounds();
    return TRUE;
}
