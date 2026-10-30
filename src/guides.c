/*
 * guides.c - Lineale (oben/links) und Hilfslinien
 *
 * Aus dem oberen Lineal zieht man waagerechte, aus dem linken senkrechte
 * Linien. Mit dem Verschieben-Werkzeug (oder Strg) lassen sie sich greifen;
 * zurueck ins Lineal bzw. aus der Ansicht gezogen werden sie geloescht.
 * Lage in ganzen Dokumentpixeln, gespeichert je Dokument (.aps: "GUID").
 */
#include <exec/types.h>
#include <stdio.h>
#include <math.h>
#include "apx.h"

BOOL rulers_on, guides_show = TRUE, guides_lock, guides_snap = TRUE;

#define GUIDE_COL  0xFF36D8F0
#define RULER_BG   0xFF323232
#define RULER_TICK 0xFF8A8A8A
#define SNAP_PX    6            /* Fangbereich in Bildschirmpixeln */

static LONG gd_idx = -1;        /* gezogene Linie */
static LONG mark_x = -1, mark_y = -1;

static BOOL in_view(LONG x, LONG y)
{
    return x >= cvx && x < cvx + cvw && y >= cvy && y < cvy + cvh;
}

/* ---- Lineale ---- */

/* Hauptschritt so, dass Beschriftungen mindestens ~80 Pixel auseinander liegen */
static LONG ruler_step(LONG *sub)
{
    static const LONG steps[] = { 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500,
                                  1000, 2000, 2500, 5000, 10000, 20000, 50000 };
    LONG i, s = steps[sizeof(steps) / sizeof(steps[0]) - 1], n;
    for (i = 0; i < (LONG)(sizeof(steps) / sizeof(steps[0])); i++)
        if (steps[i] * zoom >= 80.0f) {
            s = steps[i];
            break;
        }
    n = 10;
    if (s % 10 || s * zoom / 10 < 5.0f)
        n = (s % 5 == 0 && s * zoom / 5 >= 5.0f) ? 5 : (s % 2 == 0 ? 2 : 1);
    *sub = s / n;
    return s;
}

static void ruler_top(void)
{
    LONG y0 = cvy - RULER_SZ, sub, step = ruler_step(&sub);
    LONG v = (LONG)floorf(scr2docx(cvx) / sub) * sub;
    char buf[12];
    fill(cvx, y0, cvw, RULER_SZ - 1, RULER_BG);
    fill(cvx, y0 + RULER_SZ - 1, cvw, 1, COL_LINE);
    gfx_clip(cvx, y0, cvx + cvw, y0 + RULER_SZ);
    for (;; v += sub) {
        LONG x = doc2scrx((float)v), h;
        if (x >= cvx + cvw)
            break;
        if (v % step == 0) {
            h = RULER_SZ - 1;
            sprintf(buf, "%ld", (long)v);
            text_small(x + 2, y0 - 1, buf, COL_DIM);
        } else
            h = (step / sub == 10 && v % (step / 2) == 0) ? 6 : 3;
        fill(x, y0 + RULER_SZ - 1 - h, 1, h, RULER_TICK);
    }
    if (mark_x >= cvx && mark_x < cvx + cvw)
        fill(mark_x, y0, 1, RULER_SZ - 1, COL_TEXT);
    gfx_noclip();
}

static void ruler_left(void)
{
    LONG x0 = cvx - RULER_SZ, sub, step = ruler_step(&sub);
    LONG v = (LONG)floorf(scr2docy(cvy) / sub) * sub;
    char buf[12], ch[2];
    fill(x0, cvy, RULER_SZ - 1, cvh, RULER_BG);
    fill(x0 + RULER_SZ - 1, cvy, 1, cvh, COL_LINE);
    gfx_clip(x0, cvy, x0 + RULER_SZ, cvy + cvh);
    ch[1] = 0;
    for (;; v += sub) {
        LONG y = doc2scry((float)v), w;
        if (y >= cvy + cvh)
            break;
        if (v % step == 0) {
            /* Ziffern untereinander */
            LONG k, yy = y + 1;
            w = RULER_SZ - 1;
            sprintf(buf, "%ld", (long)v);
            for (k = 0; buf[k]; k++, yy += 9) {
                ch[0] = buf[k];
                text_small(x0 + 2, yy - 2, ch, COL_DIM);
            }
        } else
            w = (step / sub == 10 && v % (step / 2) == 0) ? 6 : 3;
        fill(x0 + RULER_SZ - 1 - w, y, w, 1, RULER_TICK);
    }
    if (mark_y >= cvy && mark_y < cvy + cvh)
        fill(x0, mark_y, RULER_SZ - 1, 1, COL_TEXT);
    gfx_noclip();
}

void rulers_draw(void)
{
    LONG y0 = cvy - RULER_SZ;
    if (!rulers_on || !doc)
        return;
    fill(cax, y0, RULER_SZ, RULER_SZ, RULER_BG);     /* Ecke */
    fill(cax + RULER_SZ - 1, y0, 1, RULER_SZ, COL_LINE);
    fill(cax, y0 + RULER_SZ - 1, RULER_SZ, 1, COL_LINE);
    ruler_top();
    ruler_left();
    blit(cax, y0, caw, RULER_SZ);
    blit(cax, cvy, RULER_SZ, cvh);
}

void rulers_mark(LONG x, LONG y)
{
    if (!in_view(x, y))
        x = y = -1;
    if (x == mark_x && y == mark_y)
        return;
    mark_x = x;
    mark_y = y;
    rulers_draw();
}

void rulers_set(BOOL on)
{
    rulers_on = on;
    view_layout();
}

/* ---- Hilfslinien ---- */

static LONG scr_of(LONG i)
{
    return doc->gvert[i] ? doc2scrx((float)doc->guide[i]) : doc2scry((float)doc->guide[i]);
}

static void inval_guide(LONG i)
{
    LONG s = scr_of(i);
    if (doc->gvert[i])
        view_inval(s - 1, cvy, s + 2, cvy + cvh);
    else
        view_inval(cvx, s - 1, cvx + cvw, s + 2);
}

void guides_changed(void)
{
    view_inval_all();
}

void guides_overlay(void)
{
    LONG i;
    if (!guides_show)
        return;
    for (i = 0; i < doc->nguides; i++) {
        LONG s = scr_of(i);
        if (doc->gvert[i])
            fill(s, cvy, 1, cvh, GUIDE_COL);
        else
            fill(cvx, s, cvw, 1, GUIDE_COL);
    }
}

void guides_add(LONG pos, BOOL vert)
{
    if (doc->nguides >= MAX_GUIDES)
        return;
    doc->guide[doc->nguides] = pos;
    doc->gvert[doc->nguides] = vert ? 1 : 0;
    inval_guide(doc->nguides++);
    doc->modified = TRUE;
}

static void guides_remove(LONG i)
{
    inval_guide(i);
    doc->nguides--;
    for (; i < doc->nguides; i++) {
        doc->guide[i] = doc->guide[i + 1];
        doc->gvert[i] = doc->gvert[i + 1];
    }
    doc->modified = TRUE;
}

void guides_clear(void)
{
    if (!doc->nguides)
        return;
    doc->nguides = 0;
    doc->modified = TRUE;
    guides_changed();
}

LONG guides_at(LONG x, LONG y)
{
    LONG i;
    if (!guides_show || guides_lock || !in_view(x, y))
        return -1;
    for (i = doc->nguides - 1; i >= 0; i--) {
        LONG d = (doc->gvert[i] ? x : y) - scr_of(i);
        if (d >= -3 && d <= 3)
            return i;
    }
    return -1;
}

static void drag_status(void)
{
    char buf[48];
    sprintf(buf, "%s guide: %ld px", doc->gvert[gd_idx] ? "Vertical" : "Horizontal", (long)doc->guide[gd_idx]);
    status_message(buf);
}

BOOL guides_ruler_down(LONG x, LONG y)
{
    BOOL top, left;
    if (!rulers_on)
        return FALSE;
    top = y >= cvy - RULER_SZ && y < cvy && x >= cvx && x < cvx + cvw;
    left = x >= cvx - RULER_SZ && x < cvx && y >= cvy && y < cvy + cvh;
    if (!top && !left)
        return FALSE;
    if (doc->nguides >= MAX_GUIDES) {
        status_message("Too many guides");
        return TRUE;
    }
    if (!guides_show) {
        guides_show = TRUE;
        guides_changed();
    }
    doc->guide[doc->nguides] = left ? (LONG)floorf(scr2docx(x) + 0.5f) : (LONG)floorf(scr2docy(y) + 0.5f);
    doc->gvert[doc->nguides] = left ? 1 : 0;
    gd_idx = doc->nguides++;
    inval_guide(gd_idx);
    drag_status();
    return TRUE;
}

BOOL guides_grab(LONG x, LONG y)
{
    LONG i = guides_at(x, y);
    if (i < 0)
        return FALSE;
    gd_idx = i;
    drag_status();
    return TRUE;
}

BOOL guides_dragging(void)
{
    return gd_idx >= 0;
}

void guides_drag_move(LONG x, LONG y)
{
    LONG p;
    if (gd_idx < 0)
        return;
    p = doc->gvert[gd_idx] ? (LONG)floorf(scr2docx(x) + 0.5f) : (LONG)floorf(scr2docy(y) + 0.5f);
    if (p == doc->guide[gd_idx])
        return;
    inval_guide(gd_idx);
    doc->guide[gd_idx] = p;
    inval_guide(gd_idx);
    drag_status();
}

void guides_drag_end(LONG x, LONG y)
{
    LONG i = gd_idx;
    if (i < 0)
        return;
    guides_drag_move(x, y);
    gd_idx = -1;
    /* ausserhalb der Ansicht (z. B. zurueck ins Lineal) losgelassen: loeschen */
    if (!in_view(x, y)) {
        guides_remove(i);
        status_message("Guide removed");
    } else {
        doc->modified = TRUE;
        status_message("");
    }
}

/* ---- Einrasten ---- */

static float snap1(float v, BOOL vert, LONG size)
{
    float best = v, bd = SNAP_PX / zoom;
    LONG i;
    if (!guides_snap)
        return v;
    if (fabsf(v) <= bd) { best = 0.0f; bd = fabsf(v); }
    if (fabsf(v - size) <= bd) { best = (float)size; bd = fabsf(v - size); }
    if (guides_show)
        for (i = 0; i < doc->nguides; i++)
            if ((doc->gvert[i] != 0) == vert && fabsf(v - doc->guide[i]) <= bd) {
                best = (float)doc->guide[i];
                bd = fabsf(v - doc->guide[i]);
            }
    return best;
}

void guides_snap_pt(float *x, float *y)
{
    *x = snap1(*x, TRUE, doc->w);
    *y = snap1(*y, FALSE, doc->h);
}

void guides_snap_scr(LONG *x, LONG *y)
{
    float dx = scr2docx(*x), dy = scr2docy(*y), sx = dx, sy = dy;
    guides_snap_pt(&sx, &sy);
    if (sx != dx)
        *x = doc2scrx(sx);
    if (sy != dy)
        *y = doc2scry(sy);
}
