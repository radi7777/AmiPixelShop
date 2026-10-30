/*
 * AmiPixelShop - Hauptprogramm: Initialisierung, Event-Schleife, Werkzeuge
 *
 * Aufruf: AmiPixelShop [Breite Hoehe]
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/modeid.h>
#include <devices/timer.h>
#include <devices/inputevent.h>
#include <libraries/Picasso96.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <clib/alib_protos.h>
#include <proto/timer.h>
#include <proto/keymap.h>
#include <proto/asl.h>
#include <libraries/asl.h>
#include <proto/Picasso96.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/cybergraphics.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "apx.h"
#include "version.h"
#include "net.h"
#include "quant.h"

#define APS_VERSION APS_VER_STR
/* fuer "Version AmiPixelShop" in der Shell */
static const char verstag[] __attribute__((used)) = "$VER: AmiPixelShop " APS_VERSION " (30.10.2026) Early Beta";

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *P96Base;
struct Library *CyberGfxBase;   /* Ersatz ohne Picasso96 (MorphOS) */
struct Library *KeymapBase;
struct Library *AslBase;
struct Device *TimerBase;

#define APS_STACK 262144   /* stb_image (GIF: 32 KB LZW-Tabelle) braucht Stapel */

static struct MsgPort *tport;
static struct timerequest *treq;
static BOOL timer_open;
static UWORD *blank_ptr;
static BOOL ptr_hidden;

/* Interaktion */
static BOOL painting, panning, space_down, lasso_drag, marquee_drag, poly_active, picking;
static BOOL grad_drag, cropping, shaping, quicksel, penning, texting;
static float tb_x0, tb_y0;
static float qs_lx, qs_ly;
static float shp_x0, shp_y0, shp_x1, shp_y1;

static float grad_x0, grad_y0;
static LONG mouse_x, mouse_y, pan_lx, pan_ly, drag_mode;
static float mq_x0, mq_y0, last_bx = -1.0f, last_by = -1.0f;
static ULONG last_secs, last_mics;
static BOOL quit;
static BOOL moving, move_pending, tf_dragging, tf_pending, tf_shift, lq_dragging;

static void busy(BOOL on);
static ULONG ask_screenmode(struct Screen *on, ULONG initial);
static BOOL mode_size(ULONG id, LONG *w, LONG *h);
static void after_edit(void);
static BOOL switch_screen(ULONG modeid);
static float mv_sx, mv_sy;
static LONG mv_dx, mv_dy;
static char last_drawer[256] = "Work:";
static LONG last_cx = -100, last_cy = -100;   /* Ort des letzten Klicks */

#define QUAL_SHIFT (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)
#define QUAL_ALT   (IEQUALIFIER_LALT | IEQUALIFIER_RALT)
#define QUAL_CMD   (IEQUALIFIER_CONTROL | IEQUALIFIER_LCOMMAND | IEQUALIFIER_RCOMMAND)

static BOOL in_canvas(LONG x, LONG y)
{
    return x >= cvx && x < cvx + cvw && y >= cvy && y < cvy + cvh;
}

static BOOL brush_tool(void)
{
    return (tool == T_BRUSH || tool == T_ERASER || tool == T_CLONE || tool == T_BLUR || tool == T_DODGE ||
            tool == T_HISTBRUSH || tool == T_HEAL || tool == T_QUICKSEL) &&
           !space_down && !transform_active();
}

static void update_pointer(void)
{
    BOOL hide = brush_tool() && in_canvas(mouse_x, mouse_y) && !ui_dragging() && !ui_menu_open();
    if (hide == ptr_hidden || !blank_ptr)
        return;
    ptr_hidden = hide;
    if (hide)
        SetPointer(win, blank_ptr, 1, 16, 0, 0);
    else
        ClearPointer(win);
}

static void update_cursor(void)
{
    if (liquify_active() && !space_down && in_canvas(mouse_x, mouse_y) && !ui_dragging() && !ui_menu_open()) {
        LONG r = (LONG)(lq_size * zoom * 0.5f + 0.5f);
        view_set_cursor(mouse_x, mouse_y, r < 3 ? 3 : r);
    } else if (brush_tool() && in_canvas(mouse_x, mouse_y) && !ui_dragging() && !ui_menu_open()) {
        LONG r = (LONG)(brush_size * zoom * 0.5f + 0.5f);
        view_set_cursor(mouse_x, mouse_y, r < 1 ? 1 : r);
    } else
        view_set_cursor(-1, -1, 0);
    update_pointer();
}

/* Ansichtsbefehle: schliessen Text/Transformation/Verfluessigen nicht ab */
static BOOL view_cmd(LONG c)
{
    return c == CMD_ZOOM_IN || c == CMD_ZOOM_OUT || c == CMD_ZOOM_FIT || c == CMD_ZOOM_100 ||
           (c >= CMD_RULERS && c <= CMD_GUIDE_NEW);
}

static LONG mode_from_qual(UWORD qual)
{
    BOOL sh = (qual & QUAL_SHIFT) != 0, al = (qual & QUAL_ALT) != 0;
    if (sh && al) return SEL_ISECT;
    if (sh) return SEL_ADD;
    if (al) return SEL_SUB;
    return sel_mode;
}

static void cancel_path(void)
{
    mag_end();
    path_n = 0;
    path_rubber = FALSE;
    path_closed_preview = FALSE;
    poly_active = lasso_drag = marquee_drag = FALSE;
    view_path_changed();
}

/* Anzahl der im Panel ausgewaehlten Ebenen (mit der aktiven) */
static LONG npicked(void)
{
    LONG p[MAX_LAYERS];
    return doc_picked(p);
}

static void selection_changed(void)
{
    view_ants_rebuild();
}

/* Color Range: Auswahl live zeigen */
static ULONG *cr_col;
static LONG *cr_fuzz, *cr_inv;

static void color_range_preview(void)
{
    sel_color_range(*cr_col, *cr_fuzz, *cr_inv);
    selection_changed();
}

static void add_path_point(float x, float y)
{
    if (path_n < MAX_PATH) {
        path_x[path_n] = x;
        path_y[path_n] = y;
        path_n++;
    }
}

/* Magnetisches Lasso zum Start schliessen und auswaehlen */
static void finish_mag(void)
{
    if (mag_close() >= 3)
        sel_polygon(path_x, path_y, path_n, drag_mode);
    cancel_path();
    selection_changed();
}

static void finish_poly(void)
{
    if (path_n >= 3)
        sel_polygon(path_x, path_y, path_n, drag_mode);
    cancel_path();
    selection_changed();
}

void main_tool_changed(void)
{
    if (text_active()) {
        text_commit();
        ui_draw_panels();
        ui_draw_tab();
    }
    if (transform_active()) {
        transform_apply();
        ui_draw_options();
        ui_draw_panels();
    }
    cancel_path();
    /* Arbeitspfad nur mit dem Pen Tool sichtbar */
    path_show(tool == T_PEN);
    /* Freistellen: Rahmen erscheint beim Waehlen, verschwindet beim Wechsel */
    if (tool == T_CROP && !crop_active())
        crop_begin();
    else if (tool != T_CROP && crop_active())
        crop_end();
    update_cursor();
}

static void marquee_preview(float x1, float y1)
{
    LONG i;
    if (tool == T_RECT) {
        path_x[0] = mq_x0; path_y[0] = mq_y0;
        path_x[1] = x1;    path_y[1] = mq_y0;
        path_x[2] = x1;    path_y[2] = y1;
        path_x[3] = mq_x0; path_y[3] = y1;
        path_n = 4;
    } else {
        float cx = (mq_x0 + x1) * 0.5f, cy = (mq_y0 + y1) * 0.5f;
        float rx = fabsf(x1 - mq_x0) * 0.5f, ry = fabsf(y1 - mq_y0) * 0.5f;
        for (i = 0; i < 72; i++) {
            float a = i * (6.2831853f / 72);
            path_x[i] = cx + rx * cosf(a);
            path_y[i] = cy + ry * sinf(a);
        }
        path_n = 72;
    }
    path_closed_preview = TRUE;
    view_path_changed();
}

static void pick_color(LONG sx, LONG sy)
{
    LONG dx = (LONG)floorf(scr2docx(sx)), dy = (LONG)floorf(scr2docy(sy));
    if (dx < 0 || dy < 0 || dx >= doc->w || dy >= doc->h)
        return;
    fg_color = doc_pick(dx, dy);
    ui_draw_toolbar();
}

/* Endpunkt mit Shift: Quadrat/Kreis bzw. Linie in 45°-Schritten */
static void shape_end_point(float dx, float dy)
{
    if (tf_shift) {
        float w = dx - shp_x0, h = dy - shp_y0, aw = fabsf(w), ah = fabsf(h);
        if (shape_type == SHAPE_LINE) {
            if (aw > ah * 2.0f) h = 0.0f;
            else if (ah > aw * 2.0f) w = 0.0f;
            else { float m = (aw + ah) * 0.5f; w = w < 0.0f ? -m : m; h = h < 0.0f ? -m : m; }
        } else {
            float m = aw > ah ? aw : ah;
            w = w < 0.0f ? -m : m;
            h = h < 0.0f ? -m : m;
        }
        dx = shp_x0 + w;
        dy = shp_y0 + h;
    }
    shp_x1 = dx;
    shp_y1 = dy;
}

/* Vorschau als Pfad */
static void shape_preview(void)
{
    path_n = 0;
    path_closed_preview = shape_type != SHAPE_LINE;
    if (shape_type == SHAPE_LINE) {
        add_path_point(shp_x0, shp_y0);
        add_path_point(shp_x1, shp_y1);
    } else if (shape_type == SHAPE_RECT) {
        add_path_point(shp_x0, shp_y0);
        add_path_point(shp_x1, shp_y0);
        add_path_point(shp_x1, shp_y1);
        add_path_point(shp_x0, shp_y1);
    } else {
        LONG k;
        float cx = (shp_x0 + shp_x1) * 0.5f, cy = (shp_y0 + shp_y1) * 0.5f;
        float rx = (shp_x1 - shp_x0) * 0.5f, ry = (shp_y1 - shp_y0) * 0.5f;
        for (k = 0; k < 48; k++)
            add_path_point(cx + rx * cosf(k * 0.1308997f), cy + ry * sinf(k * 0.1308997f));
    }
    view_path_changed();
}

/* Vorschau fuer Korrekturebenen: neu zusammensetzen */
static void adj_preview(void)
{
    if (adj_curves_target)
        memcpy(adj_curves_target->curves, curves, sizeof(adj_curves_target->curves));
    doc_mark_all_dirty();
    doc_process_dirty();
}

/* Aktive Ebene gesperrt? (Maske bearbeiten bleibt erlaubt) */
static BOOL locked(LONG what)
{
    Layer *l = doc->layers[doc->active];
    if (l->kind == LK_GROUP && what == LOCK_POS) {
        /* Gruppe verschieben: keine Ebene darin darf in der Lage gesperrt sein */
        LONG k, top = doc->active;
        for (k = top - layer_block(top) + 1; k <= top; k++)
            if (doc->layers[k]->lock & LOCK_POS) {
                status_message("A layer in the group is locked (position)");
                return TRUE;
            }
        return FALSE;
    }
    if (l->kind != LK_PIXEL && !doc_target_is_mask()) {
        status_message(l->kind == LK_GROUP ? "Not possible on a group - select a layer"
                                           : "Not a pixel layer - select its mask or another layer");
        return TRUE;
    }
    if (!(l->lock & what) || ((what & LOCK_PIXELS) && !(what & LOCK_POS) && doc_target_is_mask()))
        return FALSE;
    status_message(l->lock & what & LOCK_PIXELS ? "Layer is locked (pixels)" : "Layer is locked (position)");
    return TRUE;
}

/* ---- Maus ---- */

static void mouse_down(UWORD qual, ULONG secs, ULONG mics)
{
    LONG t = space_down ? T_HAND : tool;
    float dx = scr2docx(mouse_x), dy = scr2docy(mouse_y);

    status_message("");
    ui_qualifier = qual;
    if (ui_mouse_down(mouse_x, mouse_y)) {
        update_cursor();
        return;
    }
    if (!in_canvas(mouse_x, mouse_y))
        return;
    if (ai_bar_click(mouse_x, mouse_y))
        return;

    if (liquify_active() && !space_down) {
        liquify_mouse_down(dx, dy);
        lq_dragging = TRUE;
        return;
    }
    if (transform_active() && !space_down) {
        transform_mouse_down(mouse_x, mouse_y, (qual & QUAL_SHIFT) != 0);
        tf_dragging = TRUE;
        tf_shift = (qual & QUAL_SHIFT) != 0;
        return;
    }

    /* Hilfslinie greifen: Verschieben-Werkzeug oder Strg (wie Photoshop) */
    if ((t == T_MOVE || ((qual & QUAL_CMD) && t != T_HAND && t != T_ZOOM && t != T_PEN)) &&
        guides_grab(mouse_x, mouse_y))
        return;

    switch (t) {
    case T_BRUSH: case T_ERASER: case T_CLONE: case T_BLUR: case T_DODGE: case T_GRADIENT: case T_BUCKET:
    case T_HISTBRUSH: case T_HEAL:
        if (!(t == T_BRUSH && (qual & QUAL_ALT)) && !(t == T_CLONE && (qual & QUAL_ALT)) && locked(LOCK_PIXELS))
            return;
        break;
    case T_MOVE:
        /* Auto-Select (Strg kehrt um): Ebene unter dem Mauszeiger aktivieren */
        if ((move_autosel != 0) != ((qual & QUAL_CMD) != 0) && !doc->hassel) {
            LONG li = doc_layer_at((LONG)floorf(dx), (LONG)floorf(dy), move_autogroup);
            if (li >= 0 && (li != doc->active || doc->edit_mask)) {
                doc->active = li;
                doc->edit_mask = FALSE;
                doc_pick_clear();
                ui_draw_panels();
                ui_draw_tab();
            }
        }
        if (locked(doc->hassel ? LOCK_POS | LOCK_PIXELS : LOCK_POS))
            return;
        break;
    }

    switch (t) {
    case T_BRUSH:
    case T_ERASER:
        if (t == T_BRUSH && (qual & QUAL_ALT)) {
            picking = TRUE;
            pick_color(mouse_x, mouse_y);
            break;
        }
        if ((qual & QUAL_SHIFT) && last_bx >= 0.0f) {
            /* Shift+Klick: gerade Linie vom letzten Punkt */
            stroke_begin(last_bx, last_by, t == T_ERASER);
            stroke_to(dx, dy);
        } else
            stroke_begin(dx, dy, t == T_ERASER);
        painting = TRUE;
        break;
    case T_HISTBRUSH:
        history_brush_begin(dx, dy);
        painting = TRUE;
        break;
    case T_HEAL:
        heal_begin(dx, dy);
        painting = TRUE;
        break;
    case T_BLUR:
    case T_DODGE:
        stroke_begin_fx(dx, dy, t == T_BLUR ? FXT_BLUR + retouch_mode : FXT_DODGE + toning_mode);
        painting = TRUE;
        break;
    case T_CLONE:
        if (qual & QUAL_ALT) {
            clone_set_source(dx, dy);
            status_message("Clone source set");
            break;
        }
        if (!clone_has_source()) {
            status_message("Alt+click to define a source point first");
            break;
        }
        clone_begin(dx, dy);
        painting = TRUE;
        break;
    case T_SHAPE:
        guides_snap_pt(&dx, &dy);
        shp_x0 = shp_x1 = dx;
        shp_y0 = shp_y1 = dy;
        tf_shift = (qual & QUAL_SHIFT) != 0;
        shaping = TRUE;
        shape_preview();
        break;
    case T_CROP:
        guides_snap_scr(&mouse_x, &mouse_y);
        crop_mouse_down(mouse_x, mouse_y);
        cropping = TRUE;
        break;
    case T_PEN:
        path_mouse_down(mouse_x, mouse_y);
        penning = TRUE;
        ui_draw_options();
        break;
    case T_GRADIENT:
        grad_x0 = dx;
        grad_y0 = dy;
        path_n = 0;
        add_path_point(dx, dy);
        add_path_point(dx, dy);
        grad_drag = TRUE;
        view_path_changed();
        break;
    case T_BUCKET:
        busy(TRUE);
        tool_bucket((LONG)floorf(dx), (LONG)floorf(dy));
        busy(FALSE);
        after_edit();
        break;
    case T_RECT:
    case T_ELLIPSE:
        drag_mode = mode_from_qual(qual);
        guides_snap_pt(&dx, &dy);
        mq_x0 = dx;
        mq_y0 = dy;
        marquee_drag = TRUE;
        break;
    case T_LASSO:
        drag_mode = mode_from_qual(qual);
        path_n = 0;
        add_path_point(dx, dy);
        lasso_drag = TRUE;
        view_path_changed();
        break;
    case T_POLY:
        if (!poly_active) {
            drag_mode = mode_from_qual(qual);
            poly_active = TRUE;
            path_n = 0;
            add_path_point(dx, dy);
            path_rubber = TRUE;
            rubber_x = mouse_x;
            rubber_y = mouse_y;
        } else {
            LONG fx = doc2scrx(path_x[0]), fy = doc2scry(path_y[0]);
            BOOL near = path_n >= 3 && abs(mouse_x - fx) <= 6 && abs(mouse_y - fy) <= 6;
            BOOL dbl = abs(mouse_x - last_cx) <= 4 && abs(mouse_y - last_cy) <= 4 &&
                       DoubleClick(last_secs, last_mics, secs, mics);
            if (near || dbl) {
                finish_poly();
                break;
            }
            add_path_point(dx, dy);
        }
        view_path_changed();
        break;
    case T_MAGNETIC:
        if (!mag_active()) {
            drag_mode = mode_from_qual(qual);
            if (!mag_begin(dx, dy))
                status_message("Not enough memory");
        } else if (mag_near_start(mouse_x, mouse_y) ||
                   (abs(mouse_x - last_cx) <= 4 && abs(mouse_y - last_cy) <= 4 &&
                    DoubleClick(last_secs, last_mics, secs, mics)))
            finish_mag();
        else
            mag_click(dx, dy);
        break;
    case T_QUICKSEL: {
        LONG m = mode_from_qual(qual);
        sel_quick_begin(m);
        if (m == SEL_NEW) {
            sel_mode = SEL_ADD;         /* wie Photoshop: danach hinzufuegen */
            ui_draw_options();
        }
        sel_quick_dab(dx, dy, brush_size);
        qs_lx = dx;
        qs_ly = dy;
        quicksel = TRUE;
        selection_changed();
        break; }
    case T_WAND:
        sel_wand((LONG)floorf(dx), (LONG)floorf(dy), wand_tol, wand_contig, wand_aa,
                 mode_from_qual(qual));
        selection_changed();
        break;
    case T_MOVE:
        guides_move_begin();    /* vor move_begin: das nimmt die Kacheln aus der Ebene */
        move_begin();
        if (move_active()) {
            moving = TRUE;
            mv_sx = dx;
            mv_sy = dy;
            mv_dx = mv_dy = 0;
        }
        break;
    case T_TEXT: {
        Layer *tl;
        if (text_active())
            text_commit();
        /* Klick auf eine Textebene bearbeitet sie, sonst neuer Text */
        if ((tl = text_layer_at((LONG)floorf(dx), (LONG)floorf(dy)))) {
            text_begin_edit(tl);
            after_edit();
            ui_draw_options();
            ui_draw_toolbar();
        } else {
            /* Klick: Punkttext, Ziehen: Absatztext (entschieden beim Loslassen) */
            texting = TRUE;
            tb_x0 = dx;
            tb_y0 = dy;
        }
        break; }
    case T_EYEDROP:
        picking = TRUE;
        pick_color(mouse_x, mouse_y);
        break;
    case T_HAND:
        panning = TRUE;
        pan_lx = mouse_x;
        pan_ly = mouse_y;
        break;
    case T_ZOOM:
        view_zoom_step((qual & QUAL_ALT) ? -1 : 1, mouse_x, mouse_y);
        break;
    }
    last_secs = secs;
    last_mics = mics;
    last_cx = mouse_x;
    last_cy = mouse_y;
}

static void mouse_move(void)
{
    float dx = scr2docx(mouse_x), dy = scr2docy(mouse_y);

    rulers_mark(mouse_x, mouse_y);
    if (guides_dragging()) {
        guides_drag_move(mouse_x, mouse_y);
        return;
    }
    if (!painting && !moving && !panning)
        ui_hover(mouse_x, mouse_y);

    if (ui_dragging() || ui_menu_open()) {
        ui_mouse_move(mouse_x, mouse_y);
        return;
    }
    if (lq_dragging) {
        liquify_mouse_move(dx, dy);
    } else if (tf_dragging) {
        if (transform_mouse_move(mouse_x, mouse_y, tf_shift))
            tf_pending = TRUE;
    } else if (painting)
        stroke_to(dx, dy);
    else if (moving) {
        /* nur merken; angewendet wird einmal pro Durchlauf (apply_move) */
        mv_dx = (LONG)floorf(dx - mv_sx + 0.5f);
        mv_dy = (LONG)floorf(dy - mv_sy + 0.5f);
        guides_snap_move(&mv_dx, &mv_dy);
        move_pending = TRUE;
    } else if (picking)
        pick_color(mouse_x, mouse_y);
    else if (panning) {
        view_pan(mouse_x - pan_lx, mouse_y - pan_ly);
        pan_lx = mouse_x;
        pan_ly = mouse_y;
    } else if (lasso_drag) {
        LONG lx = doc2scrx(path_x[path_n - 1]), ly = doc2scry(path_y[path_n - 1]);
        if (abs(mouse_x - lx) + abs(mouse_y - ly) >= 2) {
            add_path_point(dx, dy);
            view_path_changed();
        }
    } else if (quicksel) {
        /* entlang der Strecke tupfen (Abstand ~ halber Radius) */
        float ddx = dx - qs_lx, ddy = dy - qs_ly, dist = sqrtf(ddx * ddx + ddy * ddy);
        float step = brush_size * 0.25f, t;
        if (step < 1.0f) step = 1.0f;
        if (dist >= step) {
            for (t = step; t <= dist; t += step)
                sel_quick_dab(qs_lx + ddx * t / dist, qs_ly + ddy * t / dist, brush_size);
            qs_lx = dx;
            qs_ly = dy;
            selection_changed();
        }
    } else if (shaping) {
        guides_snap_pt(&dx, &dy);
        shape_end_point(dx, dy);
        shape_preview();
    } else if (texting) {
        text_box_preview((LONG)floorf(tb_x0), (LONG)floorf(tb_y0), (LONG)floorf(dx), (LONG)floorf(dy));
    } else if (penning) {
        path_mouse_move(mouse_x, mouse_y);
    } else if (cropping) {
        LONG cx = mouse_x, cy = mouse_y;
        guides_snap_scr(&cx, &cy);
        crop_mouse_move(cx, cy);
        ui_draw_options();
    } else if (grad_drag) {
        path_n = 1;
        add_path_point(dx, dy);
        view_path_changed();
    } else if (marquee_drag) {
        guides_snap_pt(&dx, &dy);
        marquee_preview(dx, dy);
    }
    else if (mag_active())
        mag_move(dx, dy);
    else if (poly_active) {
        rubber_x = mouse_x;
        rubber_y = mouse_y;
        view_path_changed();
    }
    update_cursor();
}

static void apply_move(void)
{
    if (moving && move_pending) {
        move_update(mv_dx, mv_dy);
        move_pending = FALSE;
    }
    if (tf_pending) {
        transform_update(FALSE);    /* schnelle Vorschau beim Ziehen */
        tf_pending = FALSE;
        ui_draw_options();
    }
}

/* Beim Loslassen zuerst die Endposition verarbeiten: Intuition schickt nur
 * eine Bewegungsnachricht gleichzeitig, waehrend wir rechnen gehen
 * Zwischenpositionen verloren - die letzte darf es nicht. */
static void mouse_up(void)
{
    if (guides_dragging()) {
        guides_drag_end(mouse_x, mouse_y);
        return;
    }
    if (ui_dragging()) {
        ui_mouse_move(mouse_x, mouse_y);
        ui_mouse_up();
        update_cursor();
        return;
    }
    if (lq_dragging) {
        liquify_mouse_move(scr2docx(mouse_x), scr2docy(mouse_y));
        liquify_mouse_up();
        lq_dragging = FALSE;
        return;
    }
    if (tf_dragging) {
        transform_mouse_move(mouse_x, mouse_y, tf_shift);
        tf_pending = FALSE;
        busy(TRUE);
        transform_mouse_up();
        busy(FALSE);
        tf_dragging = FALSE;
        ui_draw_options();
        return;
    }
    if (painting) {
        stroke_to(scr2docx(mouse_x), scr2docy(mouse_y));
        stroke_end();
        doc_masks_compact();
        painting = FALSE;
        last_bx = scr2docx(mouse_x);
        last_by = scr2docy(mouse_y);
        ui_draw_panels();
        ui_draw_tab();
    }
    if (moving) {
        mv_dx = (LONG)floorf(scr2docx(mouse_x) - mv_sx + 0.5f);
        mv_dy = (LONG)floorf(scr2docy(mouse_y) - mv_sy + 0.5f);
        guides_snap_move(&mv_dx, &mv_dy);
        move_update(mv_dx, mv_dy);
        move_end();
        moving = move_pending = FALSE;
        ui_draw_panels();
        ui_draw_tab();
    }
    picking = FALSE;
    panning = FALSE;
    if (quicksel) {
        quicksel = FALSE;
        selection_changed();
    }
    if (shaping) {
        float ex = scr2docx(mouse_x), ey = scr2docy(mouse_y);
        guides_snap_pt(&ex, &ey);
        shape_end_point(ex, ey);
        shaping = FALSE;
        path_closed_preview = FALSE;
        cancel_path();
        if (fabsf(shp_x1 - shp_x0) * zoom > 2.0f || fabsf(shp_y1 - shp_y0) * zoom > 2.0f) {
            busy(TRUE);
            tool_shape(shp_x0, shp_y0, shp_x1, shp_y1);
            busy(FALSE);
            after_edit();
        }
    }
    if (texting) {
        float x = scr2docx(mouse_x), y = scr2docy(mouse_y);
        LONG x0 = (LONG)floorf(tb_x0 < x ? tb_x0 : x), y0 = (LONG)floorf(tb_y0 < y ? tb_y0 : y);
        LONG w = (LONG)fabsf(x - tb_x0), h = (LONG)fabsf(y - tb_y0);
        texting = FALSE;
        text_box_preview(0, 0, 0, -1);
        if (w * zoom > 8 && h * zoom > 8)
            text_begin_box(x0, y0, w, h);
        else
            text_begin((LONG)floorf(tb_x0), (LONG)floorf(tb_y0));
        after_edit();
        ui_draw_options();
        ui_draw_toolbar();
    }
    if (penning) {
        path_mouse_up(mouse_x, mouse_y);
        penning = FALSE;
        ui_draw_options();
        ui_draw_panels();     /* Miniatur und Protokoll */
    }
    if (cropping) {
        LONG cx = mouse_x, cy = mouse_y;
        guides_snap_scr(&cx, &cy);
        crop_mouse_up(cx, cy);
        cropping = FALSE;
        ui_draw_options();
    }
    if (grad_drag) {
        float ex = scr2docx(mouse_x), ey = scr2docy(mouse_y);
        BOOL moved = fabsf(ex - grad_x0) * zoom > 2.0f || fabsf(ey - grad_y0) * zoom > 2.0f;
        grad_drag = FALSE;
        cancel_path();
        if (moved) {
            busy(TRUE);
            tool_gradient(grad_x0, grad_y0, ex, ey);
            busy(FALSE);
            after_edit();
        }
    }
    if (lasso_drag) {
        add_path_point(scr2docx(mouse_x), scr2docy(mouse_y));
        if (path_n >= 3)
            sel_polygon(path_x, path_y, path_n, drag_mode);
        else if (drag_mode == SEL_NEW)
            sel_none();
        cancel_path();
        selection_changed();
    }
    if (marquee_drag) {
        float dx = scr2docx(mouse_x), dy = scr2docy(mouse_y);
        BOOL moved;
        guides_snap_pt(&dx, &dy);
        moved = fabsf(dx - mq_x0) * zoom > 2.0f || fabsf(dy - mq_y0) * zoom > 2.0f;
        if (moved) {
            if (tool == T_RECT)
                sel_rect(mq_x0, mq_y0, dx, dy, drag_mode);
            else
                sel_ellipse(mq_x0, mq_y0, dx, dy, drag_mode);
        } else if (drag_mode == SEL_NEW)
            sel_none();
        cancel_path();
        selection_changed();
    }
}

/* ---- Tastatur ---- */

static void brush_size_step(LONG dir)
{
    LONG s = brush_size, d = s < 10 ? 1 : s < 50 ? 5 : s < 100 ? 10 : s < 200 ? 25 : 50;
    s += dir * d;
    if (s < 1) s = 1;
    if (s > 500) s = 500;
    brush_size = s;
    brush_rebuild();
    ui_draw_options();
    update_cursor();
}

static void brush_hard_step(LONG dir)
{
    brush_hard += dir * 25;
    if (brush_hard < 0) brush_hard = 0;
    if (brush_hard > 100) brush_hard = 100;
    brush_rebuild();
    ui_draw_options();
}

static BOOL handle_key(UWORD code, UWORD qual)
{
    struct InputEvent ie;
    char buf[4];
    LONG n;
    BOOL cmd = (qual & QUAL_CMD) != 0, shift = (qual & QUAL_SHIFT) != 0;
    BOOL alt = (qual & QUAL_ALT) != 0;

    if (text_active() && !(code & 0x80)) {
        if (code == 0x45) {
            do_command(CMD_TEXT_CANCEL);
            return FALSE;
        }
        if (code == 0x44 || code == 0x43) {
            if (cmd)
                do_command(CMD_TEXT_COMMIT);
            else
                text_key_char('\n');
            return FALSE;
        }
        if (code == 0x41) {
            text_key_char('\b');
            return FALSE;
        }
        if (!cmd) {
            struct InputEvent tie;
            UBYTE c[4];
            memset(&tie, 0, sizeof(tie));
            tie.ie_Class = IECLASS_RAWKEY;
            tie.ie_Code = code;
            tie.ie_Qualifier = qual & (QUAL_SHIFT | QUAL_ALT);
            if (MapRawKey(&tie, (STRPTR)c, sizeof(c), NULL) == 1)
                text_key_char(c[0]);
            return FALSE;
        }
        /* Ctrl+Taste: Text uebernehmen, dann normal weiter */
        do_command(CMD_TEXT_COMMIT);
    }
    if (code == 0x40) { space_down = TRUE; update_cursor(); return FALSE; }
    if (code == 0xC0) { space_down = FALSE; update_cursor(); return FALSE; }
    if (code & 0x80)
        return FALSE;

    switch (code) {
    case 0x54:                  /* F5 */
        if (shift)
            do_command(CMD_EDIT_FILL);
        return FALSE;
    case 0x45:                  /* Esc */
        if (ui_menu_open())
            ui_close_menu();
        else if (liquify_active())
            do_command(CMD_LQ_CANCEL);
        else if (transform_active())
            do_command(CMD_TF_CANCEL);
        else if (crop_active())
            do_command(CMD_CROP_CANCEL);
        else if (tool == T_PEN)
            path_end();
        cancel_path();
        return FALSE;
    case 0x44: case 0x43:       /* Return, Enter */
        if (liquify_active())
            do_command(CMD_LQ_APPLY);
        else if (transform_active())
            do_command(CMD_TF_APPLY);
        else if (crop_active())
            do_command(CMD_CROP_COMMIT);
        else if (poly_active)
            finish_poly();
        else if (mag_active())
            finish_mag();
        else if (tool == T_PEN && cmd)
            do_command(CMD_PATH_SEL);   /* wie Photoshop: Strg+Enter */
        else if (tool == T_PEN)
            path_end();
        return FALSE;
    case 0x41:                  /* Backspace */
        if (tool == T_PEN && path_exists() && !alt && !cmd) {
            path_delete_last();
            ui_draw_options();
        } else if (mag_active()) {
            if (!mag_back())
                cancel_path();
        } else if (poly_active) {
            if (path_n > 1) {
                path_n--;
                view_path_changed();
            } else
                cancel_path();
        } else if (alt || cmd)
            do_command(alt ? CMD_FILL_FG : CMD_FILL_BG);
        return FALSE;
    case 0x42:                  /* Tab */
        if (cmd)
            do_command(CMD_NEXT_DOC);
        return FALSE;
    case 0x46:                  /* Entf */
        do_command(CMD_ERASE);
        return FALSE;
    case 0x7A:                  /* Mausrad */
        view_zoom_step(1, mouse_x, mouse_y);
        update_cursor();
        return FALSE;
    case 0x7B:
        view_zoom_step(-1, mouse_x, mouse_y);
        update_cursor();
        return FALSE;
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:     /* Pfeiltasten */
        if (tool == T_MOVE && !moving && !painting && !locked(doc->hassel ? LOCK_POS | LOCK_PIXELS : LOCK_POS)) {
            LONG st = shift ? 10 : 1;
            move_nudge(code == 0x4E ? st : code == 0x4F ? -st : 0,
                       code == 0x4D ? st : code == 0x4C ? -st : 0);
            ui_draw_panels();
            ui_draw_tab();
        }
        return FALSE;
    case 0x55:                  /* F6 */
        if (shift)
            do_command(CMD_FEATHER);
        return FALSE;
    case 0x1A:                  /* [ bzw. Ue */
        if (cmd) do_command(CMD_LAYER_DOWN);
        else if (shift) brush_hard_step(-1);
        else brush_size_step(-1);
        return FALSE;
    case 0x1B:                  /* ] bzw. + */
        if (cmd) do_command(CMD_LAYER_UP);
        else if (shift) brush_hard_step(1);
        else brush_size_step(1);
        return FALSE;
    }

    memset(&ie, 0, sizeof(ie));
    ie.ie_Class = IECLASS_RAWKEY;
    ie.ie_Code = code;
    ie.ie_Qualifier = qual & QUAL_SHIFT;
    n = MapRawKey(&ie, buf, sizeof(buf), NULL);
    if (n != 1)
        return FALSE;

    if (cmd && alt) {
        LONG c = CMD_NONE;
        switch (code) {
        case 0x24: c = CMD_CLIP_TOGGLE; break;      /* G */
        case 0x20: c = CMD_ALL_LAYERS; break;       /* A */
        case 0x17: c = CMD_IMAGE_SIZE; break;       /* I */
        case 0x33: c = CMD_CANVAS_SIZE; break;      /* C */
        case 0x11: c = CMD_CLOSE_ALL; break;        /* W */
        case 0x31: c = CMD_TOGGLE_LAST; break;      /* Z */
        }
        if (c != CMD_NONE) {
            do_command(c);
            return quit;
        }
    }
    if (cmd) {
        LONG c = CMD_NONE;
        switch (buf[0]) {
        case 'q': case 'Q': c = CMD_QUIT; break;
        case 'w': case 'W': c = CMD_CLOSE; break;
        case 't': case 'T': c = CMD_FREE_TF; break;
        case 'c': c = CMD_COPY; break;
        case 'C': c = CMD_COPY_MERGED; break;
        case 'x': c = CMD_CUT; break;
        case 'X': c = CMD_FLT_LIQUIFY; break;
        case 'v': case 'V': c = CMD_PASTE; break;
        case 'n': c = CMD_NEW; break;
        case 'N': c = CMD_LAYER_NEW; break;
        case 'o': case 'O': c = CMD_OPEN; break;
        case 's': c = CMD_SAVE; break;
        case 'S': c = CMD_SAVEAS; break;
        case 'z': c = CMD_UNDO; break;
        case 'Z': c = CMD_REDO; break;
        case 'a': case 'A': c = CMD_SEL_ALL; break;
        case 'd': c = CMD_SEL_NONE; break;
        case 'I': c = CMD_SEL_INVERT; break;
        case 'i': c = CMD_ADJ_INVERT; break;
        case 'l': case 'L': c = CMD_ADJ_LEVELS; break;
        case 'm': case 'M': c = CMD_ADJ_CURVES; break;
        case 'u': c = CMD_ADJ_HUESAT; break;
        case 'U': c = CMD_ADJ_DESAT; break;
        case 'f': case 'F': c = CMD_FLT_LAST; break;
        case 'j': c = CMD_LAYER_VIA_COPY; break;
        case 'J': c = CMD_LAYER_VIA_CUT; break;
        case 'D': c = CMD_RESELECT; break;
        case 'E': c = CMD_MERGE_VISIBLE; break;
        case 'e': c = CMD_MERGE_DOWN; break;
        case 'g': c = CMD_GROUP; break;
        case 'G': c = CMD_UNGROUP; break;
        case '+': case '=': c = CMD_ZOOM_IN; break;
        case '-': c = CMD_ZOOM_OUT; break;
        case '0': c = CMD_ZOOM_FIT; break;
        case '1': c = CMD_ZOOM_100; break;
        case 'r': case 'R': c = CMD_RULERS; break;
        case ';': c = CMD_GUIDES_SHOW; break;
        case ':': c = CMD_GUIDES_SNAP; break;
        }
        if (c != CMD_NONE)
            do_command(c);
        return quit;
    }

    switch (buf[0]) {
    case 'v': ui_set_tool(T_MOVE); break;
    case 'm': if (tool != T_RECT && tool != T_ELLIPSE) ui_set_tool(T_RECT); break;
    case 'M': ui_set_tool(tool == T_RECT ? T_ELLIPSE : T_RECT); break;
    case 'l': if (tool != T_LASSO && tool != T_POLY && tool != T_MAGNETIC) ui_set_tool(T_LASSO); break;
    case 'L': ui_set_tool(tool == T_LASSO ? T_POLY : tool == T_POLY ? T_MAGNETIC : T_LASSO); break;
    case 'i': ui_set_tool(T_EYEDROP); break;
    case 'c': ui_set_tool(T_CROP); break;
    case 'p': ui_set_tool(T_PEN); break;
    case 'u': ui_set_tool(T_SHAPE); break;
    case 'r': ui_set_tool(T_BLUR); break;
    case 'y': ui_set_tool(T_HISTBRUSH); break;
    case 'j': ui_set_tool(T_HEAL); break;
    case 'R':
        if (tool == T_BLUR)
            retouch_mode = (retouch_mode + 1) % 3;
        ui_set_tool(T_BLUR);
        ui_draw_options();
        break;
    case 'o': ui_set_tool(T_DODGE); break;
    case 'O':
        if (tool == T_DODGE)
            toning_mode ^= 1;
        ui_set_tool(T_DODGE);
        ui_draw_options();
        break;
    case 'U':
        if (tool == T_SHAPE)
            shape_type = (shape_type + 1) % 3;
        ui_set_tool(T_SHAPE);
        ui_draw_options();
        break;
    case 'w': if (tool != T_WAND && tool != T_QUICKSEL) ui_set_tool(T_QUICKSEL); break;
    case 'W': ui_set_tool(tool == T_QUICKSEL ? T_WAND : T_QUICKSEL); break;
    case 't': ui_set_tool(T_TEXT); break;
    case 'b': ui_set_tool(T_BRUSH); break;
    case 'e': ui_set_tool(T_ERASER); break;
    case 's': ui_set_tool(T_CLONE); break;
    case 'g': if (tool != T_GRADIENT && tool != T_BUCKET) ui_set_tool(T_GRADIENT); break;
    case 'G': ui_set_tool(tool == T_GRADIENT ? T_BUCKET : T_GRADIENT); break;
    case 'h': ui_set_tool(T_HAND); break;
    case 'z': ui_set_tool(T_ZOOM); break;
    case 'x': {
        ULONG t = fg_color; fg_color = bg_color; bg_color = t;
        ui_draw_toolbar();
        break; }
    case 'd':
        fg_color = 0x000000; bg_color = 0xFFFFFF;
        ui_draw_toolbar();
        break;
    case ',': brush_size_step(-1); break;
    case '.': brush_size_step(1); break;
    default:
        if (buf[0] >= '0' && buf[0] <= '9') {
            brush_opacity = buf[0] == '0' ? 100 : (buf[0] - '0') * 10;
            ui_draw_options();
        }
        break;
    }
    return FALSE;
}

/* ---- Requester, Dateien, Befehle ---- */

static void redraw_all(void)
{
    if (tool == T_CROP && !crop_active())
        crop_begin();
    ui_draw_all();
    view_inval_all();
}

/* Beschaeftigt: Warte-Zeiger und niedrigere Prioritaet, damit Netzwerk,
 * Workbench und andere Programme waehrend langer Arbeiten bedienbar bleiben */
static void busy(BOOL on)
{
    if (on) {
        SetWindowPointer(win, WA_BusyPointer, TRUE, TAG_DONE);
        SetTaskPri(FindTask(NULL), -1);
        ptr_hidden = FALSE;
    } else {
        SetTaskPri(FindTask(NULL), 0);
        ClearPointer(win);
        ptr_hidden = FALSE;
        update_pointer();
    }
}

/* Texte fuer System-Requester sind Latin-1 */
LONG request(const char *body, const char *gadgets)
{
    struct EasyStruct es;
    LONG r;
    es.es_StructSize = sizeof(es);
    es.es_Flags = 0;
    es.es_Title = (UBYTE *)"AmiPixelShop";
    es.es_TextFormat = (UBYTE *)body;
    es.es_GadgetFormat = (UBYTE *)gadgets;
    r = EasyRequestArgs(win, &es, NULL, NULL);
    redraw_all();
    return r;
}

static BOOL discard_ok(void)
{
    if (!doc->modified)
        return TRUE;
    return request("This image has unsaved changes.\nDiscard them?",
                   "Discard|Cancel") == 1;
}

static const char *ask_ext = ".aps";    /* Vorschlag fuer neue Dateinamen */
static BOOL ask_as_aps;                 /* Save As: Namen mit .aps vorschlagen (Ebenen behalten) */
static const char *ask_title;           /* eigener Titel des Dateidialogs oder NULL */

static BOOL ask_file(BOOL save, char *out, LONG outlen)
{
    struct FileRequester *fr;
    BOOL ok = FALSE;
    char init[108], docdir[256];
    LONG savemode = save;

    if (!AslBase && !(AslBase = OpenLibrary("asl.library", 39)))
        return FALSE;
    /* Speichern: Ordner des Bildes vorschlagen (wie Photoshop), sonst den zuletzt benutzten */
    docdir[0] = 0;
    if (save && doc->path[0]) {
        LONG n = (char *)PathPart(doc->path) - doc->path;
        if (n > 0 && n < (LONG)sizeof(docdir)) {
            memcpy(docdir, doc->path, n);
            docdir[n] = 0;
        }
    }
    strcpy(init, doc_title());
    if (!doc->path[0] || strcmp(ask_ext, ".aps") || ask_as_aps) {
        char *dot = strrchr(init, '.');
        if (doc->path[0] && dot)
            *dot = 0;           /* Endung ersetzen */
        strcat(init, ask_ext);
    }
    fr = AllocAslRequestTags(ASL_FileRequest,
        ASLFR_Screen, (ULONG)scr,
        ASLFR_TitleText, (ULONG)(ask_title ? ask_title : !save ? "Open" :
                                 strcmp(ask_ext, ".aps") ? "Export as IFF ILBM" : "Save As (.aps, .png, .jpg, .iff)"),
        ASLFR_DoSaveMode, savemode,
        ASLFR_RejectIcons, TRUE,
        ASLFR_InitialDrawer, (ULONG)(docdir[0] ? docdir : last_drawer),
        ASLFR_InitialFile, (ULONG)(save ? init : ""),
        ASLFR_InitialWidth, 520,
        ASLFR_InitialHeight, SH * 2 / 3,
        ASLFR_InitialLeftEdge, (SW - 520) / 2,
        ASLFR_InitialTopEdge, SH / 6,
        TAG_DONE);
    if (fr) {
        if (AslRequestTags(fr, TAG_DONE) && fr->fr_File[0]) {
            strncpy(out, fr->fr_Drawer, outlen - 1);
            out[outlen - 1] = 0;
            ok = AddPart(out, fr->fr_File, outlen) != 0;
            /* gemerkter Ordner aus dem fertigen Pfad: auch bei "Work:x/bild.png" im Dateifeld */
            if (ok) {
                LONG n = (char *)PathPart(out) - out;
                if (n >= (LONG)sizeof(last_drawer))
                    n = sizeof(last_drawer) - 1;
                memcpy(last_drawer, out, n);
                last_drawer[n] = 0;
            }
        }
        FreeAslRequest(fr);
    }
    redraw_all();
    return ok;
}

/* aktuelle Auswahl fuer Reselect merken */
static void remember_sel(void)
{
    if (!doc->hassel)
        return;
    if (!doc->lastsel)
        doc->lastsel = AllocVec(doc->w * doc->h, MEMF_ANY);
    if (doc->lastsel)
        CopyMem(doc->sel, doc->lastsel, doc->w * doc->h);
}

/* laufende Maus-Aktionen sauber beenden (vor Dokumentwechsel) */
static void finish_interaction(void)
{
    if (text_active())
        text_commit();
    if (liquify_active()) {
        lq_dragging = FALSE;
        liquify_apply();
    }
    if (transform_active()) {
        tf_dragging = tf_pending = FALSE;
        transform_apply();
    }
    if (painting)
        stroke_end();
    if (moving)
        move_end();
    painting = picking = panning = moving = FALSE;
    cancel_path();
    if (crop_active())
        crop_end();             /* Rahmen gehoert zum alten Dokument */
    cropping = FALSE;
}

/* neues Dokument als Tab oeffnen */
static void switch_doc(Doc *d)
{
    /* ein unberuehrtes leeres Startbild wird ersetzt statt daneben zu bleiben */
    Doc *pristine = ndocs == 1 && !docs[0]->path[0] && !docs[0]->modified &&
                    !docs[0]->ucount ? docs[0] : NULL;
    finish_interaction();
    if (!doc_open(d)) {
        doc_free(d);
        request("There are already 8 images open.", "OK");
        return;
    }
    if (pristine) {
        Doc *cur = doc;
        doc_close(pristine);
        doc_activate(cur);
    }
    redraw_all();
}

void main_activate_doc(Doc *d)
{
    if (d == doc)
        return;
    finish_interaction();
    doc_activate(d);
    redraw_all();
}

/* aktives Dokument schliessen; das letzte wird durch ein leeres ersetzt */
static void close_doc(void)
{
    finish_interaction();
    doc_close(doc);
    if (!ndocs) {
        Doc *d = doc_new(1280, 960, DOC_WHITE);
        if (d)
            doc_open(d);
    }
    redraw_all();
}

static void open_file(const char *path)
{
    char err[80], msg[160];
    Doc *d;
    status_message("Loading...");
    busy(TRUE);
    d = file_load(path, err);
    busy(FALSE);
    if (!d) {
        sprintf(msg, "Cannot open %s:\n%s", path, err[0] ? err : "Unknown file format");
        request(msg, "OK");
        status_message("");
        return;
    }
    switch_doc(d);
    status_message("Loaded");
}

/* Ginge beim Speichern als PNG/JPEG/IFF etwas verloren (Ebenen, Text, Masken, Stile ...)? */
static BOOL doc_is_layered(void)
{
    Layer *l = doc->layers[0];
    return doc->nlayers > 1 || l->mask || l->text || l->fx || l->adj || l->gen || l->ovf ||
           l->kind != LK_PIXEL || l->opacity < 255 || l->fill < 255 || !l->visible || l->mode != BM_NORMAL;
}

static BOOL is_aps(const char *path)
{
    const char *dot = strrchr(FilePart((STRPTR)path), '.');
    return dot && !strcasecmp(dot, ".aps");
}

static void save_file(const char *path)
{
    char err[80], msg[160];
    BOOL ok;
    status_message("Saving...");
    busy(TRUE);
    ok = file_save(doc, path, err);
    busy(FALSE);
    if (!ok) {
        sprintf(msg, "Cannot save %s:\n%s", path, err);
        request(msg, "OK");
        status_message("");
        return;
    }
    /* flaches Format bei Ebenen-Dokument: nur eine Kopie (wie Photoshop), das Dokument
     * behaelt Namen und Aenderungsstatus - sonst gingen die Ebenen beim Schliessen still verloren */
    if (!is_aps(path) && doc_is_layered()) {
        status_message("Saved a flattened copy - layers are kept only in .aps");
        return;
    }
    strncpy(doc->path, path, sizeof(doc->path) - 1);
    doc->modified = FALSE;
    ui_draw_tab();
    status_message("Saved");
}

static void after_edit(void)
{
    doc_masks_compact();
    ui_draw_panels();
    ui_draw_tab();
}

void do_command(LONG c)
{
    char path[256];

    /* gesperrte Ebene: Befehle, die Pixel oder Lage aendern */
    if (((c >= CMD_ADJ_BRICON && c <= CMD_FLT_LAST) || c == CMD_FILL_FG || c == CMD_FILL_BG ||
         c == CMD_ERASE || c == CMD_CUT || c == CMD_LAYER_VIA_CUT || c == CMD_RASTERIZE ||
         c == CMD_EDIT_FILL || c == CMD_EDIT_STROKE ||
         c == CMD_TEXT_EDIT) && locked(LOCK_PIXELS))
        return;
    if ((c == CMD_FREE_TF || (c >= CMD_TF_DISTORT && c <= CMD_TF_WARP) || (c >= CMD_FLIP_H && c <= CMD_ROT90CCW)) &&
        !transform_active() && locked(LOCK_POS | LOCK_PIXELS))
        return;

    /* andere Befehle schliessen einen offenen Text ab */
    if (text_active() && c != CMD_TEXT_COMMIT && c != CMD_TEXT_CANCEL &&
        !view_cmd(c) &&
        !(c >= CMD_TYPE_CHAR && c <= CMD_LOREM)) {   /* Textbefehle wirken im laufenden Text */
        text_commit();
        ui_draw_options();
    }
    if (liquify_active() && c != CMD_LQ_APPLY && c != CMD_LQ_CANCEL && c != CMD_FLT_LIQUIFY &&
        !view_cmd(c)) {
        lq_dragging = FALSE;
        liquify_apply();
        ui_draw_options();
    }
    /* andere Bearbeitungen schliessen eine offene Transformation ab */
    if (transform_active() && c != CMD_TF_APPLY && c != CMD_TF_CANCEL && c != CMD_FREE_TF &&
        !(c >= CMD_TF_FREE && c <= CMD_TF_WARP) &&
        !view_cmd(c)) {
        tf_dragging = tf_pending = FALSE;
        transform_apply();
        ui_draw_options();
    }

    switch (c) {
    case CMD_TEXT_COMMIT:
        text_commit();
        ui_draw_options();
        after_edit();
        break;
    case CMD_TEXT_CANCEL:
        text_cancel();
        ui_draw_options();
        after_edit();
        break;
    case CMD_FREE_TF:
        if (!transform_active() && !painting && !moving && transform_begin()) {
            ui_draw_options();
            status_message("Return: apply   Esc: cancel");
        }
        break;
    case CMD_FLT_LIQUIFY:
        if (liquify_active() || painting || moving || locked(LOCK_PIXELS))
            break;
        finish_interaction();
        if (liquify_begin())
            status_message("Liquify: paint over the image   Return: apply   Esc: cancel");
        ui_draw_options();
        update_cursor();
        break;
    case CMD_LQ_APPLY:
    case CMD_LQ_CANCEL:
        lq_dragging = FALSE;
        busy(TRUE);
        if (c == CMD_LQ_APPLY)
            liquify_apply();
        else
            liquify_cancel();
        busy(FALSE);
        ui_draw_options();
        update_cursor();
        after_edit();
        break;
    case CMD_TF_DISTORT:
    case CMD_TF_PERSP:
    case CMD_TF_WARP:
        if (!transform_active())
            do_command(CMD_FREE_TF);
        if (transform_active()) {
            transform_set_mode(c - CMD_TF_FREE);
            transform_update(TRUE);
            ui_draw_options();
        }
        break;
    case CMD_TF_APPLY:
        tf_dragging = tf_pending = FALSE;
        busy(TRUE);
        transform_apply();
        busy(FALSE);
        ui_draw_options();
        after_edit();
        break;
    case CMD_TF_CANCEL:
        tf_dragging = tf_pending = FALSE;
        transform_cancel();
        ui_draw_options();
        after_edit();
        break;
    case CMD_FLIP_H: case CMD_FLIP_V: case CMD_ROT180: case CMD_ROT90CW: case CMD_ROT90CCW:
        if (painting || moving)
            break;
        busy(TRUE);
        transform_quick(c == CMD_FLIP_H ? -1.0f : 1.0f, c == CMD_FLIP_V ? -1.0f : 1.0f,
                        c == CMD_ROT180 ? 3.14159265f : c == CMD_ROT90CW ? 1.5707963f :
                        c == CMD_ROT90CCW ? -1.5707963f : 0.0f);
        busy(FALSE);
        after_edit();
        break;
    case CMD_NEW: {
        static NewDocSpec ns = { "", 1920, 1080, 72, 0, 0 };
        Doc *d;
        if (!ui_new_document(&ns))
            break;
        if ((d = doc_new(ns.w, ns.h, DOC_EMPTY))) {
            Layer *l = d->layers[0];
            LONG i, k;
            if (ns.bg == 3)
                strcpy(l->name, "Layer 1");     /* transparent: normale Ebene */
            else {
                ULONG c = ns.bg == 0 ? 0xFFFFFFFF : ns.bg == 1 ? 0xFF000000 : 0xFF000000 | bg_color;
                for (i = 0; i < d->tw * d->th; i++) {
                    Tile *t = tile_new();
                    if (!t)
                        break;
                    for (k = 0; k < TILE_PIX; k++)
                        t->px[k] = c;
                    tile_update_flag(d, t, i % d->tw, i / d->tw);
                    l->tiles[i] = t;
                }
            }
            strncpy(d->name, ns.name, sizeof(d->name) - 1);
            d->colors = ns.colors;
            d->ppi = ns.ppi;
            switch_doc(d);
        } else
            request("Not enough memory", "OK");
        break; }
    case CMD_OPEN:
        if (ask_file(FALSE, path, sizeof(path)))
            open_file(path);
        break;
    case CMD_CLOSE_ALL:
        while (ndocs > 1 && discard_ok())
            close_doc();
        if (ndocs == 1 && discard_ok())
            close_doc();
        break;
    case CMD_REVERT: {
        char p[256];
        if (!doc->path[0] || request("Revert to the saved version?\nAll changes will be lost.",
                                     "Revert|Cancel") != 1)
            break;
        strcpy(p, doc->path);
        doc->modified = FALSE;
        close_doc();
        open_file(p);
        break; }
    case CMD_TOGGLE_LAST:
        /* zwischen letztem und vorletztem Zustand wechseln */
        if (doc->ucur && !doc->ucur->next)
            do_command(CMD_UNDO);
        else
            do_command(CMD_REDO);
        break;
    case CMD_IROT180: case CMD_IROT90CW: case CMD_IROT90CCW: case CMD_IROT_ARB:
    case CMD_IFLIP_H: case CMD_IFLIP_V: {
        static LONG arb = 15;
        LONG deg = c == CMD_IROT180 ? 180 : c == CMD_IROT90CW ? 90 : c == CMD_IROT90CCW ? -90 : 0;
        BOOL r;
        if (c == CMD_IROT_ARB) {
            DlgSlider d = { "Angle (CW):", &arb, -359, 359, "°", 1 };
            if (!dialog_run("Rotate Canvas", &d, 1, NULL))
                break;
            deg = arb;
        }
        finish_interaction();
        busy(TRUE);
        r = doc_rotate(deg, c == CMD_IFLIP_H, c == CMD_IFLIP_V,
                       c == CMD_IFLIP_H ? "Flip Canvas Horizontal" : c == CMD_IFLIP_V ? "Flip Canvas Vertical" :
                       "Rotate Canvas");
        busy(FALSE);
        if (!r)
            request("Not enough memory", "OK");
        selection_changed();
        ui_draw_all();
        view_inval_all();
        break; }
    case CMD_COLOR_RANGE: {
        static LONG fuzz = 40, inv = 0;
        static ULONG col;
        UBYTE *old = AllocVec(doc->w * doc->h, MEMF_ANY);
        DlgSlider ds[] = {
            { "Color:", (LONG *)&col, DLG_COLOR, DLG_COLOR, "", 1 },
            { "Fuzziness:", &fuzz, 1, 200, "", 1 },
            { "Invert", &inv, DLG_CHECK, DLG_CHECK, "", 1 },
        };
        if (!old)
            break;
        finish_interaction();
        CopyMem(doc->sel, old, doc->w * doc->h);
        col = fg_color;
        cr_col = &col; cr_fuzz = &fuzz; cr_inv = &inv;
        if (!dialog_run("Color Range", ds, 3, color_range_preview)) {
            CopyMem(old, doc->sel, doc->w * doc->h);
            sel_update_bounds();
        } else
            sel_color_range(col, fuzz, inv);
        FreeVec(old);
        selection_changed();
        break; }
    case CMD_ALL_LAYERS: {
        /* alle Ebenen der obersten Stufe ausser dem Hintergrund */
        LONG k, top = -1;
        for (k = 0; k < doc->nlayers; k++) {
            Layer *l = doc->layers[k];
            l->picked = l->depth == 0 && !(k == 0 && !strcmp(l->name, "Background"));
            if (l->picked)
                top = k;
        }
        if (top >= 0)
            doc->active = top;
        doc->edit_mask = FALSE;
        ui_draw_all();
        break; }
    case CMD_DESEL_LAYERS:
        doc_pick_clear();
        ui_draw_all();
        break;
    case CMD_SEL_GROW_SIM:
    case CMD_SEL_SIMILAR:
        busy(TRUE);
        if (!sel_grow_similar(c == CMD_SEL_SIMILAR, wand_tol))
            request("Not enough memory", "OK");
        busy(FALSE);
        selection_changed();
        break;
    case CMD_AA_SMOOTH: case CMD_AA_SHARP: case CMD_AA_NONE:
    case CMD_ALIGN_LEFT: case CMD_ALIGN_CENTER: case CMD_ALIGN_RIGHT:
        if (c <= CMD_AA_NONE)
            text_aa = c - CMD_AA_SMOOTH;
        else
            text_align = c - CMD_ALIGN_LEFT;
        text_restyle();
        after_edit();
        ui_draw_options();
        break;
    case CMD_TYPE_CHAR: {
        LONG tr = text_track, ld = text_lead;
        DlgSlider ds[] = {
            { "Tracking:", &text_track, -200, 1000, "", 1 },
            { "Leading (0 = Auto):", &text_lead, 0, 1000, "px", 1 },
        };
        if (!text_active() && doc->layers[doc->active]->text) {
            text_track = doc->layers[doc->active]->text->tracking;
            text_lead = doc->layers[doc->active]->text->leading;
            tr = text_track; ld = text_lead;
        }
        if (!dialog_run("Character", ds, 2, text_active() ? text_refresh : NULL)) {
            text_track = tr;
            text_lead = ld;
            text_refresh();
            break;
        }
        text_restyle();
        after_edit();
        break; }
    case CMD_LOREM:
        text_insert("Lorem ipsum dolor sit amet,\nconsectetur adipiscing elit.");
        break;
    case CMD_PATH_SEL:
        if (path_to_selection()) {
            path_end();
            selection_changed();
        }
        break;
    case CMD_PATH_FILL:
    case CMD_PATH_STROKE:
        if (locked(LOCK_PIXELS))
            break;
        busy(TRUE);
        if (c == CMD_PATH_FILL)
            path_fill();
        else
            path_stroke();
        busy(FALSE);
        after_edit();
        break;
    case CMD_PATH_DEL:
        path_delete();
        ui_draw_options();
        break;
    case CMD_EDIT_FILL:
        finish_interaction();
        edit_fill();
        after_edit();
        break;
    case CMD_EDIT_STROKE:
        finish_interaction();
        edit_stroke();
        after_edit();
        break;
    case CMD_CLIP_TOGGLE:
        doc_clip_toggle();
        after_edit();
        break;
    case CMD_GROUP_NEW:
        doc_new_group();
        after_edit();
        break;
    case CMD_GROUP:
        if (npicked() > 1)
            doc_group_picked("Group Layers");
        else
            doc_group_layers();
        after_edit();
        break;
    case CMD_UNGROUP:
        doc_ungroup();
        after_edit();
        break;
    case CMD_MERGE_GROUP:
        finish_interaction();
        busy(TRUE);
        doc_merge_group();
        busy(FALSE);
        after_edit();
        break;
    case CMD_ADJ_EDIT:
        doc_edit_adjust(adj_preview);
        after_edit();
        break;
    case CMD_NADJ_BRICON: case CMD_NADJ_LEVELS: case CMD_NADJ_CURVES: case CMD_NADJ_HUESAT:
    case CMD_NADJ_DESAT: case CMD_NADJ_INVERT: case CMD_NADJ_THRESH: case CMD_NADJ_POSTER: {
        static const LONG map[8] = { CMD_ADJ_BRICON, CMD_ADJ_LEVELS, CMD_ADJ_CURVES, CMD_ADJ_HUESAT,
                                     CMD_ADJ_DESAT, CMD_ADJ_INVERT, CMD_ADJ_THRESH, CMD_ADJ_POSTER };
        finish_interaction();
        doc_new_adjust(map[c - CMD_NADJ_BRICON],
                       map[c - CMD_NADJ_BRICON] == CMD_ADJ_CURVES ? adj_preview : NULL);
        after_edit();
        break; }
    case CMD_SCREENMODE: {
        ULONG id = ask_screenmode(scr, GetVPModeID(&scr->ViewPort));
        LONG w, h;
        redraw_all();
        if (id != INVALID_ID && mode_size(id, &w, &h) && id != GetVPModeID(&scr->ViewPort)) {
            finish_interaction();
            if (!switch_screen(id)) {
                if (!win) {
                    quit = TRUE;    /* kein Bildschirm mehr */
                    break;
                }
                request("This screen mode could not be opened.", "OK");
            } else {
                pref_mode = id;
                ui_prefs_save();
            }
        }
        break; }
    case CMD_TOOLBAR:
        ui_toolbar_dialog();
        break;
    case CMD_CROP_CANCEL:
        crop_end();
        crop_begin();           /* Rahmen zurueck auf das ganze Bild */
        ui_draw_options();
        break;
    case CMD_CROP_COMMIT:
        busy(TRUE);
        if (!crop_commit())
            request("Not enough memory", "OK");
        busy(FALSE);
        selection_changed();
        ui_draw_all();
        view_inval_all();
        break;
    case CMD_CROP:
    case CMD_TRIM:
    case CMD_REVEAL_ALL: {
        BOOL r;
        if (c == CMD_CROP && !doc->hassel)
            break;
        finish_interaction();
        busy(TRUE);
        r = c == CMD_CROP ? doc_canvas_rect(doc->selx0, doc->sely0, doc->selx1, doc->sely1) :
            c == CMD_TRIM ? doc_trim() : doc_reveal_all();
        busy(FALSE);
        if (!r)
            request("Not enough memory", "OK");
        selection_changed();
        ui_draw_all();
        view_inval_all();
        break; }
    case CMD_DUP_DOC: {
        Doc *d;
        finish_interaction();
        busy(TRUE);
        d = doc_duplicate();
        busy(FALSE);
        if (d)
            switch_doc(d);
        else
            request("Not enough memory", "OK");
        break; }
    case CMD_LAYER_VIA_COPY:
    case CMD_LAYER_VIA_CUT:
        finish_interaction();
        doc_layer_via(c == CMD_LAYER_VIA_CUT);
        if (doc->hassel) {
            sel_none();
            selection_changed();
        }
        after_edit();
        break;
    case CMD_LAYER_TOP:
    case CMD_LAYER_BOTTOM:
        {
            /* ganz nach oben/unten (aus Gruppen heraus) */
            UndoRec *u = undo_begin_struct("Layer Order");
            doc_move_block(doc->active, c == CMD_LAYER_TOP ? doc->nlayers - layer_block(doc->active) : 0, 0);
            undo_commit(u);
        }
        after_edit();
        break;
    case CMD_MERGE_VISIBLE:
        finish_interaction();
        busy(TRUE);
        doc_merge_visible();
        busy(FALSE);
        after_edit();
        break;
    case CMD_CLOSE:
        if (discard_ok())
            close_doc();
        break;
    case CMD_CUT:
    case CMD_COPY:
    case CMD_COPY_MERGED:
        if (painting || moving)
            break;
        busy(TRUE);
        if (clip_copy(c == CMD_COPY_MERGED, c == CMD_CUT))
            status_message(c == CMD_CUT ? "Cut" : "Copied");
        busy(FALSE);
        if (c == CMD_CUT)
            after_edit();
        break;
    case CMD_PASTE:
        if (painting || moving)
            break;
        busy(TRUE);
        if (clip_paste() && doc->hassel) {
            remember_sel();     /* wie Photoshop: Einfuegen hebt die Auswahl auf, Reselect holt sie */
            sel_none();
            selection_changed();
        }
        busy(FALSE);
        after_edit();
        break;
    case CMD_NEW_FROM_CLIP: {
        Doc *d;
        busy(TRUE);
        d = clip_new_doc();
        busy(FALSE);
        if (d)
            switch_doc(d);
        break; }
    case CMD_NEXT_DOC:
        if (ndocs > 1)
            main_activate_doc(docs[(doc_index(doc) + 1) % ndocs]);
        break;
    case CMD_SAVE:
        if (doc->path[0] && (is_aps(doc->path) || !doc_is_layered()))
            save_file(doc->path);
        else {
            /* neu oder Ebenen in PNG/JPEG/IFF: Save As mit .aps-Vorschlag */
            ask_as_aps = doc->path[0] != 0;
            do_command(CMD_SAVEAS);
            ask_as_aps = FALSE;
        }
        break;
    case CMD_PAL_LOAD: {
        char err[80], msg[400];
        QPalette *q = AllocVec(sizeof(QPalette), MEMF_ANY | MEMF_CLEAR);
        BOOL ok;
        if (!q)
            break;
        ask_title = "Load Palette (IFF picture or palette)";
        ok = ask_file(FALSE, path, sizeof(path));
        ask_title = NULL;
        if (ok && !file_load_palette(path, q, err)) {
            sprintf(msg, "Cannot load the palette from %s:\n%s", path, err);
            request(msg, "OK");
            ok = FALSE;
        }
        if (!ok) {
            FreeVec(q);
            break;
        }
        if (doc->fixpal)
            FreeVec(doc->fixpal);
        doc->fixpal = q;
        doc->modified = TRUE;
        ui_draw_tab();
        filter_reduce_fixed();      /* gleich ansehen, wie das Bild damit aussieht */
        after_edit();
        break; }
    case CMD_PAL_SAVE: {
        char err[80], msg[400];
        const void *q = doc->qpal ? doc->qpal : doc->fixpal;
        BOOL ok;
        if (!q) {
            request("This image has no palette yet.\nUse Filter > Amiga > Reduce Colors first.", "OK");
            break;
        }
        ask_ext = ".pal";
        ask_title = "Save Palette (IFF)";
        ok = ask_file(TRUE, path, sizeof(path));
        ask_ext = ".aps";
        ask_title = NULL;
        if (!ok)
            break;
        if (!strchr(FilePart(path), '.'))
            strncat(path, ".pal", sizeof(path) - strlen(path) - 1);
        if (!file_save_palette(path, q, err)) {
            sprintf(msg, "Cannot save %s:\n%s", path, err);
            request(msg, "OK");
        } else
            status_message("Palette saved");
        break; }
    case CMD_GEN_FILL:
        ai_open_prompt();
        break;
    case CMD_GEN_REMOVE:
        ai_start(NULL);
        break;
    case CMD_SEL_SUBJECT:
        ai_segment(1);
        break;
    case CMD_AI_SETTINGS:
        ai_settings();
        break;
    case CMD_AI_KEY:
        ai_key_dialog();
        break;
    case CMD_GEN_AGAIN:
        ai_regenerate();
        break;
    case CMD_GEN_PREV:
    case CMD_GEN_NEXT:
        ai_gen_step(c == CMD_GEN_NEXT ? 1 : -1);
        break;
    case CMD_UPD_CHECK:
        upd_check(TRUE);
        break;
    case CMD_UPD_AUTO:
        upd_auto = !upd_auto;
        ui_prefs_save();
        break;
    case CMD_TASKBAR:
        ai_taskbar = !ai_taskbar;
        ui_prefs_save();
        ai_bar_refresh();
        view_inval_all();
        break;
    case CMD_REMOVE_BG:
        ai_segment(2);
        break;
    case CMD_PAL_FIX:
        if (!doc->qpal) {
            request("This image has no palette yet.\nUse Filter > Amiga > Reduce Colors first.", "OK");
            break;
        }
        if (!doc->fixpal && !(doc->fixpal = AllocVec(sizeof(QPalette), MEMF_ANY)))
            break;
        *(QPalette *)doc->fixpal = *(QPalette *)doc->qpal;
        ((QPalette *)doc->fixpal)->mode = QM_NORMAL;
        if (((QPalette *)doc->fixpal)->n > 32 && ((QPalette *)doc->qpal)->mode == QM_EHB)
            ((QPalette *)doc->fixpal)->n = 32;
        doc->modified = TRUE;
        ui_draw_tab();
        status_message("Palette fixed - Reduce Colors > Palette: Fixed uses it");
        break;
    case CMD_EXPORT_IFF: {
        char err[80], msg[400];
        BOOL ok;
        ask_ext = ".iff";
        ok = ask_file(TRUE, path, sizeof(path));
        ask_ext = ".aps";
        if (!ok)
            break;
        if (!strchr(FilePart(path), '.'))
            strncat(path, ".iff", sizeof(path) - strlen(path) - 1);
        status_message("Exporting...");
        busy(TRUE);
        ok = file_export_iff(doc, path, err);
        busy(FALSE);
        if (!ok) {
            sprintf(msg, "Cannot export %s:\n%s", path, err);
            request(msg, "OK");
            status_message("");
        } else
            status_message("Exported as IFF ILBM");
        break; }
    case CMD_SAVEAS:
        if (ask_file(TRUE, path, sizeof(path))) {
            if (!strchr(FilePart(path), '.'))
                strncat(path, ".aps", sizeof(path) - strlen(path) - 1);
            save_file(path);
        }
        break;
    case CMD_QUIT: {
        LONG i, n = 0;
        char msg[96];
        for (i = 0; i < ndocs; i++)
            n += docs[i]->modified ? 1 : 0;
        if (n == 0)
            quit = TRUE;
        else {
            sprintf(msg, n == 1 ? "One image has unsaved changes.\nQuit anyway?"
                                : "%ld images have unsaved changes.\nQuit anyway?", (long)n);
            if (request(msg, "Quit|Cancel") == 1)
                quit = TRUE;
        }
        break; }
    case CMD_UNDO:
    case CMD_REDO: {
        /* am Anfang/Ende des Protokolls: Zustand vor/nach einer Groessenaenderung */
        Doc *to = NULL;
        if (c == CMD_UNDO && !doc->ucur)
            to = doc->size_prev;
        else if (c == CMD_REDO && !(doc->ucur ? doc->ucur->next : doc->uhead))
            to = doc->size_next;
        if (to) {
            finish_interaction();
            doc_size_switch(to);
            selection_changed();
            redraw_all();
        } else if (c == CMD_UNDO ? undo_step() : redo_step())
            after_edit();
        break; }
    case CMD_FILL_FG:
    case CMD_FILL_BG:
        doc_fill_selection(c == CMD_FILL_FG ? fg_color : bg_color, FALSE);
        after_edit();
        break;
    case CMD_ERASE:
        doc_fill_selection(0, TRUE);
        after_edit();
        break;
    case CMD_SEL_ALL:
        sel_all();
        selection_changed();
        break;
    case CMD_SEL_NONE:
        remember_sel();
        sel_none();
        selection_changed();
        break;
    case CMD_RESELECT:
        if (doc->lastsel) {
            CopyMem(doc->lastsel, doc->sel, doc->w * doc->h);
            sel_update_bounds();
            selection_changed();
        }
        break;
    case CMD_SEL_INVERT:
        sel_invert();
        selection_changed();
        break;
    case CMD_FEATHER:
    case CMD_GROW:
    case CMD_SHRINK:
    case CMD_BORDER:
    case CMD_SMOOTH: {
        static LONG r_feather = 5, r_grow = 5, r_shrink = 5, r_border = 10, r_smooth = 2;
        DlgSlider d = { c == CMD_BORDER ? "Width:" : c == CMD_SMOOTH ? "Sample Radius:" : "Radius:",
                        NULL, 1, 100, "px", 1 };
        const char *title = c == CMD_FEATHER ? "Feather Selection" : c == CMD_GROW ? "Expand Selection" :
                            c == CMD_SHRINK ? "Contract Selection" : c == CMD_BORDER ? "Border Selection" :
                            "Smooth Selection";
        if (!doc->hassel) {
            status_message("No selection");
            break;
        }
        d.val = c == CMD_FEATHER ? &r_feather : c == CMD_GROW ? &r_grow : c == CMD_SHRINK ? &r_shrink :
                c == CMD_BORDER ? &r_border : &r_smooth;
        if (!dialog_run(title, &d, 1, NULL))
            break;
        busy(TRUE);
        if (c == CMD_FEATHER)
            sel_feather(*d.val);
        else if (c == CMD_GROW)
            sel_grow(*d.val);
        else if (c == CMD_SHRINK)
            sel_shrink(*d.val);
        else if (c == CMD_BORDER)
            sel_border(*d.val);
        else
            sel_smooth(*d.val);
        busy(FALSE);
        selection_changed();
        break; }
    case CMD_LAYER_NEW:
        doc_add_layer();
        after_edit();
        break;
    case CMD_LAYER_DEL:
        if (npicked() > 1)
            doc_delete_picked();
        else
            doc_delete_layer();
        after_edit();
        break;
    case CMD_LAYER_DUP:
        doc_duplicate_layer();
        after_edit();
        break;
    case CMD_LAYER_UP:
    case CMD_LAYER_DOWN:
        doc_step_layer(c == CMD_LAYER_UP);
        after_edit();
        break;
    case CMD_MERGE_DOWN:
        busy(TRUE);
        if (npicked() > 1)
            doc_merge_picked();         /* Merge Layers */
        else if (doc->layers[doc->active]->kind == LK_GROUP)
            doc_merge_group();          /* wie Photoshop: Strg+E auf Gruppe */
        else
            doc_merge_down();
        busy(FALSE);
        after_edit();
        break;
    case CMD_LAYER_RENAME:
        ui_rename_layer();
        break;
    case CMD_MASK_REVEAL_ALL:
    case CMD_MASK_HIDE_ALL:
    case CMD_MASK_REVEAL_SEL:
        doc_mask_add(c == CMD_MASK_HIDE_ALL ? MASK_HIDE_ALL : c == CMD_MASK_REVEAL_SEL ? MASK_REVEAL_SEL : MASK_REVEAL_ALL);
        selection_changed();
        after_edit();
        break;
    case CMD_MASK_DELETE:
        doc_mask_delete();
        after_edit();
        break;
    case CMD_MASK_APPLY:
        doc_mask_apply();
        after_edit();
        break;
    case CMD_MASK_TOGGLE:
        doc_mask_toggle();
        after_edit();
        break;
    case CMD_TEXT_EDIT:
        if (doc->layers[doc->active]->text) {
            ui_set_tool(T_TEXT);
            text_begin_edit(doc->layers[doc->active]);
            ui_draw_options();
            ui_draw_toolbar();
            after_edit();
        }
        break;
    case CMD_IMAGE_SIZE:
    case CMD_CANVAS_SIZE: {
        static BOOL lock = TRUE;
        static LONG anchor = 4;
        LONG nw = doc->w, nh = doc->h;
        BOOL r;
        finish_interaction();
        if (!size_dialog(c == CMD_IMAGE_SIZE ? "Image Size" : "Canvas Size", &nw, &nh, doc->w, doc->h,
                         c == CMD_IMAGE_SIZE ? &lock : NULL, c == CMD_CANVAS_SIZE ? &anchor : NULL))
            break;
        busy(TRUE);
        r = c == CMD_IMAGE_SIZE ? doc_image_size(nw, nh) : doc_canvas_size(nw, nh, anchor);
        busy(FALSE);
        if (!r)
            request("Not enough memory", "OK");
        selection_changed();
        ui_draw_all();
        view_inval_all();
        break; }
    case CMD_LAYER_STYLE:
        if (doc->layers[doc->active]->kind != LK_PIXEL) {
            status_message("Layer styles work on pixel layers");
            break;
        }
        finish_interaction();
        fx_dialog();
        after_edit();
        break;
    case CMD_LAYER_STYLE_CLEAR:
        fx_clear();
        after_edit();
        break;
    case CMD_RASTERIZE:
        doc_rasterize_text(doc->layers[doc->active]);
        after_edit();
        break;
    case CMD_FLATTEN:
        doc_flatten();
        after_edit();
        break;
    case CMD_ZOOM_IN:
        view_zoom_step(1, cvx + cvw / 2, cvy + cvh / 2);
        break;
    case CMD_ZOOM_OUT:
        view_zoom_step(-1, cvx + cvw / 2, cvy + cvh / 2);
        break;
    case CMD_ZOOM_FIT:
        view_fit();
        break;
    case CMD_ZOOM_100:
        view_set_zoom(1.0f, cvx + cvw / 2, cvy + cvh / 2);
        break;
    case CMD_RULERS:
        rulers_set(!rulers_on);
        ui_prefs_save();
        break;
    case CMD_GUIDES_SHOW:
        guides_show = !guides_show;
        guides_changed();
        break;
    case CMD_GUIDES_SNAP:
        guides_snap = !guides_snap;
        ui_prefs_save();
        status_message(guides_snap ? "Snap on" : "Snap off");
        break;
    case CMD_GUIDES_LOCK:
        guides_lock = !guides_lock;
        break;
    case CMD_GUIDES_CLEAR:
        guides_clear();
        ui_draw_tab();
        break;
    case CMD_GUIDE_NEW: {
        static LONG orient, pos;
        DlgSlider sl[2] = {
            { "Orientation:", &orient, DLG_CHOICE, DLG_CHOICE, "Horizontal|Vertical", 1 },
            { "Position:", &pos, 0, 0, "px", 1 } };
        sl[1].max = doc->w > doc->h ? doc->w : doc->h;
        if (pos > sl[1].max)
            pos = sl[1].max;
        if (dialog_run("New Guide", sl, 2, NULL)) {
            if (!guides_show) {
                guides_show = TRUE;
                guides_changed();
            }
            guides_add(pos, orient == 1);
            ui_draw_tab();
        }
        break; }
    case CMD_ADJ_BRICON: case CMD_ADJ_LEVELS: case CMD_ADJ_HUESAT: case CMD_ADJ_DESAT:
    case CMD_ADJ_INVERT: case CMD_ADJ_THRESH: case CMD_ADJ_POSTER: case CMD_ADJ_CURVES:
    case CMD_FLT_GAUSS: case CMD_FLT_UNSHARP: case CMD_FLT_NOISE: case CMD_FLT_EMBOSS:
    case CMD_FLT_MOSAIC: case CMD_FLT_LAST:
    case CMD_FLT_MOTION: case CMD_FLT_BOX: case CMD_FLT_MEDIAN: case CMD_FLT_SHARPEN:
    case CMD_FLT_SHARPEN_MORE: case CMD_FLT_HIGHPASS: case CMD_FLT_EDGES: case CMD_FLT_QUANT:
        if (painting || moving)
            break;
        filter_command(c);
        after_edit();
        break;
    case CMD_ABOUT:
        request("AmiPixelShop " APS_VERSION " - Early Beta\nImage editor for AmigaOS 3.2 (RTG)\n"
                "MIT License - github.com/radi7777/AmiPixelShop\n\n"
                "stb_image, stb_image_write, stb_truetype: Sean Barrett (Public Domain / MIT)\n"
                "zlib: Jean-loup Gailly, Mark Adler (zlib License)\n"
                "UI font: Open Sans (Apache License 2.0)", "OK");
        break;
    }
    update_cursor();
}

/* ---- Start / Ende ---- */

void main_request_quit(void)
{
    quit = TRUE;
}

static void cleanup(void)
{
    LONG i;
    ai_shutdown();
    upd_shutdown();
    for (i = 0; i < ndocs; i++) {
        doc = docs[i];          /* Undo-Verlauf gehoert zum jeweiligen Dokument */
        undo_clear();
        doc_free(docs[i]);
    }
    if (!ndocs && doc)          /* CONVERT: Dokument ohne Tab */
        doc_free(doc);
    ndocs = 0;
    doc = NULL;
    if (win) {
        if (ptr_hidden)
            ClearPointer(win);
        CloseWindow(win);
    }
    if (scr) CloseScreen(scr);
    if (blank_ptr) FreeVec(blank_ptr);
    if (bb) FreeVec(bb);
    if (timer_open) CloseDevice((struct IORequest *)treq);
    if (treq) DeleteIORequest((struct IORequest *)treq);
    if (tport) DeleteMsgPort(tport);
    clip_free();
    text_free();
    filter_free();
    if (AslBase) CloseLibrary(AslBase);
    if (KeymapBase) CloseLibrary(KeymapBase);
    if (P96Base) CloseLibrary(P96Base);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
}

/* nur Modi von 1024x600 bis 1920x1080 mit mindestens 24 Bit */
static ULONG mode_filter(struct Hook *h, APTR req, ULONG id)
{
    struct DimensionInfo di;
    LONG w, h2;
    if (ModeNotAvailable(id) || !GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof(di), DTAG_DIMS, id))
        return FALSE;
    w = di.Nominal.MaxX - di.Nominal.MinX + 1;
    h2 = di.Nominal.MaxY - di.Nominal.MinY + 1;
    return w >= 1024 && h2 >= 600 && w <= 1920 && h2 <= 1080 && di.MaxDepth >= 24;
}

/* Bildschirmmodus waehlen: RTG, ab 24 Bit, hoechstens 1920x1080 */
static ULONG ask_screenmode(struct Screen *on, ULONG initial)
{
    static struct Hook filter;
    struct ScreenModeRequester *sm;
    ULONG id = INVALID_ID;
    filter.h_Entry = (HOOKFUNC)HookEntry;
    filter.h_SubEntry = (HOOKFUNC)mode_filter;
    if (!AslBase && !(AslBase = OpenLibrary("asl.library", 39)))
        return INVALID_ID;
    sm = AllocAslRequestTags(ASL_ScreenModeRequest,
        ASLSM_TitleText, (ULONG)"AmiPixelShop: Screen Mode (max. 1920 x 1080)",
        ASLSM_MinWidth, 1024, ASLSM_MinHeight, 600,
        ASLSM_MaxWidth, 1920, ASLSM_MaxHeight, 1080,
        ASLSM_MinDepth, 24,
        ASLSM_FilterFunc, (ULONG)&filter,
        ASLSM_InitialDisplayID, initial,
        on ? ASLSM_Screen : TAG_IGNORE, (ULONG)on,
        TAG_DONE);
    if (!sm)
        return INVALID_ID;
    if (AslRequestTags(sm, TAG_DONE))
        id = sm->sm_DisplayID;
    FreeAslRequest(sm);
    return id;
}

/* Groesse eines Modus (Nominalgroesse) */
static BOOL mode_size(ULONG id, LONG *w, LONG *h)
{
    struct DimensionInfo di;
    if (id == INVALID_ID || ModeNotAvailable(id) ||
        !GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof(di), DTAG_DIMS, id))
        return FALSE;
    *w = di.Nominal.MaxX - di.Nominal.MinX + 1;
    *h = di.Nominal.MaxY - di.Nominal.MinY + 1;
    return *w <= 1920 && *h <= 1080;
}

/* eigenen Bildschirm samt Fenster und Hintergrundpuffer oeffnen */
static BOOL open_screen(ULONG modeid)
{
    scr = OpenScreenTags(NULL,
        SA_DisplayID, modeid,
        SA_Width, SW, SA_Height, SH,
        SA_Depth, 24,
        SA_Quiet, TRUE,
        SA_ShowTitle, FALSE,
        SA_Type, CUSTOMSCREEN,
        SA_Title, (ULONG)"AmiPixelShop",
        SA_LikeWorkbench, TRUE,
        TAG_DONE);
    if (!scr)
        return FALSE;
    SW = scr->Width;
    SH = scr->Height;

    win = OpenWindowTags(NULL,
        WA_CustomScreen, (ULONG)scr,
        WA_Left, 0, WA_Top, 0, WA_Width, SW, WA_Height, SH,
        WA_Backdrop, TRUE, WA_Borderless, TRUE,
        WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_ReportMouse, TRUE,
        WA_IDCMP, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_RAWKEY | IDCMP_INTUITICKS,
        TAG_DONE);
    bb = AllocVec(SW * SH * 4, MEMF_ANY);
    return win && bb;
}

static void close_screen(void)
{
    if (win) {
        if (ptr_hidden)
            ClearPointer(win);
        ptr_hidden = FALSE;
        CloseWindow(win);
    }
    if (scr)
        CloseScreen(scr);
    if (bb)
        FreeVec(bb);
    win = NULL;
    scr = NULL;
    bb = NULL;
}

/* Bildschirmmodus wechseln, ohne neu zu starten */
static BOOL switch_screen(ULONG modeid)
{
    ULONG old = GetVPModeID(&scr->ViewPort);
    LONG ow = SW, oh = SH, i;
    if (!mode_size(modeid, &SW, &SH)) {
        SW = ow;
        SH = oh;
        return FALSE;
    }
    view_save();
    close_screen();
    if (!open_screen(modeid)) {
        /* zurueck zum alten Modus */
        close_screen();
        SW = ow;
        SH = oh;
        if (!open_screen(old))
            return FALSE;       /* sehr unwahrscheinlich: Programm endet */
    }
    gfx_noclip();
    view_free();
    view_init();
    for (i = 0; i < ndocs; i++)
        docs[i]->vset = FALSE;  /* neu einpassen */
    view_restore();
    ui_draw_all();
    view_inval_all();
    view_flush();
    return TRUE;
}

static BOOL open_display(void)
{
    struct Screen *wb;
    struct TagItem bidtags[5];
    ULONG modeid;

    if (!(wb = LockPubScreen(NULL)))
        return FALSE;
    SW = wb->Width;
    SH = wb->Height;
    /* gespeicherter Modus, sonst fragen */
    modeid = ui_prefs_mode();
    if (!mode_size(modeid, &SW, &SH)) {
        modeid = ask_screenmode(NULL, GetVPModeID(&wb->ViewPort));
        if (mode_size(modeid, &SW, &SH))
            pref_mode = modeid;     /* wird mit den Prefs gespeichert */
        else
            modeid = INVALID_ID;
    }
    if (modeid != INVALID_ID) {
        UnlockPubScreen(NULL, wb);
        goto open;
    }
    SW = wb->Width > 1920 ? 1920 : wb->Width;
    SH = wb->Height > 1080 ? 1080 : wb->Height;
    if (P96Base) {
        bidtags[0].ti_Tag = P96BIDTAG_NominalWidth;  bidtags[0].ti_Data = SW;
        bidtags[1].ti_Tag = P96BIDTAG_NominalHeight; bidtags[1].ti_Data = SH;
        bidtags[2].ti_Tag = P96BIDTAG_Depth;         bidtags[2].ti_Data = 32;
        bidtags[3].ti_Tag = P96BIDTAG_FormatsAllowed;
        bidtags[3].ti_Data = RGBFF_A8R8G8B8 | RGBFF_B8G8R8A8 | RGBFF_R8G8B8A8;
        bidtags[4].ti_Tag = TAG_DONE;
        modeid = p96BestModeIDTagList(bidtags);
    } else {
        bidtags[0].ti_Tag = CYBRBIDTG_NominalWidth;  bidtags[0].ti_Data = SW;
        bidtags[1].ti_Tag = CYBRBIDTG_NominalHeight; bidtags[1].ti_Data = SH;
        bidtags[2].ti_Tag = CYBRBIDTG_Depth;         bidtags[2].ti_Data = 32;
        bidtags[3].ti_Tag = TAG_DONE;
        modeid = BestCModeIDTagList(bidtags);
    }
    if (modeid == (ULONG)INVALID_ID)
        modeid = GetVPModeID(&wb->ViewPort);
    UnlockPubScreen(NULL, wb);
open:
    blank_ptr = AllocVec(16, MEMF_CHIP | MEMF_CLEAR);
    return open_screen(modeid);
}

/* Kommandozeile: AmiPixelShop BLITTEST - Tempo der Uebertragung in den Bildschirm
 * (Picasso96 oder CyberGraphX), Vollbild und 400x300 */
static void blit_test(void)
{
    ULONG t, i;
    LONG k;
    for (k = 0; k < SW * SH; k++)
        bb[k] = 0xFF000000 | (k * 2654435761UL >> 8);
    Printf("%s, %ld x %ld\n", (ULONG)(P96Base ? "Picasso96" : "CyberGraphX"), SW, SH);
    t = now_us();
    for (i = 0; i < 100; i++)
        blit(0, 0, SW, SH);
    t = now_us() - t;
    Printf("full screen: %ld.%ld fps\n", 100000000 / t, (1000000000 / t) % 10);
    t = now_us();
    for (i = 0; i < 1000; i++)
        blit(100 + (i & 63), 100, 400, 300);
    t = now_us() - t;
    Printf("400x300:     %ld us per blit\n", t / 1000);
}

/* Kommandozeile: AmiPixelShop NETTEST - prueft HTTPS, Base64, JSON und den OpenRouter-Key */
static void net_test(void)
{
    static const char *keyfiles[] = { "ENV:AmiPixelShop/openrouter.key", "ENVARC:AmiPixelShop/openrouter.key" };
    char key[160], hdr[200], val[160];
    NetResp r;
    NetCtl ctl;
    const char *v;
    LONG i, n, vl;
    ULONG t0;
    UBYTE raw[256], dec[256];
    char *enc;

    for (i = 0; i < 256; i++)
        raw[i] = i * 7 + 3;
    enc = b64_encode(raw, 256, &n);
    vl = enc ? b64_decode(enc, n, dec) : 0;
    Printf("Base64: %s\n", (ULONG)(vl == 256 && !memcmp(raw, dec, 256) ? "OK" : "FAILED"));
    net_freemem(enc);

    key[0] = 0;
    for (i = 0; i < 2 && !key[0]; i++) {
        BPTR fh = Open((STRPTR)keyfiles[i], MODE_OLDFILE);
        if (fh) {
            n = Read(fh, key, sizeof(key) - 1);
            Close(fh);
            key[n > 0 ? n : 0] = 0;
            while (n > 0 && (UBYTE)key[n - 1] <= ' ')
                key[--n] = 0;
        }
    }
    Printf("API key: %s\n", (ULONG)(key[0] ? "found" : "missing (ENVARC:AmiPixelShop/openrouter.key)"));

    memset(&ctl, 0, sizeof(ctl));
    sprintf(hdr, "Authorization: Bearer %s\r\n", key);
    if (key[0]) {
        t0 = now_us();
        if (!net_https("openrouter.ai", "GET", "/api/v1/key", hdr, NULL, 0, &r, &ctl)) {
            Printf("/key: %s\n", (ULONG)r.err);
        } else {
            Printf("/key: HTTP %ld in %ld ms\n", r.status, (now_us() - t0) / 1000);
            if ((v = json_get((char *)r.body, "data.limit_remaining", &vl)) ||
                (v = json_get((char *)r.body, "data.usage", &vl))) {
                json_str(v, vl, val, sizeof(val));
                Printf("  limit remaining / usage: %s\n", (ULONG)val);
            } else {
                Printf("  %.200s\n", (ULONG)r.body);
            }
            net_free(&r);
        }
    }

    t0 = now_us();
    if (!net_https("openrouter.ai", "GET", "/api/v1/models?output_modalities=image", key[0] ? hdr : NULL,
                   NULL, 0, &r, &ctl)) {
        Printf("/models: %s\n", (ULONG)r.err);
        return;
    }
    Printf("/models: HTTP %ld, %ld bytes in %ld ms\n", r.status, r.len, (now_us() - t0) / 1000);
    for (i = 0; i < 100; i++) {
        char id[96], pr[200];
        sprintf(val, "data.%ld.id", i);
        if (!(v = json_get((char *)r.body, val, &vl)))
            break;
        json_str(v, vl, id, sizeof(id));
        sprintf(val, "data.%ld.pricing", i);
        pr[0] = 0;
        if ((v = json_get((char *)r.body, val, &vl))) {
            if (vl > 199)
                vl = 199;
            memcpy(pr, v, vl);
            pr[vl] = 0;
        }
        Printf("  %s %s\n", (ULONG)id, (ULONG)pr);
    }
    net_free(&r);
}

static int app_main(int argc, char **argv)
{
    LONG dw = 1280, dh = 960;
    const char *loadpath = NULL;
    LONG nfiles = 0;

    if (argc == 3 && argv[1][0] >= '0' && argv[1][0] <= '9') {
        dw = atol(argv[1]);
        dh = atol(argv[2]);
        if (dw < 16) dw = 16;
        if (dh < 16) dh = 16;
        if (dw > 8000) dw = 8000;
        if (dh > 8000) dh = 8000;
    } else if (argc >= 2) {
        loadpath = argv[1];     /* eine oder mehrere Dateien */
        nfiles = argc - 1;
    }

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    {
        /* Picasso96 bevorzugt; sonst CyberGraphX (MorphOS). ENV:AmiPixelShop/cgx = 1
         * erzwingt CyberGraphX zum Testen. */
        char v[4];
        if (GetVar("AmiPixelShop/cgx", v, sizeof(v), GVF_GLOBAL_ONLY) <= 0 || v[0] != '1')
            P96Base = OpenLibrary("Picasso96API.library", 2);
        if (!P96Base)
            CyberGfxBase = OpenLibrary("cybergraphics.library", 41);
    }
    KeymapBase = OpenLibrary("keymap.library", 37);
    if (!IntuitionBase || !GfxBase || (!P96Base && !CyberGfxBase) || !KeymapBase) {
        PutStr("Missing libraries: needs Picasso96API.library or cybergraphics.library\n");
        cleanup();
        return RETURN_FAIL;
    }
    if (!(tport = CreateMsgPort()) ||
        !(treq = (struct timerequest *)CreateIORequest(tport, sizeof(struct timerequest))) ||
        OpenDevice(TIMERNAME, UNIT_MICROHZ, (struct IORequest *)treq, 0) != 0) {
        cleanup();
        return RETURN_FAIL;
    }
    timer_open = TRUE;
    TimerBase = treq->tr_node.io_Device;

    /* Kommandozeile: AmiPixelShop BENCH */
    if (argc == 2 && !strcasecmp(argv[1], "BENCH")) {
        extern void bench_run(void);
        pixel_init();
        bench_run();
        cleanup();
        return RETURN_OK;
    }

    if (argc == 3 && !strcasecmp(argv[1], "LAYERTEST")) {
        extern void layer_test(const char *outdir);
        pixel_init();
        layer_test(argv[2]);
        cleanup();
        return RETURN_OK;
    }

    if (argc == 4 && !strcasecmp(argv[1], "FILTERTEST")) {
        extern void filter_test(const char *in, const char *outdir);
        pixel_init();
        filter_test(argv[2], argv[3]);
        cleanup();
        return RETURN_OK;
    }

    if (argc == 3 && !strcasecmp(argv[1], "UPDVERIFY")) {
        upd_verify_file(argv[2]);
        cleanup();
        return RETURN_OK;
    }

    if (argc == 2 && !strcasecmp(argv[1], "NETTEST")) {
        net_test();
        cleanup();
        return RETURN_OK;
    }

    /* Kommandozeile: AmiPixelShop CONVERT ein aus */
    if (argc == 4 && !strcasecmp(argv[1], "CONVERT")) {
        char err[80];
        ULONG t0, t1, t2;
        pixel_init();
        t0 = now_us();
        doc = file_load(argv[2], err);
        t1 = now_us();
        if (!doc) {
            Printf("Load failed: %s\n", (ULONG)err);
            cleanup();
            return RETURN_ERROR;
        }
        doc_process_dirty();
        if (!file_save(doc, argv[3], err)) {
            Printf("Save failed: %s\n", (ULONG)err);
            cleanup();
            return RETURN_ERROR;
        }
        t2 = now_us();
        Printf("%ld x %ld, %ld layers: loaded in %ld ms, saved in %ld ms\n",
               doc->w, doc->h, doc->nlayers, (t1 - t0) / 1000, (t2 - t1) / 1000);
        cleanup();
        return RETURN_OK;
    }

    if (!open_display()) {
        PutStr("Cannot open screen or window (out of memory?)\n");
        cleanup();
        return RETURN_FAIL;
    }
    gfx_noclip();
    fill(0, 0, SW, SH, COL_PASTE);      /* kein grauer Blitz beim Start */
    blit(0, 0, SW, SH);
    pixel_init();
    view_init();
    {
        Doc *d = doc_new(dw, dh, DOC_WHITE);
        if (!d || !doc_open(d)) {
            PutStr("Not enough memory for the document\n");
            cleanup();
            return RETURN_FAIL;
        }
    }
    ui_prefs_load();
    if (rulers_on) {
        view_layout();
        view_fit();
    }
    if (pref_mode != INVALID_ID)
        ui_prefs_save();        /* gerade gewaehlten Modus festhalten */
    brush_rebuild();
    ui_draw_all();
    view_flush();
    if (loadpath && !strcasecmp(loadpath, "BLITTEST")) {
        blit_test();
        quit = TRUE;
    } else if (loadpath && !strcasecmp(loadpath, "PERF")) {
        extern void perf_run(void);
        perf_run();
        quit = TRUE;
    } else if (loadpath) {
        LONG i;
        for (i = 1; i <= nfiles; i++)
            open_file(argv[i]);
    } else if (argc == 0 && argv) {
        /* Workbench: mitgegebene Bild-Icons (Shift-Klick, Standardprogramm) */
        struct WBStartup *wbs = (struct WBStartup *)argv;
        static char wbpath[512];
        LONG i;
        for (i = 1; i < wbs->sm_NumArgs; i++) {
            struct WBArg *a = &wbs->sm_ArgList[i];
            if (!a->wa_Lock || !a->wa_Name || !a->wa_Name[0] ||
                !NameFromLock(a->wa_Lock, wbpath, sizeof(wbpath)) || !AddPart(wbpath, a->wa_Name, sizeof(wbpath)))
                continue;
            open_file(wbpath);
        }
    }

    if (upd_auto && !quit)
        upd_check(FALSE);

    while (!quit) {
        ULONG winsig = 1UL << win->UserPort->mp_SigBit;
        ULONG sigs = Wait(winsig | ai_sigmask() | upd_sigmask() | SIGBREAKF_CTRL_C);
        struct IntuiMessage *im;

        if (sigs & SIGBREAKF_CTRL_C)
            quit = TRUE;
        if (sigs & ai_sigmask())
            ai_poll();
        if (sigs & upd_sigmask())
            upd_poll();
        while ((im = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG class = im->Class, secs = im->Seconds, mics = im->Micros;
            UWORD code = im->Code, qual = im->Qualifier;
            mouse_x = im->MouseX;
            mouse_y = im->MouseY;
            ReplyMsg((struct Message *)im);

            switch (class) {
            case IDCMP_MOUSEBUTTONS:
                if (code == MENUDOWN)
                    ui_right_click(mouse_x, mouse_y);
                else if (code == SELECTDOWN)
                    mouse_down(qual, secs, mics);
                else if (code == SELECTUP)
                    mouse_up();
                break;
            case IDCMP_MOUSEMOVE:
                tf_shift = (qual & QUAL_SHIFT) != 0;
                mouse_move();
                break;
            case IDCMP_RAWKEY:
                if (handle_key(code, qual))
                    quit = TRUE;
                break;
            case IDCMP_INTUITICKS:
                if (lq_dragging)
                    liquify_tick();
                apply_move();
                view_flush();
                view_ants_tick();
                ui_tick();
                ai_tick();
                upd_tick();
                break;
            }
        }
        apply_move();
        mag_update();
        view_flush();
    }

    if (painting)
        stroke_end();
    cleanup();
    upd_relaunch();
    return RETURN_OK;
}

/* Icon/Shell geben oft nur 4-8 KB Stapel: selbst einen grossen holen.
   Variablen statisch, weil nach StackSwap SP-relative Locals ungueltig sind. */
static struct StackSwapStruct sss;
static int m_argc, m_ret;
static char **m_argv;
static APTR m_stk;

/* ohne Argumente: alle Stapeloperationen bleiben auf dem neuen Stapel */
static void __attribute__((noinline)) call_app(void)
{
    m_ret = app_main(m_argc, m_argv);
}

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    if ((ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower >= APS_STACK)
        return app_main(argc, argv);
    m_stk = AllocVec(APS_STACK, MEMF_ANY);
    if (!m_stk)
        return app_main(argc, argv);
    m_argc = argc;
    m_argv = argv;
    sss.stk_Lower = m_stk;
    sss.stk_Upper = (ULONG)m_stk + APS_STACK;
    sss.stk_Pointer = (APTR)sss.stk_Upper;
    StackSwap(&sss);
    call_app();
    StackSwap(&sss);
    FreeVec(m_stk);
    return m_ret;
}
