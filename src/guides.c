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
#include <stdlib.h>
#include <math.h>
#include "apx.h"

BOOL rulers_on, guides_show = TRUE, guides_lock, guides_snap = TRUE;

#define GUIDE_COL  0xFF36D8F0
#define RULER_BG   0xFF323232
#define RULER_TICK 0xFF8A8A8A
#define SNAP_PX    6            /* Fangbereich in Bildschirmpixeln */

static LONG gd_idx = -1;        /* gezogene Linie */
#define MAX_TGT (MAX_LAYERS * 3 + 3)
static LONG tgt[MAX_TGT], ntgt;  /* Fangziele der gezogenen Linie */
static void drag_targets(BOOL vert);
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
    drag_targets(left);
    guides_drag_move(x, y);
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
    drag_targets(doc->gvert[i]);
    drag_status();
    return TRUE;
}

BOOL guides_dragging(void)
{
    return gd_idx >= 0;
}

void guides_drag_move(LONG x, LONG y)
{
    LONG p, i;
    float f, bd = SNAP_PX / zoom + 0.001f;
    if (gd_idx < 0)
        return;
    f = doc->gvert[gd_idx] ? scr2docx(x) : scr2docy(y);
    p = (LONG)floorf(f + 0.5f);
    /* an Bildrand, Bildmitte und Ebenenkanten/-mitten einrasten */
    if (guides_snap)
        for (i = 0; i < ntgt; i++)
            if (fabsf(f - tgt[i]) < bd) {
                bd = fabsf(f - tgt[i]);
                p = tgt[i];
            }
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

/* ---- Einrasten beim Verschieben (Move Tool) ---- */

static BOOL mb_ok;
static LONG mb_x0, mb_y0, mb_x1, mb_y1;     /* Umriss des bewegten Inhalts, x1/y1 exklusiv */

static void mb_add(LONG x0, LONG y0, LONG x1, LONG y1)
{
    if (x0 < mb_x0) mb_x0 = x0;
    if (y0 < mb_y0) mb_y0 = y0;
    if (x1 > mb_x1) mb_x1 = x1;
    if (y1 > mb_y1) mb_y1 = y1;
}

/* sichtbare Pixel einer Ebene (Kacheln und Ueberhang) */
static void mb_layer(Layer *l)
{
    LONG tx, ty, x, y;
    if (l->kind != LK_PIXEL)
        return;
    for (ty = 0; ty < doc->th; ty++)
        for (tx = 0; tx < doc->tw; tx++) {
            Tile *t = l->tiles[ty * doc->tw + tx];
            LONG bx = tx << TILE_SHIFT, by = ty << TILE_SHIFT;
            LONG x0 = TILE_SIZE, y0 = TILE_SIZE, x1 = -1, y1 = -1;
            if (!t || t->flag == TF_EMPTY)
                continue;
            if (bx >= mb_x0 && by >= mb_y0 && bx + TILE_SIZE <= mb_x1 && by + TILE_SIZE <= mb_y1)
                continue;           /* liegt schon ganz innen */
            for (y = 0; y < TILE_SIZE; y++) {
                const ULONG *p = t->px + (y << TILE_SHIFT);
                for (x = 0; x < TILE_SIZE; x++)
                    if (p[x] >> 24) {
                        if (x < x0) x0 = x;
                        if (x > x1) x1 = x;
                        if (y < y0) y0 = y;
                        y1 = y;
                    }
            }
            if (x1 >= 0)
                mb_add(bx + x0, by + y0, bx + x1 + 1, by + y1 + 1);
        }
    if (l->ovf) {
        Ovf *o = l->ovf;
        for (y = 0; y < o->h; y++)
            for (x = 0; x < o->w; x++)
                if (o->px[y * o->w + x] >> 24)
                    mb_add(o->x + x, o->y + y, o->x + x + 1, o->y + y + 1);
    }
}

void guides_move_begin(void)
{
    LONG pk[MAX_LAYERS], np, q, k;
    mb_ok = FALSE;
    if (doc->hassel) {
        mb_x0 = doc->selx0; mb_y0 = doc->sely0;
        mb_x1 = doc->selx1; mb_y1 = doc->sely1;
        mb_ok = mb_x0 < mb_x1 && mb_y0 < mb_y1;
        return;
    }
    mb_x0 = mb_y0 = 0x7FFFFFFF;
    mb_x1 = mb_y1 = -0x7FFFFFFF;
    np = doc_picked(pk);
    if (np > 1 || doc->layers[doc->active]->kind == LK_GROUP) {
        for (q = 0; q < np; q++)
            for (k = pk[q] - layer_block(pk[q]) + 1; k <= pk[q]; k++)
                mb_layer(doc->layers[k]);
    } else
        mb_layer(doc->layers[doc->active]);
    mb_ok = mb_x0 < mb_x1 && mb_y0 < mb_y1;
}

/* Versatz, der Kante a0/a1 oder Mitte auf ein Ziel legt (0: nichts in Reichweite) */
static LONG snap_shift(LONG a0, LONG a1, BOOL vert, LONG size)
{
    float tol = SNAP_PX / zoom, bd = tol + 0.001f;
    LONG best = 0, i, k, n = guides_show ? doc->nguides : 0;
    for (i = -3; i < n; i++) {
        LONG t, d;
        if (i >= 0 && (doc->gvert[i] != 0) != vert)
            continue;
        t = i == -3 ? 0 : i == -2 ? size : i == -1 ? size / 2 : doc->guide[i];
        for (k = 0; k < 3; k++) {
            if (i == -1 && k < 2)
                continue;       /* Bildmitte nur fuer die Mitte */
            d = t - (k == 0 ? a0 : k == 1 ? a1 : (a0 + a1) / 2);
            if ((float)abs(d) < bd) {
                bd = (float)abs(d);
                best = d;
            }
        }
    }
    return best;
}

void guides_snap_move(LONG *dx, LONG *dy)
{
    if (!mb_ok || !guides_snap)
        return;
    *dx += snap_shift(mb_x0 + *dx, mb_x1 + *dx, TRUE, doc->w);
    *dy += snap_shift(mb_y0 + *dy, mb_y1 + *dy, FALSE, doc->h);
}

/* Fangziele fuer eine gezogene Linie: Bildrand und -mitte, Kanten und Mitte
 * jeder sichtbaren Pixelebene */
static void drag_targets(BOOL vert)
{
    LONG size = vert ? doc->w : doc->h, k;
    ntgt = 0;
    tgt[ntgt++] = 0;
    tgt[ntgt++] = size;
    tgt[ntgt++] = size / 2;
    for (k = 0; k < doc->nlayers; k++) {
        Layer *l = doc->layers[k];
        LONG a0, a1;
        if (!l->visible || l->kind != LK_PIXEL)
            continue;
        mb_x0 = mb_y0 = 0x7FFFFFFF;
        mb_x1 = mb_y1 = -0x7FFFFFFF;
        mb_layer(l);
        if (mb_x0 >= mb_x1 || mb_y0 >= mb_y1)
            continue;
        a0 = vert ? mb_x0 : mb_y0;
        a1 = vert ? mb_x1 : mb_y1;
        if (a0 == 0 && a1 == size)
            continue;           /* fuellt das Bild: schon drin */
        tgt[ntgt++] = a0;
        tgt[ntgt++] = a1;
        tgt[ntgt++] = (a0 + a1) / 2;
    }
}
