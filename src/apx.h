/*
 * AmiPixelShop - gemeinsame Definitionen
 */
#ifndef APX_H
#define APX_H

#include <exec/types.h>
#include <math.h>
#include "fmath.h"

/* ---- Kacheln ---- */
#define TILE_SHIFT 6
#define TILE_SIZE  64
#define TILE_MASK  63
#define TILE_PIX   (TILE_SIZE * TILE_SIZE)

enum { TF_EMPTY = 0, TF_MIX = 1, TF_OPAQUE = 2 };

/* Ebenen-Kachel: ARGB, straight alpha */
typedef struct Tile {
    ULONG px[TILE_PIX];
    UBYTE flag;
} Tile;

#define MAX_LAYERS 64

/* Fuellmethoden (Reihenfolge wie im Photoshop-Menue) */
enum {
    BM_NORMAL,
    BM_DARKEN, BM_MULTIPLY, BM_COLORBURN, BM_LINEARBURN,
    BM_LIGHTEN, BM_SCREEN, BM_COLORDODGE, BM_LINEARDODGE,
    BM_OVERLAY, BM_SOFTLIGHT, BM_HARDLIGHT, BM_VIVIDLIGHT, BM_LINEARLIGHT, BM_PINLIGHT, BM_HARDMIX,
    BM_DIFFERENCE, BM_EXCLUSION, BM_SUBTRACT, BM_DIVIDE,
    BM_HUE, BM_SATURATION, BM_COLOR, BM_LUMINOSITY,
    BM_COUNT
};
extern const char *blend_names[BM_COUNT];

/* Textebene: Inhalt bleibt bearbeitbar, Pixel werden daraus gerendert */
typedef struct TextInfo {
    char str[256];              /* UTF-8 */
    char font[64];              /* Dateiname ohne Endung */
    LONG size;
    ULONG color;
    LONG x, y;                  /* Grundlinie der ersten Zeile */
    LONG bx0, by0, bx1, by1;    /* Umriss (Dokument), fuer Klicks */
    /* ab hier spaeter angehaengt (alte Dateien: 0) */
    LONG align;                 /* 0 links, 1 Mitte, 2 rechts (am Ankerpunkt) */
    LONG aa;                    /* 0 Smooth, 1 Sharp, 2 None */
    LONG tracking;              /* 1/1000 em */
    LONG leading;               /* Zeilenabstand px, 0 = Auto */
    LONG boxw, boxh;            /* Absatztext: Rahmen (x, y = oben links), 0 = Punkttext */
    LONG m[4];                  /* Drehen/Spiegeln um den Anker (x, y): a b c d in 1/65536,
                                 * Punkt = Anker + (a b; c d) * (p - Anker); alles 0 = keine */
} TextInfo;
#define TEXTINFO_V1 352         /* Groesse ohne die angehaengten Felder */
#define TEXTINFO_V2 368         /* mit align .. leading */
#define TEXTINFO_V3 376         /* mit boxw, boxh (bis 0.6.1; m steht im Chunk TMAT) */

/* Ueberhang: Pixel einer Ebene ausserhalb der Arbeitsflaeche (ARGB straight;
 * Pixel innerhalb der Flaeche werden nicht benutzt) */
typedef struct Ovf {
    LONG x, y, w, h;
    ULONG px[1];
} Ovf;

#define CURVE_MAXPTS 16
typedef struct { LONG n; LONG x[CURVE_MAXPTS], y[CURVE_MAXPTS]; } Curve;

/* Korrekturebene: welche Korrektur (CMD_ADJ_*) mit welchen Werten */
typedef struct AdjInfo {
    LONG cmd;
    LONG v[6];
    Curve curves[4];
    LONG lv[3][5];              /* Levels je Kanal R, G, B (wie v[0..4]); Gamma 0 = neutral */
} AdjInfo;
#define ADJINFO_V1 ((LONG)(sizeof(LONG) * 7 + sizeof(Curve) * 4))   /* Groesse bis 0.6 (ohne lv) */

/* Ebenenarten */
enum { LK_PIXEL, LK_GROUP, LK_ADJUST };
#define MAX_DEPTH 8

/* Ebenenstil (nicht-destruktiv) */
typedef struct LayerFx {
    LONG sh_on, sh_opacity, sh_angle, sh_dist, sh_size;    /* Schlagschatten */
    ULONG sh_color;
    LONG gl_on, gl_opacity, gl_size;                        /* Schein nach aussen */
    ULONG gl_color;
    LONG st_on, st_size, st_opacity;                        /* Kontur aussen */
    ULONG st_color;
} LayerFx;
enum { FX_SHADOW, FX_GLOW, FX_STROKE, FX_COUNT };

typedef struct APLayer {
    Tile **tiles;               /* tw*th Zeiger, NULL = leer */
    UBYTE opacity;
    UBYTE visible;
    UBYTE mode;                 /* BM_* */
    UBYTE mask_on;              /* Maske aktiv */
    Tile **mask;                /* Ebenenmaske: tw*th graue Kacheln (NULL-Kachel = weiss) oder NULL */
    TextInfo *text;             /* Textebene oder NULL */
    char name[32];
    LayerFx *fx;                /* Ebenenstil oder NULL */
    Tile **fxc[FX_COUNT];       /* Zwischenspeicher der Effekte (ARGB straight) */
    BOOL fx_valid;              /* FALSE: beim naechsten Zeichnen ganz neu rechnen */
    Ovf *ovf;                   /* Pixel ausserhalb der Arbeitsflaeche oder NULL */
    Ovf *movf;                  /* Maske ausserhalb (0xFF000000 | Grau), fehlt: unbekannt */
    UBYTE fill;                 /* Flaechen-Deckkraft (ohne Ebenenstil) */
    UBYTE lock;                 /* LOCK_* */
    UBYTE kind;                 /* LK_* */
    UBYTE depth;                /* Verschachtelung; Kinder liegen direkt unter dem Gruppenkopf */
    UBYTE pass;                 /* Gruppe: Hindurchwirken (Pass Through) */
    UBYTE collapsed;            /* Gruppe im Panel zugeklappt */
    AdjInfo *adj;               /* Korrekturebene */
    UBYTE clip;                 /* Schnittmaske: auf die Ebene darunter beschnitten */
    UBYTE picked;               /* im Ebenen-Panel mit ausgewaehlt (nur Bedienung) */
    UBYTE mask_unlink;          /* 0: Maske bewegt sich mit der Ebene (Kette) */
    struct GenInfo *gen;        /* generative Ebene (KI): Prompt und Varianten, sonst NULL */
} Layer;

/* Generative Ebene: Prompt und Varianten (nur der Prompt wird gespeichert) */
#define GEN_MAXVAR 6
enum { GEN_FILL, GEN_REMOVE, GEN_EXTEND };
typedef struct GenVar { LONG x, y, w, h; ULONG *px; } GenVar;   /* px: ARGB, w*h, malloc */
typedef struct GenInfo {
    char prompt[200];
    LONG mode;                  /* GEN_* */
    LONG nvar, cur;
    GenVar var[GEN_MAXVAR];
} GenInfo;
void ai_gen_free(GenInfo *g);
enum { LOCK_ALPHA = 1, LOCK_PIXELS = 2, LOCK_POS = 4, LOCK_ALL = 7 };

/* Arbeitspfad (Pen Tool) */
#define PATH_MAXPTS 1024
#define PATH_MAXSUB 64
typedef struct { float x, y, ix, iy, ox, oy; } PathPt;     /* Anker, Griff ein/aus */
typedef struct Path {
    LONG npts, nsub;
    BOOL drawing;               /* letzter Teilpfad nimmt noch Punkte an */
    LONG sub[PATH_MAXSUB];      /* erster Punkt je Teilpfad */
    UBYTE closed[PATH_MAXSUB];
    PathPt pt[PATH_MAXPTS];
} Path;

#define MAX_GUIDES 64

typedef struct Doc {
    LONG w, h, tw, th;
    Layer *layers[MAX_LAYERS];  /* [0] = unterste */
    LONG nlayers, active;
    Path *wpath;                /* Arbeitspfad (Pen Tool) oder NULL */
    ULONG *flat;                /* Komposit, ARGB premultiplied, w*h */
    UBYTE *flatflag;            /* TF_* je Kachel */
    UBYTE *dirty;               /* Kachel muss neu komponiert werden */
    BOOL anydirty;
    UBYTE *sel;                 /* Auswahlmaske w*h, 0..255 */
    UBYTE *lastsel;             /* letzte Auswahl fuer Reselect oder NULL */
    BOOL hassel;
    LONG selx0, sely0, selx1, sely1;    /* Begrenzung der Auswahl */
    BOOL edit_mask;             /* Malen/Filter wirken auf die Maske der aktiven Ebene */
    LONG layer_counter, group_counter;         /* fuer "Ebene N" */
    char path[256];             /* leer = noch nie gespeichert */
    BOOL modified;
    /* je Dokument: Undo-Verlauf und Ansicht */
    struct UndoRec *uhead, *ucur;
    LONG ucount;
    float vzoom;
    LONG vox, voy;
    BOOL vset;
    LONG untitled;              /* "Unbenannt-N" */
    char title[40];
    char name[40];              /* Name aus New Document (bis zum ersten Speichern) */
    LONG colors;                /* Zielfarbtiefe: 0 = RGB, sonst Farben; -6/-8 = HAM6/HAM8 */
    LONG ppi;                   /* Aufloesung (nur Angabe) */
    void *qpal;                 /* QPalette der letzten Farbreduktion (fuer IFF) oder NULL */
    void *fixpal;               /* vorgegebene Palette (Reduce Colors: Fixed) oder NULL */
    LONG qopt[6];               /* deren Einstellungen (QuantOpts) */
    struct Doc *size_prev;      /* Zustand vor Bildgroesse/Arbeitsflaeche/Freistellen (Undo) */
    struct Doc *size_next;      /* ... und danach (Redo) */
    char base_name[32];         /* erster Protokolleintrag, z. B. "Crop"; leer = Open/New */
    LONG hist_src;              /* Quelle des Protokollpinsels (Eintrag im History-Panel) */
    ULONG *bcache;              /* Komposit der Ebenen 0..bcache_upto-1 (waehrend Ziehen/Vorschau) */
    UBYTE *bstate;
    LONG bcache_upto;
    LONG nguides;               /* Hilfslinien: Lage in Dokumentpixeln */
    LONG guide[MAX_GUIDES];
    UBYTE gvert[MAX_GUIDES];    /* 1 = senkrecht (x), 0 = waagerecht (y) */
} Doc;

extern Doc *doc;                /* aktives Dokument */

#define MAX_DOCS 8
extern Doc *docs[MAX_DOCS];     /* offene Dokumente = Tabs */
extern LONG ndocs;

/* ---- Farben (Photoshop CS6 dunkel) ---- */
#define COL_PASTE    0xFF282828     /* Arbeitsflaeche um das Bild */
#define COL_MENU     0xFF2B2B2B
#define COL_BAR      0xFF393939
#define COL_PANEL    0xFF3A3A3A
#define COL_PANELHD  0xFF2E2E2E
#define COL_LINE     0xFF1B1B1B
#define COL_LIGHT    0xFF4C4C4C
#define COL_TEXT     0xFFD8D8D8
#define COL_DIM      0xFF999999
#define COL_TOOLSEL  0xFF1F1F1F
#define COL_HOVER    0xFF474747
#define COL_ACCENT   0xFF3E6AA8
#define COL_FIELD    0xFF262626

/* ---- Layout ---- */
#define MENU_H    22
#define OPT_H     34
#define TOOL_W    40
#define PANEL_W   260
#define TAB_H     24
#define STATUS_H  22

/* ---- Bildschirm / Backbuffer (gfx.c) ---- */
extern LONG SW, SH;
extern ULONG *bb;
extern struct Screen *scr;
extern struct Window *win;

void gfx_clip(LONG x0, LONG y0, LONG x1, LONG y1);
void gfx_noclip(void);
void fill(LONG x, LONG y, LONG w, LONG h, ULONG c);
void frame(LONG x, LONG y, LONG w, LONG h, ULONG c);
void pset(LONG x, LONG y, ULONG c);
void line(LONG x0, LONG y0, LONG x1, LONG y1, ULONG c);
void ant_line(LONG x0, LONG y0, LONG x1, LONG y1, LONG phase);
void circle(LONG cx, LONG cy, LONG r, ULONG c);
LONG text(LONG x, LONG y, const char *s, ULONG c);
LONG text_bold(LONG x, LONG y, const char *s, ULONG c);
LONG text_width(const char *s);
LONG text_small(LONG x, LONG y, const char *s, ULONG c);   /* 9 px, fuer Lineale */
LONG text_small_width(const char *s);
void icon(LONG x, LONG y, LONG id, ULONG c);
void blit(LONG x, LONG y, LONG w, LONG h);
ULONG now_us(void);

#define FONT_H   17
#define FONT_ASC 13

/* ---- Heisse Schleifen (pixel.c, spaeter Assembler) ---- */
extern ULONG inv_tab[256];
void pixel_init(void);
void span_checker(ULONG *d, LONG n, LONG cx, LONG cyp);
extern ULONG chk_tab_ff[256], chk_tab_cc[256];
void span_copy_map(ULONG *d, const ULONG *row, const LONG *xmap, LONG n);
void span_layer_first(ULONG *d, const ULONG *s, LONG n, ULONG op);
/* Ebene mit Fuellmethode ueber premultiplied Hintergrund legen */
void span_blend(ULONG *d, const ULONG *s, LONG n, ULONG op, LONG mode);

/* Je Kern eine C-Referenz (_c) und eine Assembler-Fassung (_asm, pixel_asm.s);
 * aufgerufen wird ueber den Zeiger, Standard ist Assembler. */
void span_show_mix_c(ULONG *d, const ULONG *s, LONG n, LONG cx, LONG cyp);
void span_show_mix_asm(ULONG *d, const ULONG *s, LONG n, LONG cx, LONG cyp);
extern void (*span_show_mix)(ULONG *d, const ULONG *s, LONG n, LONG cx, LONG cyp);
void span_show_mix_map_c(ULONG *d, const ULONG *row, const LONG *xmap, LONG n, LONG cx, LONG cyp);
void span_show_mix_map_asm(ULONG *d, const ULONG *row, const LONG *xmap, LONG n, LONG cx, LONG cyp);
extern void (*span_show_mix_map)(ULONG *d, const ULONG *row, const LONG *xmap, LONG n, LONG cx, LONG cyp);
void span_layer_over_c(ULONG *d, const ULONG *s, LONG n, ULONG op);
void span_layer_over_asm(ULONG *d, const ULONG *s, LONG n, ULONG op);
extern void (*span_layer_over)(ULONG *d, const ULONG *s, LONG n, ULONG op);
void blur_h_c(const ULONG *src, ULONG *dst, LONG w, LONG h, LONG r, ULONG *pad);
void blur_h_asm(const ULONG *src, ULONG *dst, LONG w, LONG h, LONG r, ULONG *pad);
extern void (*blur_h)(const ULONG *src, ULONG *dst, LONG w, LONG h, LONG r, ULONG *pad);
void blur_v_c(const ULONG *src, ULONG *dst, LONG w, LONG h, LONG r, ULONG *sums);
void blur_v_asm(const ULONG *src, ULONG *dst, LONG w, LONG h, LONG r, ULONG *sums);
extern void (*blur_v)(const ULONG *src, ULONG *dst, LONG w, LONG h, LONG r, ULONG *sums);
void span_dab(ULONG *px, const ULONG *orig, UBYTE *cov, const UBYTE *mask,
              const UBYTE *sel, LONG n, ULONG color, ULONG flow, ULONG opacity, LONG erase);

/* ---- Dokument (doc.c) ---- */
enum { DOC_WHITE, DOC_EMPTY };    /* weisser Hintergrund + Ebene 1 / nur leere Ebene */
Doc *doc_new(LONG w, LONG h, LONG mode);
void doc_free(Doc *d);
BOOL doc_open(Doc *d);           /* als neuen Tab aufnehmen und aktivieren */
void doc_activate(Doc *d);
void doc_close(Doc *d);
LONG doc_index(Doc *d);
const char *doc_title(void);
const char *doc_title_of(Doc *d);
Layer *layer_new(Doc *d, const char *name);
Ovf *ovf_new(LONG x, LONG y, LONG w, LONG h);
Ovf *ovf_clone(const Ovf *o);
ULONG ovf_get(const Ovf *o, LONG x, LONG y);
Ovf *ovf_build(LONG x0, LONG y0, LONG x1, LONG y1, LONG cw, LONG ch,
               ULONG (*get)(LONG x, LONG y));
void layer_free(Layer *l, Doc *d);
LONG doc_add_layer(void);
void doc_delete_layer(void);
void doc_duplicate_layer(void);
void doc_move_layer(LONG from, LONG to);
void doc_merge_down(void);
Tile *tile_new(void);
void tile_update_flag(Doc *d, Tile *t, LONG tx, LONG ty);
void doc_flatten(void);
void doc_mark_dirty(LONG tx, LONG ty);
void doc_mark_all_dirty(void);
void doc_mark_rect_dirty(LONG x0, LONG y0, LONG x1, LONG y1);
void doc_process_dirty(void);
ULONG doc_pick(LONG x, LONG y);
ULONG layer_get(Layer *l, LONG x, LONG y);

/* Undo: Kachel-Snapshots und/oder Ebenen-Aufbau (Reihenfolge, Eigenschaften) */
typedef struct LayerState {
    LONG n, active;
    Layer *l[MAX_LAYERS];
    UBYTE opacity[MAX_LAYERS], mode[MAX_LAYERS], mask_on[MAX_LAYERS], has_text[MAX_LAYERS];
    Tile **mask[MAX_LAYERS];
    char name[MAX_LAYERS][32];
    TextInfo text[MAX_LAYERS];
    UBYTE has_fx[MAX_LAYERS], fill[MAX_LAYERS], lock[MAX_LAYERS];
    UBYTE kind[MAX_LAYERS], depth[MAX_LAYERS], pass[MAX_LAYERS], collapsed[MAX_LAYERS];
    UBYTE has_adj[MAX_LAYERS], clip[MAX_LAYERS];
    AdjInfo adj[MAX_LAYERS];
    LayerFx fx[MAX_LAYERS];
} LayerState;

typedef struct UndoRec {
    struct UndoRec *prev, *next;
    Layer *layer;
    LONG n;
    LONG *idx;
    Tile **tiles;
    char name[32];
    LayerState *before, *after;     /* nur bei Ebenen-Aenderungen */
    BOOL has_ovf;                   /* Ueberhang von layer wird mitgetauscht */
    BOOL chain;                     /* gehoert zum vorherigen Eintrag (gemeinsam rueckgaengig) */
    Ovf *ovf;
    BOOL on_mask;                   /* Kacheln gehoeren zur Maske */
    BOOL is_path;                   /* Arbeitspfad-Eintrag: path wird getauscht */
    struct Path *path;
} UndoRec;
UndoRec *undo_begin(Layer *l, const char *name);
UndoRec *undo_begin_struct(const char *name);
UndoRec *undo_begin_path(const char *name);
UndoRec *undo_begin_target(const char *name);   /* aktive Ebene oder ihre Maske */
void undo_discard(UndoRec *u);

/* Ziel fuer Malen/Filter/Fuellen: Pixel der aktiven Ebene oder ihre Maske */
Tile **doc_target(void);
BOOL mask_dense(Layer *l);
void mask_compact(Doc *d, Layer *l);
void doc_masks_compact(void);
BOOL doc_target_is_mask(void);
ULONG mask_gray(ULONG rgb);     /* Farbe -> Grauwert fuer die Maske (0xRRGGBB) */
Tile *tile_new_gray(ULONG gray);
enum { MASK_REVEAL_ALL, MASK_HIDE_ALL, MASK_REVEAL_SEL };
void doc_mask_add(LONG how);
void doc_mask_delete(void);
void doc_mask_apply(void);
void doc_mask_toggle(void);
void doc_rasterize_text(Layer *l);  /* Textebene in Pixel-Ebene wandeln (mit Undo) */
LONG undo_history(const char **names, LONG max, LONG *cur);
void undo_goto(LONG pos);
void undo_add(UndoRec *u, LONG idx, Tile *orig);
void undo_commit(UndoRec *u);
BOOL undo_step(void);
BOOL redo_step(void);
void undo_clear(void);
Tile *tile_clone(const Tile *t);

/* Auswahl mit Vordergrundfarbe fuellen oder leeren */
void doc_fill_selection(ULONG color, BOOL clear);

/* ---- Ansicht (view.c) ---- */
extern float zoom;
extern LONG ox, oy;
extern LONG cvx, cvy, cvw, cvh;
extern LONG cax, caw;           /* Bereich zwischen Werkzeugleiste und Panels (mit Linealen) */
void view_layout(void);
void view_init(void);
void view_free(void);
void view_fit(void);
void view_save(void);           /* Zoom/Position im aktiven Dokument merken */
void view_restore(void);
void view_set_zoom(float z, LONG mx, LONG my);
void view_zoom_step(LONG dir, LONG mx, LONG my);
void view_pan(LONG dx, LONG dy);
void view_update_maps(void);
void view_inval(LONG x0, LONG y0, LONG x1, LONG y1);
void view_inval_doc(LONG x0, LONG y0, LONG x1, LONG y1);
void view_inval_all(void);
void view_flush(void);
void view_ants_rebuild(void);
void view_ants_tick(void);
float scr2docx(LONG sx);
float scr2docy(LONG sy);
LONG doc2scrx(float dx);
LONG doc2scry(float dy);
extern ULONG last_flush_us;
extern void (*view_overlay_cb)(void);  /* zeichnet z. B. einen Dialog ueber die Ansicht */
extern void (*view_transform_overlay)(void);    /* Rahmen beim Transformieren */
extern void (*view_text_overlay)(void);         /* Schreibmarke beim Texten */

/* Overlay-Zustand */
extern LONG cursor_x, cursor_y;   /* Bildschirm, -1 = aus */
extern LONG cursor_r;
void view_set_cursor(LONG x, LONG y, LONG r);

#define MAX_PATH 8192
extern float path_x[MAX_PATH], path_y[MAX_PATH];
extern LONG path_n;
extern BOOL path_rubber;          /* Gummiband zur Maus (Polygon-Lasso) */
extern BOOL path_closed_preview;  /* Rechteck/Ellipse: geschlossen zeichnen */
extern LONG rubber_x, rubber_y;   /* Endpunkt des Gummibands (Bildschirm) */
void view_path_changed(void);

/* ---- Malen (paint.c) ---- */
extern LONG brush_size, brush_hard, brush_opacity, brush_flow;
extern ULONG fg_color, bg_color;
void brush_rebuild(void);
void stroke_begin(float x, float y, BOOL erase);
void stroke_to(float x, float y);
void stroke_end(void);

/* ---- Auswahl (select.c) ---- */
enum { SEL_NEW, SEL_ADD, SEL_SUB, SEL_ISECT };
void sel_polygon(const float *xs, const float *ys, LONG n, LONG mode);
void sel_rect(float x0, float y0, float x1, float y1, LONG mode);
void sel_ellipse(float x0, float y0, float x1, float y1, LONG mode);
void sel_all(void);
void sel_none(void);
void sel_invert(void);
void sel_update_bounds(void);
void sel_wand(LONG x, LONG y, LONG tol, BOOL contiguous, BOOL antialias, LONG mode);
void sel_feather(LONG radius);
void sel_grow(LONG radius);
void sel_shrink(LONG radius);
void sel_border(LONG width);
void sel_smooth(LONG radius);

/* ---- Text (text.c) ---- */
extern LONG text_size, text_font;
BOOL text_active(void);
BOOL text_begin(LONG x, LONG y);
BOOL text_begin_edit(Layer *l);
BOOL text_begin_box(LONG x, LONG y, LONG w, LONG h);
void text_box_preview(LONG x0, LONG y0, LONG x1, LONG y1);
Layer *text_layer_at(LONG x, LONG y);
BOOL text_transform(Layer *l, const float *L, float sx, float sy, float dx, float dy, const char *undo_name);
void text_commit(void);
void text_cancel(void);
void text_key_char(UBYTE c);
void text_refresh(void);
extern LONG text_align, text_aa, text_track, text_lead;
void text_restyle(void);
void text_set_props(LONG size, LONG align, LONG track, LONG lead);
void text_insert(const char *s);
BOOL text_layer_active(void);
LONG text_font_count(void);
const char *text_font_name(LONG i);
void text_free(void);

/* ---- Transformieren (transform.c) ---- */
BOOL transform_active(void);
BOOL transform_begin(void);
void transform_apply(void);
void transform_set_mode(LONG m);
extern LONG lq_tool, lq_size, lq_pressure;
BOOL liquify_active(void);
BOOL liquify_begin(void);
void liquify_mouse_down(float x, float y);
void liquify_mouse_move(float x, float y);
void liquify_mouse_up(void);
void liquify_tick(void);
void liquify_apply(void);
void liquify_cancel(void);
LONG transform_mode(void);
void transform_cancel(void);
void transform_quick(float sx, float sy, float ang);
void transform_mouse_down(LONG mx, LONG my, BOOL shift);
BOOL transform_mouse_move(LONG mx, LONG my, BOOL shift);
void transform_mouse_up(void);
void transform_update(BOOL smooth);
void transform_get(float *sx, float *sy, float *ang);

/* ---- Verschieben (move.c) ---- */
void move_begin(void);
void move_update(LONG dx, LONG dy);
void move_end(void);
BOOL move_active(void);
void move_nudge(LONG dx, LONG dy);

/* ---- Oberflaeche (ui.c) ---- */
enum { T_MOVE, T_RECT, T_ELLIPSE, T_LASSO, T_POLY, T_MAGNETIC, T_QUICKSEL, T_WAND, T_CROP, T_EYEDROP,
       T_BRUSH, T_CLONE, T_HISTBRUSH, T_HEAL, T_ERASER, T_GRADIENT, T_BUCKET, T_BLUR, T_DODGE, T_PEN, T_TEXT, T_SHAPE, T_HAND, T_ZOOM, T_COUNT };

/* tools.c: Verlauf und Fuellwerkzeug */
enum { GRAD_LINEAR, GRAD_RADIAL };
enum { GRAD_FG_BG, GRAD_FG_TRANS };
extern LONG grad_type, grad_mode, grad_opacity;
extern BOOL grad_reverse;
extern LONG bucket_tol, bucket_opacity;
extern BOOL bucket_contig, bucket_aa;
void tool_gradient(float x0, float y0, float x1, float y1);
extern AdjInfo *adj_curves_target;

/* filter.c: Korrekturebenen */
void adjust_defaults(AdjInfo *a, LONG cmd);
void adjust_apply(const AdjInfo *a, ULONG *p, LONG n);
BOOL adjust_dialog(AdjInfo *a, void (*preview)(void));
const char *adjust_name(LONG cmd);

/* doc.c: Gruppen und Korrekturebenen */
void doc_cache_below(LONG upto);
void doc_cache_drop(void);
void filter_free(void);
LONG layer_block(LONG i);
LONG clip_base(LONG i);
LONG doc_picked(LONG *out);
void doc_pick_clear(void);
BOOL doc_picked_one(void);
LONG doc_layer_at(LONG x, LONG y, BOOL group);
extern LONG move_autosel, move_autogroup;
void doc_delete_picked(void);
void doc_group_picked(const char *name);
void doc_merge_picked(void);
void doc_clip_toggle(void);               /* Anzahl Ebenen im Block (Gruppe samt Kindern) */
BOOL layer_shown(LONG i);               /* sichtbar samt aller Gruppen darueber */
void doc_new_group(void);
void doc_group_layers(void);
void doc_ungroup(void);
void doc_merge_group(void);
void doc_move_block(LONG top, LONG dest, LONG depth);
void doc_step_layer(BOOL up);
BOOL doc_new_adjust(LONG cmd, void (*preview)(void));
BOOL doc_edit_adjust(void (*preview)(void));
enum { SHAPE_RECT, SHAPE_ELLIPSE, SHAPE_LINE };
extern LONG shape_type, shape_stroke_w, shape_weight;
extern ULONG shape_fill, shape_stroke;
extern BOOL shape_fill_on;
void tool_shape(float x0, float y0, float x1, float y1);

/* crop.c */
BOOL crop_active(void);
void crop_begin(void);
void crop_end(void);
void crop_get(LONG *w, LONG *h);
void crop_mouse_down(LONG sx, LONG sy);
void crop_mouse_move(LONG sx, LONG sy);
void crop_mouse_up(LONG sx, LONG sy);
BOOL crop_commit(void);

/* fx.c: Ebenenstile */
BOOL fx_active(const LayerFx *f);
void fx_free_cache(Layer *l, Doc *d);
void fx_process(void);
void fx_dialog(void);
void fx_clear(void);
void mask_blur(UBYTE *m, LONG stride, LONG x0, LONG y0, LONG x1, LONG y1, LONG r);
void tool_bucket(LONG x, LONG y);
void edit_fill(void);
void fill_selection_fg(const char *name);
BOOL path_exists(void);
void path_free_bufs(void);
void path_show(BOOL on);
void path_mouse_down(LONG sx, LONG sy);
void path_mouse_move(LONG sx, LONG sy);
void path_mouse_up(LONG sx, LONG sy);
void path_end(void);
void path_delete_last(void);
void path_delete(void);
BOOL path_to_selection(void);
void path_fill(void);
void path_stroke(void);
void path_draw_thumb(LONG x, LONG y, LONG w, LONG h, ULONG c);
extern void (*view_pen_overlay)(void);
BOOL doc_rotate(LONG deg, BOOL fh, BOOL fv, const char *name);
void sel_color_range(ULONG rgb, LONG fuzz, BOOL invert);
BOOL sel_grow_similar(BOOL similar, LONG tol);
void edit_stroke(void);
void sel_quick_begin(LONG mode);
void sel_quick_dab(float cx, float cy, LONG size);
UBYTE *wand_mask(Tile **arr, LONG sx, LONG sy, LONG tol, BOOL contiguous, BOOL antialias, LONG *bb);

/* paint.c: Retusche-Pinsel */
enum { FXT_NONE, FXT_BLUR, FXT_SHARPEN, FXT_SMUDGE, FXT_DODGE, FXT_BURN };
extern LONG retouch_strength, retouch_mode, toning_mode, toning_exposure;
void stroke_begin_fx(float x, float y, LONG fx);
void history_brush_begin(float x, float y);
void heal_begin(float x, float y);

/* paint.c: Kopierstempel */
extern BOOL clone_aligned;
extern BOOL heal_ai;
void clone_set_source(float x, float y);
BOOL clone_has_source(void);
void clone_begin(float x, float y);
extern LONG tool;
extern LONG sel_mode;
extern LONG wand_tol;
/* Magnetisches Lasso (maglasso.c) */
extern LONG mag_width, mag_contrast, mag_freq;
BOOL mag_active(void);
BOOL mag_begin(float x, float y);
void mag_move(float x, float y);
void mag_update(void);
void mag_click(float x, float y);
BOOL mag_back(void);
LONG mag_close(void);
void mag_end(void);
BOOL mag_near_start(LONG sx, LONG sy);
extern BOOL wand_contig, wand_aa;

/* Modaler Dialog mit Reglern; TRUE = OK. div > 1: Anzeige als Kommazahl (Wert / div).
 * preview (optional) wird nach jeder Aenderung aufgerufen. */
typedef struct { const char *label; LONG *val; LONG min, max; const char *unit; LONG div; } DlgSlider;
LONG adjust_sliders(LONG cmd, DlgSlider *out);
#define DLG_CHECK (-1)          /* min = max = DLG_CHECK: Checkbox (Abschnitt) */
#define DLG_COLOR (-2)          /* min = max = DLG_COLOR: Farbfeld, Klick = Farbwaehler */
#define DLG_CHOICE (-3)         /* min = max = DLG_CHOICE: Knopfgruppe, unit = "A|B|C" */
#define DLG_PALETTE (-4)        /* min = max = DLG_PALETTE: Farbtabelle (ui_dialog_palette) */
void ui_dialog_palette(const ULONG *col, const LONG *n);
BOOL dialog_run(const char *title, DlgSlider *sl, LONG n, void (*preview)(void));
BOOL ui_color_picker(const char *title, ULONG *color);
void ui_prefs_load(void);
ULONG ui_prefs_mode(void);
extern ULONG pref_mode;
void ui_prefs_save(void);
void ui_toolbar_dialog(void);
void ui_hover(LONG x, LONG y);
void ui_tick(void);
void ui_hover_hide(void);
BOOL ui_right_click(LONG x, LONG y);
typedef struct {
    char name[40];
    LONG w, h, ppi, colors, bg;     /* bg: 0 White, 1 Black, 2 Background Color, 3 Transparent */
} NewDocSpec;
BOOL ui_new_document(NewDocSpec *s);
LONG doc_next_untitled(void);
void color_depth_name(LONG colors, char *buf);
BOOL size_dialog(const char *title, LONG *w, LONG *h, LONG ow, LONG oh, BOOL *lock, LONG *anchor);
BOOL doc_image_size(LONG nw, LONG nh);
BOOL doc_canvas_size(LONG nw, LONG nh, LONG anchor);
BOOL doc_canvas_rect(LONG x0, LONG y0, LONG x1, LONG y1);
BOOL doc_trim(void);
BOOL doc_reveal_all(void);
Doc *doc_duplicate(void);
void doc_size_switch(Doc *to);
void doc_merge_visible(void);
void doc_layer_via(BOOL cut);
void ui_draw_all(void);
void ui_draw_options(void);
void ui_draw_toolbar(void);
void ui_draw_panels(void);
void ui_draw_tab(void);
void ui_draw_status(void);
BOOL ui_mouse_down(LONG x, LONG y);
void ui_mouse_move(LONG x, LONG y);
void ui_mouse_up(void);
BOOL ui_dragging(void);
void ui_set_tool(LONG t);
const char *ui_tool_name(LONG t);
extern UWORD ui_qualifier;
BOOL ui_menu_open(void);
void ui_close_menu(void);
LONG ui_popup(LONG x, LONG y, const char **items, LONG n, LONG cur);
BOOL ui_edit_text(LONG x, LONG y, LONG w, char *buf, LONG max);
void ui_rename_layer(void);

/* ---- Dateien (fileio.c) ---- */
Doc *file_load(const char *path, char *err);
Doc *load_psd(const char *path, char *err);
BOOL file_export_iff(Doc *d, const char *path, char *err);
BOOL file_load_palette(const char *path, void *qpal, char *err);
BOOL file_save_palette(const char *path, const void *qpal, char *err);
void filter_reduce_fixed(void);
BOOL file_save(Doc *d, const char *path, char *err);
ULONG *dt_read_argb(APTR name, ULONG srctype, LONG *w, LONG *h, char *err);
Doc *doc_from_argb_pub(LONG w, LONG h, const ULONG *src);

/* ---- Zwischenablage (clip.c) ---- */
BOOL clip_copy(BOOL merged, BOOL cut);
BOOL clip_paste(void);
Doc *clip_new_doc(void);
void clip_free(void);

/* ---- Befehle (Menue und Tastatur) ---- */
enum {
    CMD_NONE, CMD_NEW, CMD_OPEN, CMD_SAVE, CMD_SAVEAS, CMD_QUIT,
    CMD_UNDO, CMD_REDO, CMD_FILL_FG, CMD_FILL_BG, CMD_ERASE,
    CMD_SEL_ALL, CMD_SEL_NONE, CMD_SEL_INVERT, CMD_FEATHER, CMD_GROW, CMD_SHRINK, CMD_BORDER, CMD_SMOOTH,
    CMD_LAYER_NEW, CMD_LAYER_DEL, CMD_FLATTEN,
    CMD_LAYER_DUP, CMD_LAYER_UP, CMD_LAYER_DOWN, CMD_MERGE_DOWN, CMD_LAYER_RENAME,
    CMD_CLOSE, CMD_NEXT_DOC, CMD_CUT, CMD_COPY, CMD_COPY_MERGED, CMD_PASTE, CMD_NEW_FROM_CLIP,
    CMD_FREE_TF, CMD_TF_APPLY, CMD_TF_CANCEL, CMD_TEXT_COMMIT, CMD_TEXT_CANCEL,
    CMD_MASK_REVEAL_ALL, CMD_MASK_HIDE_ALL, CMD_MASK_REVEAL_SEL, CMD_MASK_DELETE, CMD_MASK_APPLY,
    CMD_MASK_TOGGLE, CMD_RASTERIZE, CMD_TEXT_EDIT, CMD_IMAGE_SIZE, CMD_CANVAS_SIZE, CMD_LAYER_STYLE, CMD_LAYER_STYLE_CLEAR,
    CMD_CLOSE_ALL, CMD_REVERT, CMD_TOGGLE_LAST, CMD_CROP, CMD_TRIM, CMD_REVEAL_ALL, CMD_DUP_DOC,
    CMD_LAYER_VIA_COPY, CMD_LAYER_VIA_CUT, CMD_LAYER_TOP, CMD_LAYER_BOTTOM, CMD_MERGE_VISIBLE,
    CMD_RESELECT, CMD_CROP_COMMIT, CMD_CROP_CANCEL, CMD_TOOLBAR, CMD_SCREENMODE, CMD_CLIP_TOGGLE, CMD_EDIT_FILL, CMD_EDIT_STROKE,
    CMD_IROT180, CMD_IROT90CW, CMD_IROT90CCW, CMD_IROT_ARB, CMD_IFLIP_H, CMD_IFLIP_V,
    CMD_COLOR_RANGE, CMD_SEL_GROW_SIM, CMD_SEL_SIMILAR,
    CMD_TYPE_CHAR, CMD_AA_SMOOTH, CMD_AA_SHARP, CMD_AA_NONE, CMD_ALIGN_LEFT, CMD_ALIGN_CENTER,
    CMD_ALIGN_RIGHT, CMD_LOREM, CMD_ALL_LAYERS, CMD_DESEL_LAYERS,
    CMD_PATH_SEL, CMD_PATH_FILL, CMD_PATH_STROKE, CMD_PATH_DEL,
    CMD_TF_FREE, CMD_TF_DISTORT, CMD_TF_PERSP, CMD_TF_WARP,
    CMD_FLT_LIQUIFY, CMD_LQ_APPLY, CMD_LQ_CANCEL, CMD_EXPORT_IFF,
    CMD_PAL_LOAD, CMD_PAL_SAVE, CMD_PAL_FIX, CMD_GEN_FILL, CMD_GEN_REMOVE, CMD_SEL_SUBJECT, CMD_REMOVE_BG, CMD_TASKBAR,
    CMD_AI_SETTINGS, CMD_AI_KEY, CMD_GEN_AGAIN, CMD_GEN_PREV, CMD_GEN_NEXT, CMD_UPD_CHECK, CMD_UPD_AUTO,
    CMD_RULERS, CMD_GUIDES_SHOW, CMD_GUIDES_LOCK, CMD_GUIDES_SNAP, CMD_GUIDES_CLEAR, CMD_GUIDE_NEW,
    CMD_GROUP_NEW, CMD_GROUP, CMD_UNGROUP, CMD_MERGE_GROUP, CMD_ADJ_EDIT,
    CMD_NADJ_BRICON, CMD_NADJ_LEVELS, CMD_NADJ_CURVES, CMD_NADJ_HUESAT, CMD_NADJ_DESAT,
    CMD_NADJ_INVERT, CMD_NADJ_THRESH, CMD_NADJ_POSTER, CMD_FLIP_H, CMD_FLIP_V, CMD_ROT180, CMD_ROT90CW, CMD_ROT90CCW,
    CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_FIT, CMD_ZOOM_100, CMD_ABOUT,
    CMD_ADJ_BRICON, CMD_ADJ_LEVELS, CMD_ADJ_HUESAT, CMD_ADJ_DESAT, CMD_ADJ_INVERT,
    CMD_ADJ_THRESH, CMD_ADJ_POSTER, CMD_ADJ_CURVES,
    CMD_FLT_GAUSS, CMD_FLT_UNSHARP, CMD_FLT_NOISE, CMD_FLT_EMBOSS, CMD_FLT_MOSAIC,
    CMD_FLT_MOTION, CMD_FLT_BOX, CMD_FLT_MEDIAN, CMD_FLT_SHARPEN, CMD_FLT_SHARPEN_MORE,
    CMD_FLT_HIGHPASS, CMD_FLT_EDGES, CMD_FLT_QUANT, CMD_FLT_LAST
};

/* ---- Lineale und Hilfslinien (guides.c) ---- */
#define RULER_SZ 16
extern BOOL rulers_on, guides_show, guides_lock, guides_snap;
void rulers_set(BOOL on);
void rulers_draw(void);
void rulers_mark(LONG x, LONG y);            /* Mausposition in den Linealen */
BOOL guides_ruler_down(LONG x, LONG y);      /* Ziehen aus dem Lineal: neue Linie */
BOOL guides_grab(LONG x, LONG y);            /* vorhandene Linie greifen */
BOOL guides_dragging(void);
void guides_drag_move(LONG x, LONG y);
void guides_drag_end(LONG x, LONG y);
LONG guides_at(LONG x, LONG y);              /* Linie unter dem Zeiger oder -1 */
void guides_overlay(void);
void guides_changed(void);
void guides_add(LONG pos, BOOL vert);
void guides_clear(void);
void guides_snap_pt(float *x, float *y);     /* Dokumentkoordinaten an Linien/Rand */
void guides_snap_scr(LONG *x, LONG *y);      /* dasselbe in Bildschirmkoordinaten */
void guides_move_begin(void);                /* Umriss des bewegten Inhalts merken */
void guides_snap_move(LONG *dx, LONG *dy);   /* Versatz beim Verschieben einrasten */

/* ---- Korrekturen und Filter (filter.c) ---- */
void filter_command(LONG cmd);

/* Gradationskurven: 0 = RGB, 1..3 = Rot, Gruen, Blau */
extern Curve curves[4];
void curve_table(const Curve *c, UBYTE *out);
void curves_reset(void);
BOOL curves_dialog(void (*preview)(void));
/* Histogramm (0 = RGB, 1..3 = R, G, B) und Levels-Dialog; v = Input Black, Gamma*100,
 * Input White, Output Black, Output White */
extern ULONG hist[4][256];
BOOL levels_dialog(LONG *v, LONG (*ch)[5], void (*preview)(void));
extern LONG levels_ch[3][5];    /* Kanalwerte fuer Image > Adjustments > Levels */
const char *filter_last_name(void);

/* ---- main.c ---- */
void main_tool_changed(void);
void main_activate_doc(Doc *d);
void do_command(LONG cmd);
void status_message(const char *msg);
LONG request(const char *body, const char *gadgets);

/* ---- KI (ai.c) ---- */
BOOL ai_start(const char *prompt);  /* NULL: Inhalt der Auswahl entfernen */
BOOL ai_start_mask(UBYTE *m);       /* Entfernen mit eigener Maske (w*h, wird veraendert) */
BOOL ai_segment(LONG kind);         /* 1: Select Subject, 2: Remove Background */
extern BOOL ai_taskbar;             /* schwebende KI-Leiste anzeigen */
extern LONG ai_fill_model, ai_fast_model, ai_variants;
void ai_settings(void);
void ai_key_dialog(void);
BOOL ai_regenerate(void);
void ai_gen_step(LONG dir);
void ai_bar_refresh(void);
void ai_open_prompt(void);
void ai_cancel(void);
BOOL ai_busy(void);
ULONG ai_sigmask(void);
void ai_poll(void);
void ai_tick(void);
void ai_overlay(void);
BOOL ai_bar_click(LONG mx, LONG my);
void ai_shutdown(void);

/* ---- Update ueber GitHub (update.c) ---- */
extern BOOL upd_auto;
extern char upd_skip[32];
void upd_check(BOOL manual);
ULONG upd_sigmask(void);
void upd_poll(void);
void upd_tick(void);
void upd_shutdown(void);
void upd_relaunch(void);
void main_request_quit(void);

#endif
