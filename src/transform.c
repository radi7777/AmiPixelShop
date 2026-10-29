/*
 * transform.c - Frei transformieren (Strg+T), Spiegeln, Drehen
 *
 * Wie beim Verschieben werden die Pixel herausgeloest (mit Auswahl) bzw. die
 * ganze Ebene genommen und als "schwebender" Puffer (premultiplied) gehalten.
 * Abbildung: Ziel = C + R(winkel) * S * (Quelle - Quellmitte).
 * Gerendert wird rueckwaerts (je Zielpixel die Quellposition), die innere
 * Schleife laeuft affin mit konstanten Schrittweiten.
 * Distort/Perspective: vier freie Ecken, projektive Abbildung (Heckbert,
 * Einheitsquadrat -> Viereck, invertiert). Warp: 4x4 Kontrollpunkte einer
 * Bezier-Flaeche; die Rueckwaertsabbildung wird je Zelle (inverse bilineare
 * Abbildung) in eine Tabelle uvmap gerechnet.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "apx.h"

static BOOL tf_on;
static Layer *tf_layer;
static Tile **tf_orig;          /* Kopien aller Kacheln vorher */
static Tile **tf_base;          /* Ebene ohne die schwebenden Pixel */
static UBYTE *tf_base_own;
static UBYTE *tf_touched;
static ULONG *fpx;              /* schwebende Pixel, premultiplied */
static UBYTE *fsel;             /* zugehoerige Auswahl (oder NULL) */
static LONG fx0, fy0, fw, fh;   /* Lage der Quelle */
static BOOL tf_hadsel;
static BOOL tf_text;            /* Textebene ohne Auswahl: bleibt Text (Abbildung geht in die Matrix) */

/* aktuelle Abbildung */
static float tcx, tcy, tsx = 1.0f, tsy = 1.0f, tang;
static LONG bx0, by0, bx1, by1; /* zuletzt gerenderter Zielbereich */
static BOOL tf_rendered;

/* Freie Formen */
enum { TM_AFFINE, TM_DISTORT, TM_PERSP, TM_WARP };
static LONG tf_mode;
static float qx[4], qy[4];      /* Ecken: oben links, oben rechts, unten rechts, unten links */
static float wx[16], wy[16];    /* Warp-Kontrollpunkte, Zeile fuer Zeile */
static double hinv[9];          /* Ziel -> Quelle (0..1) */
static float *uvmap;            /* Warp: je Zielpixel u, v in Quellpixeln (u < -1e8: leer) */
static LONG uv_x0, uv_y0, uv_w, uv_h;
#define WARP_N 16               /* Zellen je Richtung */

/* Ziehen */
enum { DR_NONE, DR_MOVE, DR_ROTATE, DR_SCALE, DR_POINT };
static float d_qx[4], d_qy[4], d_wx[16], d_wy[16];
static LONG drag, drag_handle;
static float d_mx, d_my, d_cx, d_cy, d_sx, d_sy, d_ang;
static LONG ov_x0, ov_y0, ov_x1, ov_y1;    /* Overlay-Box am Schirm */

BOOL transform_active(void)
{
    return tf_on;
}

/* ---- Hilfen ---- */

static ULONG premul(ULONG p)
{
    ULONG a = p >> 24, a2 = a + (a >> 7);
    if (a == 255)
        return p;
    return (a << 24) | ((((p & 0xFF00FF) * a2) >> 8) & 0xFF00FF) | ((((p & 0x00FF00) * a2) >> 8) & 0x00FF00);
}

static ULONG unpremul(ULONG p)
{
    ULONG a = p >> 24, inv, r, g, b;
    if (a == 255 || !a)
        return a ? p : 0;
    inv = (255 * 65536 + a / 2) / a;
    r = (((p >> 16) & 0xFF) * inv) >> 16;
    g = (((p >> 8) & 0xFF) * inv) >> 16;
    b = ((p & 0xFF) * inv) >> 16;
    return (a << 24) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}

/* straight src-over */
static ULONG over(ULONG d, ULONG s)
{
    ULONG sa = s >> 24, da, oa, inv;
    if (!sa)
        return d;
    if (sa == 255)
        return s;
    da = ((d >> 24) * (255 - sa) * 257 + 0x8080) >> 16;
    oa = sa + da;
    inv = inv_tab[oa];
    return (oa << 24)
         | (((((s >> 16) & 0xFF) * sa + ((d >> 16) & 0xFF) * da) * inv >> 16) << 16)
         | (((((s >> 8) & 0xFF) * sa + ((d >> 8) & 0xFF) * da) * inv >> 16) << 8)
         | (((s & 0xFF) * sa + (d & 0xFF) * da) * inv >> 16);
}

/* Quellpunkt (relativ zur Quellmitte, unskaliert) -> Dokument */
static void map_pt(float u, float v, float *x, float *y)
{
    float c = cosf(tang), s = sinf(tang);
    u *= tsx;
    v *= tsy;
    *x = tcx + c * u - s * v;
    *y = tcy + s * u + c * v;
}

/* Ecke i (0 oben links, im Uhrzeigersinn) bzw. Kantenmitte i (4..7) */
static void handle_pt(LONG i, float *x, float *y)
{
    static const float hu[8] = { -1, 1, 1, -1, 0, 1, 0, -1 };
    static const float hv[8] = { -1, -1, 1, 1, -1, 0, 1, 0 };
    map_pt(hu[i] * fw * 0.5f, hv[i] * fh * 0.5f, x, y);
}

static void dest_bounds(LONG *x0, LONG *y0, LONG *x1, LONG *y1)
{
    float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f, x, y;
    LONG i, n = tf_mode == TM_WARP ? 16 : 4;
    for (i = 0; i < n; i++) {
        if (tf_mode == TM_AFFINE)
            handle_pt(i, &x, &y);
        else if (tf_mode == TM_WARP) {
            x = wx[i]; y = wy[i];   /* Bezier liegt in der Huelle der Punkte */
        } else {
            x = qx[i]; y = qy[i];
        }
        if (x < mnx) mnx = x;
        if (y < mny) mny = y;
        if (x > mxx) mxx = x;
        if (y > mxy) mxy = y;
    }
    *x0 = (LONG)floorf(mnx) - 1;
    *y0 = (LONG)floorf(mny) - 1;
    *x1 = (LONG)ceilf(mxx) + 1;
    *y1 = (LONG)ceilf(mxy) + 1;
}

/* ---- Freie Formen ---- */

static void build_homography(void)
{
    double x0 = qx[0], y0 = qy[0], x1 = qx[1], y1 = qy[1], x2 = qx[2], y2 = qy[2], x3 = qx[3], y3 = qy[3];
    double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3, g = 0, h = 0;
    double a, b, c, d, e, f, A, B, C, D, E, F, G, H, I, det;
    if (fabs(sx) > 1e-9 || fabs(sy) > 1e-9) {
        double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2, den = dx1 * dy2 - dx2 * dy1;
        if (fabs(den) > 1e-12) {
            g = (sx * dy2 - dx2 * sy) / den;
            h = (dx1 * sy - sx * dy1) / den;
        }
    }
    a = x1 - x0 + g * x1; b = x3 - x0 + h * x3; c = x0;
    d = y1 - y0 + g * y1; e = y3 - y0 + h * y3; f = y0;
    /* Inverse ueber die Adjunkte (i = 1) */
    A = e - f * h; B = -(d - f * g); C = d * h - e * g;
    D = -(b - c * h); E = a - c * g; F = -(a * h - b * g);
    G = b * f - c * e; H = -(a * f - c * d); I = a * e - b * d;
    det = a * A + b * B + c * C;
    if (fabs(det) < 1e-12)
        det = 1e-12;
    hinv[0] = A / det; hinv[1] = D / det; hinv[2] = G / det;
    hinv[3] = B / det; hinv[4] = E / det; hinv[5] = H / det;
    hinv[6] = C / det; hinv[7] = F / det; hinv[8] = I / det;
}

/* Bezier-Flaeche an (s, t) */
static void warp_eval(float s, float t, float *x, float *y)
{
    float bs[4], bt[4], us = 1.0f - s, ut = 1.0f - t, sx = 0, sy = 0;
    LONG i, j;
    bs[0] = us * us * us; bs[1] = 3 * us * us * s; bs[2] = 3 * us * s * s; bs[3] = s * s * s;
    bt[0] = ut * ut * ut; bt[1] = 3 * ut * ut * t; bt[2] = 3 * ut * t * t; bt[3] = t * t * t;
    for (j = 0; j < 4; j++)
        for (i = 0; i < 4; i++) {
            float w = bs[i] * bt[j];
            sx += w * wx[j * 4 + i];
            sy += w * wy[j * 4 + i];
        }
    *x = sx;
    *y = sy;
}

static float crs(float ax, float ay, float bx, float by) { return ax * by - ay * bx; }

/* Warp: Rueckwaertstabelle fuer den Zielbereich neu rechnen */
static void build_uvmap(LONG x0, LONG y0, LONG x1, LONG y1)
{
    static float gx[(WARP_N + 1) * (WARP_N + 1)], gy[(WARP_N + 1) * (WARP_N + 1)];
    LONG i, j, n;
    if (uvmap)
        FreeVec(uvmap);
    uv_x0 = x0; uv_y0 = y0; uv_w = x1 - x0; uv_h = y1 - y0;
    uvmap = NULL;
    if (uv_w <= 0 || uv_h <= 0 || !(uvmap = AllocVec(uv_w * uv_h * 2 * sizeof(float), MEMF_ANY)))
        return;
    n = uv_w * uv_h * 2;
    for (i = 0; i < n; i += 2)
        uvmap[i] = -1e9f;
    for (j = 0; j <= WARP_N; j++)
        for (i = 0; i <= WARP_N; i++)
            warp_eval((float)i / WARP_N, (float)j / WARP_N, &gx[j * (WARP_N + 1) + i], &gy[j * (WARP_N + 1) + i]);
    for (j = 0; j < WARP_N; j++)
        for (i = 0; i < WARP_N; i++) {
            LONG k = j * (WARP_N + 1) + i, px, py, bx0, by0, bx1, by1;
            float ax = gx[k], ay = gy[k], bx = gx[k + 1], by = gy[k + 1];
            float cx = gx[k + WARP_N + 2], cy = gy[k + WARP_N + 2], dx = gx[k + WARP_N + 1], dy = gy[k + WARP_N + 1];
            float ex = bx - ax, ey = by - ay, fx = dx - ax, fy = dy - ay;
            float hx = ax - bx + cx - dx, hy = ay - by + cy - dy;
            float cxs[4], cys[4], mnx = ax, mxx = ax, mny = ay, mxy = ay;
            LONG c;
            cxs[0] = ax; cxs[1] = bx; cxs[2] = cx; cxs[3] = dx;
            cys[0] = ay; cys[1] = by; cys[2] = cy; cys[3] = dy;
            for (c = 1; c < 4; c++) {
                if (cxs[c] < mnx) mnx = cxs[c];
                if (cxs[c] > mxx) mxx = cxs[c];
                if (cys[c] < mny) mny = cys[c];
                if (cys[c] > mxy) mxy = cys[c];
            }
            bx0 = (LONG)floorf(mnx); by0 = (LONG)floorf(mny);
            bx1 = (LONG)ceilf(mxx) + 1; by1 = (LONG)ceilf(mxy) + 1;
            if (bx0 < x0) bx0 = x0;
            if (by0 < y0) by0 = y0;
            if (bx1 > x1) bx1 = x1;
            if (by1 > y1) by1 = y1;
            for (py = by0; py < by1; py++)
                for (px = bx0; px < bx1; px++) {
                    /* inverse bilineare Abbildung (Zelle A B C D) */
                    float qpx = px + 0.5f - ax, qpy = py + 0.5f - ay;
                    float k2 = crs(hx, hy, fx, fy), k1 = crs(ex, ey, fx, fy) + crs(qpx, qpy, hx, hy);
                    float k0 = crs(qpx, qpy, ex, ey), u, v, den;
                    if (fabsf(k2) < 1e-6f) {
                        if (fabsf(k1) < 1e-9f)
                            continue;
                        v = -k0 / k1;
                    } else {
                        float w = k1 * k1 - 4.0f * k0 * k2, ik2;
                        if (w < 0.0f)
                            continue;
                        w = sqrtf(w);
                        ik2 = 0.5f / k2;
                        v = (-k1 - w) * ik2;
                        if (v < -0.001f || v > 1.001f)
                            v = (-k1 + w) * ik2;
                    }
                    den = fabsf(ex + hx * v) > fabsf(ey + hy * v) ? ex + hx * v : ey + hy * v;
                    if (fabsf(den) < 1e-9f)
                        continue;
                    u = fabsf(ex + hx * v) > fabsf(ey + hy * v) ? (qpx - fx * v) / den : (qpy - fy * v) / den;
                    if (u < -0.001f || u > 1.001f || v < -0.001f || v > 1.001f)
                        continue;
                    {
                        float *m = uvmap + ((py - y0) * uv_w + (px - x0)) * 2;
                        m[0] = (i + u) / WARP_N * fw;
                        m[1] = (j + v) / WARP_N * fh;
                    }
                }
        }
}

/* Quellposition (Pufferpixel) fuer den Zielpunkt (x, y); FALSE = keine */
static BOOL src_uv(float x, float y, float *u, float *v)
{
    if (tf_mode == TM_WARP) {
        LONG ix = (LONG)floorf(x) - uv_x0, iy = (LONG)floorf(y) - uv_y0;
        float *m;
        if (!uvmap || ix < 0 || iy < 0 || ix >= uv_w || iy >= uv_h)
            return FALSE;
        m = uvmap + (iy * uv_w + ix) * 2;
        if (m[0] < -1e8f)
            return FALSE;
        *u = m[0];
        *v = m[1];
        return TRUE;
    } else {
        double W = hinv[6] * x + hinv[7] * y + hinv[8];
        if (W <= 1e-12)
            return FALSE;
        *u = (float)((hinv[0] * x + hinv[1] * y + hinv[2]) / W * fw);
        *v = (float)((hinv[3] * x + hinv[4] * y + hinv[5]) / W * fh);
        return TRUE;
    }
}

/* Modus wechseln: aktuelle Form uebernehmen */
void transform_set_mode(LONG m)
{
    LONG i, j;
    if (!tf_on || m == tf_mode)
        return;
    if (tf_mode == TM_AFFINE)
        for (i = 0; i < 4; i++)
            handle_pt(i, &qx[i], &qy[i]);
    else if (tf_mode == TM_WARP) {
        qx[0] = wx[0]; qy[0] = wy[0]; qx[1] = wx[3]; qy[1] = wy[3];
        qx[2] = wx[15]; qy[2] = wy[15]; qx[3] = wx[12]; qy[3] = wy[12];
    }
    if (m == TM_WARP)
        for (j = 0; j < 4; j++)
            for (i = 0; i < 4; i++) {
                float s = i / 3.0f, t = j / 3.0f;
                wx[j * 4 + i] = (1 - s) * (1 - t) * qx[0] + s * (1 - t) * qx[1] + s * t * qx[2] + (1 - s) * t * qx[3];
                wy[j * 4 + i] = (1 - s) * (1 - t) * qy[0] + s * (1 - t) * qy[1] + s * t * qy[2] + (1 - s) * t * qy[3];
            }
    if (m == TM_AFFINE) {
        /* zurueck ins Affine geht nur ohne Verformung: Form bleibt, Modus nicht */
        return;
    }
    tf_mode = m;
    if (m != TM_WARP)
        build_homography();
    tf_rendered = TRUE;
    view_inval_all();
}

/* Anfasser i des aktuellen Modus (Dokument); Rueckgabe: Anzahl */
static LONG ctl_pt(LONG i, float *x, float *y)
{
    *x = *y = 0.0f;
    if (tf_mode == TM_AFFINE) {
        if (i < 8)
            handle_pt(i, x, y);
        return 8;
    }
    if (tf_mode == TM_WARP) {
        if (i < 16) {
            *x = wx[i];
            *y = wy[i];
        }
        return 16;
    }
    if (i < 4) {
        *x = qx[i];
        *y = qy[i];
    } else if (i < 8) {             /* Kantenmitten */
        *x = (qx[i - 4] + qx[(i - 3) & 3]) * 0.5f;
        *y = (qy[i - 4] + qy[(i - 3) & 3]) * 0.5f;
    }
    return 8;
}

static LONG ctl_count(void)
{
    return tf_mode == TM_WARP ? 16 : 8;
}

LONG transform_mode(void)
{
    return tf_on ? tf_mode : -1;
}

/* ---- Overlay: Rahmen und Anfasser ---- */

static void tf_overlay(void)
{
    float px[8], py[8];
    LONG i, sx[8], sy[8];
    if (!tf_on)
        return;
    if (tf_mode != TM_AFFINE) {
        LONG n = ctl_count(), k;
        if (tf_mode == TM_WARP) {
            /* Netz: je 4 Linien in beide Richtungen, als Kurven */
            for (k = 0; k < 4; k++) {
                LONG j, lx = 0, ly = 0, mx = 0, my = 0;
                for (j = 0; j <= 24; j++) {
                    float a, b, c, d;
                    warp_eval(k / 3.0f, j / 24.0f, &a, &b);
                    warp_eval(j / 24.0f, k / 3.0f, &c, &d);
                    a = ox + a * zoom; b = oy + b * zoom; c = ox + c * zoom; d = oy + d * zoom;
                    if (j) {
                        line(lx, ly, (LONG)a, (LONG)b, 0xFFFFFFFF);
                        line(mx, my, (LONG)c, (LONG)d, 0xFFFFFFFF);
                    }
                    lx = (LONG)a; ly = (LONG)b; mx = (LONG)c; my = (LONG)d;
                }
            }
        } else
            for (k = 0; k < 4; k++)
                ant_line(ox + (LONG)(qx[k] * zoom), oy + (LONG)(qy[k] * zoom),
                         ox + (LONG)(qx[(k + 1) & 3] * zoom), oy + (LONG)(qy[(k + 1) & 3] * zoom), 0);
        for (k = 0; k < n; k++) {
            float x, y;
            LONG a, b;
            ctl_pt(k, &x, &y);
            a = ox + (LONG)floorf(x * zoom + 0.5f);
            b = oy + (LONG)floorf(y * zoom + 0.5f);
            fill(a - 3, b - 3, 7, 7, 0xFFFFFFFF);
            frame(a - 4, b - 4, 9, 9, 0xFF000000);
        }
        return;
    }
    for (i = 0; i < 8; i++) {
        handle_pt(i, &px[i], &py[i]);
        sx[i] = ox + (LONG)floorf(px[i] * zoom + 0.5f);
        sy[i] = oy + (LONG)floorf(py[i] * zoom + 0.5f);
    }
    for (i = 0; i < 4; i++)
        ant_line(sx[i], sy[i], sx[(i + 1) & 3], sy[(i + 1) & 3], 0);
    for (i = 0; i < 8; i++) {
        fill(sx[i] - 3, sy[i] - 3, 7, 7, 0xFFFFFFFF);
        frame(sx[i] - 4, sy[i] - 4, 9, 9, 0xFF000000);
    }
    {
        LONG cx = ox + (LONG)floorf(tcx * zoom + 0.5f), cy = oy + (LONG)floorf(tcy * zoom + 0.5f);
        line(cx - 4, cy, cx + 4, cy, 0xFF000000);
        line(cx, cy - 4, cx, cy + 4, 0xFF000000);
    }
}

static void overlay_changed(void)
{
    LONG i, x0 = 0x7FFFFFFF, y0 = 0x7FFFFFFF, x1 = -0x7FFFFFFF, y1 = -0x7FFFFFFF;
    view_inval(ov_x0, ov_y0, ov_x1, ov_y1);
    for (i = 0; i < ctl_count(); i++) {
        float x, y;
        LONG s, t;
        ctl_pt(i, &x, &y);
        s = ox + (LONG)floorf(x * zoom + 0.5f);
        t = oy + (LONG)floorf(y * zoom + 0.5f);
        if (s < x0) x0 = s;
        if (t < y0) y0 = t;
        if (s > x1) x1 = s;
        if (t > y1) y1 = t;
    }
    ov_x0 = x0 - 6; ov_y0 = y0 - 6;
    ov_x1 = x1 + 7; ov_y1 = y1 + 7;
    view_inval(ov_x0, ov_y0, ov_x1, ov_y1);
}

/* ---- Rendern ---- */

/* Kacheln im Rechteck aus tf_base + transformierter Quelle neu bauen */
static void render_rect(LONG x0, LONG y0, LONG x1, LONG y1, BOOL smooth)
{
    float c = cosf(tang), s = sinf(tang);
    /* inverse Abbildung: Quelle = M * (Ziel - C) + Quellmitte (in Pufferkoord.) */
    float m00 = c / tsx, m01 = s / tsx, m10 = -s / tsy, m11 = c / tsy;
    LONG tx, ty;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    if (x0 >= x1 || y0 >= y1)
        return;

    for (ty = y0 >> TILE_SHIFT; ty <= (y1 - 1) >> TILE_SHIFT; ty++)
        for (tx = x0 >> TILE_SHIFT; tx <= (x1 - 1) >> TILE_SHIFT; tx++) {
            LONG idx = ty * doc->tw + tx, gx0 = tx << TILE_SHIFT, gy0 = ty << TILE_SHIFT, x, y;
            Tile *t = tf_layer->tiles[idx];
            if (!t && !(t = tile_new()))
                continue;
            tf_layer->tiles[idx] = t;
            if (tf_base[idx])
                memcpy(t->px, tf_base[idx]->px, sizeof(t->px));
            else
                memset(t->px, 0, sizeof(t->px));
            for (y = 0; y < TILE_SIZE; y++) {
                LONG gy = gy0 + y;
                float dy = gy + 0.5f - tcy;
                /* Quellkoordinaten am Zeilenanfang, Schritt je Pixel */
                float dx = gx0 + 0.5f - tcx;
                float u = m00 * dx + m01 * dy + fw * 0.5f;
                float v = m10 * dx + m11 * dy + fh * 0.5f;
                ULONG *row = t->px + (y << TILE_SHIFT);
                if (gy >= doc->h)
                    break;
                for (x = 0; x < TILE_SIZE; x++, u += m00, v += m10) {
                    ULONG p;
                    if (gx0 + x >= doc->w)
                        break;
                    if (tf_mode != TM_AFFINE && !src_uv(gx0 + x + 0.5f, gy + 0.5f, &u, &v))
                        continue;
                    if (!smooth) {
                        LONG iu = (LONG)floorf(u), iv = (LONG)floorf(v);
                        if (iu < 0 || iv < 0 || iu >= fw || iv >= fh)
                            continue;
                        p = fpx[iv * fw + iu];
                    } else {
                        /* bilinear, ausserhalb = transparent (weiche Kanten) */
                        float uu = u - 0.5f, vv = v - 0.5f;
                        LONG iu = (LONG)floorf(uu), iv = (LONG)floorf(vv), k;
                        ULONG q[4], wu, wv, out = 0;
                        if (iu < -1 || iv < -1 || iu >= fw || iv >= fh)
                            continue;
                        wu = (ULONG)((uu - iu) * 256.0f);
                        wv = (ULONG)((vv - iv) * 256.0f);
                        q[0] = (iu >= 0 && iv >= 0) ? fpx[iv * fw + iu] : 0;
                        q[1] = (iu + 1 < fw && iv >= 0) ? fpx[iv * fw + iu + 1] : 0;
                        q[2] = (iu >= 0 && iv + 1 < fh) ? fpx[(iv + 1) * fw + iu] : 0;
                        q[3] = (iu + 1 < fw && iv + 1 < fh) ? fpx[(iv + 1) * fw + iu + 1] : 0;
                        for (k = 0; k < 32; k += 8) {
                            ULONG a = (q[0] >> k) & 0xFF, b = (q[1] >> k) & 0xFF;
                            ULONG cc = (q[2] >> k) & 0xFF, d = (q[3] >> k) & 0xFF;
                            ULONG top = a * (256 - wu) + b * wu, bot = cc * (256 - wu) + d * wu;
                            out |= (((top * (256 - wv) + bot * wv) + 32768) >> 16) << k;
                        }
                        p = out;
                    }
                    if (p)
                        row[x] = over(row[x], unpremul(p));
                }
            }
            tile_update_flag(doc, t, tx, ty);
            if (t->flag == TF_EMPTY) {
                FreeVec(t);
                tf_layer->tiles[idx] = NULL;
            }
            tf_touched[idx] = 1;
            doc_mark_dirty(tx, ty);
        }
}

static void render(BOOL smooth)
{
    LONG x0, y0, x1, y1;
    dest_bounds(&x0, &y0, &x1, &y1);
    if (tf_mode == TM_WARP)
        build_uvmap(x0 < 0 ? 0 : x0, y0 < 0 ? 0 : y0, x1 > doc->w ? doc->w : x1, y1 > doc->h ? doc->h : y1);
    else if (tf_mode != TM_AFFINE)
        build_homography();
    if (tf_rendered) {
        /* alte Lage freiraeumen (falls ausserhalb der neuen) */
        if (bx0 < x0 || by0 < y0 || bx1 > x1 || by1 > y1) {
            LONG ux0 = bx0 < x0 ? bx0 : x0, uy0 = by0 < y0 ? by0 : y0;
            LONG ux1 = bx1 > x1 ? bx1 : x1, uy1 = by1 > y1 ? by1 : y1;
            render_rect(ux0, uy0, ux1, uy1, smooth);
        } else
            render_rect(x0, y0, x1, y1, smooth);
    } else {
        /* erstes Mal: auch die Ausgangslage (Loch) neu bauen */
        LONG ux0 = fx0 < x0 ? fx0 : x0, uy0 = fy0 < y0 ? fy0 : y0;
        LONG ux1 = fx0 + fw > x1 ? fx0 + fw : x1, uy1 = fy0 + fh > y1 ? fy0 + fh : y1;
        render_rect(ux0, uy0, ux1, uy1, smooth);
    }
    bx0 = x0; by0 = y0; bx1 = x1; by1 = y1;
    tf_rendered = TRUE;
    overlay_changed();
}

/* ---- Beginn / Ende ---- */

static void free_state(void)
{
    LONG i, n = doc->tw * doc->th;
    for (i = 0; i < n; i++) {
        if (tf_base_own && tf_base_own[i] && tf_base[i])
            FreeVec(tf_base[i]);
        if (tf_orig && tf_orig[i])
            FreeVec(tf_orig[i]);
    }
    if (tf_orig) FreeVec(tf_orig);
    if (tf_base) FreeVec(tf_base);
    if (tf_base_own) FreeVec(tf_base_own);
    if (tf_touched) FreeVec(tf_touched);
    if (fpx) FreeVec(fpx);
    if (fsel) FreeVec(fsel);
    tf_orig = tf_base = NULL;
    tf_base_own = tf_touched = NULL;
    fpx = NULL;
    fsel = NULL;
    if (uvmap) FreeVec(uvmap);
    uvmap = NULL;
    tf_mode = TM_AFFINE;
    tf_on = FALSE;
    view_transform_overlay = NULL;
}

BOOL transform_begin(void)
{
    LONG n = doc->tw * doc->th, i, x, y;
    if (tf_on)
        return TRUE;
    tf_text = doc->layers[doc->active]->text && !doc->hassel;
    if (!tf_text)
        doc_rasterize_text(doc->layers[doc->active]);
    doc_cache_below(doc->active);   /* Unterbau aendert sich beim Ziehen nicht */
    tf_layer = doc->layers[doc->active];
    tf_hadsel = doc->hassel;
    tf_orig = AllocVec(n * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR);
    tf_base = AllocVec(n * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR);
    tf_base_own = AllocVec(n, MEMF_ANY | MEMF_CLEAR);
    tf_touched = AllocVec(n, MEMF_ANY | MEMF_CLEAR);
    if (!tf_orig || !tf_base || !tf_base_own || !tf_touched)
        goto fail;
    for (i = 0; i < n; i++)
        if (tf_layer->tiles[i] && !(tf_orig[i] = tile_clone(tf_layer->tiles[i])))
            goto fail;

    if (tf_hadsel) {
        fx0 = doc->selx0; fy0 = doc->sely0;
        fw = doc->selx1 - fx0; fh = doc->sely1 - fy0;
        for (i = 0; i < n; i++)
            tf_base[i] = tf_orig[i];
    } else {
        /* Inhalt der Ebene eingrenzen */
        LONG x0 = doc->w, y0 = doc->h, x1 = 0, y1 = 0;
        for (y = 0; y < doc->h; y++)
            for (x = 0; x < doc->w; x++)
                if (layer_get(tf_layer, x, y) >> 24) {
                    if (x < x0) x0 = x;
                    if (x >= x1) x1 = x + 1;
                    if (y < y0) y0 = y;
                    y1 = y + 1;
                }
        if (x1 <= x0) {
            status_message("The layer is empty");
            free_state();
            doc_cache_drop();
            return FALSE;
        }
        fx0 = x0; fy0 = y0; fw = x1 - x0; fh = y1 - y0;
        /* Basis: leer (die ganze Ebene schwebt) */
    }
    fpx = AllocVec(fw * fh * 4, MEMF_ANY);
    if (tf_hadsel)
        fsel = AllocVec(fw * fh, MEMF_ANY);
    if (!fpx || (tf_hadsel && !fsel))
        goto fail;
    for (y = 0; y < fh; y++)
        for (x = 0; x < fw; x++) {
            LONG gx = fx0 + x, gy = fy0 + y;
            ULONG p = layer_get(tf_layer, gx, gy);
            if (tf_hadsel) {
                ULONG s = doc->sel[gy * doc->w + gx], a = p >> 24;
                LONG idx = (gy >> TILE_SHIFT) * doc->tw + (gx >> TILE_SHIFT);
                fsel[y * fw + x] = (UBYTE)s;
                p = (((a * s * 257 + 0x8080) >> 16) << 24) | (p & 0xFFFFFF);
                /* Loch in die Basis stanzen */
                if (s && a && tf_orig[idx]) {
                    ULONG *q;
                    if (!tf_base_own[idx]) {
                        Tile *c = tile_clone(tf_orig[idx]);
                        if (c) {
                            tf_base[idx] = c;
                            tf_base_own[idx] = 1;
                        }
                    }
                    if (tf_base_own[idx]) {
                        q = &tf_base[idx]->px[((gy & TILE_MASK) << TILE_SHIFT) | (gx & TILE_MASK)];
                        *q = ((((*q >> 24) * (255 - s) * 257 + 0x8080) >> 16) << 24) | (*q & 0xFFFFFF);
                    }
                }
            }
            fpx[y * fw + x] = premul(p);
        }
    tcx = fx0 + fw * 0.5f;
    tcy = fy0 + fh * 0.5f;
    tsx = tsy = 1.0f;
    tang = 0.0f;
    tf_rendered = FALSE;
    drag = DR_NONE;
    tf_on = TRUE;
    if (tf_hadsel) {
        doc->hassel = FALSE;    /* waehrend des Transformierens keine Ameisen */
        view_ants_rebuild();
    }
    view_transform_overlay = tf_overlay;
    ov_x0 = ov_x1 = 0;
    overlay_changed();
    return TRUE;
fail:
    doc_cache_drop();
    free_state();
    status_message("Not enough memory");
    return FALSE;
}

/* Auswahl mittransformieren (naechster Nachbar) */
static void transform_selection(void)
{
    float c = cosf(tang), s = sinf(tang);
    float m00 = c / tsx, m01 = s / tsx, m10 = -s / tsy, m11 = c / tsy;
    LONG x, y, x0, y0, x1, y1;
    memset(doc->sel, 0, doc->w * doc->h);
    dest_bounds(&x0, &y0, &x1, &y1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            float dx = x + 0.5f - tcx, dy = y + 0.5f - tcy, fu, fv;
            LONG iu = (LONG)floorf(m00 * dx + m01 * dy + fw * 0.5f);
            LONG iv = (LONG)floorf(m10 * dx + m11 * dy + fh * 0.5f);
            if (tf_mode != TM_AFFINE) {
                if (!src_uv(x + 0.5f, y + 0.5f, &fu, &fv))
                    continue;
                iu = (LONG)floorf(fu);
                iv = (LONG)floorf(fv);
            }
            if (iu >= 0 && iv >= 0 && iu < fw && iv < fh)
                doc->sel[y * doc->w + x] = fsel[iv * fw + iu];
        }
    sel_update_bounds();
}

void transform_apply(void)
{
    UndoRec *u;
    LONG i, n;
    if (!tf_on)
        return;
    if (!tf_rendered && tsx == 1.0f && tsy == 1.0f && tang == 0.0f) {
        transform_cancel();     /* nichts veraendert */
        return;
    }
    if (tf_text && tf_mode == TM_AFFINE) {
        /* Text: Vorschau verwerfen, Abbildung auf Anker und Matrix, neu rendern */
        float c = cosf(tang), s = sinf(tang), L[4];
        float sx = fx0 + fw * 0.5f, sy = fy0 + fh * 0.5f, dx = tcx, dy = tcy;
        Layer *l = tf_layer;
        L[0] = c * tsx; L[1] = -s * tsy; L[2] = s * tsx; L[3] = c * tsy;
        transform_cancel();
        text_transform(l, L, sx, sy, dx, dy, "Free Transform");
        return;
    }
    if (tf_text)
        doc_rasterize_text(tf_layer);   /* Distort/Perspective/Warp: wie Photoshop zu Pixeln */
    render(TRUE);               /* endgueltig in voller Qualitaet */
    doc_cache_drop();
    n = doc->tw * doc->th;
    u = undo_begin(tf_layer, "Free Transform");
    for (i = 0; i < n; i++)
        if (tf_touched[i]) {
            undo_add(u, i, tf_orig[i]);
            tf_orig[i] = NULL;
        }
    undo_commit(u);
    if (tf_hadsel)
        transform_selection();
    view_inval(ov_x0, ov_y0, ov_x1, ov_y1);
    free_state();
    view_ants_rebuild();
}

void transform_cancel(void)
{
    LONG i, n;
    if (!tf_on)
        return;
    doc_cache_drop();
    n = doc->tw * doc->th;
    for (i = 0; i < n; i++)
        if (tf_touched[i]) {
            if (tf_layer->tiles[i])
                FreeVec(tf_layer->tiles[i]);
            tf_layer->tiles[i] = tf_orig[i];
            tf_orig[i] = NULL;
            doc_mark_dirty(i % doc->tw, i / doc->tw);
        }
    if (tf_hadsel) {
        doc->hassel = TRUE;
        sel_update_bounds();
    }
    view_inval(ov_x0, ov_y0, ov_x1, ov_y1);
    free_state();
    view_ants_rebuild();
}

/* Spiegeln / Drehen in einem Schritt (naechster Nachbar = pixelgenau) */
void transform_quick(float sx, float sy, float ang)
{
    Layer *l = doc->layers[doc->active];
    if (l->text && !doc->hassel) {
        /* Text bleibt Text: um die Mitte des Umrisses */
        TextInfo *t = l->text;
        float c = cosf(ang), s = sinf(ang), L[4];
        float mx = (t->bx0 + t->bx1) * 0.5f, my = (t->by0 + t->by1) * 0.5f;
        L[0] = c * sx; L[1] = -s * sy; L[2] = s * sx; L[3] = c * sy;
        text_transform(l, L, mx, my, mx, my, "Transform");
        return;
    }
    if (!transform_begin())
        return;
    tsx = sx;
    tsy = sy;
    tang = ang;
    render(FALSE);
    {
        /* render(TRUE) in apply wuerde bei 90-Grad-Schritten weichzeichnen */
        UndoRec *u;
        LONG i, n = doc->tw * doc->th;
        u = undo_begin(tf_layer, "Transform");
        for (i = 0; i < n; i++)
            if (tf_touched[i]) {
                undo_add(u, i, tf_orig[i]);
                tf_orig[i] = NULL;
            }
        undo_commit(u);
        if (tf_hadsel)
            transform_selection();
        view_inval(ov_x0, ov_y0, ov_x1, ov_y1);
        free_state();
        view_ants_rebuild();
    }
}

/* ---- Maus ---- */

static LONG hit_handle(LONG mx, LONG my)
{
    LONG i, n = ctl_count();
    for (i = 0; i < n; i++) {
        float x, y;
        LONG sx, sy;
        ctl_pt(i, &x, &y);
        sx = ox + (LONG)floorf(x * zoom + 0.5f);
        sy = oy + (LONG)floorf(y * zoom + 0.5f);
        if (mx >= sx - 6 && mx <= sx + 6 && my >= sy - 6 && my <= sy + 6)
            return i;
    }
    return -1;
}

/* Punkt im (gedrehten) Rahmen? */
static BOOL inside(float x, float y)
{
    if (tf_mode != TM_AFFINE) {
        /* Viereck aus den Eckpunkten (Warp: aeussere Ecken), Vorzeichen der Kreuzprodukte */
        float px[4], py[4];
        LONG k, pos = 0, neg = 0;
        if (tf_mode == TM_WARP) {
            px[0] = wx[0]; py[0] = wy[0]; px[1] = wx[3]; py[1] = wy[3];
            px[2] = wx[15]; py[2] = wy[15]; px[3] = wx[12]; py[3] = wy[12];
        } else
            for (k = 0; k < 4; k++) {
                px[k] = qx[k];
                py[k] = qy[k];
            }
        for (k = 0; k < 4; k++) {
            float c = crs(px[(k + 1) & 3] - px[k], py[(k + 1) & 3] - py[k], x - px[k], y - py[k]);
            if (c > 0) pos++;
            if (c < 0) neg++;
        }
        return !pos || !neg;
    }
    float c = cosf(-tang), s = sinf(-tang), dx = x - tcx, dy = y - tcy;
    float u = c * dx - s * dy, v = s * dx + c * dy;
    return fabsf(u) <= fabsf(tsx) * fw * 0.5f && fabsf(v) <= fabsf(tsy) * fh * 0.5f;
}

void transform_mouse_down(LONG mx, LONG my, BOOL shift)
{
    float x = scr2docx(mx), y = scr2docy(my);
    LONG h = hit_handle(mx, my);
    d_mx = x; d_my = y;
    d_cx = tcx; d_cy = tcy;
    d_sx = tsx; d_sy = tsy;
    d_ang = tang;
    if (tf_mode != TM_AFFINE) {
        memcpy(d_qx, qx, sizeof(qx)); memcpy(d_qy, qy, sizeof(qy));
        memcpy(d_wx, wx, sizeof(wx)); memcpy(d_wy, wy, sizeof(wy));
        drag_handle = h;
        drag = h >= 0 ? DR_POINT : inside(x, y) ? DR_MOVE : DR_NONE;
        return;
    }
    if (h >= 0) {
        drag = DR_SCALE;
        drag_handle = h;
    } else if (inside(x, y))
        drag = DR_MOVE;
    else
        drag = DR_ROTATE;
    (void)shift;
}

/* liefert TRUE, wenn sich etwas geaendert hat (dann rendern) */
BOOL transform_mouse_move(LONG mx, LONG my, BOOL shift)
{
    float x = scr2docx(mx), y = scr2docy(my);
    if (drag == DR_NONE)
        return FALSE;
    if (tf_mode != TM_AFFINE) {
        float ddx = x - d_mx, ddy = y - d_my;
        LONG i, h = drag_handle;
        memcpy(qx, d_qx, sizeof(qx)); memcpy(qy, d_qy, sizeof(qy));
        memcpy(wx, d_wx, sizeof(wx)); memcpy(wy, d_wy, sizeof(wy));
        if (drag == DR_MOVE) {
            for (i = 0; i < 4; i++) { qx[i] += ddx; qy[i] += ddy; }
            for (i = 0; i < 16; i++) { wx[i] += ddx; wy[i] += ddy; }
        } else if (tf_mode == TM_WARP) {
            wx[h] += ddx;
            wy[h] += ddy;
        } else if (h >= 4) {            /* Kantenmitte: beide Ecken der Kante */
            LONG a = h - 4, b = (h - 3) & 3;
            if (tf_mode == TM_PERSP) {  /* Perspektive: Kante nur entlang ihrer Richtung */
                if (a == 0 || a == 2) { ddy = 0; } else { ddx = 0; }
            }
            qx[a] += ddx; qy[a] += ddy; qx[b] += ddx; qy[b] += ddy;
        } else if (tf_mode == TM_PERSP) {
            /* Perspektive: Partnerecke derselben Kante spiegelbildlich */
            if (fabsf(ddx) >= fabsf(ddy)) {
                LONG p = h ^ 1;             /* 0<->1, 2<->3: waagrechte Kante */
                qx[h] += ddx;
                qx[p] -= ddx;
            } else {
                LONG p = 3 - h;             /* 0<->3, 1<->2: senkrechte Kante */
                qy[h] += ddy;
                qy[p] -= ddy;
            }
        } else {
            qx[h] += ddx;
            qy[h] += ddy;
            if (shift) {                    /* Shift: nur waagrecht oder senkrecht */
                if (fabsf(ddx) > fabsf(ddy)) qy[h] = d_qy[h]; else qx[h] = d_qx[h];
            }
        }
        return TRUE;
    }
    if (drag == DR_MOVE) {
        tcx = d_cx + (x - d_mx);
        tcy = d_cy + (y - d_my);
    } else if (drag == DR_ROTATE) {
        float a0 = atan2f(d_my - d_cy, d_mx - d_cx), a1 = atan2f(y - d_cy, x - d_cx);
        tang = d_ang + (a1 - a0);
        if (shift) {
            float st = 3.14159265f / 12.0f;     /* 15 Grad */
            tang = floorf(tang / st + 0.5f) * st;
        }
    } else {
        /* Skalieren im lokalen (ungedrehten) System, Gegenseite bleibt fest */
        static const float hu[8] = { -1, 1, 1, -1, 0, 1, 0, -1 };
        static const float hv[8] = { -1, -1, 1, 1, -1, 0, 1, 0 };
        float c = cosf(d_ang), s = sinf(d_ang);
        float dx = x - d_cx, dy = y - d_cy;
        float lu = c * dx + s * dy, lv = -s * dx + c * dy;   /* Maus lokal */
        float hw = d_sx * fw * 0.5f, hh = d_sy * fh * 0.5f;  /* halbe Ausdehnung */
        float u0 = -hw, u1 = hw, v0 = -hh, v1 = hh, nsx = d_sx, nsy = d_sy, mu, mv;
        LONG h = drag_handle;
        if (hu[h] < 0) u0 = lu;
        if (hu[h] > 0) u1 = lu;
        if (hv[h] < 0) v0 = lv;
        if (hv[h] > 0) v1 = lv;
        if (hu[h] != 0)
            nsx = (u1 - u0) / fw;
        if (hv[h] != 0)
            nsy = (v1 - v0) / fh;
        if (shift && hu[h] != 0 && hv[h] != 0) {
            /* proportional: groessere relative Aenderung gewinnt */
            float rx = nsx / d_sx, ry = nsy / d_sy, r = fabsf(rx) > fabsf(ry) ? rx : ry;
            nsx = d_sx * r;
            nsy = d_sy * r;
            if (hu[h] < 0) u0 = u1 - nsx * fw; else u1 = u0 + nsx * fw;
            if (hv[h] < 0) v0 = v1 - nsy * fh; else v1 = v0 + nsy * fh;
        }
        if (fabsf(nsx * fw) < 1.0f) nsx = nsx < 0 ? -1.0f / fw : 1.0f / fw;
        if (fabsf(nsy * fh) < 1.0f) nsy = nsy < 0 ? -1.0f / fh : 1.0f / fh;
        tsx = nsx;
        tsy = nsy;
        /* neue Mitte: Mitte der lokalen Box, zurueckgedreht */
        mu = (u0 + u1) * 0.5f;
        mv = (v0 + v1) * 0.5f;
        tcx = d_cx + c * mu - s * mv;
        tcy = d_cy + s * mu + c * mv;
    }
    return TRUE;
}

void transform_mouse_up(void)
{
    if (drag == DR_NONE)
        return;
    drag = DR_NONE;
    render(TRUE);               /* nach dem Loslassen geglaettet */
}

void transform_update(BOOL smooth)
{
    if (tf_on)
        render(smooth);
}

void transform_get(float *sx, float *sy, float *ang)
{
    *sx = tsx;
    *sy = tsy;
    *ang = tang;
}
