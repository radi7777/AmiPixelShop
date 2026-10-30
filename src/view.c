/*
 * view.c - Ansicht: Zoom/Pan, Anzeige des Komposits, Overlays
 *
 * Neu gezeichnet wird nur das ungueltige Rechteck (view_inval*).
 * Pro Kachel entscheidet der Merker in doc->flatflag:
 *   TF_EMPTY  -> Schachbrett, TF_OPAQUE -> kopieren, TF_MIX -> mischen
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include <math.h>
#include "apx.h"

float zoom = 1.0f;
LONG ox, oy;                    /* Bildschirmposition von Dokument (0,0) */
LONG cvx, cvy, cvw, cvh;        /* Canvas-Bereich auf dem Schirm */
LONG cax, caw;                  /* derselbe Bereich samt Linealen */
ULONG last_flush_us;
void (*view_overlay_cb)(void);
void (*view_transform_overlay)(void);
void (*view_text_overlay)(void);
void (*view_pen_overlay)(void);

static LONG *xmap, *ymap;       /* Bildschirm -> Dokument, -1 = ausserhalb */
static LONG *xrunend;           /* Ende des Laufs in derselben Kachelspalte */
static LONG docsx0, docsx1;     /* sichtbarer Dokumentbereich (Index in xmap) */

/* ungueltiger Bildschirmbereich */
static LONG ix0, iy0, ix1, iy1;

/* Overlays */
LONG cursor_x = -1, cursor_y = -1, cursor_r;
float path_x[MAX_PATH], path_y[MAX_PATH];
LONG path_n;
BOOL path_rubber, path_closed_preview;
LONG rubber_x, rubber_y;
static LONG pbx0, pby0, pbx1, pby1;     /* Begrenzung des Pfads */

/* Ameisen: Schachbrett aus 4x4-Bloecken (verschoben um die Phase). Nur x+y waere auf
 * Kanten mit x+y = konstant (nach rechts oben) einfarbig durchgezogen */
#define ANT_COL(x, y) (((((x) + ant_phase) >> 2) + (((y) + ant_phase) >> 2)) & 1 ? 0xFF000000 : 0xFFFFFFFF)
static ULONG *ants;             /* (x << 16) | y */
static LONG nants, maxants;
static LONG abx0, aby0, abx1, aby1;
static LONG ant_phase;

static const float zoom_levels[] = {
    0.0625f, 0.125f, 0.1667f, 0.25f, 0.3333f, 0.5f, 0.6667f,
    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f, 12.0f, 16.0f, 32.0f
};
#define NZOOM (sizeof(zoom_levels) / sizeof(zoom_levels[0]))

float scr2docx(LONG sx) { return (sx - ox) / zoom; }
float scr2docy(LONG sy) { return (sy - oy) / zoom; }
LONG doc2scrx(float dx) { return ox + (LONG)floorf(dx * zoom); }
LONG doc2scry(float dy) { return oy + (LONG)floorf(dy * zoom); }

void view_free(void)
{
    if (xmap) FreeVec(xmap);
    if (xrunend) FreeVec(xrunend);
    if (ymap) FreeVec(ymap);
    xmap = xrunend = NULL;
    ymap = NULL;
}

static void layout(void)
{
    LONG r = rulers_on ? RULER_SZ : 0;
    cax = TOOL_W;
    caw = SW - TOOL_W - PANEL_W;
    cvx = cax + r;
    cvy = MENU_H + OPT_H + TAB_H + r;
    cvw = caw - r;
    cvh = SH - MENU_H - OPT_H - TAB_H - STATUS_H - r;
}

void view_init(void)
{
    BOOL r = rulers_on;
    rulers_on = FALSE;          /* Tabellen fuer die volle Groesse */
    layout();
    rulers_on = r;
    xmap = AllocVec(cvw * sizeof(LONG), MEMF_ANY);
    xrunend = AllocVec(cvw * sizeof(LONG), MEMF_ANY);
    ymap = AllocVec(cvh * sizeof(LONG), MEMF_ANY);
    layout();
    ix0 = iy0 = 0x7FFFFFFF;
    ix1 = iy1 = -0x7FFFFFFF;
    pbx1 = pbx0 = 0;
    abx1 = abx0 = 0;
}

void view_update_maps(void)
{
    LONG i;
    docsx0 = docsx1 = cvw;
    for (i = 0; i < cvw; i++) {
        LONG dx = (LONG)floorf((cvx + i - ox) / zoom);
        if (dx < 0 || dx >= doc->w)
            dx = -1;
        else {
            if (docsx0 == cvw)
                docsx0 = i;
            docsx1 = i + 1;
        }
        xmap[i] = dx;
    }
    for (i = cvw - 1; i >= 0; i--) {
        if (xmap[i] >= 0 && i + 1 < cvw && xmap[i + 1] >= 0 &&
            (xmap[i + 1] >> TILE_SHIFT) == (xmap[i] >> TILE_SHIFT))
            xrunend[i] = xrunend[i + 1];
        else
            xrunend[i] = i + 1;
    }
    for (i = 0; i < cvh; i++) {
        LONG dy = (LONG)floorf((cvy + i - oy) / zoom);
        ymap[i] = (dy < 0 || dy >= doc->h) ? -1 : dy;
    }
}

/* ---- Invalidierung ---- */

void view_inval(LONG x0, LONG y0, LONG x1, LONG y1)
{
    if (x0 < cvx) x0 = cvx;
    if (y0 < cvy) y0 = cvy;
    if (x1 > cvx + cvw) x1 = cvx + cvw;
    if (y1 > cvy + cvh) y1 = cvy + cvh;
    if (x0 >= x1 || y0 >= y1)
        return;
    if (x0 < ix0) ix0 = x0;
    if (y0 < iy0) iy0 = y0;
    if (x1 > ix1) ix1 = x1;
    if (y1 > iy1) iy1 = y1;
}

void view_inval_doc(LONG x0, LONG y0, LONG x1, LONG y1)
{
    view_inval(ox + (LONG)floorf(x0 * zoom), oy + (LONG)floorf(y0 * zoom),
               ox + (LONG)ceilf(x1 * zoom), oy + (LONG)ceilf(y1 * zoom));
}

void view_inval_all(void)
{
    view_inval(cvx, cvy, cvx + cvw, cvy + cvh);
}

/* ---- Anzeige ---- */

static void display(LONG x0, LONG y0, LONG x1, LONG y1)
{
    LONG sy;
    BOOL z1 = (zoom == 1.0f);
    for (sy = y0; sy < y1; sy++) {
        ULONG *d = bb + sy * SW + cvx;
        LONG dy = ymap[sy - cvy], i = x0 - cvx, ie = x1 - cvx, e;
        const ULONG *row;
        const UBYTE *ff;
        LONG cyp;

        if (dy < 0) {
            for (; i < ie; i++)
                d[i] = COL_PASTE;
            continue;
        }
        row = doc->flat + dy * doc->w;
        ff = doc->flatflag + (dy >> TILE_SHIFT) * doc->tw;
        cyp = ((sy - oy) >> 3) & 1;

        e = docsx0 < ie ? docsx0 : ie;
        for (; i < e; i++)
            d[i] = COL_PASTE;
        e = docsx1 < ie ? docsx1 : ie;
        while (i < e) {
            LONG dx = xmap[i], re = xrunend[i], n;
            ULONG *dp = d + i;
            LONG cx = cvx + i - ox;
            if (re > e)
                re = e;
            n = re - i;
            switch (ff[dx >> TILE_SHIFT]) {
            case TF_EMPTY:
                span_checker(dp, n, cx, cyp);
                break;
            case TF_OPAQUE:
                if (z1)
                    memcpy(dp, row + dx, n * 4);
                else
                    span_copy_map(dp, row, xmap + i, n);
                break;
            default:
                if (z1)
                    span_show_mix(dp, row + dx, n, cx, cyp);
                else
                    span_show_mix_map(dp, row, xmap + i, n, cx, cyp);
                break;
            }
            i = re;
        }
        for (; i < ie; i++)
            d[i] = COL_PASTE;
    }
}

static void draw_path(void)
{
    LONG i, px = 0, py = 0, fx = 0, fy = 0;
    for (i = 0; i < path_n; i++) {
        LONG x = doc2scrx(path_x[i]), y = doc2scry(path_y[i]);
        if (i == 0) {
            fx = x; fy = y;
            pset(x, y, 0xFF000000);
        } else
            ant_line(px, py, x, y, ant_phase);
        px = x; py = y;
    }
    if (path_n && path_closed_preview)
        ant_line(px, py, fx, fy, ant_phase);
    if (path_n && path_rubber)
        ant_line(px, py, rubber_x, rubber_y, ant_phase);
}

static void overlays(LONG x0, LONG y0, LONG x1, LONG y1)
{
    LONG i;
    gfx_clip(x0, y0, x1, y1);
    guides_overlay();
    for (i = 0; i < nants; i++) {
        LONG x = ants[i] >> 16, y = ants[i] & 0xFFFF;
        pset(x, y, ANT_COL(x, y));
    }
    if (path_n)
        draw_path();
    if (view_transform_overlay)
        view_transform_overlay();
    if (view_text_overlay)
        view_text_overlay();
    if (view_pen_overlay)
        view_pen_overlay();
    ai_overlay();
    if (cursor_x >= 0) {
        if (cursor_r >= 3) {
            circle(cursor_x, cursor_y, cursor_r, 0xFF000000);
            circle(cursor_x, cursor_y, cursor_r - 1, 0xFFFFFFFF);
        } else {
            line(cursor_x - 4, cursor_y, cursor_x + 4, cursor_y, 0xFF000000);
            line(cursor_x, cursor_y - 4, cursor_x, cursor_y + 4, 0xFF000000);
        }
    }
    gfx_noclip();
}

void view_flush(void)
{
    ULONG t;
    doc_process_dirty();
    if (ix0 >= ix1 || iy0 >= iy1)
        return;
    t = now_us();
    display(ix0, iy0, ix1, iy1);
    overlays(ix0, iy0, ix1, iy1);
    if (view_overlay_cb)
        view_overlay_cb();
    blit(ix0, iy0, ix1 - ix0, iy1 - iy0);
    last_flush_us = now_us() - t;
    ix0 = iy0 = 0x7FFFFFFF;
    ix1 = iy1 = -0x7FFFFFFF;
    ui_draw_status();
}

/* ---- Zoom / Pan ---- */

static void view_changed(void)
{
    view_update_maps();
    view_ants_rebuild();
    view_inval_all();
    ui_draw_tab();
    rulers_draw();
}

/* Lineale ein/aus: Ansicht neu einteilen, Dokument bleibt an seiner Stelle */
void view_layout(void)
{
    layout();
    /* vorgemerkter Bereich stammt evtl. noch von der groesseren Flaeche */
    if (ix0 < cvx) ix0 = cvx;
    if (iy0 < cvy) iy0 = cvy;
    if (ix1 > cvx + cvw) ix1 = cvx + cvw;
    if (iy1 > cvy + cvh) iy1 = cvy + cvh;
    if (doc)
        view_changed();
}

void view_set_zoom(float z, LONG mx, LONG my)
{
    float dx = scr2docx(mx), dy = scr2docy(my);
    zoom = z;
    ox = mx - (LONG)floorf(dx * zoom + 0.5f);
    oy = my - (LONG)floorf(dy * zoom + 0.5f);
    view_changed();
}

void view_zoom_step(LONG dir, LONG mx, LONG my)
{
    LONG i, best = 7;
    for (i = 0; i < (LONG)NZOOM; i++)
        if (zoom_levels[i] >= zoom - 0.001f) {
            best = i;
            break;
        }
    if (dir > 0) {
        if (zoom_levels[best] > zoom + 0.001f)
            ;                   /* naechste Stufe ist schon groesser */
        else if (best < (LONG)NZOOM - 1)
            best++;
    } else {
        if (best > 0)
            best--;
    }
    view_set_zoom(zoom_levels[best], mx, my);
}

void view_fit(void)
{
    float zx = (float)(cvw - 40) / doc->w, zy = (float)(cvh - 40) / doc->h;
    float z = zx < zy ? zx : zy;
    LONG i;
    zoom = zoom_levels[0];
    for (i = 0; i < (LONG)NZOOM; i++)
        if (zoom_levels[i] <= z && zoom_levels[i] <= 1.0f)
            zoom = zoom_levels[i];
    ox = cvx + (cvw - (LONG)(doc->w * zoom)) / 2;
    oy = cvy + (cvh - (LONG)(doc->h * zoom)) / 2;
    view_changed();
}

void view_save(void)
{
    doc->vzoom = zoom;
    doc->vox = ox;
    doc->voy = oy;
    doc->vset = TRUE;
}

void view_restore(void)
{
    if (!doc->vset) {
        view_fit();
        return;
    }
    zoom = doc->vzoom;
    ox = doc->vox;
    oy = doc->voy;
    view_changed();
}

void view_pan(LONG dx, LONG dy)
{
    if (!dx && !dy)
        return;
    ox += dx;
    oy += dy;
    view_changed();
}

/* ---- Overlays ---- */

void view_set_cursor(LONG x, LONG y, LONG r)
{
    if (x == cursor_x && y == cursor_y && r == cursor_r)
        return;
    if (cursor_x >= 0)
        view_inval(cursor_x - cursor_r - 5, cursor_y - cursor_r - 5,
                   cursor_x + cursor_r + 6, cursor_y + cursor_r + 6);
    cursor_x = x;
    cursor_y = y;
    cursor_r = r;
    if (cursor_x >= 0)
        view_inval(cursor_x - cursor_r - 5, cursor_y - cursor_r - 5,
                   cursor_x + cursor_r + 6, cursor_y + cursor_r + 6);
}

void view_path_changed(void)
{
    LONG i;
    view_inval(pbx0, pby0, pbx1, pby1);
    if (!path_n) {
        pbx0 = pbx1 = 0;
        return;
    }
    pbx0 = pby0 = 0x7FFFFFFF;
    pbx1 = pby1 = -0x7FFFFFFF;
    for (i = 0; i < path_n + (path_rubber ? 1 : 0); i++) {
        LONG x = i < path_n ? doc2scrx(path_x[i]) : rubber_x;
        LONG y = i < path_n ? doc2scry(path_y[i]) : rubber_y;
        if (x < pbx0) pbx0 = x;
        if (y < pby0) pby0 = y;
        if (x > pbx1) pbx1 = x;
        if (y > pby1) pby1 = y;
    }
    pbx0 -= 1; pby0 -= 1; pbx1 += 2; pby1 += 2;
    view_inval(pbx0, pby0, pbx1, pby1);
}

static BOOL ants_push(LONG x, LONG y)
{
    if (nants >= maxants) {
        LONG nm = maxants ? maxants * 2 : 16384;
        ULONG *na = AllocVec(nm * sizeof(ULONG), MEMF_ANY);
        if (!na)
            return FALSE;
        if (ants) {
            CopyMem(ants, na, nants * sizeof(ULONG));
            FreeVec(ants);
        }
        ants = na;
        maxants = nm;
    }
    ants[nants++] = ((ULONG)x << 16) | (ULONG)y;
    return TRUE;
}

/* Ameisen = Pixel innerhalb (>= 50 %), deren 4er-Nachbar ausserhalb liegt */
void view_ants_rebuild(void)
{
    LONG sx0, sy0, sx1, sy1, w, sy, i;
    UBYTE *rows, *prev, *cur, *next;

    view_inval(abx0, aby0, abx1, aby1);
    nants = 0;
    abx0 = abx1 = 0;
    if (!doc->hassel)
        return;

    sx0 = ox + (LONG)floorf(doc->selx0 * zoom) - 1;
    sy0 = oy + (LONG)floorf(doc->sely0 * zoom) - 1;
    sx1 = ox + (LONG)ceilf(doc->selx1 * zoom) + 1;
    sy1 = oy + (LONG)ceilf(doc->sely1 * zoom) + 1;
    if (sx0 < cvx) sx0 = cvx;
    if (sy0 < cvy) sy0 = cvy;
    if (sx1 > cvx + cvw) sx1 = cvx + cvw;
    if (sy1 > cvy + cvh) sy1 = cvy + cvh;
    if (sx0 >= sx1 || sy0 >= sy1)
        return;
    w = sx1 - sx0;
    rows = AllocVec((w + 2) * 3, MEMF_ANY | MEMF_CLEAR);
    if (!rows)
        return;
    prev = rows + 1;
    cur = prev + w + 2;
    next = cur + w + 2;

    /* Zeile yy (Bildschirm) als 0/1 in buf. Nachbarn ausserhalb der Ansicht
     * gelten als gleich dem Randpixel, damit am Ansichtsrand keine Ameisen
     * entstehen. */
#define FILLROW(buf, yy) do { \
        LONG _yy = (yy) < cvy ? cvy : (yy) >= cvy + cvh ? cvy + cvh - 1 : (yy); \
        LONG _dy = ymap[_yy - cvy]; \
        for (i = 0; i < w; i++) { \
            LONG _dx = xmap[sx0 + i - cvx]; \
            (buf)[i] = (_dy >= 0 && _dx >= 0 && doc->sel[_dy * doc->w + _dx] >= 128); \
        } \
        (buf)[-1] = sx0 == cvx ? (buf)[0] : 0; \
        (buf)[w] = sx1 == cvx + cvw ? (buf)[w - 1] : 0; \
    } while (0)

    FILLROW(prev, sy0 - 1);
    FILLROW(cur, sy0);
    for (sy = sy0; sy < sy1; sy++) {
        UBYTE *t;
        FILLROW(next, sy + 1);
        for (i = 0; i < w; i++)
            if (cur[i] && (!prev[i] || !next[i] || !cur[i - 1] || !cur[i + 1]))
                if (!ants_push(sx0 + i, sy))
                    break;
        t = prev; prev = cur; cur = next; next = t;
    }
#undef FILLROW
    FreeVec(rows);

    abx0 = sx0; aby0 = sy0; abx1 = sx1; aby1 = sy1;
    view_inval(abx0, aby0, abx1, aby1);
}

void view_ants_tick(void)
{
    LONG i;
    ant_phase--;
    if (path_n)
        view_inval(pbx0, pby0, pbx1, pby1);
    if (!nants)
        return;
    gfx_clip(cvx, cvy, cvx + cvw, cvy + cvh);
    for (i = 0; i < nants; i++) {
        LONG x = ants[i] >> 16, y = ants[i] & 0xFFFF;
        pset(x, y, ANT_COL(x, y));
    }
    gfx_noclip();
    blit(abx0, aby0, abx1 - abx0, aby1 - aby0);
}
