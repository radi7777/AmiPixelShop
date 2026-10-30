/*
 * ui.c - Oberflaeche im Photoshop-CS6-Stil (dunkel)
 */
#include <exec/types.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <intuition/intuition.h>
#include <dos/dos.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/keymap.h>
#include <math.h>
#include "apx.h"

LONG tool = T_BRUSH;
LONG sel_mode = SEL_NEW;
static LONG tool_sel_mode[T_COUNT];     /* gemerkter Auswahlmodus je Werkzeug (0 = SEL_NEW) */
LONG wand_tol = 32;
BOOL wand_contig = TRUE, wand_aa = TRUE;

extern void main_tool_changed(void);

static const char *tool_names[T_COUNT] = {
    "Move Tool (V)", "Rectangular Marquee (M)", "Elliptical Marquee (M)",
    "Lasso Tool (L)", "Polygonal Lasso (L)", "Magnetic Lasso (L)", "Quick Selection Tool (W)", "Magic Wand (W)", "Crop Tool (C)", "Eyedropper (I)",
    "Brush Tool (B)", "Clone Stamp Tool (S)", "History Brush Tool (Y)", "Spot Healing Brush Tool (J)", "Eraser Tool (E)", "Gradient Tool (G)",
    "Paint Bucket Tool (G)", "Blur Tool (R)", "Dodge Tool (O)", "Pen Tool (P)", "Horizontal Type Tool (T)", "Shape Tool (U)", "Hand Tool (H)", "Zoom Tool (Z)"
};

static const char *menu_items[] = {
    "File", "Edit", "Image", "Layer", "Type", "Select", "Filter",
    "View", "Window", "Help", NULL
};
#define NMENUS 10

/* label NULL + cmd CMD_NONE = Trennlinie, cmd -1 = Ende, cmd CMD_NONE = ausgegraut,
 * sub != NULL = Untermenue */
typedef struct APMenuItem { const char *label, *key; LONG cmd; const struct APMenuItem *sub; } MenuItem;
#define SEP { NULL, NULL, CMD_NONE, NULL }
#define END { NULL, NULL, -1, NULL }
#define SUB(l, s) { l, "", CMD_NONE, s }

static const MenuItem m_file[] = {
    { "New...", "Ctrl+N", CMD_NEW }, { "New from Clipboard", "", CMD_NEW_FROM_CLIP },
    { "Open...", "Ctrl+O", CMD_OPEN }, SEP,
    { "Close", "Ctrl+W", CMD_CLOSE }, { "Close All", "Alt+Ctrl+W", CMD_CLOSE_ALL }, SEP,
    { "Save", "Ctrl+S", CMD_SAVE }, { "Save As...", "Shift+Ctrl+S", CMD_SAVEAS },
    { "Export as IFF ILBM...", "", CMD_EXPORT_IFF },
    { "Revert", "F12", CMD_REVERT }, SEP,
    { "Quit", "Ctrl+Q", CMD_QUIT }, END };
static const MenuItem m_fill[] = {
    { "Foreground Color", "Alt+Backspace", CMD_FILL_FG },
    { "Background Color", "Ctrl+Backspace", CMD_FILL_BG }, END };
static const MenuItem m_transform[] = {
    { "Distort", "", CMD_TF_DISTORT }, { "Perspective", "", CMD_TF_PERSP }, { "Warp", "", CMD_TF_WARP }, SEP,
    { "Flip Horizontal", "", CMD_FLIP_H }, { "Flip Vertical", "", CMD_FLIP_V }, SEP,
    { "Rotate 180°", "", CMD_ROT180 }, { "Rotate 90° Clockwise", "", CMD_ROT90CW },
    { "Rotate 90° Counter Clockwise", "", CMD_ROT90CCW }, END };
static const MenuItem m_edit[] = {
    { "Undo", "Ctrl+Z", CMD_UNDO }, { "Redo", "Shift+Ctrl+Z", CMD_REDO },
    { "Toggle Last State", "Alt+Ctrl+Z", CMD_TOGGLE_LAST }, SEP,
    { "Cut", "Ctrl+X", CMD_CUT }, { "Copy", "Ctrl+C", CMD_COPY },
    { "Copy Merged", "Shift+Ctrl+C", CMD_COPY_MERGED },
    { "Paste", "Ctrl+V", CMD_PASTE }, { "Clear", "Del", CMD_ERASE }, SEP,
    { "Fill...", "Shift+F5", CMD_EDIT_FILL }, { "Stroke...", "", CMD_EDIT_STROKE }, SUB("Quick Fill", m_fill), SEP,
    { "Generative Fill...", "", CMD_GEN_FILL }, { "Generative Remove", "", CMD_GEN_REMOVE },
    { "AI Settings...", "", CMD_AI_SETTINGS }, { "AI API Key...", "", CMD_AI_KEY }, SEP,
    { "Free Transform", "Ctrl+T", CMD_FREE_TF }, SUB("Transform", m_transform), SEP,
    { "Toolbar...", "", CMD_TOOLBAR }, { "Screen Mode...", "", CMD_SCREENMODE }, END };
static const MenuItem m_adjust[] = {
    { "Brightness/Contrast...", "", CMD_ADJ_BRICON },
    { "Levels...", "Ctrl+L", CMD_ADJ_LEVELS },
    { "Curves...", "Ctrl+M", CMD_ADJ_CURVES }, SEP,
    { "Hue/Saturation...", "Ctrl+U", CMD_ADJ_HUESAT },
    { "Desaturate", "Shift+Ctrl+U", CMD_ADJ_DESAT }, SEP,
    { "Invert", "Ctrl+I", CMD_ADJ_INVERT },
    { "Threshold...", "", CMD_ADJ_THRESH }, { "Posterize...", "", CMD_ADJ_POSTER }, END };
static const MenuItem m_irot[] = {
    { "180°", "", CMD_IROT180 }, { "90° Clockwise", "", CMD_IROT90CW },
    { "90° Counter Clockwise", "", CMD_IROT90CCW }, { "Arbitrary...", "", CMD_IROT_ARB }, SEP,
    { "Flip Canvas Horizontal", "", CMD_IFLIP_H }, { "Flip Canvas Vertical", "", CMD_IFLIP_V }, END };
static const MenuItem m_image[] = {
    SUB("Adjustments", m_adjust), SEP,
    { "Image Size...", "Alt+Ctrl+I", CMD_IMAGE_SIZE }, { "Canvas Size...", "Alt+Ctrl+C", CMD_CANVAS_SIZE },
    SUB("Image Rotation", m_irot),
    { "Crop", "", CMD_CROP }, { "Trim", "", CMD_TRIM }, { "Reveal All", "", CMD_REVEAL_ALL }, SEP,
    { "Duplicate...", "", CMD_DUP_DOC }, END };
static const MenuItem m_lnew[] = {
    { "Layer", "Shift+Ctrl+N", CMD_LAYER_NEW }, { "Group", "", CMD_GROUP_NEW }, SEP,
    { "Layer via Copy", "Ctrl+J", CMD_LAYER_VIA_COPY }, { "Layer via Cut", "Shift+Ctrl+J", CMD_LAYER_VIA_CUT }, END };
static const MenuItem m_lstyle[] = {
    { "Layer Style...", "", CMD_LAYER_STYLE }, { "Clear Layer Style", "", CMD_LAYER_STYLE_CLEAR }, END };
static const MenuItem m_lmask[] = {
    { "Reveal All", "", CMD_MASK_REVEAL_ALL }, { "Hide All", "", CMD_MASK_HIDE_ALL },
    { "Reveal Selection", "", CMD_MASK_REVEAL_SEL }, SEP,
    { "Delete", "", CMD_MASK_DELETE }, { "Apply", "", CMD_MASK_APPLY },
    { "Disable/Enable", "Shift+click", CMD_MASK_TOGGLE }, END };
static const MenuItem m_nadj[] = {
    { "Brightness/Contrast", "", CMD_NADJ_BRICON }, { "Levels", "", CMD_NADJ_LEVELS },
    { "Curves...", "", CMD_NADJ_CURVES }, SEP,
    { "Hue/Saturation", "", CMD_NADJ_HUESAT }, { "Desaturate", "", CMD_NADJ_DESAT }, SEP,
    { "Invert", "", CMD_NADJ_INVERT }, { "Threshold", "", CMD_NADJ_THRESH },
    { "Posterize", "", CMD_NADJ_POSTER }, END };
static const MenuItem m_arrange[] = {
    { "Bring to Front", "Shift+Ctrl+]", CMD_LAYER_TOP }, { "Bring Forward", "Ctrl+]", CMD_LAYER_UP },
    { "Send Backward", "Ctrl+[", CMD_LAYER_DOWN }, { "Send to Back", "Shift+Ctrl+[", CMD_LAYER_BOTTOM }, END };
static const MenuItem m_layer[] = {
    SUB("New", m_lnew), { "Duplicate Layer", "", CMD_LAYER_DUP },
    { "Delete Layer", "", CMD_LAYER_DEL }, SEP,
    { "Rename Layer...", "", CMD_LAYER_RENAME }, SUB("Layer Style", m_lstyle), SEP,
    SUB("New Adjustment Layer", m_nadj), { "Layer Content Options...", "", CMD_ADJ_EDIT }, SEP,
    SUB("Layer Mask", m_lmask), { "Remove Background", "", CMD_REMOVE_BG },
    { "Rasterize Type", "", CMD_RASTERIZE }, SEP,
    { "Create/Release Clipping Mask", "Alt+Ctrl+G", CMD_CLIP_TOGGLE }, SEP,
    { "Group Layers", "Ctrl+G", CMD_GROUP }, { "Ungroup Layers", "Shift+Ctrl+G", CMD_UNGROUP }, SEP,
    SUB("Arrange", m_arrange), SEP,
    { "Merge Down", "Ctrl+E", CMD_MERGE_DOWN }, { "Merge Group", "", CMD_MERGE_GROUP },
    { "Merge Visible", "Shift+Ctrl+E", CMD_MERGE_VISIBLE },
    { "Flatten Image", "", CMD_FLATTEN }, END };
static const MenuItem m_modify[] = {
    { "Feather...", "Shift+F6", CMD_FEATHER },
    { "Expand...", "", CMD_GROW }, { "Contract...", "", CMD_SHRINK },
    { "Border...", "", CMD_BORDER }, { "Smooth...", "", CMD_SMOOTH }, END };
static const MenuItem m_select[] = {
    { "All", "Ctrl+A", CMD_SEL_ALL }, { "Deselect", "Ctrl+D", CMD_SEL_NONE },
    { "Reselect", "Shift+Ctrl+D", CMD_RESELECT },
    { "Inverse", "Shift+Ctrl+I", CMD_SEL_INVERT }, SEP,
    { "All Layers", "Alt+Ctrl+A", CMD_ALL_LAYERS }, { "Deselect Layers", "", CMD_DESEL_LAYERS }, SEP,
    { "Color Range...", "", CMD_COLOR_RANGE }, { "Subject", "", CMD_SEL_SUBJECT }, SEP,
    SUB("Modify", m_modify), { "Grow", "", CMD_SEL_GROW_SIM }, { "Similar", "", CMD_SEL_SIMILAR }, END };
static const MenuItem m_fblur[] = {
    { "Box Blur...", "", CMD_FLT_BOX }, { "Gaussian Blur...", "", CMD_FLT_GAUSS },
    { "Motion Blur...", "", CMD_FLT_MOTION }, END };
static const MenuItem m_fsharp[] = {
    { "Sharpen", "", CMD_FLT_SHARPEN }, { "Sharpen More", "", CMD_FLT_SHARPEN_MORE },
    { "Unsharp Mask...", "", CMD_FLT_UNSHARP }, END };
static const MenuItem m_fnoise[] = {
    { "Add Noise...", "", CMD_FLT_NOISE }, { "Median...", "", CMD_FLT_MEDIAN }, END };
static const MenuItem m_fpix[] = { { "Mosaic...", "", CMD_FLT_MOSAIC }, END };
static const MenuItem m_fstyl[] = {
    { "Emboss...", "", CMD_FLT_EMBOSS }, { "Find Edges", "", CMD_FLT_EDGES }, END };
static const MenuItem m_fother[] = { { "High Pass...", "", CMD_FLT_HIGHPASS }, END };
static const MenuItem m_famiga[] = { { "Reduce Colors...", "", CMD_FLT_QUANT }, SEP,
    { "Load Palette...", "", CMD_PAL_LOAD }, { "Save Palette...", "", CMD_PAL_SAVE },
    { "Use Current Palette as Fixed", "", CMD_PAL_FIX }, END };
static const MenuItem m_filter[] = {
    { "Last Filter", "Ctrl+F", CMD_FLT_LAST }, SEP,
    { "Liquify...", "Shift+Ctrl+X", CMD_FLT_LIQUIFY }, SEP,
    SUB("Blur", m_fblur), SUB("Noise", m_fnoise), SUB("Pixelate", m_fpix),
    SUB("Sharpen", m_fsharp), SUB("Stylize", m_fstyl), SUB("Other", m_fother), SEP,
    SUB("Amiga", m_famiga), END };
static const MenuItem m_view[] = {
    { "Zoom In", "Ctrl++", CMD_ZOOM_IN }, { "Zoom Out", "Ctrl+-", CMD_ZOOM_OUT }, SEP,
    { "Fit on Screen", "Ctrl+0", CMD_ZOOM_FIT }, { "100%", "Ctrl+1", CMD_ZOOM_100 }, SEP,
    { "Rulers", "Ctrl+R", CMD_RULERS }, SEP,
    { "Show Guides", "Ctrl+;", CMD_GUIDES_SHOW }, { "Snap", "Shift+Ctrl+;", CMD_GUIDES_SNAP },
    { "Lock Guides", "", CMD_GUIDES_LOCK }, { "Clear Guides", "", CMD_GUIDES_CLEAR },
    { "New Guide...", "", CMD_GUIDE_NEW }, END };
static const MenuItem m_window[] = {
    { "Next Document", "Ctrl+Tab", CMD_NEXT_DOC }, SEP,
    { "Contextual Task Bar", "", CMD_TASKBAR }, END };
static const MenuItem m_help[] = {
    { "Check for Updates...", "", CMD_UPD_CHECK }, { "Check for Updates at Start", "", CMD_UPD_AUTO }, SEP,
    { "About AmiPixelShop...", "", CMD_ABOUT }, END };
static const MenuItem m_taa[] = {
    { "Smooth", "", CMD_AA_SMOOTH }, { "Sharp", "", CMD_AA_SHARP }, { "None", "", CMD_AA_NONE }, END };
static const MenuItem m_talign[] = {
    { "Left", "", CMD_ALIGN_LEFT }, { "Center", "", CMD_ALIGN_CENTER }, { "Right", "", CMD_ALIGN_RIGHT }, END };
static const MenuItem m_type[] = {
    { "Character...", "", CMD_TYPE_CHAR }, SEP,
    SUB("Anti-Alias", m_taa), SUB("Align Text", m_talign), SEP,
    { "Paste Lorem Ipsum", "", CMD_LOREM }, SEP,
    { "Rasterize Type Layer", "", CMD_RASTERIZE }, END };
static const MenuItem *menus[NMENUS] = {
    m_file, m_edit, m_image, m_layer, m_type, m_select, m_filter, m_view, m_window, m_help };

static LONG menu_open = -1;
static LONG title_x[NMENUS + 1];
static char status_msg[96];

static const ULONG swatches[] = {
    0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF, 0xFF00FF, 0xFFFFFF,
    0xEBEBEB, 0xD6D6D6, 0xC2C2C2, 0xADADAD, 0x999999, 0x858585, 0x707070,
    0x5C5C5C, 0x474747, 0x333333, 0x1F1F1F, 0x000000, 0xF7977A, 0xFDC68A,
    0xFFF79A, 0xC4DF9B, 0x82CA9D, 0x6ECFF6, 0x8493CA, 0xA187BE, 0xF49AC2,
    0xED1C24, 0xF26522, 0xFFF200, 0x39B54A, 0x00A651, 0x00AEEF, 0x0054A6,
    0x2E3192, 0x662D91, 0xEC008C, 0x8C6239, 0x603913, 0x754C24, 0xC69C6D
};
#define NSWATCH (sizeof(swatches) / sizeof(swatches[0]))
#define SW_COLS 14
#define SW_CELL 17

/* ---- Regler ---- */
typedef struct Slider {
    const char *label;
    LONG *val;
    LONG min, max;
    const char *unit;
    BOOL quad;                  /* quadratische Skala (Pinselgroesse) */
    void (*changed)(void);
    LONG x, y, tx, tw;          /* Lage, Spur */
    LONG div;                   /* > 1: Anzeige als Kommazahl */
    LONG lw;                    /* > 0: feste Breite der Beschriftungsspalte */
} Slider;

static void brush_changed(void) { brush_rebuild(); }
static void nop_changed(void) { }

static LONG layer_op_pct = 100;
static void layer_op_changed(void)
{
    doc->layers[doc->active]->opacity = (UBYTE)((layer_op_pct * 255 + 50) / 100);
    doc_mark_all_dirty();
}

static Slider sl_size  = { "Size:", &brush_size, 1, 500, "px", TRUE, brush_changed };
static Slider sl_hard  = { "Hardness:", &brush_hard, 0, 100, "%", FALSE, brush_changed };
static Slider sl_opac  = { "Opacity:", &brush_opacity, 1, 100, "%", FALSE, nop_changed };
static Slider sl_flow  = { "Flow:", &brush_flow, 1, 100, "%", FALSE, nop_changed };
static Slider sl_layer = { "Opacity:", &layer_op_pct, 0, 100, "%", FALSE, layer_op_changed };
static LONG layer_fill_pct = 100;
static void layer_fill_changed(void)
{
    doc->layers[doc->active]->fill = (UBYTE)((layer_fill_pct * 255 + 50) / 100);
    doc_mark_all_dirty();
}
static Slider sl_lfill = { "Fill:", &layer_fill_pct, 0, 100, "%", FALSE, layer_fill_changed };

/* kleine Symbole 12x12 fuer die Sperr-Knoepfe */
static const char *lock_icons[4][12] = {
    { "##..##..##..", "##..##..##..", "..##..##..##", "..##..##..##", "##..##..##..", "##..##..##..",
      "..##..##..##", "..##..##..##", "##..##..##..", "##..##..##..", "..##..##..##", "..##..##..##" },
    { "..........##", ".........###", "........###.", ".......###..", "......###...", ".....###....",
      "....##......", "...###......", "..####......", ".#####......", ".####.......", "##.........." },
    { ".....##.....", "....####....", ".....##.....", ".....##.....", ".#...##...#.", "############",
      "############", ".#...##...#.", ".....##.....", ".....##.....", "....####....", ".....##....." },
    { "...######...", "..##....##..", "..#......#..", "..#......#..", ".##########.", ".##########.",
      ".####..####.", ".####..####.", ".#####.####.", ".##########.", ".##########.", "............" },
};

static void draw_icon12(LONG x, LONG y, const char **ic, ULONG c)
{
    LONG i, j;
    for (j = 0; j < 12; j++)
        for (i = 0; i < 12; i++)
            if (ic[j][i] == '#')
                pset(x + i, y + j, c);
}

static LONG ly_locky;
static Slider sl_tol   = { "Tolerance:", &wand_tol, 0, 255, "", FALSE, nop_changed };
static void text_size_changed(void) { text_refresh(); }
static Slider sl_gopac = { "Opacity:", &grad_opacity, 1, 100, "%", FALSE, nop_changed };
static Slider sl_btol  = { "Tolerance:", &bucket_tol, 0, 255, "", FALSE, nop_changed };
static Slider sl_mwidth = { "Width:", &mag_width, 1, 40, "px", FALSE, nop_changed };
static Slider sl_mcontr = { "Contrast:", &mag_contrast, 1, 100, "%", FALSE, nop_changed };
static Slider sl_mfreq  = { "Frequency:", &mag_freq, 0, 100, "", FALSE, nop_changed };
static Slider sl_bopac = { "Opacity:", &bucket_opacity, 1, 100, "%", FALSE, nop_changed };
static Slider sl_sstroke = { "Stroke:", &shape_stroke_w, 0, 100, "px", FALSE, nop_changed };
static Slider sl_sweight = { "Weight:", &shape_weight, 1, 100, "px", FALSE, nop_changed };
static Slider sl_rstr = { "Strength:", &retouch_strength, 1, 100, "%", FALSE, nop_changed };
static Slider sl_texp = { "Exposure:", &toning_exposure, 1, 100, "%", FALSE, nop_changed };
static Slider sl_tsize = { "Size:", &text_size, 4, 500, "px", TRUE, text_size_changed };

static Slider *opt_sliders[4];
static LONG n_opt_sliders;
static Slider *drag_slider;

static LONG slider_draw(Slider *s, LONG x, LONG y)
{
    char buf[48];
    LONG pos, cy = y + 8;
    s->x = x;
    s->y = y;
    x = text(x, y, s->label, COL_DIM) + 6;
    if (s->lw)
        x = s->x + s->lw + 6;
    s->tx = x;
    if (!s->tw)
        s->tw = 90;
    fill(x, cy - 1, s->tw, 3, COL_FIELD);
    {
        float t = (float)(*s->val - s->min) / (float)(s->max - s->min);
        if (s->quad)
            t = (float)__builtin_sqrtf(t);
        pos = (LONG)(t * s->tw);
    }
    fill(x, cy - 1, pos, 3, COL_ACCENT);
    fill(x + pos - 3, cy - 5, 7, 11, COL_TEXT);
    frame(x + pos - 3, cy - 5, 7, 11, COL_LINE);
    if (s->div > 1) {
        LONG v = *s->val, a = v < 0 ? -v : v;
        /* libnix-sprintf kennt kein "%0*ld" */
        sprintf(buf, s->div >= 100 ? "%s%ld.%02ld %s" : "%s%ld.%ld %s", v < 0 ? "-" : "",
                (long)(a / s->div), (long)(a % s->div), s->unit);
    } else
        /* "100%", "12°" ohne Leerzeichen, "30 px" mit */
        sprintf(buf, (s->unit[0] == '%' || (UBYTE)s->unit[0] == 0xC2) ? "%ld%s" : "%ld %s",
                (long)*s->val, s->unit);
    /* Wert als Eingabefeld (Klick: Zahl eintippen) */
    fill(x + s->tw + 6, y - 2, 56, FONT_H + 4, COL_FIELD);
    frame(x + s->tw + 6, y - 2, 56, FONT_H + 4, COL_LINE);
    text(x + s->tw + 10, y, buf, COL_TEXT);
    return x + s->tw + 66;
}

/* Zahl eintippen: Klick auf den Wert eines Reglers */
static BOOL value_hit(Slider *s, LONG x, LONG y)
{
    return x >= s->tx + s->tw + 6 && x < s->tx + s->tw + 72 && y >= s->y - 4 && y < s->y + FONT_H + 4;
}

static LONG parse_value(const char *b, LONG div, BOOL *ok)
{
    LONG sign = 1, ip = 0, fp = 0, fd = 1, n = 0;
    while (*b == ' ')
        b++;
    if (*b == '-') { sign = -1; b++; }
    while (*b >= '0' && *b <= '9') { ip = ip * 10 + (*b++ - '0'); n++; }
    if ((*b == '.' || *b == ',') && div > 1) {
        b++;
        while (*b >= '0' && *b <= '9' && fd < div) { fp = fp * 10 + (*b++ - '0'); fd *= 10; n++; }
    }
    *ok = n > 0;
    return sign * (ip * (div > 1 ? div : 1) + (div > 1 ? fp * div / fd : 0));
}

static BOOL slider_edit_value(Slider *s)
{
    char buf[24];
    LONG v = *s->val, d = s->div > 1 ? s->div : 1;
    BOOL ok;
    if (d > 1)
        sprintf(buf, d >= 100 ? "%ld.%02ld" : "%ld.%ld", (long)(v / d), (long)((v < 0 ? -v : v) % d));
    else
        sprintf(buf, "%ld", (long)v);
    if (!ui_edit_text(s->tx + s->tw + 6, s->y - 3, 66, buf, sizeof(buf)))
        return FALSE;
    v = parse_value(buf, d, &ok);
    if (!ok)
        return FALSE;
    if (v < s->min) v = s->min;
    if (v > s->max) v = s->max;
    if (v == *s->val)
        return FALSE;
    *s->val = v;
    s->changed();
    return TRUE;
}

static BOOL slider_hit(Slider *s, LONG x, LONG y)
{
    return x >= s->tx - 6 && x < s->tx + s->tw + 6 && y >= s->y - 6 && y < s->y + FONT_H + 6;
}

static void slider_set(Slider *s, LONG mx)
{
    float t = (float)(mx - s->tx) / (float)s->tw;
    LONG v;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    if (s->quad)
        t = t * t;
    v = s->min + (LONG)(t * (s->max - s->min) + 0.5f);
    if (v != *s->val) {
        *s->val = v;
        s->changed();
    }
}

/* ---- kleine Hilfen ---- */
static void button(LONG x, LONG y, LONG w, LONG h, const char *label, BOOL active)
{
    fill(x, y, w, h, active ? COL_TOOLSEL : COL_LIGHT);
    frame(x, y, w, h, COL_LINE);
    text(x + (w - text_width(label)) / 2, y + (h - FONT_H) / 2, label, active ? COL_TEXT : 0xFFC0C0C0);
}

static void fmt_zoom(char *buf, float z)
{
    LONG p10 = (LONG)(z * 1000.0f + 0.5f);
    if (p10 % 10 == 0)
        sprintf(buf, "%ld%%", (long)(p10 / 10));
    else
        sprintf(buf, "%ld.%ld%%", (long)(p10 / 10), (long)(p10 % 10));
}

/* ---- Menueleiste ---- */
static void draw_menu(void)
{
    LONG x = 34, i;
    fill(0, 0, SW, MENU_H, COL_MENU);
    fill(0, MENU_H - 1, SW, 1, COL_LINE);
    fill(6, 2, 22, 18, COL_ACCENT);
    text_bold(8, 2, "Ap", 0xFFFFFFFF);
    for (i = 0; menu_items[i]; i++) {
        LONG w = text_width(menu_items[i]);
        title_x[i] = x - 8;
        if (i == menu_open)
            fill(x - 8, 0, w + 16, MENU_H - 1, COL_ACCENT);
        x = text(x, 2, menu_items[i], COL_TEXT) + 16;
    }
    title_x[i] = x - 8;
    text(SW - text_width("AmiPixelShop") - 10, 2, "AmiPixelShop", COL_DIM);
}

/* ---- Optionsleiste ---- */
static void down_arrow(LONG x, LONG y, ULONG c);
static void color_panel_sync(void);
UWORD ui_qualifier;             /* Qualifier des letzten Mausklicks (von main gesetzt) */
static void choose_font(void);
static LONG tfm_bx[4], tfm_bw[4], lq_bx[5], lq_bw[5];
static Slider sl_lqsize = { "Size:", &lq_size, 10, 600, "px", TRUE, nop_changed };
static Slider sl_lqpress = { "Pressure:", &lq_pressure, 1, 100, "%", FALSE, nop_changed };
static LONG mode_bx[4], mode_bw[4], zoom_b1x, zoom_b2x, chk_x[2], chk_w[2], tf_bx, font_bx, tx_bx, al_bx, aa_bx;
static const char *aa_names[3] = { "Smooth", "Sharp", "None" };
static LONG pen_bx[4], pen_bw[4], as_bx;
LONG move_autosel, move_autogroup;
static LONG gb_x[4], gb_w[4], sw_fill_x, sw_stroke_x;

/* kleines Farbfeld in der Optionsleiste */
static LONG opt_swatch(LONG x, LONG y, const char *label, ULONG c, BOOL on, LONG *sx)
{
    x = text(x, y + (OPT_H - FONT_H) / 2, label, COL_DIM) + 6;
    *sx = x;
    fill(x, y + 8, 22, 18, on ? 0xFF000000 | c : COL_FIELD);
    frame(x, y + 8, 22, 18, 0xFFE0E0E0);
    if (!on)
        line(x + 1, y + 24, x + 20, y + 9, 0xFFE03030);     /* keine Fuellung */
    return x + 22;
}

/* Knopfgruppe; Lage in gb_x/gb_w ab Index first */
static LONG button_group(LONG x, LONG y, const char **labels, LONG n, LONG first, LONG active)
{
    LONG i;
    for (i = 0; i < n; i++) {
        LONG w = text_width(labels[i]) + 16;
        gb_x[first + i] = x;
        gb_w[first + i] = w;
        button(x, y + 6, w, OPT_H - 12, labels[i], i == active);
        x += w + 4;
    }
    return x;
}

static LONG checkbox(LONG x, LONG y, const char *label, BOOL on)
{
    LONG cy = y + (OPT_H - 13) / 2;
    fill(x, cy, 13, 13, COL_FIELD);
    frame(x, cy, 13, 13, 0xFF6A6A6A);
    if (on) {
        /* Haken */
        line(x + 3, cy + 6, x + 5, cy + 9, COL_TEXT);
        line(x + 5, cy + 9, x + 10, cy + 3, COL_TEXT);
        line(x + 3, cy + 7, x + 5, cy + 10, COL_TEXT);
        line(x + 5, cy + 10, x + 10, cy + 4, COL_TEXT);
    }
    return text(x + 19, y + (OPT_H - FONT_H) / 2, label, COL_TEXT);
}

static LONG draw_modes(LONG x, LONG y)
{
    static const char *modes[4] = { "New", "Add", "Subtract", "Intersect" };
    LONG i;
    for (i = 0; i < 4; i++) {
        LONG w = text_width(modes[i]) + 16;
        mode_bx[i] = x;
        mode_bw[i] = w;
        button(x, y + 6, w, OPT_H - 12, modes[i], sel_mode == i);
        x += w + 4;
    }
    return x;
}

/* ---- Pinselspitze: Knopf in der Optionsleiste und Popup ---- */
static LONG brush_bx = -1;

/* weicher Kreis (Haerte wie brush_rebuild) mit Deckung in cov (Breite cw) */
static void soft_dot(UBYTE *cov, LONG cw, LONG ch, float cx, float cy, float r, LONG hard)
{
    LONG x, y, x0 = (LONG)(cx - r - 1), x1 = (LONG)(cx + r + 2), y0 = (LONG)(cy - r - 1), y1 = (LONG)(cy + r + 2);
    float h = hard / 100.0f, hr = r * h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > cw) x1 = cw;
    if (y1 > ch) y1 = ch;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy), v;
            if (h >= 0.999f || r <= 1.0f)
                v = r + 0.5f - d;
            else if (d <= hr)
                v = 1.0f;
            else if (d >= r)
                v = 0.0f;
            else {
                float t = (d - hr) / (r - hr);
                v = 1.0f - t * t * (3.0f - 2.0f * t);
            }
            if (v > 0.0f) {
                LONG k = (LONG)(v * 255.0f);
                if (k > 255) k = 255;
                if (k > cov[y * cw + x])
                    cov[y * cw + x] = (UBYTE)k;
            }
        }
}

/* Deckung als helle Farbe auf den Hintergrund des Backbuffers legen */
static void draw_cov(const UBYTE *cov, LONG cw, LONG ch, LONG sx, LONG sy)
{
    LONG x, y;
    for (y = 0; y < ch; y++) {
        ULONG *p = bb + (sy + y) * SW + sx;
        for (x = 0; x < cw; x++) {
            ULONG a = cov[y * cw + x], d = p[x];
            if (!a)
                continue;
            p[x] = 0xFF000000 | ((((d >> 16) & 0xFF) * (255 - a) / 255 + 0xEE * a / 255) << 16) |
                   ((((d >> 8) & 0xFF) * (255 - a) / 255 + 0xEE * a / 255) << 8) |
                   ((d & 0xFF) * (255 - a) / 255 + 0xEE * a / 255);
        }
    }
}

/* Spitze (Durchmesser auf max px begrenzt) bei sx/sy zeichnen */
static void draw_tip(LONG sx, LONG sy, LONG size, LONG hard, LONG max)
{
    static UBYTE cov[40 * 40];
    float r = (size > max ? max : size) * 0.5f;
    memset(cov, 0, sizeof(cov));
    soft_dot(cov, 40, 40, 20.0f, 20.0f, r, hard);
    draw_cov(cov, 40, 40, sx - 20, sy - 20);
}

static LONG brush_button(LONG x, LONG y)
{
    char buf[8];
    brush_bx = x;
    fill(x, y + 4, 70, OPT_H - 8, COL_LIGHT);
    frame(x, y + 4, 70, OPT_H - 8, COL_LINE);
    gfx_clip(x + 1, y + 5, x + 69, y + OPT_H - 5);
    draw_tip(x + 14, y + OPT_H / 2, brush_size, brush_hard, 18);
    gfx_noclip();
    sprintf(buf, "%ld", (long)brush_size);
    text(x + 28, y + (OPT_H - FONT_H) / 2, buf, COL_TEXT);
    down_arrow(x + 59, y + OPT_H / 2 - 2, COL_DIM);
    return x + 70;
}

typedef struct { const char *name; LONG size, hard; } BrushPreset;
static const BrushPreset presets[] = {
    { "Hard Round", 30, 100 }, { "Soft Round", 45, 0 }, { "Hard Round Fine", 5, 100 },
    { "Soft Round Large", 300, 0 }, { "Hard Round Large", 100, 100 }, { "Soft Round Small", 13, 0 },
    { "Soft Edge", 60, 50 },
};
#define NPRESET (sizeof(presets) / sizeof(presets[0]))
#define BP_ROW 54

static LONG bp_x0, bp_y0, bp_w, bp_h, bp_ly;

static void bp_draw(void)
{
    LONG i;
    gfx_noclip();
    fill(bp_x0, bp_y0, bp_w, bp_h, COL_BAR);
    frame(bp_x0, bp_y0, bp_w, bp_h, COL_LINE);
    sl_size.tw = sl_hard.tw = 200;
    sl_size.lw = sl_hard.lw = 70;
    slider_draw(&sl_size, bp_x0 + 14, bp_y0 + 14);
    slider_draw(&sl_hard, bp_x0 + 14, bp_y0 + 44);
    fill(bp_x0 + 10, bp_ly - 10, bp_w - 20, 1, 0xFF2C2C2C);
    for (i = 0; i < (LONG)NPRESET; i++) {
        static UBYTE cov[230 * 44];
        const BrushPreset *p = &presets[i];
        LONG ry = bp_ly + i * BP_ROW, k;
        BOOL cur = p->size == brush_size && p->hard == brush_hard;
        char buf[8];
        float r = (p->size > 24 ? 24 : p->size) * 0.5f;
        fill(bp_x0 + 8, ry, bp_w - 16, BP_ROW - 4, cur ? 0xFF3A5A8A : 0xFF4A4A4A);
        draw_tip(bp_x0 + 34, ry + 18, p->size, p->hard, 24);
        sprintf(buf, "%ld", (long)p->size);
        text(bp_x0 + 34 - text_width(buf) / 2, ry + 32, buf, COL_DIM);
        text(bp_x0 + 64, ry + 30, p->name, COL_TEXT);
        /* Strich-Vorschau: S-Kurve */
        memset(cov, 0, sizeof(cov));
        for (k = 0; k <= 200; k += 2) {
            float t = k / 200.0f;
            soft_dot(cov, 230, 44, 15.0f + k, 22.0f + sinf(t * 6.2831853f) * 7.0f, r, p->hard);
        }
        draw_cov(cov, 230, 44, bp_x0 + 150, ry + 3);
    }
    blit(bp_x0, bp_y0, bp_w, bp_h);
}

static void brush_popup(void)
{
    BOOL done = FALSE;
    Slider *drag = NULL;
    LONG saved_tw = sl_size.tw, saved_lw = sl_size.lw, htw = sl_hard.tw, hlw = sl_hard.lw;
    bp_w = 400;
    bp_ly = 0;
    bp_x0 = brush_bx;
    bp_y0 = MENU_H + OPT_H - 2;
    bp_ly = bp_y0 + 86;
    bp_h = 86 + NPRESET * BP_ROW + 6;
    bp_draw();
    while (!done) {
        struct IntuiMessage *im;
        if (Wait((1UL << win->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
            done = TRUE;
        }
        while (!done && (im = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG class = im->Class;
            UWORD code = im->Code;
            LONG mx = im->MouseX, my = im->MouseY;
            ReplyMsg((struct Message *)im);
            if (class == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                if (mx < bp_x0 || mx >= bp_x0 + bp_w || my < bp_y0 || my >= bp_y0 + bp_h) {
                    done = TRUE;        /* Klick daneben schliesst */
                } else if (value_hit(&sl_size, mx, my)) {
                    slider_edit_value(&sl_size);
                } else if (value_hit(&sl_hard, mx, my)) {
                    slider_edit_value(&sl_hard);
                } else if (slider_hit(&sl_size, mx, my)) {
                    drag = &sl_size;
                    slider_set(drag, mx);
                } else if (slider_hit(&sl_hard, mx, my)) {
                    drag = &sl_hard;
                    slider_set(drag, mx);
                } else if (my >= bp_ly) {
                    LONG i = (my - bp_ly) / BP_ROW;
                    if (i >= 0 && i < (LONG)NPRESET) {
                        brush_size = presets[i].size;
                        brush_hard = presets[i].hard;
                        brush_rebuild();
                    }
                }
                bp_draw();
            } else if (class == IDCMP_MOUSEBUTTONS && code == SELECTUP) {
                if (drag)
                    slider_set(drag, mx);
                drag = NULL;
                bp_draw();
            } else if (class == IDCMP_MOUSEMOVE && drag) {
                LONG old = *drag->val;
                slider_set(drag, mx);
                if (old != *drag->val)
                    bp_draw();
            } else if (class == IDCMP_RAWKEY && (code == 0x44 || code == 0x43 || code == 0x45))
                done = TRUE;
        }
    }
    sl_size.tw = saved_tw; sl_size.lw = saved_lw;
    sl_hard.tw = htw; sl_hard.lw = hlw;
    ui_draw_all();
    view_inval_all();
    view_flush();
}

void ui_draw_options(void)
{
    LONG y = MENU_H, x = 10, ty = y + (OPT_H - FONT_H) / 2, i;

    fill(0, y, SW, OPT_H, COL_BAR);
    fill(0, y + OPT_H - 1, SW, 1, COL_LINE);
    icon(x, y + (OPT_H - 16) / 2, tool, COL_TEXT);
    x += 26;
    fill(x, y + 6, 1, OPT_H - 12, COL_LINE);
    x += 12;
    n_opt_sliders = 0;

    if (liquify_active()) {
        static const char *lt[5] = { "Forward Warp", "Reconstruct", "Pucker", "Bloat", "Twirl" };
        LONG k;
        for (k = 0; k < 5; k++) {
            LONG w = text_width(lt[k]) + 16;
            lq_bx[k] = x;
            lq_bw[k] = w;
            button(x, y + 6, w, OPT_H - 12, lt[k], lq_tool == k);
            x += w + 4;
        }
        x += 12;
        opt_sliders[n_opt_sliders++] = &sl_lqsize;
        opt_sliders[n_opt_sliders++] = &sl_lqpress;
        x = slider_draw(&sl_lqsize, x, ty) + 10;
        x = slider_draw(&sl_lqpress, x, ty) + 10;
        tf_bx = SW - PANEL_W - 210;
        button(tf_bx, y + 6, 96, OPT_H - 12, "Cancel", FALSE);
        fill(tf_bx + 104, y + 6, 96, OPT_H - 12, COL_ACCENT);
        frame(tf_bx + 104, y + 6, 96, OPT_H - 12, COL_LINE);
        text(tf_bx + 104 + (96 - text_width("Apply")) / 2, y + (OPT_H - FONT_H) / 2, "Apply", 0xFFFFFFFF);
        blit(0, y, SW, OPT_H);
        return;
    }
    if (transform_active()) {
        float sx, sy, ang;
        char buf[96];
        LONG deg;
        {
            static const char *ml[4] = { "Free", "Distort", "Perspective", "Warp" };
            LONG k, m = transform_mode();
            for (k = 0; k < 4; k++) {
                LONG w = text_width(ml[k]) + 18;
                tfm_bx[k] = x;
                tfm_bw[k] = w;
                button(x, y + 6, w, OPT_H - 12, ml[k], m == k);
                x += w + 4;
            }
            x += 14;
            if (m != 0) {
                text(x, ty, m == 3 ? "Drag the mesh points or the inside   Return: apply   Esc: cancel"
                              : m == 2 ? "Drag a corner: the opposite corner follows   Return: apply   Esc: cancel"
                                       : "Drag corners or edges freely (Shift: straight)   Return: apply   Esc: cancel",
                     COL_DIM);
                tf_bx = SW - PANEL_W - 210;
                button(tf_bx, y + 6, 96, OPT_H - 12, "Cancel", FALSE);
                fill(tf_bx + 104, y + 6, 96, OPT_H - 12, COL_ACCENT);
                frame(tf_bx + 104, y + 6, 96, OPT_H - 12, COL_LINE);
                text(tf_bx + 104 + (96 - text_width("Apply")) / 2, y + (OPT_H - FONT_H) / 2, "Apply", 0xFFFFFFFF);
                blit(0, y, SW, OPT_H);
                return;
            }
        }
        transform_get(&sx, &sy, &ang);
        deg = (LONG)floorf(ang * 180.0f / 3.14159265f + 0.5f);
        while (deg > 180) deg -= 360;
        while (deg < -180) deg += 360;
        sprintf(buf, "W: %ld%%     H: %ld%%     Angle: %ld°", (long)floorf(sx * 100.0f + 0.5f),
                (long)floorf(sy * 100.0f + 0.5f), (long)deg);
        text(x, ty, buf, COL_TEXT);
        tf_bx = SW - PANEL_W - 210;
        button(tf_bx, y + 6, 96, OPT_H - 12, "Cancel", FALSE);
        fill(tf_bx + 104, y + 6, 96, OPT_H - 12, COL_ACCENT);
        frame(tf_bx + 104, y + 6, 96, OPT_H - 12, COL_LINE);
        text(tf_bx + 104 + (96 - text_width("Apply")) / 2, y + (OPT_H - FONT_H) / 2, "Apply", 0xFFFFFFFF);
        text(x + text_width(buf) + 30, ty, "Corners: scale (Shift: proportional)  Outside: rotate  Return/Esc", COL_DIM);
        blit(0, y, SW, OPT_H);
        return;
    }

    switch (tool) {
    case T_BRUSH:
    case T_ERASER:
        x = brush_button(x, y) + 12;
        opt_sliders[n_opt_sliders++] = &sl_opac;
        opt_sliders[n_opt_sliders++] = &sl_flow;
        for (i = 0; i < n_opt_sliders; i++)
            x = slider_draw(opt_sliders[i], x, ty) + 14;
        break;
    case T_CROP: {
        LONG cw = 0, ch = 0;
        char buf[64];
        if (crop_active())
            crop_get(&cw, &ch);
        sprintf(buf, "W: %ld px    H: %ld px", (long)cw, (long)ch);
        text(x, ty, buf, COL_TEXT);
        text(x + text_width(buf) + 30, ty, "Drag the handles, drag inside to move   Return: commit   Esc: reset", COL_DIM);
        tf_bx = SW - PANEL_W - 210;
        button(tf_bx, y + 6, 96, OPT_H - 12, "Cancel", FALSE);
        fill(tf_bx + 104, y + 6, 96, OPT_H - 12, COL_ACCENT);
        frame(tf_bx + 104, y + 6, 96, OPT_H - 12, COL_LINE);
        text(tf_bx + 104 + (96 - text_width("Commit")) / 2, ty, "Commit", 0xFFFFFFFF);
        break; }
    case T_BLUR: {
        static const char *m[3] = { "Blur", "Sharpen", "Smudge" };
        x = brush_button(x, y) + 12;
        x = button_group(x, y, m, 3, 0, retouch_mode) + 16;
        opt_sliders[n_opt_sliders++] = &sl_rstr;
        x = slider_draw(&sl_rstr, x, ty) + 14;
        break; }
    case T_DODGE: {
        static const char *m[2] = { "Dodge", "Burn" };
        x = brush_button(x, y) + 12;
        x = button_group(x, y, m, 2, 0, toning_mode) + 16;
        opt_sliders[n_opt_sliders++] = &sl_texp;
        x = slider_draw(&sl_texp, x, ty) + 14;
        text(x, ty, "Range: Midtones", COL_DIM);
        break; }
    case T_HISTBRUSH:
    case T_HEAL:
        x = brush_button(x, y) + 12;
        if (tool == T_HISTBRUSH) {
            opt_sliders[n_opt_sliders++] = &sl_opac;
            opt_sliders[n_opt_sliders++] = &sl_flow;
            for (i = 0; i < n_opt_sliders; i++)
                x = slider_draw(opt_sliders[i], x, ty) + 14;
            text(x, ty, "Source: click the left column of the History panel", COL_DIM);
        } else {
            chk_x[0] = x;
            x = checkbox(x, y, "Generative AI", heal_ai);
            chk_w[0] = x - chk_x[0];
            text(x + 18, ty, heal_ai ? "Paint over an object - AI removes it" :
                 "Paint over a spot - it is replaced by matching texture from nearby", COL_DIM);
        }
        break;
    case T_CLONE:
        x = brush_button(x, y) + 12;
        opt_sliders[n_opt_sliders++] = &sl_opac;
        opt_sliders[n_opt_sliders++] = &sl_flow;
        for (i = 0; i < n_opt_sliders; i++)
            x = slider_draw(opt_sliders[i], x, ty) + 14;
        chk_x[0] = x;
        x = checkbox(x, y, "Aligned", clone_aligned);
        chk_w[0] = x - chk_x[0];
        text(x + 18, ty, "Alt+click: set source", COL_DIM);
        break;
    case T_GRADIENT: {
        static const char *types[2] = { "Linear", "Radial" };
        static const char *modes[2] = { "Foreground to Background", "Foreground to Transparent" };
        x = button_group(x, y, types, 2, 0, grad_type) + 12;
        x = button_group(x, y, modes, 2, 2, grad_mode) + 16;
        opt_sliders[n_opt_sliders++] = &sl_gopac;
        x = slider_draw(&sl_gopac, x, ty) + 6;
        chk_x[0] = x;
        x = checkbox(x, y, "Reverse", grad_reverse);
        chk_w[0] = x - chk_x[0];
        break; }
    case T_BUCKET:
        opt_sliders[n_opt_sliders++] = &sl_btol;
        opt_sliders[n_opt_sliders++] = &sl_bopac;
        for (i = 0; i < n_opt_sliders; i++)
            x = slider_draw(opt_sliders[i], x, ty) + 14;
        chk_x[0] = x;
        x = checkbox(x, y, "Contiguous", bucket_contig);
        chk_w[0] = x - chk_x[0];
        x += 18;
        chk_x[1] = x;
        x = checkbox(x, y, "Anti-alias", bucket_aa);
        chk_w[1] = x - chk_x[1];
        break;
    case T_SHAPE: {
        static const char *types[3] = { "Rectangle", "Ellipse", "Line" };
        x = button_group(x, y, types, 3, 0, shape_type) + 16;
        if (shape_type == SHAPE_LINE) {
            x = opt_swatch(x, y, "Color:", shape_fill, TRUE, &sw_fill_x) + 18;
            opt_sliders[n_opt_sliders++] = &sl_sweight;
            x = slider_draw(&sl_sweight, x, ty) + 14;
            sw_stroke_x = -100;
        } else {
            x = opt_swatch(x, y, "Fill:", shape_fill, shape_fill_on, &sw_fill_x) + 18;
            x = opt_swatch(x, y, "Stroke:", shape_stroke, shape_stroke_w > 0, &sw_stroke_x) + 10;
            opt_sliders[n_opt_sliders++] = &sl_sstroke;
            sl_sstroke.label = "";
            x = slider_draw(&sl_sstroke, x, ty) + 14;
        }
        text(x, ty, "Shift: square/circle/45°   Each shape gets its own layer", COL_DIM);
        break; }
    case T_MAGNETIC:
        x = draw_modes(x, y) + 16;
        opt_sliders[n_opt_sliders++] = &sl_mwidth;
        x = slider_draw(&sl_mwidth, x, ty) + 10;
        opt_sliders[n_opt_sliders++] = &sl_mcontr;
        x = slider_draw(&sl_mcontr, x, ty) + 10;
        opt_sliders[n_opt_sliders++] = &sl_mfreq;
        x = slider_draw(&sl_mfreq, x, ty) + 14;
        text(x, ty, "Click: anchor  Backspace: undo  Return: close", COL_DIM);
        break;
    case T_RECT:
    case T_ELLIPSE:
    case T_LASSO:
    case T_POLY:
        x = draw_modes(x, y) + 16;
        text(x, ty, tool == T_POLY ? "Click to add points, double-click or Return to close, Esc to cancel"
                                   : "Shift: add   Alt: subtract   Shift+Alt: intersect",
             COL_DIM);
        break;
    case T_ZOOM:
    case T_HAND:
        zoom_b1x = x;
        button(x, y + 6, 70, OPT_H - 12, "100%", FALSE);
        zoom_b2x = x + 78;
        button(x + 78, y + 6, 100, OPT_H - 12, "Fit on Screen", FALSE);
        text(x + 196, ty, tool == T_ZOOM ? "Click: zoom in   Alt+click: zoom out   Wheel: zoom"
                                         : "Drag to scroll the view (or hold Space)", COL_DIM);
        break;
    case T_QUICKSEL:
        x = draw_modes(x, y) + 16;
        x = brush_button(x, y) + 14;
        text(x, ty, "Drag over the object   Alt: subtract", COL_DIM);
        break;
    case T_WAND:
        x = draw_modes(x, y) + 16;
        opt_sliders[n_opt_sliders++] = &sl_tol;
        x = slider_draw(&sl_tol, x, ty) + 6;
        chk_x[0] = x;
        x = checkbox(x, y, "Contiguous", wand_contig);
        chk_w[0] = x - chk_x[0];
        x += 18;
        chk_x[1] = x;
        x = checkbox(x, y, "Anti-alias", wand_aa);
        chk_w[1] = x - chk_x[1];
        break;
    case T_TEXT:
        font_bx = x;
        fill(x, y + 6, 210, OPT_H - 12, COL_FIELD);
        frame(x, y + 6, 210, OPT_H - 12, COL_LINE);
        gfx_clip(x, y, x + 190, y + OPT_H);
        text(x + 8, ty, text_font_name(text_font), COL_TEXT);
        gfx_noclip();
        down_arrow(x + 196, y + OPT_H / 2 - 2, COL_DIM);
        x += 226;
        opt_sliders[n_opt_sliders++] = &sl_tsize;
        x = slider_draw(&sl_tsize, x, ty) + 14;
        /* Ausrichtung: drei Knoepfe mit Zeilen-Symbol */
        al_bx = x;
        {
            LONG k, j;
            for (k = 0; k < 3; k++) {
                LONG bx = x + k * 28;
                button(bx, y + 6, 26, OPT_H - 12, "", text_align == k);
                for (j = 0; j < 4; j++) {
                    LONG lw = j & 1 ? 8 : 14, lx = k == 0 ? bx + 6 : k == 1 ? bx + 13 - lw / 2 : bx + 20 - lw;
                    fill(lx, y + 11 + j * 4, lw, 1, COL_TEXT);
                }
            }
        }
        x += 3 * 28 + 10;
        aa_bx = x;
        text(x, ty, "aa", COL_DIM);
        fill(x + 20, y + 6, 80, OPT_H - 12, COL_FIELD);
        frame(x + 20, y + 6, 80, OPT_H - 12, COL_LINE);
        text(x + 28, ty, aa_names[text_aa], COL_TEXT);
        down_arrow(x + 88, y + OPT_H / 2 - 2, COL_DIM);
        x += 112;
        fill(x, y + 8, 18, 18, 0xFF000000 | fg_color);         /* Farbe = Vordergrund */
        frame(x, y + 8, 18, 18, 0xFFE0E0E0);
        x += 30;
        if (text_active()) {
            tx_bx = SW - PANEL_W - 210;
            button(tx_bx, y + 6, 96, OPT_H - 12, "Cancel", FALSE);
            fill(tx_bx + 104, y + 6, 96, OPT_H - 12, COL_ACCENT);
            frame(tx_bx + 104, y + 6, 96, OPT_H - 12, COL_LINE);
            text(tx_bx + 104 + (96 - text_width("Commit")) / 2, ty, "Commit", 0xFFFFFFFF);
            text(x, ty, "Ctrl+Return: commit   Esc: cancel", COL_DIM);
        } else
            text(x, ty, "Click to set the text baseline", COL_DIM);
        break;
    case T_PEN: {
        static const char *lab[4] = { "Make Selection", "Fill Path", "Stroke Path", "Delete Path" };
        LONG k;
        for (k = 0; k < 4; k++) {
            LONG w = text_width(lab[k]) + 20;
            pen_bx[k] = x;
            pen_bw[k] = w;
            button(x, y + 6, w, OPT_H - 12, lab[k], FALSE);
            x += w + 6;
        }
        text(x + 10, ty, path_exists() ? "Drag points to edit   Enter: end path   Backspace: remove last point"
                                       : "Click: corner point   Drag: curve point   Click first point: close", COL_DIM);
        break; }
    case T_MOVE:
        chk_x[0] = x;
        x = checkbox(x, y, "Auto-Select:", move_autosel);
        chk_w[0] = x - chk_x[0];
        x += 6;
        as_bx = x;
        fill(x, y + 6, 80, OPT_H - 12, COL_FIELD);
        frame(x, y + 6, 80, OPT_H - 12, COL_LINE);
        text(x + 8, ty, move_autogroup ? "Group" : "Layer", COL_TEXT);
        down_arrow(x + 68, y + OPT_H / 2 - 2, COL_DIM);
        x += 100;
        text(x, ty, "Ctrl+click: toggle Auto-Select   Arrow keys: 1 px, Shift: 10 px", COL_DIM);
        break;
    case T_EYEDROP:
        text(x, ty, "Sample: All Layers   (Brush Tool: Alt+click)", COL_DIM);
        break;
    default:
        text(x, ty, "Not implemented yet", COL_DIM);
        break;
    }
    blit(0, y, SW, OPT_H);
}

/* ---- Werkzeugleiste ---- */
LONG tool_visible[T_COUNT];             /* per Edit > Toolbar... waehlbar */
static LONG tb_y[T_COUNT];              /* -1 = ausgeblendet */
static LONG tb_more_y = -1, tb_colors_y;

/* Werkzeuggruppen: ein Platz je Gruppe (aufeinanderfolgende Werkzeuge) */
static LONG tool_group(LONG t)
{
    switch (t) {
    case T_RECT: case T_ELLIPSE: return T_RECT;
    case T_LASSO: case T_POLY: case T_MAGNETIC: return T_LASSO;
    case T_QUICKSEL: case T_WAND: return T_QUICKSEL;
    case T_GRADIENT: case T_BUCKET: return T_GRADIENT;
    }
    return t;
}
static LONG grp_cur[T_COUNT];           /* je Gruppe (erstes Werkzeug) das zuletzt benutzte */
static LONG slot_tool[T_COUNT], slot_y[T_COUNT], slot_cnt[T_COUNT], nslots;
static LONG tb_hold = -1;               /* Platz, auf dem die Maus gedrueckt ist */
static ULONG tb_hold_t0;

/* Lage der Plaetze; Abstaende zwischen den Abschnitten */
static void toolbar_layout(void)
{
    LONG i, y = MENU_H + OPT_H + 12, last = -1, any_hidden = FALSE;
    nslots = 0;
    for (i = 0; i < T_COUNT; i++) {
        tb_y[i] = -1;
        if (!tool_visible[i])
            any_hidden = TRUE;
    }
    for (i = 0; i < T_COUNT; ) {
        LONG g = tool_group(i), j, n = 0, show = -1;
        for (j = i; j < T_COUNT && tool_group(j) == g; j++)
            if (tool_visible[j]) {
                n++;
                if (show < 0 || j == grp_cur[g])
                    show = j;
            }
        if (n) {
            if (last >= 0 && ((g >= T_RECT && last < T_RECT) || (g >= T_EYEDROP && last < T_EYEDROP) ||
                              (g >= T_BRUSH && last < T_BRUSH) || (g >= T_PEN && last < T_PEN) ||
                              (g >= T_HAND && last < T_HAND)))
                y += 8;
            slot_tool[nslots] = show;
            slot_y[nslots] = y;
            slot_cnt[nslots] = n;
            nslots++;
            for (; i < j; i++)
                if (tool_visible[i])
                    tb_y[i] = y;
            y += 30;
            last = g;
        }
        i = j;
    }
    tb_more_y = any_hidden ? y + 4 : -1;
    if (any_hidden)
        y += 30;
    tb_colors_y = y + 18;
}

/* Varianten eines Platzes als Liste; liefert gewaehltes Werkzeug oder -1 */
static LONG slot_popup(LONG k)
{
    const char *items[T_COUNT];
    LONG map[T_COUNT], n = 0, cur = 0, j, g = tool_group(slot_tool[k]), r;
    LONG shown = tool_group(tool) == g ? tool : slot_tool[k];
    for (j = 0; j < T_COUNT; j++)
        if (tool_group(j) == g && tool_visible[j]) {
            if (j == shown)
                cur = n;
            items[n] = tool_names[j];
            map[n++] = j;
        }
    r = ui_popup(TOOL_W, slot_y[k], items, n, cur);
    return r >= 0 ? map[r] : -1;
}

static LONG colors_y(void)
{
    return tb_colors_y;
}

/* ---- Tooltips der Werkzeugleiste ---- */
static const char *tool_desc[T_COUNT] = {
    "Moves the layer, the group or the selected pixels",
    "Selects a rectangular area",
    "Selects an elliptical area",
    "Draws a freehand selection",
    "Selects with straight-edged segments",
    "Snaps to edges while you move along them",
    "Paint over an object - the selection snaps to its edges",
    "Selects areas of similar color",
    "Crops the image or extends the canvas",
    "Picks a color from the image",
    "Paints with the foreground color",
    "Paints with pixels copied from another spot (Alt+click: source)",
    "Paints back an earlier state from the History panel",
    "Removes spots using texture from the surroundings",
    "Erases pixels to transparency",
    "Fills with a color gradient (drag the direction)",
    "Fills areas of similar color with the foreground color",
    "Blurs, sharpens or smudges with a brush",
    "Lightens (Dodge) or darkens (Burn) areas with a brush",
    "Draws paths: click for corners, drag for curves",
    "Adds editable text (click to edit existing text)",
    "Draws rectangles, ellipses and lines on a new layer",
    "Scrolls the view (or hold Space)",
    "Zooms in (Alt+click: zoom out)",
};
static LONG tt_id = -1;         /* Werkzeug, -2 "...", -3 Farben, -1 nichts */
static BOOL tt_shown;
static ULONG tt_t0;
static ULONG *tt_save;
static LONG tt_x, tt_y, tt_w, tt_h;

static void tt_hide(void)
{
    LONG y;
    if (!tt_shown)
        return;
    tt_shown = FALSE;
    if (tt_save) {
        for (y = 0; y < tt_h; y++)
            CopyMem(tt_save + y * tt_w, bb + (tt_y + y) * SW + tt_x, tt_w * 4);
        FreeVec(tt_save);
        tt_save = NULL;
        blit(tt_x, tt_y, tt_w, tt_h);
    }
}

static void tt_show(void)
{
    const char *title, *desc;
    LONG y, w1, w2;
    if (tt_id >= 0) {
        title = tool_names[tt_id];
        desc = tool_desc[tt_id];
        tt_y = tb_y[tt_id];
    } else if (tt_id == -2) {
        title = "More Tools";
        desc = "Tools hidden via Edit > Toolbar...";
        tt_y = tb_more_y;
    } else {
        title = "Foreground / Background Color";
        desc = "Click to open the Color Picker  (X: swap, D: default)";
        tt_y = tb_colors_y;
    }
    w1 = text_width(title);
    w2 = text_width(desc);
    tt_w = (w1 > w2 ? w1 : w2) + 20;
    tt_h = 44;
    tt_x = TOOL_W + 4;
    if (tt_y + tt_h > SH)
        tt_y = SH - tt_h;
    if (!(tt_save = AllocVec(tt_w * tt_h * 4, MEMF_ANY)))
        return;
    for (y = 0; y < tt_h; y++)
        CopyMem(bb + (tt_y + y) * SW + tt_x, tt_save + y * tt_w, tt_w * 4);
    gfx_noclip();
    fill(tt_x, tt_y, tt_w, tt_h, 0xFF4A4A4A);
    frame(tt_x, tt_y, tt_w, tt_h, 0xFF202020);
    text_bold(tt_x + 10, tt_y + 4, title, COL_TEXT);
    text(tt_x + 10, tt_y + 22, desc, 0xFFC8C8C8);
    blit(tt_x, tt_y, tt_w, tt_h);
    tt_shown = TRUE;
}

/* Maus ueber der Leiste: welches Element? (von main bei jeder Bewegung) */
void ui_hover(LONG x, LONG y)
{
    LONG id = -1, i;
    if (x < TOOL_W && !ui_menu_open()) {
        for (i = 0; i < nslots; i++)
            if (y >= slot_y[i] && y < slot_y[i] + 28)
                id = tool_group(tool) == tool_group(slot_tool[i]) ? tool : slot_tool[i];
        if (id < 0 && tb_more_y >= 0 && y >= tb_more_y && y < tb_more_y + 28)
            id = -2;
        if (id < 0 && y >= tb_colors_y - 2 && y < tb_colors_y + 30)
            id = -3;
    }
    if (id != tt_id) {
        tt_hide();
        tt_id = id;
        tt_t0 = now_us();
    }
}

/* Takt (10/s): Tooltip nach kurzer Ruhe zeigen */
void ui_tick(void)
{
    if (tb_hold >= 0 && now_us() - tb_hold_t0 > 400000) {
        LONG k = tb_hold, t;
        tb_hold = -1;
        tt_hide();
        t = slot_popup(k);
        if (t >= 0)
            ui_set_tool(t);
        ui_draw_toolbar();
        return;
    }
    if (tt_id != -1 && !tt_shown && !ui_menu_open() && now_us() - tt_t0 > 500000)
        tt_show();
}

/* Rechtsklick: Varianten eines Werkzeugplatzes */
BOOL ui_right_click(LONG x, LONG y)
{
    LONG i, t;
    if (x >= TOOL_W)
        return FALSE;
    for (i = 0; i < nslots; i++)
        if (y >= slot_y[i] && y < slot_y[i] + 28) {
            tt_hide();
            t = slot_popup(i);
            if (t >= 0)
                ui_set_tool(t);
            ui_draw_toolbar();
            return TRUE;
        }
    return FALSE;
}

void ui_hover_hide(void)
{
    tt_hide();
    tt_t0 = now_us();           /* erst nach erneuter Ruhe wieder zeigen */
}

/* ---- Einstellungen (ENV:/ENVARC:AmiPixelShop.prefs, Zeilen key=value) ---- */
ULONG pref_mode = 0xFFFFFFFF;           /* Bildschirmmodus (INVALID_ID = fragen) */

/* nur den Bildschirmmodus lesen (vor dem Oeffnen des Bildschirms) */
ULONG ui_prefs_mode(void)
{
    char buf[512], *p;
    BPTR fh;
    LONG n;
    ULONG v = 0;
    if (!(fh = Open("ENV:AmiPixelShop.prefs", MODE_OLDFILE)))
        return pref_mode;
    n = Read(fh, buf, sizeof(buf) - 1);
    Close(fh);
    if (n <= 0)
        return pref_mode;
    buf[n] = 0;
    if ((p = strstr(buf, "mode=0x"))) {
        for (p += 7; (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'); p++)
            v = v * 16 + (*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
        pref_mode = v;
    }
    return pref_mode;
}

/* ---- File > New: Dokumentvorlagen ---- */

typedef struct { const char *name; LONG w, h, colors; } NPreset;

/* nur Groessen bis etwa Full HD (Speicher) */
static const NPreset np_photo[] = {
    { "Landscape 4 x 6", 1800, 1200, 0 }, { "Portrait 4 x 6", 1200, 1800, 0 },
    { "Landscape 5 x 7", 1500, 1050, 0 }, { "Square", 1500, 1500, 0 },
    { "Landscape 3 x 2", 1620, 1080, 0 }, { "Portrait 2 x 3", 1080, 1620, 0 } };
static const NPreset np_print[] = {
    { "A5 @ 150 ppi", 874, 1240, 0 }, { "A6 Postcard @ 150 ppi", 620, 874, 0 },
    { "A4 @ 120 ppi", 992, 1403, 0 }, { "Letter @ 120 ppi", 1020, 1320, 0 },
    { "Business Card @ 300 ppi", 1050, 600, 0 }, { "Square Flyer @ 150 ppi", 1200, 1200, 0 } };
static const NPreset np_web[] = {
    { "Web Large", 1920, 1080, 0 }, { "Web Medium", 1366, 768, 0 },
    { "Web Small", 1024, 768, 0 }, { "Social Square", 1080, 1080, 0 },
    { "Social Banner", 1200, 628, 0 }, { "Icon", 512, 512, 0 } };
static const NPreset np_mobile[] = {
    { "Phone Portrait", 1080, 1920, 0 }, { "Phone Landscape", 1920, 1080, 0 },
    { "Story", 1080, 1920, 0 }, { "Small Phone", 750, 1334, 0 },
    { "Tablet Landscape", 1366, 1024, 0 }, { "App Icon", 1024, 1024, 0 } };
static const NPreset np_film[] = {
    { "HDTV 1080p", 1920, 1080, 0 }, { "HDTV 720p", 1280, 720, 0 },
    { "PAL D1/DV", 720, 576, 0 }, { "NTSC D1", 720, 480, 0 },
    { "PAL Widescreen", 1024, 576, 0 }, { "NTSC Widescreen", 854, 480, 0 } };
static const NPreset np_amiga[] = {
    { "PAL Lowres", 320, 256, 32 }, { "PAL Lowres EHB", 320, 256, 64 },
    { "PAL Lowres HAM6", 320, 256, -6 }, { "PAL Hires", 640, 256, 16 },
    { "PAL Hires Laced", 640, 512, 16 }, { "PAL Overscan", 368, 283, 32 },
    { "NTSC Lowres", 320, 200, 32 }, { "NTSC Hires", 640, 200, 16 },
    { "NTSC Hires Laced", 640, 400, 16 }, { "Super-Hires Laced", 1280, 512, 4 },
    { "Productivity", 640, 480, 4 }, { "AGA Lowres", 320, 256, 256 },
    { "AGA Hires Laced", 640, 512, 256 }, { "AGA HAM8", 640, 512, -8 },
    { "Workbench Icon", 64, 64, 256 }, { "RTG 800 x 600", 800, 600, 0 } };

#define NP_MAXUSER 8
static NPreset np_recent[NP_MAXUSER], np_saved[NP_MAXUSER];
static char np_rname[NP_MAXUSER][40], np_sname[NP_MAXUSER][40];
static LONG np_nrecent, np_nsaved;

static const char *np_tabs[8] = { "Recent", "Photo", "Print", "Web", "Mobile", "Film & Video", "Amiga", "Saved" };
static const LONG np_depths[11] = { 0, 2, 4, 8, 16, 32, 64, 128, 256, -6, -8 };
static const char *np_bgs[4] = { "White", "Black", "Background Color", "Transparent" };

void color_depth_name(LONG c, char *buf)
{
    if (!c)
        strcpy(buf, "RGB/8");
    else if (c == -6)
        strcpy(buf, "HAM6");
    else if (c == -8)
        strcpy(buf, "HAM8");
    else if (c == 64)
        strcpy(buf, "EHB");
    else
        sprintf(buf, "%ld colors", (long)c);
}

static void np_label(LONG c, char *buf)
{
    if (!c)
        strcpy(buf, "RGB Color, 8 bit");
    else if (c == -6)
        strcpy(buf, "Amiga HAM6 (4096 colors)");
    else if (c == -8)
        strcpy(buf, "Amiga HAM8 (262144 colors)");
    else if (c == 64)
        strcpy(buf, "Amiga EHB (64 colors)");
    else
        sprintf(buf, "Amiga %ld colors", (long)c);
}

static const NPreset *np_list(LONG tab, LONG *n)
{
    switch (tab) {
    case 0: *n = np_nrecent; return np_recent;
    case 1: *n = 6; return np_photo;
    case 2: *n = 6; return np_print;
    case 3: *n = 6; return np_web;
    case 4: *n = 6; return np_mobile;
    case 5: *n = 6; return np_film;
    case 6: *n = 16; return np_amiga;
    }
    *n = np_nsaved;
    return np_saved;
}

/* Prefs: "recent=Name|W|H|C;..." und "saved=..." */
static void np_parse(const char *p, NPreset *list, char (*names)[40], LONG *n)
{
    *n = 0;
    while (*p && *p != '\n' && *n < NP_MAXUSER) {
        LONG k = 0, w, h, c;
        char *e;
        while (*p && *p != '|' && *p != '\n' && k < 39)
            names[*n][k++] = *p++;
        names[*n][k] = 0;
        if (*p != '|')
            break;
        w = strtol(p + 1, &e, 10);
        if (*e != '|')
            break;
        h = strtol(e + 1, &e, 10);
        if (*e != '|')
            break;
        c = strtol(e + 1, &e, 10);
        if (w >= 1 && h >= 1 && w <= 16000 && h <= 16000) {
            list[*n].name = names[*n];
            list[*n].w = w;
            list[*n].h = h;
            list[*n].colors = c;
            (*n)++;
        }
        p = *e == ';' ? e + 1 : e;
    }
}

static void np_write(char *buf, const char *key, const NPreset *list, LONG n)
{
    LONG i;
    strcat(buf, key);
    for (i = 0; i < n; i++)
        sprintf(buf + strlen(buf), "%s%s|%ld|%ld|%ld", i ? ";" : "", list[i].name,
                (long)list[i].w, (long)list[i].h, (long)list[i].colors);
    strcat(buf, "\n");
}

static void np_load(const char *prefs)
{
    const char *p;
    if ((p = strstr(prefs, "recent=")))
        np_parse(p + 7, np_recent, np_rname, &np_nrecent);
    if ((p = strstr(prefs, "saved=")))
        np_parse(p + 6, np_saved, np_sname, &np_nsaved);
}

/* vorne einfuegen (gleiche Groesse/Farbtiefe nur einmal) */
static void np_push(NPreset *list, char (*names)[40], LONG *n, const char *name, LONG w, LONG h, LONG c)
{
    LONG i, j;
    char nm[40];
    strncpy(nm, name, 39);
    nm[39] = 0;
    for (i = 0; i < *n; i++)
        if (list[i].w == w && list[i].h == h && list[i].colors == c && !strcmp(list[i].name, nm))
            break;
    if (i == *n) {
        if (*n < NP_MAXUSER)
            (*n)++;
        i = *n - 1;
    }
    for (j = i; j > 0; j--) {
        strcpy(names[j], names[j - 1]);
        list[j] = list[j - 1];
        list[j].name = names[j];
    }
    strcpy(names[0], nm);
    list[0].name = names[0];
    list[0].w = w;
    list[0].h = h;
    list[0].colors = c;
}

void ui_prefs_load(void)
{
    char buf[4096];
    BPTR fh;
    LONG i, n;
    for (i = 0; i < T_COUNT; i++)
        tool_visible[i] = TRUE;
    if (!(fh = Open("ENV:AmiPixelShop.prefs", MODE_OLDFILE)))
        return;
    n = Read(fh, buf, sizeof(buf) - 1);
    Close(fh);
    if (n <= 0)
        return;
    buf[n] = 0;
    {
        /* "hide=Name;Name;..." (Namen statt Positionen: neue Werkzeuge verschieben nichts) */
        char *p = strstr(buf, "hide=");
        if (p) {
            p += 5;
            while (*p && *p != '\n') {
                char *e = p;
                while (*e && *e != ';' && *e != '\n')
                    e++;
                for (i = 0; i < T_COUNT; i++)
                    if ((LONG)strlen(tool_names[i]) == e - p && !strncmp(tool_names[i], p, e - p))
                        tool_visible[i] = FALSE;
                p = *e == ';' ? e + 1 : e;
            }
        }
    }
    np_load(buf);
    move_autosel = strstr(buf, "autosel=1") != NULL;
    move_autogroup = strstr(buf, "autogroup=1") != NULL;
    ai_taskbar = strstr(buf, "taskbar=0") == NULL;
    upd_auto = strstr(buf, "updcheck=0") == NULL;
    rulers_on = strstr(buf, "rulers=1") != NULL;
    guides_snap = strstr(buf, "snap=0") == NULL;
    {
        char *q = strstr(buf, "updskip=");
        LONG k;
        upd_skip[0] = 0;
        if (q) {
            for (q += 8, k = 0; *q && *q != '\n' && k < (LONG)sizeof(upd_skip) - 1; )
                upd_skip[k++] = *q++;
            upd_skip[k] = 0;
        }
    }
    {
        char *q;
        if ((q = strstr(buf, "aifill=")) && q[7] >= '0' && q[7] <= '2') ai_fill_model = q[7] - '0';
        if ((q = strstr(buf, "aifast=")) && q[7] >= '0' && q[7] <= '1') ai_fast_model = q[7] - '0';
        if ((q = strstr(buf, "aivar=")) && q[6] >= '1' && q[6] <= '3') ai_variants = q[6] - '0';
        if ((q = strstr(buf, "aiprov=")) && q[7] >= '0' && q[7] <= '2') ai_provider = q[7] - '0';
    }
    for (i = 0; i < T_COUNT && !tool_visible[i]; i++)
        ;
    if (i == T_COUNT)
        tool_visible[T_MOVE] = TRUE;        /* nie ganz leer */
}

void ui_prefs_save(void)
{
    static const char *files[2] = { "ENV:AmiPixelShop.prefs", "ENVARC:AmiPixelShop.prefs" };
    char buf[4096];
    LONG i, k;
    strcpy(buf, "hide=");
    for (i = 0; i < T_COUNT; i++)
        if (!tool_visible[i]) {
            if (buf[5])
                strcat(buf, ";");
            strcat(buf, tool_names[i]);
        }
    strcat(buf, "\n");
    if (pref_mode != 0xFFFFFFFF)
        sprintf(buf + strlen(buf), "mode=0x%08lx\n", (unsigned long)pref_mode);
    sprintf(buf + strlen(buf), "autosel=%ld\nautogroup=%ld\ntaskbar=%ld\n", (long)move_autosel,
            (long)move_autogroup, (long)ai_taskbar);
    sprintf(buf + strlen(buf), "aifill=%ld\naifast=%ld\naivar=%ld\naiprov=%ld\n", (long)ai_fill_model,
            (long)ai_fast_model, (long)ai_variants, (long)ai_provider);
    sprintf(buf + strlen(buf), "updcheck=%ld\nrulers=%ld\nsnap=%ld\n", (long)upd_auto, (long)rulers_on,
            (long)guides_snap);
    if (upd_skip[0])
        sprintf(buf + strlen(buf), "updskip=%s\n", upd_skip);
    if (np_nrecent)
        np_write(buf, "recent=", np_recent, np_nrecent);
    if (np_nsaved)
        np_write(buf, "saved=", np_saved, np_nsaved);
    for (k = 0; k < 2; k++) {
        BPTR fh = Open(files[k], MODE_NEWFILE);
        if (fh) {
            Write(fh, buf, strlen(buf));
            Close(fh);
        }
    }
}

/* Edit > Toolbar...: Werkzeuge der Leiste waehlen */
void ui_toolbar_dialog(void)
{
    DlgSlider ds[T_COUNT];
    LONG v[T_COUNT], i;
    for (i = 0; i < T_COUNT; i++) {
        v[i] = tool_visible[i];
        ds[i].label = tool_names[i];
        ds[i].val = &v[i];
        ds[i].min = ds[i].max = DLG_CHECK;
        ds[i].unit = "";
        ds[i].div = 1;
    }
    if (!dialog_run("Customize Toolbar", ds, T_COUNT, NULL))
        return;
    for (i = 0; i < T_COUNT && !v[i]; i++)
        ;
    if (i == T_COUNT)
        v[T_MOVE] = TRUE;
    for (i = 0; i < T_COUNT; i++)
        tool_visible[i] = v[i] != 0;
    ui_prefs_save();
    ui_draw_toolbar();
}

void ui_draw_toolbar(void)
{
    if (tt_shown) {
        tt_shown = FALSE;       /* Hintergrund wird ohnehin neu gezeichnet */
        if (tt_save) {
            FreeVec(tt_save);
            tt_save = NULL;
        }
    }
    LONG top = MENU_H + OPT_H, i, x, y, cy;
    fill(0, top, TOOL_W, SH - top, COL_BAR);
    fill(TOOL_W - 1, top, 1, SH - top, COL_LINE);
    fill(6, top + 4, TOOL_W - 12, 2, COL_LIGHT);
    toolbar_layout();
    for (i = 0; i < nslots; i++) {
        LONG t = slot_tool[i];
        BOOL on = tool_group(tool) == tool_group(t) && tool_visible[tool];
        x = 4;
        y = slot_y[i];
        if (on) {
            fill(x, y, 31, 28, COL_TOOLSEL);
            frame(x, y, 31, 28, 0xFF151515);
        }
        icon(x + 8, y + 6, on ? tool : t, on ? 0xFFFFFFFF : 0xFFC4C4C4);
        if (slot_cnt[i] > 1) {          /* Dreieck: weitere Werkzeuge */
            LONG k;
            for (k = 0; k < 4; k++)
                fill(x + 26 - k, y + 23 + k - 3, k + 1, 1, on ? 0xFFFFFFFF : 0xFF9A9A9A);
        }
    }
    if (tb_more_y >= 0) {           /* "...": ausgeblendete Werkzeuge */
        BOOL hid = !tool_visible[tool];
        if (hid) {
            fill(4, tb_more_y, 31, 28, COL_TOOLSEL);
            frame(4, tb_more_y, 31, 28, 0xFF151515);
        }
        for (i = 0; i < 3; i++)
            fill(13 + i * 6, tb_more_y + 13, 2, 2, hid ? 0xFFFFFFFF : 0xFFC4C4C4);
    }
    cy = colors_y();
    /* Hintergrundfarbe hinten, Vordergrundfarbe vorne */
    fill(15, cy + 10, 18, 18, 0xFF000000 | bg_color);
    frame(15, cy + 10, 18, 18, 0xFF9A9A9A);
    fill(6, cy, 18, 18, 0xFF000000 | fg_color);
    frame(6, cy, 18, 18, 0xFFE0E0E0);
    /* Tauschpfeil und Standardfarben (klein) */
    line(28, cy + 1, 33, cy + 1, 0xFFC4C4C4);
    line(33, cy + 1, 33, cy + 6, 0xFFC4C4C4);
    line(31, cy + 4, 33, cy + 6, 0xFFC4C4C4);
    line(26, cy + 1, 28, cy - 1, 0xFFC4C4C4);
    line(26, cy + 1, 28, cy + 3, 0xFFC4C4C4);
    fill(6, cy + 23, 5, 5, 0xFFFFFFFF);
    fill(4, cy + 21, 5, 5, 0xFF000000);
    frame(4, cy + 21, 5, 5, 0xFFC4C4C4);
    color_panel_sync();             /* Vordergrundfarbe evtl. geaendert */
    blit(0, top, TOOL_W, SH - top);
}

/* ---- Panels rechts ---- */
static LONG sw_top, ly_top, ly_list, ly_foot, ly_modey;
static LONG panel_tab;          /* 0 = Ebenen, 1 = Protokoll, 2 = Pfade */
static LONG tab_x[4];

#define ROW_H  38
#define HIST_H 22

/* Kopfzeile mit Reitern */
static void panel_tabs(LONG y, const char **titles, LONG n, LONG cur)
{
    LONG px = SW - PANEL_W, x = px + 4, i;
    fill(px + 1, y, PANEL_W - 1, 24, COL_PANELHD);
    for (i = 0; i < n; i++) {
        LONG w = text_width(titles[i]) + 20;
        tab_x[i] = x;
        if (i == cur) {
            fill(x, y + 3, w, 21, COL_PANEL);
            text_bold(x + 10, y + 4, titles[i], COL_TEXT);
        } else
            text(x + 10, y + 4, titles[i], COL_DIM);
        x += w + 2;
    }
    tab_x[n] = x;
}

static ULONG layer_px(Layer *l, LONG x, LONG y)
{
    Tile *t = l->tiles[(y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT)];
    return t ? t->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)] : 0;
}

static void draw_thumb(Layer *l, LONG x, LONG y)
{
    LONG tw = 28, th = 28, u, v, ox0, oy0;
    if (doc->w >= doc->h)
        th = 28 * doc->h / doc->w;
    else
        tw = 28 * doc->w / doc->h;
    if (th < 1) th = 1;
    if (tw < 1) tw = 1;
    ox0 = x + (28 - tw) / 2;
    oy0 = y + (28 - th) / 2;
    fill(x - 1, y - 1, 30, 30, COL_PANEL);
    for (v = 0; v < th; v++)
        for (u = 0; u < tw; u++) {
            ULONG p = layer_px(l, u * doc->w / tw, v * doc->h / th);
            ULONG a = p >> 24, g = (((u >> 2) ^ (v >> 2)) & 1) ? 0xCC : 0xFF;
            ULONG r = (((p >> 16) & 0xFF) * a + g * (255 - a)) / 255;
            ULONG gg = (((p >> 8) & 0xFF) * a + g * (255 - a)) / 255;
            ULONG b = ((p & 0xFF) * a + g * (255 - a)) / 255;
            pset(ox0 + u, oy0 + v, 0xFF000000 | (r << 16) | (gg << 8) | b);
        }
    frame(ox0 - 1, oy0 - 1, tw + 2, th + 2, COL_LINE);
}

/* Maske als Graustufen-Vorschau */
static void draw_mask_thumb(Layer *l, LONG x, LONG y)
{
    LONG tw = 28, th = 28, u, v, ox0, oy0;
    if (doc->w >= doc->h)
        th = 28 * doc->h / doc->w;
    else
        tw = 28 * doc->w / doc->h;
    if (th < 1) th = 1;
    if (tw < 1) tw = 1;
    ox0 = x + (28 - tw) / 2;
    oy0 = y + (28 - th) / 2;
    fill(x - 1, y - 1, 30, 30, COL_PANEL);
    for (v = 0; v < th; v++)
        for (u = 0; u < tw; u++) {
            LONG dx = u * doc->w / tw, dy = v * doc->h / th;
            Tile *t = l->mask[(dy >> TILE_SHIFT) * doc->tw + (dx >> TILE_SHIFT)];
            ULONG g = t ? t->px[((dy & TILE_MASK) << TILE_SHIFT) | (dx & TILE_MASK)] & 0xFF : 255;
            pset(ox0 + u, oy0 + v, 0xFF000000 | (g * 0x010101));
        }
    frame(ox0 - 1, oy0 - 1, tw + 2, th + 2, COL_LINE);
    if (!l->mask_on) {          /* ausgeschaltet: rotes X */
        line(ox0, oy0, ox0 + tw - 1, oy0 + th - 1, 0xFFE03030);
        line(ox0 + tw - 1, oy0, ox0, oy0 + th - 1, 0xFFE03030);
    }
}

/* Textebene: "T" statt Pixelvorschau (wie Photoshop) */
static void draw_text_thumb(LONG x, LONG y)
{
    fill(x - 1, y - 1, 30, 30, 0xFF606060);
    frame(x - 1, y - 1, 30, 30, COL_LINE);
    icon(x + 6, y + 6, T_TEXT, 0xFFFFFFFF);
}

static const char *eye_icon[7] = {
    "....######....", "..##......##..", ".#....##....#.", "#....####....#",
    ".#....##....#.", "..##......##..", "....######...."
};

/* kleines Dreieck nach unten (Aufklapp-Symbol) */
static void down_arrow(LONG x, LONG y, ULONG c)
{
    LONG i;
    for (i = 0; i < 4; i++)
        fill(x + i, y + i, 7 - 2 * i, 1, c);
}

/* Ziehen in der Ebenenliste */
static LONG ldrag_from = -1, ldrag_y0, ldrag_slot = -1;
static BOOL ldragging;
static ULONG last_row_click;
static LONG last_row;
static UndoRec *op_undo;        /* Deckkraft-Aenderung laeuft */

/* Anzeigezeile -> Ebenenindex (oben = oberste Ebene) */
/* Anzeigeliste: von oben, Kinder zugeklappter Gruppen fehlen */
static LONG disp[MAX_LAYERS], ndisp;
static BOOL ldrag_into;         /* Ablegen mitten auf eine Gruppe: hinein */

static void build_disp(void)
{
    LONG li;
    ndisp = 0;
    for (li = doc->nlayers - 1; li >= 0; li--) {
        LONG d = doc->layers[li]->depth, j;
        BOOL hide = FALSE;
        for (j = li + 1; j < doc->nlayers && d > 0 && !hide; j++)
            if (doc->layers[j]->depth < d) {
                hide = doc->layers[j]->collapsed;
                d = doc->layers[j]->depth;
            }
        if (!hide)
            disp[ndisp++] = li;
    }
}

static LONG row_layer(LONG row) { return row >= 0 && row < ndisp ? disp[row] : -1; }

static LONG layer_indent(Layer *l) { return l->depth * 12 + (l->clip ? 14 : 0); }

/* Ordner fuer Gruppen */
static void draw_folder(LONG x, LONG y, ULONG c)
{
    fill(x, y + 2, 7, 2, c);
    frame(x, y + 4, 18, 13, c);
    fill(x + 1, y + 5, 16, 1, c);
}

/* Symbol der Korrekturebene: Kreis halb schwarz, halb weiss */
static void draw_adjust_thumb(LONG x, LONG y)
{
    LONG i, j;
    fill(x - 1, y - 1, 30, 30, 0xFF606060);
    frame(x - 1, y - 1, 30, 30, COL_LINE);
    for (j = 0; j < 20; j++)
        for (i = 0; i < 20; i++) {
            LONG dx = 2 * i - 19, dy = 2 * j - 19;
            if (dx * dx + dy * dy <= 400)
                pset(x + 4 + i, y + 4 + j, i + j < 20 ? 0xFFFFFFFF : 0xFF202020);
        }
}

static void draw_layers_tab(LONG y)
{
    LONG px = SW - PANEL_W, i;
    char buf[48];
    Layer *al = doc->layers[doc->active];

    /* Fuellmethode */
    ly_modey = y + 6;
    fill(px + 10, ly_modey, 150, 22, COL_FIELD);
    frame(px + 10, ly_modey, 150, 22, COL_LINE);
    text(px + 18, ly_modey + 2, al->kind == LK_GROUP && al->pass ? "Pass Through" : blend_names[al->mode], COL_TEXT);
    down_arrow(px + 146, ly_modey + 9, COL_DIM);
    /* Deckkraft */
    layer_op_pct = (al->opacity * 100 + 127) / 255;
    sl_layer.tw = 100;
    slider_draw(&sl_layer, px + 10, y + 34);
    /* Sperren und Flaeche */
    ly_locky = y + 58;
    text(px + 10, ly_locky + 2, "Lock:", COL_DIM);
    for (i = 0; i < 4; i++) {
        static const UBYTE bits[4] = { LOCK_ALPHA, LOCK_PIXELS, LOCK_POS, LOCK_ALL };
        LONG bx = px + 46 + i * 24;
        BOOL on = bits[i] == LOCK_ALL ? (al->lock & LOCK_ALL) == LOCK_ALL : (al->lock & bits[i]) != 0;
        fill(bx, ly_locky, 22, 20, on ? COL_TOOLSEL : COL_PANEL);
        frame(bx, ly_locky, 22, 20, on ? 0xFF6A8AC0 : COL_LINE);
        draw_icon12(bx + 5, ly_locky + 4, lock_icons[i], on ? 0xFFFFFFFF : COL_DIM);
    }
    layer_fill_pct = (al->fill * 100 + 127) / 255;
    sl_lfill.tw = 20;
    slider_draw(&sl_lfill, px + 146, ly_locky + 2);
    fill(px + 1, y + 84, PANEL_W - 1, 1, COL_LINE);
    ly_list = y + 85;
    ly_foot = SH - 26;

    build_disp();
    for (i = 0; i < ndisp; i++) {
        LONG li = row_layer(i), ry = ly_list + i * ROW_H, k, j, ind;
        Layer *l = doc->layers[li];
        if (ry + ROW_H > ly_foot)
            break;
        ind = layer_indent(l);
        fill(px + 1, ry, PANEL_W - 1, ROW_H - 1, li == doc->active ? COL_ACCENT :
             l->picked ? 0xFF2F4F7A : COL_PANEL);
        fill(px + 1, ry + ROW_H - 1, PANEL_W - 1, 1, COL_LINE);
        fill(px + 1, ry, 28, ROW_H - 1, li == doc->active ? 0xFF345A90 : COL_PANEL);
        fill(px + 29, ry, 1, ROW_H - 1, COL_LINE);
        if (l->visible)
            for (j = 0; j < 7; j++)
                for (k = 0; k < 14; k++)
                    if (eye_icon[j][k] == '#')
                        pset(px + 8 + k, ry + 15 + j, COL_TEXT);
        if (l->kind == LK_GROUP) {
            /* Auf/Zu-Dreieck und Ordner */
            ULONG c = li == doc->active ? 0xFFFFFFFF : COL_TEXT;
            LONG tx = px + 36 + ind, ty = ry + 13;
            if (l->collapsed)
                for (k = 0; k < 5; k++)
                    fill(tx + k, ty + k, 1, 10 - 2 * k, c);
            else
                for (k = 0; k < 5; k++)
                    fill(tx + k, ty + 2 + k, 10 - 2 * k, 1, c);
            draw_folder(px + 50 + ind, ry + 9, c);
            if (l->mask)
                draw_mask_thumb(l, px + 72 + ind, ry + 4);
            if (li == doc->active && doc_target_is_mask())
                frame(px + 70 + ind, ry + 2, 32, 32, 0xFFFFFFFF);
        } else {
            if (l->clip) {          /* Schnittmaske: Pfeil nach unten */
                ULONG c = li == doc->active ? 0xFFFFFFFF : COL_DIM;
                LONG ax = px + 30 + ind - 6, ay = ry + 10;
                fill(ax, ay, 1, 12, c);
                fill(ax, ay + 11, 6, 1, c);
                fill(ax + 3, ay + 9, 1, 5, c);
            }
            if (l->kind == LK_ADJUST)
                draw_adjust_thumb(px + 36 + ind, ry + 4);
            else if (l->text)
                draw_text_thumb(px + 36 + ind, ry + 4);
            else
                draw_thumb(l, px + 36 + ind, ry + 4);
            if (l->mask) {
                draw_mask_thumb(l, px + 78 + ind, ry + 4);
                if (!l->mask_unlink) {      /* Kette: Maske mit Ebene verknuepft */
                    ULONG c = li == doc->active ? 0xFFFFFFFF : COL_DIM;
                    LONG cx = px + 68 + ind, cy = ry + 10;
                    frame(cx, cy, 5, 7, c);
                    frame(cx, cy + 6, 5, 7, c);
                    fill(cx + 2, cy + 4, 1, 5, c);
                }
            }
            if (li == doc->active)          /* was wird bearbeitet: Ebene oder Maske */
                frame((doc_target_is_mask() ? px + 76 : px + 34) + ind, ry + 2, 32, 32, 0xFFFFFFFF);
        }
        if (fx_active(l->fx))
            text_bold(SW - 26, ry + 4, "fx", li == doc->active ? 0xFFFFFFFF : COL_TEXT);
        if (l->lock)
            draw_icon12(SW - 26, ry + 20, lock_icons[3], li == doc->active ? 0xFFFFFFFF : COL_DIM);
        {
            LONG nx = (l->kind == LK_GROUP ? (l->mask ? px + 108 : px + 76) : l->mask ? px + 114 : px + 72) + ind;
            gfx_clip(nx, ry, fx_active(l->fx) ? SW - 30 : SW, ry + ROW_H);
            text(nx, ry + 4, l->name, COL_TEXT);
            if (li + 1 < doc->nlayers && clip_base(li + 1) == li)   /* Basis einer Schnittmaske */
                fill(nx, ry + 4 + FONT_H, text_width(l->name), 1, COL_TEXT);
            if ((l->kind == LK_GROUP && !l->pass) || (l->kind != LK_GROUP && l->mode != BM_NORMAL) ||
                l->opacity != 255) {
                sprintf(buf, "%s, %ld%%", l->kind == LK_GROUP && l->pass ? "Pass Through" : blend_names[l->mode],
                        (long)((l->opacity * 100 + 127) / 255));
                text(nx, ry + 19, buf, li == doc->active ? 0xFFB8C8E0 : COL_DIM);
            }
            gfx_noclip();
        }
    }
    /* Einfuegemarke beim Ziehen */
    if (ldragging && ldrag_slot >= 0) {
        LONG ly = ly_list + ldrag_slot * ROW_H - 1;
        if (ldrag_into)                 /* in die Gruppe: Zeile umrahmen */
            frame(px + 2, ly + 1, PANEL_W - 3, ROW_H - 1, 0xFFE0E0E0);
        else
            fill(px + 1, ly - 1, PANEL_W - 1, 3, 0xFFE0E0E0);
    }

    fill(px + 1, ly_foot, PANEL_W - 1, 26, COL_PANELHD);
    fill(px + 1, ly_foot, PANEL_W - 1, 1, COL_LINE);
    button(SW - 184, ly_foot + 4, 26, 18, "", FALSE);   /* neue Gruppe */
    draw_folder(SW - 180, ly_foot + 3, COL_TEXT);
    button(SW - 154, ly_foot + 4, 26, 18, "", FALSE);   /* neue Korrekturebene */
    {
        LONG i2, j2;
        for (j2 = 0; j2 < 12; j2++)
            for (i2 = 0; i2 < 12; i2++) {
                LONG dx = 2 * i2 - 11, dy = 2 * j2 - 11;
                if (dx * dx + dy * dy <= 132)
                    pset(SW - 147 + i2, ly_foot + 7 + j2, i2 + j2 < 12 ? COL_TEXT : 0xFF202020);
            }
    }
    button(SW - 124, ly_foot + 4, 26, 18, "fx", FALSE);
    button(SW - 94, ly_foot + 4, 26, 18, "", FALSE);    /* Maske hinzufuegen */
    frame(SW - 88, ly_foot + 7, 14, 12, COL_TEXT);
    fill(SW - 83, ly_foot + 11, 4, 4, COL_TEXT);
    fill(SW - 84, ly_foot + 12, 6, 2, COL_TEXT);
    fill(SW - 82, ly_foot + 10, 2, 6, COL_TEXT);
    button(SW - 64, ly_foot + 4, 26, 18, "+", FALSE);
    button(SW - 34, ly_foot + 4, 26, 18, "-", FALSE);
    sprintf(buf, doc->nlayers == 1 ? "%ld layer" : "%ld layers", (long)doc->nlayers);
    text(px + 10, ly_foot + 4, buf, COL_DIM);
}

#define HIST_MAX 48
static const char *hist_names[HIST_MAX];
static LONG hist_n, hist_cur, hist_first;

/* ---- Pfade ---- */
static LONG pa_bx[4];
static const char *pa_lab[4] = { "Fill", "Stroke", "Selection", "Delete" };
static const LONG pa_cmd[4] = { CMD_PATH_FILL, CMD_PATH_STROKE, CMD_PATH_SEL, CMD_PATH_DEL };

static void draw_paths_tab(LONG y)
{
    LONG px = SW - PANEL_W, i, x;
    ly_list = y + 4;
    ly_foot = SH - 26;
    if (path_exists()) {
        LONG ry = ly_list;
        BOOL on = tool == T_PEN;
        fill(px + 1, ry, PANEL_W - 1, ROW_H - 1, on ? COL_ACCENT : COL_PANEL);
        fill(px + 1, ry + ROW_H - 1, PANEL_W - 1, 1, COL_LINE);
        fill(px + 10, ry + 4, 44, 30, 0xFFFFFFFF);
        frame(px + 9, ry + 3, 46, 32, COL_LINE);
        path_draw_thumb(px + 11, ry + 5, 42, 28, 0xFF202020);
        text(px + 64, ry + 4, "Work Path", on ? 0xFFFFFFFF : COL_TEXT);
        text(px + 64, ry + 19, on ? "visible (Pen Tool)" : "click to show and edit",
             on ? 0xFFB8C8E0 : COL_DIM);
    } else {
        text(px + 12, ly_list + 8, "No path yet.", COL_DIM);
        text(px + 12, ly_list + 8 + FONT_H + 4, "Draw one with the Pen Tool (P).", COL_DIM);
    }
    /* Fussleiste */
    fill(px + 1, ly_foot, PANEL_W - 1, 26, COL_PANELHD);
    fill(px + 1, ly_foot, PANEL_W - 1, 1, COL_LINE);
    x = px + 8;
    for (i = 0; i < 4; i++) {
        LONG w = text_width(pa_lab[i]) + 14;
        pa_bx[i] = x;
        button(x, ly_foot + 4, w, 18, pa_lab[i], FALSE);
        x += w + 4;
    }
}

static void draw_history_tab(LONG y)
{
    LONG px = SW - PANEL_W, i, rows;
    hist_n = undo_history(hist_names, HIST_MAX, &hist_cur);
    ly_list = y + 4;
    ly_foot = SH;
    rows = (ly_foot - ly_list) / HIST_H;
    /* aktuellen Eintrag sichtbar halten */
    hist_first = hist_cur - rows + 2;
    if (hist_first > hist_n - rows) hist_first = hist_n - rows;
    if (hist_first < 0) hist_first = 0;
    for (i = 0; i < rows && hist_first + i < hist_n; i++) {
        LONG k = hist_first + i, ry = ly_list + i * HIST_H;
        if (k == hist_cur)
            fill(px + 1, ry, PANEL_W - 1, HIST_H, COL_ACCENT);
        fill(px + 26, ry, 1, HIST_H, COL_LINE);
        if (k == doc->hist_src)         /* Quelle des Protokollpinsels */
            icon(px + 5, ry + 3, T_HISTBRUSH, COL_TEXT);
        text(px + 34, ry + 2, hist_names[k], k > hist_cur ? 0xFF7A7A7A : COL_TEXT);
    }
}

/* ---- Farbgruppe: Color / Swatches ---- */
static ULONG hsv_rgb(float h, float s, float v);
static void rgb_hsv(ULONG c, float *h, float *s, float *v);
static void hex6(char *buf, ULONG c);
#define CG_H 136                /* Hoehe des Inhalts */
#define CF_W 170                /* Saettigung/Helligkeit-Feld */
#define CF_H 84
static LONG color_tab;          /* 0 = Color, 1 = Swatches */
static LONG ctab_x[3];
static LONG color_drag;         /* 1 = Feld, 2 = Farbton */
static float pc_h, pc_s, pc_v;
static ULONG pc_synced = 0xFFFFFFFF;    /* fg_color, zu der pc_* passen */

static void cg_geom(LONG *fx, LONG *fy, LONG *hx)
{
    *fx = SW - PANEL_W + 52;
    *fy = sw_top + 24 + 10;
    *hx = *fx + CF_W + 8;
}

/* Hex-Feld (62 breit), dann R/G/B (je 34 breit, Beschriftung davor) */
static LONG cg_field_x(LONG i)
{
    return SW - PANEL_W + (i ? 101 + (i - 1) * 52 : 22);
}

static LONG cg_field_w(LONG i)
{
    return i ? 34 : 62;
}

static void draw_color_group(BOOL do_blit)
{
    static const char *tabs[2] = { "Color", "Swatches" };
    LONG px = SW - PANEL_W, y0 = sw_top + 24, i;
    panel_tabs(sw_top, tabs, 2, color_tab);
    for (i = 0; i < 3; i++)
        ctab_x[i] = tab_x[i];
    fill(px + 1, y0, PANEL_W - 1, CG_H, COL_PANEL);
    if (color_tab == 0) {
        LONG fx, fy, hx, x, y, hr, hg, hb;
        ULONG hc;
        char buf[12];
        if (pc_synced != fg_color) {
            rgb_hsv(fg_color, &pc_h, &pc_s, &pc_v);
            pc_synced = fg_color;
        }
        cg_geom(&fx, &fy, &hx);
        /* Vorder-/Hintergrundfarbe */
        fill(px + 22, fy + 12, 20, 20, 0xFF000000 | bg_color);
        frame(px + 22, fy + 12, 20, 20, 0xFF9A9A9A);
        fill(px + 12, fy + 2, 20, 20, 0xFF000000 | fg_color);
        frame(px + 12, fy + 2, 20, 20, 0xFFE0E0E0);
        /* Feld */
        hc = hsv_rgb(pc_h, 1.0f, 1.0f);
        hr = (hc >> 16) & 0xFF; hg = (hc >> 8) & 0xFF; hb = hc & 0xFF;
        for (y = 0; y < CF_H; y++) {
            ULONG *row = bb + (fy + y) * SW + fx;
            LONG v = 255 - y * 255 / (CF_H - 1);
            for (x = 0; x < CF_W; x++) {
                LONG k = x * 255 / (CF_W - 1);
                LONG r = (255 * (255 - k) + hr * k) / 255, g = (255 * (255 - k) + hg * k) / 255,
                     b = (255 * (255 - k) + hb * k) / 255;
                row[x] = 0xFF000000 | ((r * v / 255) << 16) | ((g * v / 255) << 8) | (b * v / 255);
            }
        }
        frame(fx - 1, fy - 1, CF_W + 2, CF_H + 2, COL_LINE);
        {
            LONG mx = fx + (LONG)(pc_s * (CF_W - 1) + 0.5f), my = fy + (LONG)((1.0f - pc_v) * (CF_H - 1) + 0.5f);
            gfx_clip(fx, fy, fx + CF_W, fy + CF_H);
            frame(mx - 4, my - 4, 9, 9, pc_v > 0.6f && pc_s < 0.5f ? 0xFF000000 : 0xFFFFFFFF);
            gfx_noclip();
        }
        /* Farbton */
        for (y = 0; y < CF_H; y++)
            fill(hx, fy + y, 12, 1, 0xFF000000 | hsv_rgb((CF_H - 1 - y) * 360.0f / CF_H, 1.0f, 1.0f));
        frame(hx - 1, fy - 1, 14, CF_H + 2, COL_LINE);
        y = fy + CF_H - 1 - (LONG)(pc_h * CF_H / 360.0f);
        fill(hx - 3, y, 18, 1, COL_TEXT);
        /* Hex und RGB */
        y = fy + CF_H + 12;
        text(px + 10, y + 3, "#", COL_DIM);
        fill(cg_field_x(0), y, cg_field_w(0), 22, COL_FIELD);
        frame(cg_field_x(0), y, cg_field_w(0), 22, COL_LINE);
        hex6(buf, fg_color);
        text(cg_field_x(0) + 6, y + 3, buf, COL_TEXT);
        for (i = 1; i <= 3; i++) {
            static const char *lab[4] = { "", "R", "G", "B" };
            LONG fxx = cg_field_x(i);
            text(fxx - 10, y + 3, lab[i], COL_DIM);
            fill(fxx, y, cg_field_w(i), 22, COL_FIELD);
            frame(fxx, y, cg_field_w(i), 22, COL_LINE);
            sprintf(buf, "%ld", (long)((fg_color >> (24 - 8 * i)) & 0xFF));
            text(fxx + 6, y + 3, buf, COL_TEXT);
        }
    } else {
        for (i = 0; i < (LONG)NSWATCH; i++) {
            LONG sx = px + 10 + (i % SW_COLS) * SW_CELL, sy = y0 + 8 + (i / SW_COLS) * SW_CELL;
            fill(sx, sy, SW_CELL - 1, SW_CELL - 1, 0xFF000000 | swatches[i]);
        }
    }
    if (do_blit)
        blit(px, sw_top, PANEL_W, 24 + CG_H);
}

static void color_panel_sync(void)
{
    if (sw_top > 0 && color_tab == 0 && pc_synced != fg_color)
        draw_color_group(TRUE);
}

/* Ziehen im Farbfeld / Farbton aendert die Vordergrundfarbe */
static void color_drag_to(LONG x, LONG y)
{
    LONG fx, fy, hx, px, py;
    cg_geom(&fx, &fy, &hx);
    px = x - fx; py = y - fy;
    if (px < 0) px = 0;
    if (px > CF_W - 1) px = CF_W - 1;
    if (py < 0) py = 0;
    if (py > CF_H - 1) py = CF_H - 1;
    if (color_drag == 1) {
        pc_s = (float)px / (CF_W - 1);
        pc_v = 1.0f - (float)py / (CF_H - 1);
    } else
        pc_h = (CF_H - 1 - py) * 360.0f / CF_H;
    fg_color = hsv_rgb(pc_h, pc_s, pc_v);
    pc_synced = fg_color;
    draw_color_group(TRUE);
    ui_draw_toolbar();
}

static BOOL color_group_click(LONG x, LONG y)
{
    LONG fx, fy, hx, i, fyy;
    if (y < sw_top || y >= sw_top + 24 + CG_H)
        return FALSE;
    if (y < sw_top + 24) {
        for (i = 0; i < 2; i++)
            if (x >= ctab_x[i] && x < ctab_x[i + 1] && color_tab != i) {
                color_tab = i;
                draw_color_group(TRUE);
            }
        return TRUE;
    }
    if (color_tab == 1) {
        LONG px = SW - PANEL_W;
        if (x >= px + 10 && x < px + 10 + SW_COLS * SW_CELL && y >= sw_top + 32) {
            LONG idx = ((y - sw_top - 32) / SW_CELL) * SW_COLS + (x - px - 10) / SW_CELL;
            if (idx >= 0 && idx < (LONG)NSWATCH) {
                fg_color = swatches[idx];
                ui_draw_toolbar();
            }
        }
        return TRUE;
    }
    cg_geom(&fx, &fy, &hx);
    if (x >= fx && x < fx + CF_W && y >= fy && y < fy + CF_H)
        color_drag = 1;
    else if (x >= hx - 3 && x < hx + 15 && y >= fy && y < fy + CF_H)
        color_drag = 2;
    if (color_drag) {
        color_drag_to(x, y);
        return TRUE;
    }
    if (x >= SW - PANEL_W + 10 && x < SW - PANEL_W + 44 && y >= fy && y < fy + 34) {
        /* kleine Farbfelder: Klick oeffnet den Farbwaehler */
        if (x < SW - PANEL_W + 32 && y < fy + 22)
            ui_color_picker("Color Picker (Foreground Color)", &fg_color);
        else
            ui_color_picker("Color Picker (Background Color)", &bg_color);
        ui_draw_toolbar();
        return TRUE;
    }
    fyy = fy + CF_H + 12;
    if (y >= fyy && y < fyy + 22)
        for (i = 0; i <= 3; i++) {
            LONG fxx = cg_field_x(i), w = cg_field_w(i);
            if (x >= fxx && x < fxx + w) {
                char buf[12];
                LONG v = 0, n = 0;
                const char *b;
                if (i)
                    sprintf(buf, "%ld", (long)((fg_color >> (24 - 8 * i)) & 0xFF));
                else
                    hex6(buf, fg_color);
                if (!ui_edit_text(fxx, fyy, w, buf, i ? 4 : 7))
                    break;
                for (b = buf; *b == ' ' || *b == '#'; b++)
                    ;
                for (; *b; b++) {
                    LONG d = *b >= '0' && *b <= '9' ? *b - '0' :
                             !i && *b >= 'a' && *b <= 'f' ? *b - 'a' + 10 :
                             !i && *b >= 'A' && *b <= 'F' ? *b - 'A' + 10 : -1;
                    if (d < 0)
                        break;
                    v = v * (i ? 10 : 16) + d;
                    n++;
                }
                if (!i && n == 6)
                    fg_color = v;
                else if (i && n) {
                    LONG sh = 24 - 8 * i;
                    if (v > 255) v = 255;
                    fg_color = (fg_color & ~(0xFFUL << sh)) | ((ULONG)v << sh);
                }
                break;
            }
        }
    draw_color_group(TRUE);
    ui_draw_toolbar();
    return TRUE;
}

/* ---- Properties-Panel ---- */

enum { PR_DOC, PR_PIXEL, PR_TEXT, PR_ADJ, PR_GROUP };
#define PR_ROW 28
#define PR_MAXB 10
/* eigene Knopf-Aktionen (negativ, sonst Befehl) */
enum { PB_ALIGN_L = -10, PB_ALIGN_C, PB_ALIGN_R, PB_MASK_INVERT };

static BOOL pr_collapsed;
static LONG pr_top, pr_kind, pr_nsl, pr_nb;
static Slider pr_sl[6];
static LONG pr_val[6];
static LONG pr_bx[PR_MAXB], pr_by[PR_MAXB], pr_bw[PR_MAXB], pr_bcmd[PR_MAXB];
static UndoRec *pr_undo;

static void pr_changed(void)
{
    Layer *l = doc->layers[doc->active];
    LONG i;
    if (pr_kind == PR_ADJ && l->adj) {
        for (i = 0; i < pr_nsl; i++)
            l->adj->v[i] = pr_val[i];
        doc_mark_all_dirty();
    }
    /* Text: erst beim Loslassen (sonst ein Protokollschritt je Mausbewegung) */
}

/* Text-Werte uebernehmen */
static void pr_text_apply(void)
{
    Layer *l = doc->layers[doc->active];
    if (l->text)
        text_set_props(pr_val[0], l->text->align, pr_val[1], pr_val[2]);
}

static BOOL pr_is_slider(Slider *s)
{
    return s >= pr_sl && s < pr_sl + pr_nsl;
}

static LONG pr_kind_of(Layer *l)
{
    if (!l)
        return PR_DOC;
    if (l->kind == LK_ADJUST)
        return PR_ADJ;
    if (l->kind == LK_GROUP)
        return PR_GROUP;
    if (l->text)
        return PR_TEXT;
    return PR_PIXEL;
}

/* Umriss der Pixel einer Ebene (Kachel-Grenzen, dann am Rand genau) */
static BOOL layer_bounds(Layer *l, LONG *bx0, LONG *by0, LONG *bx1, LONG *by1)
{
    LONG tx, ty, x0 = 0x7FFF, y0 = 0x7FFF, x1 = -1, y1 = -1, x, y;
    for (ty = 0; ty < doc->th; ty++)
        for (tx = 0; tx < doc->tw; tx++) {
            Tile *t = l->tiles[ty * doc->tw + tx];
            if (!t || t->flag == TF_EMPTY)
                continue;
            if (tx < x0) x0 = tx;
            if (tx > x1) x1 = tx;
            if (ty < y0) y0 = ty;
            if (ty > y1) y1 = ty;
        }
    if (x1 < 0)
        return FALSE;
    *bx0 = doc->w; *by0 = doc->h; *bx1 = 0; *by1 = 0;
    /* nur Randkacheln pixelgenau absuchen */
    for (ty = y0; ty <= y1; ty++)
        for (tx = x0; tx <= x1; tx++) {
            Tile *t = l->tiles[ty * doc->tw + tx];
            if (!t || t->flag == TF_EMPTY || (tx != x0 && tx != x1 && ty != y0 && ty != y1))
                continue;
            for (y = 0; y < TILE_SIZE; y++)
                for (x = 0; x < TILE_SIZE; x++)
                    if (t->px[(y << TILE_SHIFT) | x] >> 24) {
                        LONG gx = (tx << TILE_SHIFT) + x, gy = (ty << TILE_SHIFT) + y;
                        if (gx < *bx0) *bx0 = gx;
                        if (gx + 1 > *bx1) *bx1 = gx + 1;
                        if (gy < *by0) *by0 = gy;
                        if (gy + 1 > *by1) *by1 = gy + 1;
                    }
        }
    if (*bx1 > doc->w) *bx1 = doc->w;
    if (*by1 > doc->h) *by1 = doc->h;
    return *bx1 > *bx0;
}

static LONG pr_button(LONG x, LONG y, LONG w, const char *label, LONG cmd, BOOL on, BOOL draw)
{
    if (pr_nb < PR_MAXB) {
        pr_bx[pr_nb] = x; pr_by[pr_nb] = y; pr_bw[pr_nb] = w; pr_bcmd[pr_nb] = cmd;
        pr_nb++;
    }
    if (draw)
        button(x, y, w, 22, label, on);
    return x + w + 6;
}

/* Inhalt aufbauen; draw = FALSE: nur Hoehe. Rueckgabe: Hoehe */
static LONG pr_layout(LONG y0, BOOL draw)
{
    LONG px = SW - PANEL_W, x = px + 10, y = y0 + 8, i;
    Layer *l = doc->layers[doc->active];
    char buf[64];
    pr_kind = pr_kind_of(l);
    pr_nb = 0;
    if (!draw && pr_undo)
        ;                       /* waehrend des Ziehens nichts neu belegen */
    if (pr_kind == PR_ADJ) {
        DlgSlider ds[6];
        LONG n = l->adj ? adjust_sliders(l->adj->cmd, ds) : 0;
        if (draw)
            text_bold(x, y, l->adj ? adjust_name(l->adj->cmd) : "Adjustment", COL_TEXT);
        y += 24;
        if (l->adj && (l->adj->cmd == CMD_ADJ_CURVES || l->adj->cmd == CMD_ADJ_LEVELS)) {
            pr_button(x, y, 120, l->adj->cmd == CMD_ADJ_CURVES ? "Edit Curves..." : "Edit Levels...",
                      CMD_ADJ_EDIT, FALSE, draw);
            y += PR_ROW;
        } else if (!n) {
            if (draw)
                text(x, y, "No settings", COL_DIM);
            y += PR_ROW;
        }
        if (!pr_undo) {
            pr_nsl = n;
            for (i = 0; i < n; i++) {
                memset(&pr_sl[i], 0, sizeof(Slider));
                pr_sl[i].label = ds[i].label;
                pr_val[i] = l->adj->v[i];
                pr_sl[i].val = &pr_val[i];
                pr_sl[i].min = ds[i].min;
                pr_sl[i].max = ds[i].max;
                pr_sl[i].unit = ds[i].unit;
                pr_sl[i].div = ds[i].div;
                pr_sl[i].changed = pr_changed;
                pr_sl[i].lw = 84;
                pr_sl[i].tw = 92;
            }
        }
        for (i = 0; i < pr_nsl; i++) {
            if (draw)
                slider_draw(&pr_sl[i], x, y);
            else {
                pr_sl[i].x = x; pr_sl[i].y = y; pr_sl[i].tx = x + pr_sl[i].lw + 6;
            }
            y += PR_ROW;
        }
        goto mask;
    }
    pr_nsl = 0;
    if (pr_kind == PR_TEXT) {
        static const char *lab[3] = { "Size:", "Tracking:", "Leading:" };
        static const LONG mn[3] = { 4, -200, 0 }, mx[3] = { 500, 1000, 1000 };
        static const char *un[3] = { "px", "", "px" };
        if (draw)
            text_bold(x, y, "Type Layer", COL_TEXT);
        y += 24;
        if (!pr_undo) {
            pr_val[0] = l->text->size;
            pr_val[1] = l->text->tracking;
            pr_val[2] = l->text->leading;
        }
        pr_nsl = 3;
        for (i = 0; i < 3; i++) {
            if (!pr_undo) {
                memset(&pr_sl[i], 0, sizeof(Slider));
                pr_sl[i].label = lab[i]; pr_sl[i].val = &pr_val[i];
                pr_sl[i].min = mn[i]; pr_sl[i].max = mx[i]; pr_sl[i].unit = un[i];
                pr_sl[i].changed = pr_changed; pr_sl[i].lw = 84; pr_sl[i].tw = 92;
            }
            if (draw)
                slider_draw(&pr_sl[i], x, y);
            else {
                pr_sl[i].x = x; pr_sl[i].y = y; pr_sl[i].tx = x + pr_sl[i].lw + 6;
            }
            y += PR_ROW;
        }
        x = pr_button(x, y, 70, "Left", PB_ALIGN_L, l->text->align == 0, draw);
        x = pr_button(x, y, 70, "Center", PB_ALIGN_C, l->text->align == 1, draw);
        pr_button(x, y, 70, "Right", PB_ALIGN_R, l->text->align == 2, draw);
        y += PR_ROW;
        x = px + 10;
    } else if (pr_kind == PR_GROUP) {
        LONG n = layer_block(doc->active) - 1;
        if (draw) {
            text_bold(x, y, "Group", COL_TEXT);
            sprintf(buf, "%ld layer%s inside", (long)n, n == 1 ? "" : "s");
            text(x + 70, y, buf, COL_DIM);
        }
        y += 24;
        x = pr_button(x, y, 110, "Ungroup", CMD_UNGROUP, FALSE, draw);
        pr_button(x, y, 110, "Merge Group", CMD_MERGE_GROUP, FALSE, draw);
        y += PR_ROW;
        x = px + 10;
    } else if (text_active() && !l->text) {
        /* neuer Text wird gerade getippt: die Ebene wird erst beim Uebernehmen zur Textebene */
        if (draw) {
            text_bold(x, y, "Type Layer", COL_TEXT);
            text(x, y + 22, "Typing - Commit or Ctrl+Return", COL_DIM);
        }
        y += 46;
    } else {
        LONG bx0, by0, bx1, by1;
        if (draw) {
            text_bold(x, y, "Pixel Layer", COL_TEXT);
            if (layer_bounds(l, &bx0, &by0, &bx1, &by1)) {
                sprintf(buf, "X: %ld  Y: %ld", (long)bx0, (long)by0);
                text(x, y + 22, buf, COL_DIM);
                sprintf(buf, "W: %ld  H: %ld px", (long)(bx1 - bx0), (long)(by1 - by0));
                text(x + 120, y + 22, buf, COL_DIM);
            } else
                text(x, y + 22, "Empty layer", COL_DIM);
        }
        y += 46;
        x = pr_button(x, y, 110, "Free Transform", CMD_FREE_TF, FALSE, draw);
        x = pr_button(x, y, 52, "Flip H", CMD_FLIP_H, FALSE, draw);
        pr_button(x, y, 52, "Flip V", CMD_FLIP_V, FALSE, draw);
        y += PR_ROW;
        x = px + 10;
        if (l->gen) {                   /* generative Ebene: Prompt, Varianten, neu erzeugen */
            GenInfo *g = l->gen;
            y += 4;
            if (draw) {
                fill(px + 8, y - 4, PANEL_W - 16, 1, COL_LINE);
                text_bold(x, y + 2, "Generative Layer", COL_TEXT);
                gfx_clip(x, y + 22, px + PANEL_W - 10, y + 42);
                text(x, y + 24, g->mode == GEN_REMOVE ? "(Remove)" : g->mode == GEN_EXTEND ? "(Extend)" :
                     g->prompt, COL_DIM);
                gfx_noclip();
            }
            y += 48;
            x = pr_button(x, y, 24, "<", CMD_GEN_PREV, FALSE, draw);
            if (draw) {
                if (g->nvar)
                    sprintf(buf, "%ld / %ld", (long)(g->cur + 1), (long)g->nvar);
                else
                    strcpy(buf, "-");
                text(x + (52 - text_width(buf)) / 2, y + 2, buf, COL_TEXT);
            }
            x = pr_button(x + 52, y, 24, ">", CMD_GEN_NEXT, FALSE, draw);
            pr_button(x + 4, y, 118, "Generate again", CMD_GEN_AGAIN, FALSE, draw);
            y += PR_ROW;
            x = px + 10;
        }
    }
mask:
    x = px + 10;
    if (l->mask) {
        y += 4;
        if (draw) {
            fill(px + 8, y - 4, PANEL_W - 16, 1, COL_LINE);
            text_bold(x, y + 2, "Layer Mask", COL_TEXT);
            if (!l->mask_on)
                text(x + 100, y + 2, "(disabled)", COL_DIM);
        }
        y += 26;
        x = pr_button(x, y, 56, "Invert", PB_MASK_INVERT, FALSE, draw);
        if (l->kind == LK_PIXEL)
            x = pr_button(x, y, 50, "Apply", CMD_MASK_APPLY, FALSE, draw);
        x = pr_button(x, y, 64, l->mask_on ? "Disable" : "Enable", CMD_MASK_TOGGLE, FALSE, draw);
        pr_button(x, y, 52, "Delete", CMD_MASK_DELETE, FALSE, draw);
        y += PR_ROW;
    }
    return y - y0;
}

/* nur das Dokument: Groesse und Knoepfe (ohne Ebene nicht noetig: es gibt
 * immer eine aktive Ebene, deshalb steht das Dokument nur im Kopf) */
static LONG draw_properties(LONG top)
{
    LONG px = SW - PANEL_W, h;
    char buf[48];
    pr_top = top;
    fill(px + 1, top, PANEL_W - 1, 24, COL_PANELHD);
    fill(px + 4, top + 3, text_width("Properties") + 20, 21, COL_PANEL);
    text_bold(px + 14, top + 4, "Properties", COL_TEXT);
    sprintf(buf, "%ld × %ld px", (long)doc->w, (long)doc->h);
    text(SW - 30 - text_width(buf), top + 4, buf, COL_DIM);
    /* Auf-/Zuklappen */
    if (pr_collapsed)
        fill(SW - 18, top + 10, 7, 2, COL_DIM), fill(SW - 15, top + 7, 1, 8, COL_DIM);
    else
        fill(SW - 18, top + 10, 7, 2, COL_DIM);
    if (pr_collapsed)
        return 24;
    h = pr_layout(top + 24, FALSE);
    fill(px + 1, top + 24, PANEL_W - 1, h, COL_PANEL);
    pr_layout(top + 24, TRUE);
    return 24 + h;
}

/* Klick ins Properties-Panel */
static BOOL props_click(LONG x, LONG y)
{
    LONG i, h;
    Layer *l = doc->layers[doc->active];
    if (y < pr_top)
        return FALSE;
    if (y < pr_top + 24) {
        pr_collapsed = !pr_collapsed;
        ui_draw_panels();
        return TRUE;
    }
    if (pr_collapsed)
        return FALSE;
    h = pr_layout(pr_top + 24, FALSE);
    if (y >= pr_top + 24 + h)
        return FALSE;
    for (i = 0; i < pr_nsl; i++) {
        if (value_hit(&pr_sl[i], x, y)) {
            UndoRec *u = pr_kind == PR_ADJ ? undo_begin_struct(adjust_name(l->adj->cmd)) : NULL;
            BOOL ch = slider_edit_value(&pr_sl[i]);
            if (u)
                undo_commit(u);
            if (ch && pr_kind == PR_TEXT)
                pr_text_apply();
            ui_draw_panels();
            ui_draw_tab();
            return TRUE;
        }
        if (slider_hit(&pr_sl[i], x, y)) {
            if (pr_kind == PR_ADJ) {
                pr_undo = undo_begin_struct(adjust_name(l->adj->cmd));
                doc_cache_below(doc->active);
            } else
                pr_undo = (UndoRec *)1;     /* Text: nur Ziehen markieren */
            drag_slider = &pr_sl[i];
            slider_set(drag_slider, x);
            ui_draw_panels();
            return TRUE;
        }
    }
    for (i = 0; i < pr_nb; i++)
        if (x >= pr_bx[i] && x < pr_bx[i] + pr_bw[i] && y >= pr_by[i] && y < pr_by[i] + 22) {
            LONG c = pr_bcmd[i];
            if (c >= PB_ALIGN_L && c <= PB_ALIGN_R) {
                if (l->text)
                    text_set_props(l->text->size, c - PB_ALIGN_L, l->text->tracking, l->text->leading);
            } else if (c == PB_MASK_INVERT) {
                doc->edit_mask = TRUE;
                do_command(CMD_ADJ_INVERT);
            } else
                do_command(c);
            ui_draw_all();
            return TRUE;
        }
    return TRUE;
}

/* Ziehen eines Properties-Reglers beendet */
static void props_release(void)
{
    if (pr_kind == PR_ADJ && pr_undo) {
        doc_cache_drop();
        undo_commit(pr_undo);
    }
    pr_undo = NULL;
    if (pr_kind == PR_TEXT)
        pr_text_apply();
    ui_draw_panels();
    ui_draw_tab();
}

void ui_draw_panels(void)
{
    static const char *tabs[3] = { "Layers", "History", "Paths" };
    LONG px = SW - PANEL_W, top = MENU_H + OPT_H, y;

    fill(px, top, PANEL_W, SH - top, COL_PANELHD);
    fill(px, top, 1, SH - top, COL_LINE);

    /* Farbe / Farbfelder */
    sw_top = top + 4;
    draw_color_group(FALSE);

    /* Eigenschaften */
    ly_top = sw_top + 24 + CG_H + 6;
    ly_top += draw_properties(ly_top) + 6;

    /* Ebenen / Protokoll */
    panel_tabs(ly_top, tabs, 3, panel_tab);
    y = ly_top + 24;
    fill(px + 1, y, PANEL_W - 1, SH - y, COL_PANEL);
    if (panel_tab == 0)
        draw_layers_tab(y);
    else if (panel_tab == 1)
        draw_history_tab(y);
    else
        draw_paths_tab(y);

    blit(px, top, PANEL_W, SH - top);
}

/* Fuellmethoden-Liste mit Gruppen wie in Photoshop */
static void choose_blend_mode(void)
{
    const char *items[BM_COUNT + 8];
    LONG map[BM_COUNT + 8], n = 0, m, r, cur = 0;
    Layer *l = doc->layers[doc->active];
    BOOL grp = l->kind == LK_GROUP;
    if (grp) {
        items[n] = "Pass Through";
        map[n++] = -2;
    }
    for (m = 0; m < BM_COUNT; m++) {
        if (m == BM_DARKEN || m == BM_LIGHTEN || m == BM_OVERLAY || m == BM_DIFFERENCE || m == BM_HUE) {
            items[n] = NULL;
            map[n++] = -1;
        }
        if (m == l->mode && !(grp && l->pass))
            cur = n;
        items[n] = blend_names[m];
        map[n++] = m;
    }
    r = ui_popup(SW - PANEL_W + 10, ly_modey + 22, items, n, cur);
    if (r >= 0 && (map[r] >= 0 || map[r] == -2)) {
        UndoRec *u = undo_begin_struct("Blending Change");
        if (map[r] == -2)
            l->pass = 1;
        else {
            l->mode = (UBYTE)map[r];
            l->pass = 0;
        }
        doc_mark_all_dirty();
        undo_commit(u);
    }
    ui_draw_panels();
    ui_draw_tab();
}

void ui_rename_layer(void)
{
    LONG px = SW - PANEL_W, row, ry, ind;
    Layer *l = doc->layers[doc->active];
    char buf[32];
    build_disp();
    for (row = 0; row < ndisp && disp[row] != doc->active; row++)
        ;
    if (row == ndisp)
        return;                 /* in zugeklappter Gruppe */
    ind = layer_indent(l) + (l->kind == LK_GROUP ? 4 : 0);
    if (panel_tab != 0) {
        panel_tab = 0;
        ui_draw_panels();
    }
    ry = ly_list + row * ROW_H;
    if (ry + ROW_H > ly_foot)
        return;
    strcpy(buf, l->name);
    if (ui_edit_text(px + 68 + ind, ry + 2, PANEL_W - 76 - ind, buf, sizeof(buf)) && buf[0] &&
        strcmp(buf, l->name)) {
        UndoRec *u = undo_begin_struct("Rename Layer");
        strcpy(l->name, buf);
        undo_commit(u);
    }
    ui_draw_panels();
    ui_draw_tab();
}

/* ---- Dokument-Tab ---- */
/* Tab-Leiste: ein Tab je offenem Bild. Der aktive zeigt Zoom und Ebene,
 * die anderen nur den Namen (wie Photoshop). */
static LONG tabx[MAX_DOCS + 1];

void ui_draw_tab(void)
{
    char buf[128], z[16];
    LONG y = MENU_H + OPT_H, x = cax, i;
    fmt_zoom(z, zoom);
    fill(cax, y, caw, TAB_H, COL_PANELHD);
    gfx_clip(cax, y, cax + caw, y + TAB_H);
    for (i = 0; i < ndocs; i++) {
        Doc *d = docs[i];
        LONG w;
        if (d == doc) {
            char cd[24];
            if (d->colors && !doc_target_is_mask())
                color_depth_name(d->colors, cd);
            else
                strcpy(cd, doc_target_is_mask() ? "Layer Mask/8" : "RGB/8");
            sprintf(buf, "%s%s @ %s (%s, %s)", doc_title_of(d), d->modified ? "*" : "",
                    z, d->layers[d->active]->name, cd);
        }
        else
            sprintf(buf, "%s%s", doc_title_of(d), d->modified ? "*" : "");
        w = text_width(buf) + 40;
        tabx[i] = x;
        fill(x, y, w, TAB_H - 1, d == doc ? COL_PANEL : 0xFF333333);
        fill(x + w - 1, y, 1, TAB_H - 1, COL_LINE);
        text(x + 10, y + 3, buf, d == doc ? COL_TEXT : COL_DIM);
        text(x + w - 20, y + 3, "×", COL_DIM);
        x += w;
    }
    tabx[ndocs] = x;
    gfx_noclip();
    fill(cax, y + TAB_H - 1, caw, 1, COL_LINE);
    blit(cax, y, caw, TAB_H);
}

/* ---- Statusleiste ---- */
void ui_draw_status(void)
{
    static char last[200];
    char buf[200], z[16];
    LONG y = SH - STATUS_H, x;
    ULONG us = last_flush_us;
    fmt_zoom(z, zoom);
    sprintf(buf, "|Doc: %ld × %ld px|Redraw: %ld.%ld ms%s%s",
            (long)doc->w, (long)doc->h, (long)(us / 1000), (long)((us / 100) % 10),
            status_msg[0] ? "|" : "", status_msg);
    if (!strcmp(buf, last))
        return;
    strcpy(last, buf);
    fill(cax, y, caw, STATUS_H, COL_BAR);
    fill(cax, y, caw, 1, COL_LINE);
    /* Zoom: Eingabefeld (Klick: Prozent eintippen) */
    fill(cax + 4, y + 3, 64, STATUS_H - 5, COL_FIELD);
    frame(cax + 4, y + 3, 64, STATUS_H - 5, COL_LINE);
    text(cax + 10, y + 3, z, COL_TEXT);
    {
        char *p = buf + 1, *q;
        x = cax + 78;
        while (p) {
            q = strchr(p, '|');
            if (q)
                *q = 0;
            x = text(x, y + 3, p, COL_DIM) + 10;
            fill(x, y + 5, 1, STATUS_H - 10, COL_LINE);
            x += 10;
            p = q ? q + 1 : NULL;
        }
    }
    blit(cax, y, caw, STATUS_H);
}

void ui_draw_all(void)
{
    draw_menu();
    blit(0, 0, SW, MENU_H);
    ui_draw_options();
    ui_draw_toolbar();
    ui_draw_panels();
    ui_draw_tab();
    ui_draw_status();
    rulers_draw();
}

void ui_set_tool(LONG t)
{
    grp_cur[tool_group(t)] = t;
    tt_hide();
    tt_id = -1;                 /* Tooltip erst nach neuer Mausbewegung */
    if (t == tool)
        return;
    /* Auswahlmodus gilt je Werkzeug (wie Photoshop): die Schnellauswahl schaltet
     * nach dem ersten Zug auf Add, das darf nicht fuer Lasso & Co. gelten */
    tool_sel_mode[tool] = sel_mode;
    tool = t;
    sel_mode = tool_sel_mode[t];
    ui_draw_toolbar();
    ui_draw_options();
    main_tool_changed();
}


/* ---- Aufklappmenues ---- */

/* Ist der Befehl gerade anwendbar? (sonst grau) */
static BOOL cmd_enabled(LONG c)
{
    Layer *l = doc->layers[doc->active];
    switch (c) {
    case CMD_UNDO:
        return doc->ucur != NULL || doc->size_prev != NULL;
    case CMD_TOGGLE_LAST:
        return doc->ucur != NULL;
    case CMD_REDO:
        return (doc->ucur ? doc->ucur->next : doc->uhead) != NULL || doc->size_next != NULL;
    case CMD_SEL_NONE: case CMD_SEL_INVERT: case CMD_FEATHER: case CMD_GROW: case CMD_SHRINK:
    case CMD_BORDER: case CMD_SMOOTH:
    case CMD_CROP: case CMD_FILL_FG: case CMD_FILL_BG: case CMD_ERASE: case CMD_MASK_REVEAL_SEL:
    case CMD_LAYER_VIA_CUT: case CMD_GEN_FILL: case CMD_GEN_REMOVE:
        return doc->hassel;
    case CMD_RESELECT:
        return doc->lastsel != NULL;
    case CMD_GUIDES_CLEAR:
        return doc->nguides > 0;
    case CMD_REVERT:
        return doc->modified && doc->path[0];
    case CMD_MERGE_DOWN:
        return doc->active > 0;
    case CMD_LAYER_UP: case CMD_LAYER_TOP:
        return doc->active < doc->nlayers - 1;
    case CMD_LAYER_DOWN: case CMD_LAYER_BOTTOM:
        return doc->active > 0;
    case CMD_LOREM:
        return text_active();
    case CMD_EDIT_STROKE: case CMD_SEL_GROW_SIM: case CMD_SEL_SIMILAR:
        return doc->hassel;
    case CMD_LAYER_DEL: case CMD_MERGE_VISIBLE: case CMD_FLATTEN:
        return doc->nlayers > 1;
    case CMD_MASK_REVEAL_ALL: case CMD_MASK_HIDE_ALL:
        return !l->mask;
    case CMD_CLIP_TOGGLE:
        return l->kind != LK_GROUP && doc->active > 0;
    case CMD_MASK_DELETE: case CMD_MASK_TOGGLE:
        return l->mask != NULL;
    case CMD_MASK_APPLY:
        return l->mask != NULL && l->kind == LK_PIXEL;
    case CMD_RASTERIZE:
        return l->text != NULL;
    case CMD_UNGROUP: case CMD_MERGE_GROUP:
        return l->kind == LK_GROUP;
    case CMD_ADJ_EDIT:
        return l->kind == LK_ADJUST;
    case CMD_LAYER_STYLE: case CMD_LAYER_VIA_COPY:
        return l->kind == LK_PIXEL;
    case CMD_LAYER_STYLE_CLEAR:
        return l->fx != NULL;
    case CMD_FLT_LAST:
        return filter_last_name() != NULL;
    case CMD_NEXT_DOC: case CMD_CLOSE_ALL:
        return ndocs > 1;
    }
    return c > CMD_NONE;
}

static BOOL item_enabled(const MenuItem *it)
{
    return it->sub ? TRUE : cmd_enabled(it->cmd);
}

static LONG item_h(const MenuItem *it)
{
    return it->label ? 22 : 9;
}

/* aufgeklappte Liste: [0] Menue, [1] Untermenue */
typedef struct { const MenuItem *items; LONG x, y, w, h, hover; } Drop;
static Drop dd[2];
static BOOL sub_open;
static ULONG *sub_save;         /* Hintergrund unter dem Untermenue */

static void drop_geometry(Drop *d, const MenuItem *items, LONG x, LONG y)
{
    const MenuItem *it;
    LONG w = 180, h = 8;
    for (it = items; it->cmd != -1; it++) {
        if (it->label) {
            LONG iw = text_width(it->label) + text_width(it->key) + 72;
            if (iw > w)
                w = iw;
        }
        h += item_h(it);
    }
    d->items = items;
    d->w = w;
    d->h = h;
    d->hover = -1;
    if (x + w > SW)
        x = SW - w;
    if (y + h > SH)
        y = SH - h;
    d->x = x;
    d->y = y;
}

static void drop_draw(Drop *d)
{
    const MenuItem *it;
    LONG y = d->y + 4, i = 0;
    fill(d->x, d->y, d->w, d->h, 0xFF424242);
    frame(d->x, d->y, d->w, d->h, COL_LINE);
    for (it = d->items; it->cmd != -1; it++, i++) {
        if (!it->label) {
            fill(d->x + 6, y + 4, d->w - 12, 1, 0xFF2C2C2C);
        } else {
            BOOL en = item_enabled(it);
            ULONG c = en ? COL_TEXT : 0xFF767676;
            if (i == d->hover && en)
                fill(d->x + 1, y, d->w - 2, 22, COL_ACCENT);
            text(d->x + 22, y + 2, it->label, c);
            if ((it->cmd == CMD_TASKBAR && ai_taskbar) || (it->cmd == CMD_UPD_AUTO && upd_auto) ||
                (it->cmd == CMD_RULERS && rulers_on) || (it->cmd == CMD_GUIDES_SHOW && guides_show) ||
                (it->cmd == CMD_GUIDES_SNAP && guides_snap) || (it->cmd == CMD_GUIDES_LOCK && guides_lock)) {   /* Haekchen */
                line(d->x + 7, y + 11, d->x + 10, y + 14, c);
                line(d->x + 10, y + 14, d->x + 15, y + 6, c);
            }
            if (it->sub) {          /* Pfeil nach rechts */
                LONG ax = d->x + d->w - 16, ay = y + 7, k;
                for (k = 0; k < 4; k++)
                    fill(ax + k, ay + k, 1, 9 - 2 * k, c);
            } else if (it->key[0])
                text(d->x + d->w - 14 - text_width(it->key), y + 2, it->key, en ? COL_DIM : 0xFF767676);
        }
        y += item_h(it);
    }
    blit(d->x, d->y, d->w, d->h);
}

static LONG drop_item_y(const Drop *d, LONG idx)
{
    LONG y = d->y + 4, i;
    for (i = 0; i < idx; i++)
        y += item_h(&d->items[i]);
    return y;
}

static LONG drop_item_at(const Drop *d, LONG x, LONG y)
{
    const MenuItem *it;
    LONG yy = d->y + 4, i = 0;
    if (x < d->x || x >= d->x + d->w)
        return -1;
    for (it = d->items; it->cmd != -1; it++, i++) {
        LONG h = item_h(it);
        if (y >= yy && y < yy + h)
            return it->label ? i : -1;
        yy += h;
    }
    return -1;
}

static void close_sub(void)
{
    LONG y;
    Drop *d = &dd[1];
    if (!sub_open)
        return;
    sub_open = FALSE;
    if (sub_save) {
        for (y = 0; y < d->h; y++)
            CopyMem(sub_save + y * d->w, bb + (d->y + y) * SW + d->x, d->w * 4);
        FreeVec(sub_save);
        sub_save = NULL;
        blit(d->x, d->y, d->w, d->h);
    } else {
        ui_draw_all();
        view_inval_all();
        view_flush();
        drop_draw(&dd[0]);
    }
}

static void open_sub(LONG idx)
{
    Drop *d = &dd[1];
    LONG y;
    drop_geometry(d, dd[0].items[idx].sub, dd[0].x + dd[0].w - 4, drop_item_y(&dd[0], idx) - 4);
    if (d->x < dd[0].x + dd[0].w - 4)      /* rechts kein Platz: links daneben */
        d->x = dd[0].x - d->w + 4;
    if ((sub_save = AllocVec(d->w * d->h * 4, MEMF_ANY)))
        for (y = 0; y < d->h; y++)
            CopyMem(bb + (d->y + y) * SW + d->x, sub_save + y * d->w, d->w * 4);
    sub_open = TRUE;
    drop_draw(d);
}

static LONG title_at(LONG x)
{
    LONG i;
    for (i = 0; i < NMENUS; i++)
        if (x >= title_x[i] && x < title_x[i + 1])
            return i;
    return -1;
}

static void open_menu(LONG m)
{
    if (menu_open >= 0) {
        /* altes Menue wegraeumen */
        if (sub_save) {
            FreeVec(sub_save);
            sub_save = NULL;
        }
        sub_open = FALSE;
        menu_open = -1;
        ui_draw_all();
        view_inval_all();
        view_flush();
    }
    view_set_cursor(-1, -1, 0);     /* Pinselkreis nicht ueber dem Menue */
    view_flush();
    menu_open = m;
    draw_menu();
    blit(0, 0, SW, MENU_H);
    drop_geometry(&dd[0], menus[m], title_x[m], MENU_H - 1);
    drop_draw(&dd[0]);
}

void ui_close_menu(void)
{
    if (menu_open < 0)
        return;
    if (sub_save) {
        FreeVec(sub_save);
        sub_save = NULL;
    }
    sub_open = FALSE;
    menu_open = -1;
    ui_draw_all();
    view_inval_all();
    view_flush();
}

/* Maus ueber dem offenen Menue */
static void menu_hover_at(LONG x, LONG y)
{
    LONG it;
    if (sub_open) {
        it = drop_item_at(&dd[1], x, y);
        if (it >= 0 || (x >= dd[1].x && x < dd[1].x + dd[1].w && y >= dd[1].y && y < dd[1].y + dd[1].h)) {
            if (it != dd[1].hover) {
                dd[1].hover = it;
                drop_draw(&dd[1]);
            }
            return;
        }
    }
    it = drop_item_at(&dd[0], x, y);
    if (it < 0 && sub_open)
        return;                 /* zwischen den Listen: Untermenue offen lassen */
    if (it != dd[0].hover) {
        close_sub();
        dd[0].hover = it;
        drop_draw(&dd[0]);
        if (it >= 0 && dd[0].items[it].sub)
            open_sub(it);
    }
}

BOOL ui_menu_open(void)
{
    return menu_open >= 0;
}

void status_message(const char *msg)
{
    strncpy(status_msg, msg, sizeof(status_msg) - 1);
    ui_draw_status();
}

static void choose_font(void)
{
    static const char *items[256];
    LONG n = text_font_count(), i, r;
    if (!n) {
        status_message("No TrueType fonts found in FONTS:truetype");
        return;
    }
    if (n > 256)
        n = 256;
    for (i = 0; i < n; i++)
        items[i] = text_font_name(i);
    r = ui_popup(font_bx, MENU_H + OPT_H - 2, items, n, text_font);
    if (r >= 0 && r != text_font) {
        text_font = r;
        text_refresh();
    }
    ui_draw_options();
}

/* ---- Maus ---- */

BOOL ui_dragging(void)
{
    return drag_slider != NULL || ldrag_from >= 0 || color_drag || tb_hold >= 0;
}

BOOL ui_mouse_down(LONG x, LONG y)
{
    LONG i, ty;

    tt_hide();

    /* Zoom-Feld in der Statusleiste */
    if (menu_open < 0 && y >= SH - STATUS_H && x >= cax + 4 && x < cax + 68) {
        char buf[16];
        BOOL ok;
        LONG v;
        sprintf(buf, "%ld", (long)floorf(zoom * 100.0f + 0.5f));
        if (ui_edit_text(cax + 4, SH - STATUS_H + 2, 64, buf, sizeof(buf))) {
            v = parse_value(buf, 1, &ok);
            if (ok) {
                if (v < 6) v = 6;
                if (v > 3200) v = 3200;
                view_set_zoom(v / 100.0f, cvx + cvw / 2, cvy + cvh / 2);
            }
        }
        ui_draw_status();
        return TRUE;
    }

    if (menu_open >= 0) {
        LONG m = menu_open, it;
        const MenuItem *sel = NULL;
        if (y < MENU_H) {
            LONG t = title_at(x);
            if (t < 0 || t == m)
                ui_close_menu();
            else
                open_menu(t);
            return TRUE;
        }
        if (sub_open && (it = drop_item_at(&dd[1], x, y)) >= 0)
            sel = &dd[1].items[it];
        else if ((it = drop_item_at(&dd[0], x, y)) >= 0) {
            sel = &dd[0].items[it];
            if (sel->sub)
                return TRUE;    /* Untermenue bleibt offen */
        }
        if (sel && !item_enabled(sel))
            return TRUE;        /* grauer Eintrag: Menue bleibt offen */
        ui_close_menu();
        if (sel && sel->cmd > CMD_NONE)
            do_command(sel->cmd);
        return TRUE;
    }
    if (y < MENU_H) {
        LONG t = title_at(x);
        if (t >= 0)
            open_menu(t);
        return TRUE;
    }

    if (y < MENU_H + OPT_H) {
        for (i = 0; i < n_opt_sliders; i++)
            if (value_hit(opt_sliders[i], x, y)) {
                slider_edit_value(opt_sliders[i]);
                ui_draw_options();
                return TRUE;
            }
        if ((tool == T_BLUR || tool == T_DODGE) && x >= gb_x[0] && !(x >= brush_bx && x < brush_bx + 70)) {
            for (i = 0; i < (tool == T_BLUR ? 3 : 2); i++)
                if (x >= gb_x[i] && x < gb_x[i] + gb_w[i]) {
                    if (tool == T_BLUR)
                        retouch_mode = i;
                    else
                        toning_mode = i;
                    ui_draw_options();
                    return TRUE;
                }
        }
        if ((tool == T_BRUSH || tool == T_ERASER || tool == T_CLONE || tool == T_BLUR || tool == T_DODGE ||
             tool == T_HISTBRUSH || tool == T_HEAL || tool == T_QUICKSEL) && !transform_active() &&
            x >= brush_bx && x < brush_bx + 70) {
            brush_popup();
            return TRUE;
        }
        if (tool == T_CROP) {
            if (x >= tf_bx && x < tf_bx + 96)
                do_command(CMD_CROP_CANCEL);
            else if (x >= tf_bx + 104 && x < tf_bx + 200)
                do_command(CMD_CROP_COMMIT);
            return TRUE;
        }
        if (liquify_active()) {
            LONG k;
            for (k = 0; k < 5; k++)
                if (x >= lq_bx[k] && x < lq_bx[k] + lq_bw[k]) {
                    lq_tool = k;
                    ui_draw_options();
                    return TRUE;
                }
            if (x >= tf_bx && x < tf_bx + 96)
                do_command(CMD_LQ_CANCEL);
            else if (x >= tf_bx + 104 && x < tf_bx + 200)
                do_command(CMD_LQ_APPLY);
            else
                for (k = 0; k < n_opt_sliders; k++)
                    if (slider_hit(opt_sliders[k], x, y)) {
                        drag_slider = opt_sliders[k];
                        slider_set(drag_slider, x);
                        ui_draw_options();
                    } else if (value_hit(opt_sliders[k], x, y)) {
                        slider_edit_value(opt_sliders[k]);
                        ui_draw_options();
                    }
            return TRUE;
        }
        if (transform_active()) {
            LONG k;
            for (k = 1; k < 4; k++)     /* Free: zurueck geht nicht (Form bleibt verformt) */
                if (x >= tfm_bx[k] && x < tfm_bx[k] + tfm_bw[k])
                    do_command(CMD_TF_FREE + k);
            if (x >= tf_bx && x < tf_bx + 96)
                do_command(CMD_TF_CANCEL);
            else if (x >= tf_bx + 104 && x < tf_bx + 200)
                do_command(CMD_TF_APPLY);
            return TRUE;
        }
        for (i = 0; i < n_opt_sliders; i++)
            if (slider_hit(opt_sliders[i], x, y)) {
                drag_slider = opt_sliders[i];
                slider_set(drag_slider, x);
                ui_draw_options();
                return TRUE;
            }
        if (tool >= T_RECT && tool <= T_WAND) {
            for (i = 0; i < 4; i++)
                if (x >= mode_bx[i] && x < mode_bx[i] + mode_bw[i]) {
                    sel_mode = i;
                    ui_draw_options();
                }
        }
        if (tool == T_MOVE) {
            if (x >= chk_x[0] && x < chk_x[0] + chk_w[0]) {
                move_autosel = !move_autosel;
                ui_prefs_save();
            } else if (x >= as_bx && x < as_bx + 80) {
                static const char *it[2] = { "Layer", "Group" };
                LONG r = ui_popup(as_bx, MENU_H + OPT_H - 2, it, 2, move_autogroup);
                if (r >= 0) {
                    move_autogroup = r;
                    ui_prefs_save();
                }
            }
            ui_draw_options();
            return TRUE;
        }
        if (tool == T_PEN) {
            LONG k;
            for (k = 0; k < 4; k++)
                if (x >= pen_bx[k] && x < pen_bx[k] + pen_bw[k]) {
                    if (path_exists())
                        do_command(CMD_PATH_SEL + k);
                    else
                        status_message("Draw a path first");
                }
            return TRUE;
        }
        if (tool == T_TEXT) {
            if (x >= font_bx && x < font_bx + 210)
                choose_font();
            else if (x >= al_bx && x < al_bx + 3 * 28)
                do_command(CMD_ALIGN_LEFT + (x - al_bx) / 28);
            else if (x >= aa_bx + 20 && x < aa_bx + 100) {
                LONG r = ui_popup(aa_bx + 20, MENU_H + OPT_H - 2, aa_names, 3, text_aa);
                if (r >= 0)
                    do_command(CMD_AA_SMOOTH + r);
            }
            else if (text_active() && x >= tx_bx && x < tx_bx + 96)
                do_command(CMD_TEXT_CANCEL);
            else if (text_active() && x >= tx_bx + 104 && x < tx_bx + 200)
                do_command(CMD_TEXT_COMMIT);
            return TRUE;
        }
        if (tool == T_WAND) {
            if (x >= chk_x[0] && x < chk_x[0] + chk_w[0])
                wand_contig = !wand_contig;
            else if (x >= chk_x[1] && x < chk_x[1] + chk_w[1])
                wand_aa = !wand_aa;
            ui_draw_options();
        }
        if (tool == T_CLONE && x >= chk_x[0] && x < chk_x[0] + chk_w[0]) {
            clone_aligned = !clone_aligned;
            ui_draw_options();
        }
        if (tool == T_HEAL && x >= chk_x[0] && x < chk_x[0] + chk_w[0]) {
            heal_ai = !heal_ai;
            ui_draw_options();
        }
        if (tool == T_BUCKET) {
            if (x >= chk_x[0] && x < chk_x[0] + chk_w[0])
                bucket_contig = !bucket_contig;
            else if (x >= chk_x[1] && x < chk_x[1] + chk_w[1])
                bucket_aa = !bucket_aa;
            ui_draw_options();
        }
        if (tool == T_SHAPE) {
            for (i = 0; i < 3; i++)
                if (x >= gb_x[i] && x < gb_x[i] + gb_w[i])
                    shape_type = i;
            if (x >= sw_fill_x && x < sw_fill_x + 22) {
                /* Fuellung: Farbwaehler; Abbrechen schaltet die Fuellung aus */
                if (shape_type == SHAPE_LINE)
                    ui_color_picker("Color Picker (Line Color)", &shape_fill);
                else
                    shape_fill_on = ui_color_picker("Color Picker (Fill Color)", &shape_fill);
            } else if (x >= sw_stroke_x && x < sw_stroke_x + 22) {
                if (ui_color_picker("Color Picker (Stroke Color)", &shape_stroke) && !shape_stroke_w)
                    shape_stroke_w = 3;
            }
            ui_draw_options();
            return TRUE;
        }
        if (tool == T_GRADIENT) {
            for (i = 0; i < 4; i++)
                if (x >= gb_x[i] && x < gb_x[i] + gb_w[i]) {
                    if (i < 2)
                        grad_type = i;
                    else
                        grad_mode = i - 2;
                }
            if (x >= chk_x[0] && x < chk_x[0] + chk_w[0])
                grad_reverse = !grad_reverse;
            ui_draw_options();
        }
        if (tool == T_ZOOM || tool == T_HAND) {
            if (x >= zoom_b1x && x < zoom_b1x + 70)
                view_set_zoom(1.0f, cvx + cvw / 2, cvy + cvh / 2);
            else if (x >= zoom_b2x && x < zoom_b2x + 100)
                view_fit();
        }
        return TRUE;
    }

    if (x < TOOL_W) {
        for (i = 0; i < nslots; i++)
            if (y >= slot_y[i] && y < slot_y[i] + 28) {
                if (slot_cnt[i] > 1) {
                    tb_hold = i;        /* Halten oeffnet die Varianten */
                    tb_hold_t0 = now_us();
                } else
                    ui_set_tool(slot_tool[i]);
                return TRUE;
            }
        if (tb_more_y >= 0 && y >= tb_more_y && y < tb_more_y + 28) {
            const char *items[T_COUNT];
            LONG map[T_COUNT], n = 0, r;
            for (i = 0; i < T_COUNT; i++)
                if (!tool_visible[i]) {
                    items[n] = tool_names[i];
                    map[n++] = i;
                }
            r = ui_popup(TOOL_W, tb_more_y, items, n, -1);
            if (r >= 0)
                ui_set_tool(map[r]);
            ui_draw_toolbar();
            return TRUE;
        }
        ty = colors_y();
        /* Vordergrund vorne links, Hintergrund dahinter, Tauschpfeil oben rechts */
        if (x >= 26 && x < 36 && y >= ty - 2 && y < ty + 8) {
            ULONG t = fg_color;
            fg_color = bg_color;
            bg_color = t;
        } else if (x >= 4 && x < 10 && y >= ty + 20 && y < ty + 30) {
            fg_color = 0x000000;        /* Standardfarben */
            bg_color = 0xFFFFFF;
        } else if (x >= 6 && x < 24 && y >= ty && y < ty + 18)
            ui_color_picker("Color Picker (Foreground Color)", &fg_color);
        else if (x >= 15 && x < 33 && y >= ty + 10 && y < ty + 28)
            ui_color_picker("Color Picker (Background Color)", &bg_color);
        ui_draw_toolbar();
        ui_draw_options();
        return TRUE;
    }

    if (x >= SW - PANEL_W) {
        LONG px = SW - PANEL_W;
        /* Farbe / Farbfelder */
        if (color_group_click(x, y))
            return TRUE;
        if (y < ly_top && props_click(x, y))
            return TRUE;
        /* Reiter */
        if (y >= ly_top && y < ly_top + 24) {
            for (i = 0; i < 3; i++)
                if (x >= tab_x[i] && x < tab_x[i + 1] && panel_tab != i) {
                    panel_tab = i;
                    ui_draw_panels();
                }
            return TRUE;
        }
        if (panel_tab == 2) {
            /* Pfade: Zeile zeigt den Pfad (Pen Tool), Fussleiste wie im Pen Tool */
            if (y >= ly_foot) {
                for (i = 0; i < 4; i++)
                    if (x >= pa_bx[i] && x < pa_bx[i] + text_width(pa_lab[i]) + 14) {
                        if (path_exists())
                            do_command(pa_cmd[i]);
                        else
                            status_message("Draw a path first");
                    }
            } else if (y >= ly_list && y < ly_list + ROW_H && path_exists())
                ui_set_tool(T_PEN);
            ui_draw_panels();
            return TRUE;
        }
        if (panel_tab == 1) {
            /* Protokoll: zu diesem Zustand springen */
            if (y >= ly_list) {
                LONG k = hist_first + (y - ly_list) / HIST_H;
                if (k >= 0 && k < hist_n && x < px + 26) {
                    doc->hist_src = k;  /* linke Spalte: Quelle des Protokollpinsels */
                    ui_draw_panels();
                } else if (k >= 0 && k < hist_n && k != hist_cur) {
                    undo_goto(k);
                    ui_draw_panels();
                    ui_draw_tab();
                }
            }
            return TRUE;
        }
        /* Fuellmethode */
        if (y >= ly_modey && y < ly_modey + 22 && x >= px + 10 && x < px + 160) {
            choose_blend_mode();
            return TRUE;
        }
        /* Sperren */
        if (y >= ly_locky && y < ly_locky + 20 && x >= px + 46 && x < px + 46 + 4 * 24) {
            static const UBYTE bits[4] = { LOCK_ALPHA, LOCK_PIXELS, LOCK_POS, LOCK_ALL };
            Layer *l = doc->layers[doc->active];
            LONG b = bits[(x - px - 46) / 24];
            UndoRec *u = undo_begin_struct("Lock Layer");
            if (b == LOCK_ALL)
                l->lock = (l->lock & LOCK_ALL) == LOCK_ALL ? 0 : LOCK_ALL;
            else
                l->lock ^= b;
            undo_commit(u);
            ui_draw_panels();
            return TRUE;
        }
        /* Flaeche (wie Deckkraft) */
        if (value_hit(&sl_lfill, x, y)) {
            UndoRec *u = undo_begin_struct("Fill Change");
            slider_edit_value(&sl_lfill);
            undo_commit(u);
            ui_draw_panels();
            return TRUE;
        }
        if (slider_hit(&sl_lfill, x, y)) {
            op_undo = undo_begin_struct("Fill Change");
            drag_slider = &sl_lfill;
            slider_set(drag_slider, x);
            ui_draw_panels();
            return TRUE;
        }
        /* Ebenen-Deckkraft (Undo bis zum Loslassen sammeln) */
        if (value_hit(&sl_layer, x, y)) {
            UndoRec *u = undo_begin_struct("Opacity Change");
            slider_edit_value(&sl_layer);
            undo_commit(u);
            ui_draw_panels();
            return TRUE;
        }
        if (slider_hit(&sl_layer, x, y)) {
            op_undo = undo_begin_struct("Opacity Change");
            drag_slider = &sl_layer;
            slider_set(drag_slider, x);
            ui_draw_panels();
            return TRUE;
        }
        /* Ebenenliste */
        if (y >= ly_list && y < ly_foot) {
            LONG row = (y - ly_list) / ROW_H, li = row_layer(row);
            if (li >= 0 && li < doc->nlayers) {
                if (x < px + 29) {
                    doc->layers[li]->visible ^= 1;
                    doc_mark_all_dirty();
                } else {
                    ULONG t = now_us();
                    BOOL dbl = row == last_row && t - last_row_click < 500000;
                    Layer *l = doc->layers[li];
                    LONG ind = layer_indent(l);
                    LONG namex = (l->kind == LK_GROUP ? px + 72 : l->mask ? px + 112 : px + 68) + ind;
                    last_row_click = t;
                    last_row = row;
                    if (ui_qualifier & (IEQUALIFIER_CONTROL | IEQUALIFIER_LCOMMAND | IEQUALIFIER_RCOMMAND)) {
                        /* Strg+Klick: Ebene zur Auswahl hinzu bzw. heraus */
                        if (li == doc->active) {
                            LONG k;
                            for (k = doc->nlayers - 1; k >= 0 && !(doc->layers[k]->picked && k != li); k--)
                                ;
                            if (k >= 0) {
                                l->picked = 0;
                                doc->active = k;
                            }
                        } else if (l->picked)
                            l->picked = 0;
                        else {
                            doc->layers[doc->active]->picked = 1;
                            l->picked = 1;
                            doc->active = li;
                        }
                        doc->edit_mask = FALSE;
                        ui_draw_panels();
                        ui_draw_tab();
                        return TRUE;
                    }
                    if (ui_qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT) && li != doc->active &&
                        !(l->kind == LK_GROUP && l->mask && x - ind >= px + 70 && x - ind < px + 104)) {
                        /* Shift+Klick: Bereich bis zur aktiven Ebene */
                        LONG a = li < doc->active ? li : doc->active, b = li < doc->active ? doc->active : li, k;
                        doc_pick_clear();
                        for (k = a; k <= b; k++)
                            doc->layers[k]->picked = 1;
                        doc->active = li;
                        doc->edit_mask = FALSE;
                        ui_draw_panels();
                        ui_draw_tab();
                        return TRUE;
                    }
                    doc_pick_clear();
                    if (li != doc->active)
                        doc->edit_mask = l->kind == LK_ADJUST;
                    doc->active = li;
                    x -= ind;           /* ab hier Lage ohne Einrueckung */
                    if (l->kind == LK_GROUP && l->mask && x >= px + 70 && x < px + 104) {
                        if (ui_qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT))
                            do_command(CMD_MASK_TOGGLE);
                        else
                            doc->edit_mask = TRUE;
                        ui_draw_panels();
                        ui_draw_tab();
                        return TRUE;
                    }
                    if (l->kind == LK_GROUP && x >= px + 32 && x < px + 48) {
                        l->collapsed ^= 1;      /* auf-/zuklappen (nicht im Protokoll) */
                        ui_draw_panels();
                        ui_draw_tab();
                        return TRUE;
                    }
                    if (l->kind == LK_ADJUST && dbl && x >= px + 34 && x < px + 68) {
                        last_row_click = 0;
                        ui_draw_panels();
                        do_command(CMD_ADJ_EDIT);
                        return TRUE;
                    }
                    if (fx_active(l->fx) && x >= SW - 30) {
                        ui_draw_tab();
                        ui_draw_panels();
                        do_command(CMD_LAYER_STYLE);
                        return TRUE;
                    }
                    if (l->mask && x >= px + 66 && x < px + 76 && l->kind != LK_GROUP) {
                        l->mask_unlink ^= 1;    /* Kette an/aus */
                        status_message(l->mask_unlink ? "Mask unlinked: moves separately"
                                                      : "Mask linked to the layer");
                    } else if (l->mask && x >= px + 76 && x < px + 110) {
                        if (ui_qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT))
                            do_command(CMD_MASK_TOGGLE);
                        else
                            doc->edit_mask = TRUE;
                    } else if (x >= px + 34 && x < px + 66)
                        doc->edit_mask = FALSE;
                    x += ind;
                    ui_draw_tab();
                    if (dbl && x >= px + 34 && x < px + 68 && l->text) {
                        last_row_click = 0;
                        ui_draw_panels();
                        do_command(CMD_TEXT_EDIT);
                        return TRUE;
                    }
                    if (dbl && x >= namex) {
                        last_row_click = 0;
                        ui_rename_layer();
                        return TRUE;
                    }
                    ldrag_from = li;        /* evtl. Ziehen zum Umsortieren */
                    ldrag_y0 = y;
                    ldragging = FALSE;
                    ldrag_slot = -1;
                }
                ui_draw_panels();
            }
            return TRUE;
        }
        /* Fuss: neue Ebene / loeschen */
        if (y >= ly_foot + 4 && y < ly_foot + 22) {
            if (x >= SW - 184 && x < SW - 158)
                do_command(CMD_GROUP_NEW);
            else if (x >= SW - 154 && x < SW - 128) {
                static const char *items[8] = { "Brightness/Contrast...", "Levels...", "Curves...",
                    "Hue/Saturation...", "Desaturate", "Invert", "Threshold...", "Posterize..." };
                static const LONG cmds[8] = { CMD_NADJ_BRICON, CMD_NADJ_LEVELS, CMD_NADJ_CURVES,
                    CMD_NADJ_HUESAT, CMD_NADJ_DESAT, CMD_NADJ_INVERT, CMD_NADJ_THRESH, CMD_NADJ_POSTER };
                LONG r = ui_popup(SW - 154, ly_foot - 8 * 22 - 10, items, 8, -1);
                if (r >= 0)
                    do_command(cmds[r]);
            } else if (x >= SW - 124 && x < SW - 98)
                do_command(CMD_LAYER_STYLE);
            else if (x >= SW - 94 && x < SW - 68)
                do_command(doc->hassel ? CMD_MASK_REVEAL_SEL : CMD_MASK_REVEAL_ALL);
            else if (x >= SW - 64 && x < SW - 38)
                do_command(CMD_LAYER_NEW);
            else if (x >= SW - 34 && x < SW - 8)
                do_command(CMD_LAYER_DEL);
        }
        return TRUE;
    }

    if (y >= MENU_H + OPT_H && y < MENU_H + OPT_H + TAB_H) {
        for (i = 0; i < ndocs; i++)
            if (x >= tabx[i] && x < tabx[i + 1]) {
                Doc *d = docs[i];
                BOOL close = x >= tabx[i + 1] - 26;    /* x: vor dem Neuzeichnen pruefen */
                main_activate_doc(d);
                if (close)
                    do_command(CMD_CLOSE);
                break;
            }
        return TRUE;
    }
    if (guides_ruler_down(x, y))
        return FALSE;           /* Ziehen uebernimmt main.c */
    if (y < cvy || y >= cvy + cvh || x < cvx)
        return TRUE;
    return FALSE;
}

void ui_mouse_move(LONG x, LONG y)
{
    if (color_drag) {
        color_drag_to(x, y);
        return;
    }
    if (menu_open >= 0) {
        LONG t = y < MENU_H ? title_at(x) : -1;
        if (t >= 0 && t != menu_open) {
            open_menu(t);
            return;
        }
        menu_hover_at(x, y);
        return;
    }
    if (ldrag_from >= 0) {
        if (!ldragging && (y - ldrag_y0 > 4 || ldrag_y0 - y > 4))
            ldragging = TRUE;
        if (ldragging) {
            LONG row = (y - ly_list) / ROW_H, off = (y - ly_list) - row * ROW_H, slot;
            BOOL into = FALSE;
            /* mittleres Drittel einer Gruppenzeile: hinein */
            if (row >= 0 && row < ndisp && doc->layers[disp[row]]->kind == LK_GROUP &&
                off >= ROW_H / 3 && off < ROW_H * 2 / 3) {
                slot = row;
                into = TRUE;
            } else
                slot = (y - ly_list + ROW_H / 2) / ROW_H;
            if (slot < 0) slot = 0;
            if (slot > ndisp) slot = ndisp;
            if (slot != ldrag_slot || into != ldrag_into) {
                ldrag_slot = slot;
                ldrag_into = into;
                ui_draw_panels();
            }
        }
        return;
    }
    if (!drag_slider)
        return;
    slider_set(drag_slider, x);
    if (drag_slider == &sl_layer || drag_slider == &sl_lfill || pr_is_slider(drag_slider))
        ui_draw_panels();
    else
        ui_draw_options();
}

void ui_mouse_up(void)
{
    if (tb_hold >= 0) {
        LONG k = tb_hold;
        tb_hold = -1;
        ui_set_tool(tool_group(tool) == tool_group(slot_tool[k]) ? tool : slot_tool[k]);
        return;
    }
    if (color_drag) {
        color_drag = 0;
        return;
    }
    if (ldrag_from >= 0) {
        if (ldragging && ldrag_slot >= 0) {
            /* Block (Ebene oder Gruppe) ueber die Zeile bzw. in die Gruppe legen */
            LONG top = ldrag_from, size = layer_block(top), bot = top - size + 1, dest, nd;
            LONG t = ldrag_slot < ndisp ? disp[ldrag_slot] : -1;
            BOOL ok = TRUE;
            if (t >= bot && t <= top)
                ok = FALSE;             /* auf sich selbst oder in sich hinein */
            else if (t < 0) {
                dest = 0;               /* ganz nach unten */
                nd = 0;
            } else if (ldrag_into) {
                dest = top < t ? t - size : t;
                nd = doc->layers[t]->depth + 1;
                doc->layers[t]->collapsed = 0;
            } else {
                dest = top < t ? t - size + 1 : t + 1;
                nd = doc->layers[t]->depth;
            }
            if (ok && nd < MAX_DEPTH) {
                UndoRec *u = undo_begin_struct("Layer Order");
                doc_move_block(top, dest, nd);
                undo_commit(u);
            }
        }
        ldrag_into = FALSE;
        ldrag_from = -1;
        ldragging = FALSE;
        ldrag_slot = -1;
        ui_draw_panels();
        ui_draw_tab();
        return;
    }
    if (drag_slider && pr_is_slider(drag_slider)) {
        drag_slider = NULL;
        props_release();
        return;
    }
    if (drag_slider == &sl_layer || drag_slider == &sl_lfill) {
        undo_commit(op_undo);
        op_undo = NULL;
        ui_draw_panels();
        ui_draw_tab();
    }
    drag_slider = NULL;
}

/* ---- Modaler Dialog ---- */

#define DLG_MAX 24
static Slider dlg_sl[DLG_MAX];
static LONG dlg_kind[DLG_MAX], dlg_iy[DLG_MAX], dlg_ix[DLG_MAX], dlg_colw;
static LONG dlg_cx[DLG_MAX][8], dlg_cw[DLG_MAX][8], dlg_cn[DLG_MAX];
static const ULONG *dlg_pal;
static const LONG *dlg_npal;

/* Farbtabelle fuer ein DLG_PALETTE-Element (wird bei jedem Zeichnen gelesen) */
void ui_dialog_palette(const ULONG *col, const LONG *n)
{
    dlg_pal = col;
    dlg_npal = n;
}

/* Knopfgruppe einer Auswahl zeichnen (und Lage merken) */
static LONG dlg_choice(LONG i, LONG x, LONG y, BOOL draw)
{
    const char *p = dlg_sl[i].unit;
    LONG k = 0;
    while (*p && k < 8) {
        char buf[32];
        LONG n = 0, w;
        while (*p && *p != '|' && n < 31)
            buf[n++] = *p++;
        buf[n] = 0;
        if (*p == '|')
            p++;
        w = text_width(buf) + 16;
        dlg_cx[i][k] = x;
        dlg_cw[i][k] = w;
        if (draw)
            button(x, y - 3, w, 22, buf, *dlg_sl[i].val == k);
        x += w + 4;
        k++;
    }
    dlg_cn[i] = k;
    return x;
}     /* 0 = Regler, sonst DLG_CHECK/DLG_COLOR */
static LONG dlg_n, dlg_x0, dlg_y0, dlg_w, dlg_h, dlg_okx, dlg_cax, dlg_by;
static const char *dlg_title;

static void dlg_check(LONG x, LONG y, const char *label, BOOL on)
{
    fill(x, y + 2, 13, 13, COL_FIELD);
    frame(x, y + 2, 13, 13, 0xFF6A6A6A);
    if (on) {
        line(x + 3, y + 8, x + 5, y + 11, COL_TEXT);
        line(x + 5, y + 11, x + 10, y + 5, COL_TEXT);
        line(x + 3, y + 9, x + 5, y + 12, COL_TEXT);
        line(x + 5, y + 12, x + 10, y + 6, COL_TEXT);
    }
    text_bold(x + 20, y, label, COL_TEXT);
}

/* Dialog in den Backbuffer zeichnen (ohne Blit) */
static void dlg_draw(void)
{
    LONG i;
    gfx_noclip();
    fill(dlg_x0, dlg_y0, dlg_w, dlg_h, COL_BAR);
    frame(dlg_x0, dlg_y0, dlg_w, dlg_h, COL_LINE);
    fill(dlg_x0 + 1, dlg_y0 + 1, dlg_w - 2, 26, COL_PANELHD);
    text_bold(dlg_x0 + 12, dlg_y0 + 5, dlg_title, COL_TEXT);
    for (i = 0; i < dlg_n; i++) {
        if (dlg_kind[i] == DLG_CHECK)
            dlg_check(dlg_ix[i], dlg_iy[i], dlg_sl[i].label, *dlg_sl[i].val != 0);
        else if (dlg_kind[i] == DLG_COLOR) {
            text(dlg_x0 + 16, dlg_iy[i], dlg_sl[i].label, COL_DIM);
            fill(dlg_sl[i].tx, dlg_iy[i] - 1, 40, 18, 0xFF000000 | (ULONG)*dlg_sl[i].val);
            frame(dlg_sl[i].tx, dlg_iy[i] - 1, 40, 18, 0xFFE0E0E0);
            text(dlg_sl[i].tx + 50, dlg_iy[i], "click to choose", COL_DIM);
        } else if (dlg_kind[i] == DLG_CHOICE) {
            text(dlg_x0 + 16, dlg_iy[i], dlg_sl[i].label, COL_DIM);
            dlg_choice(i, dlg_sl[i].tx, dlg_iy[i], TRUE);
        } else if (dlg_kind[i] == DLG_PALETTE) {
            LONG k, n = dlg_npal ? *dlg_npal : 0, px0 = dlg_sl[i].tx, py0 = dlg_iy[i];
            char buf[32];
            text(dlg_x0 + 16, py0, dlg_sl[i].label, COL_DIM);
            fill(px0, py0, 32 * 11, 8 * 11, COL_BAR);
            for (k = 0; k < n && k < 256 && dlg_pal; k++) {
                LONG x = px0 + (k & 31) * 11, y = py0 + (k >> 5) * 11;
                fill(x, y, 10, 10, 0xFF000000 | dlg_pal[k]);
            }
            sprintf(buf, "%ld", (long)n);
            fill(dlg_x0 + 16, py0 + FONT_H + 2, 60, FONT_H, COL_BAR);
            text(dlg_x0 + 16, py0 + FONT_H + 2, buf, COL_TEXT);
        } else
            slider_draw(&dlg_sl[i], dlg_x0 + 16, dlg_iy[i]);
    }
    fill(dlg_okx, dlg_by, 86, 26, COL_ACCENT);
    frame(dlg_okx, dlg_by, 86, 26, COL_LINE);
    text(dlg_okx + (86 - text_width("OK")) / 2, dlg_by + 4, "OK", 0xFFFFFFFF);
    button(dlg_cax, dlg_by, 92, 26, "Cancel", FALSE);
}

BOOL dialog_run(const char *title, DlgSlider *ds, LONG n, void (*preview)(void))
{
    LONG i, lw = 0;
    BOOL done = FALSE, ok = FALSE, changed = TRUE, need_preview = TRUE;
    ULONG preview_us = 0;
    Slider *drag = NULL;
    LONG y;

    if (n > DLG_MAX)
        n = DLG_MAX;
    for (i = 0; i < n; i++) {
        LONG w = text_width(ds[i].label);
        dlg_kind[i] = ds[i].min == ds[i].max && ds[i].min < 0 ? ds[i].min : 0;
        if ((!dlg_kind[i] || dlg_kind[i] == DLG_CHOICE || dlg_kind[i] == DLG_COLOR) && w > lw)
            lw = w;
    }
    for (i = 0; i < n; i++) {
        memset(&dlg_sl[i], 0, sizeof(Slider));
        dlg_sl[i].label = ds[i].label;
        dlg_sl[i].val = ds[i].val;
        dlg_sl[i].min = ds[i].min;
        dlg_sl[i].max = ds[i].max;
        dlg_sl[i].unit = ds[i].unit;
        dlg_sl[i].div = ds[i].div;
        dlg_sl[i].changed = nop_changed;
        /* Spuren buendig: Spur beginnt nach dem laengsten Label */
        dlg_sl[i].lw = lw;
        dlg_sl[i].tw = 200;
    }
    dlg_n = n;
    dlg_title = title;
    dlg_w = lw + 16 + 6 + 200 + 8 + 64 + 16;
    if (dlg_w < 380)
        dlg_w = 380;
    for (i = 0; i < n; i++)
        if (dlg_kind[i] == DLG_PALETTE && lw + 16 + 6 + 352 + 16 > dlg_w)
            dlg_w = lw + 16 + 6 + 352 + 16;
    for (i = 0; i < n; i++)
        if (dlg_kind[i] == DLG_CHOICE) {    /* breit genug fuer alle Knoepfe */
            LONG need = dlg_choice(i, 0, 0, FALSE) + lw + 16 + 6 + 16;
            if (need > dlg_w)
                dlg_w = need;
        }
    /* oben rechts ueber der Arbeitsflaeche, damit das Bild sichtbar bleibt */
    dlg_x0 = cvx + cvw - dlg_w - 16;
    dlg_y0 = cvy + 16;
    y = dlg_y0 + 42;
    dlg_colw = dlg_w - 24;
    for (i = 0; i < n && dlg_kind[i] == DLG_CHECK; i++)
        ;
    if (i == n && n > 10) {
        /* lange Haekchen-Liste: zwei Spalten */
        LONG half = (n + 1) / 2;
        dlg_colw = 260;
        dlg_w = 16 + 2 * dlg_colw + 16;
        dlg_x0 = cvx + cvw - dlg_w - 16;
        dlg_okx = dlg_x0 + dlg_w - 200;
        dlg_cax = dlg_x0 + dlg_w - 106;
        for (i = 0; i < n; i++) {
            dlg_ix[i] = dlg_x0 + 16 + (i / half) * dlg_colw;
            dlg_iy[i] = y + (i % half) * 26;
        }
        y += half * 26 + 8;
    } else
        for (i = 0; i < n; i++) {
            if (dlg_kind[i] == DLG_CHECK && i > 0)
                y += 8;         /* Abstand vor einem neuen Abschnitt */
            dlg_iy[i] = y;
            dlg_ix[i] = dlg_x0 + 16;
            dlg_sl[i].x = dlg_x0 + 16;
            dlg_sl[i].y = y;
            dlg_sl[i].tx = dlg_x0 + 16 + lw + 6;
            y += dlg_kind[i] == DLG_PALETTE ? 8 * 11 + 10 : dlg_kind[i] ? 30 : 34;
        }
    dlg_h = y - dlg_y0 + 52;
    dlg_okx = dlg_x0 + dlg_w - 200;
    dlg_cax = dlg_x0 + dlg_w - 106;
    dlg_by = dlg_y0 + dlg_h - 40;
    view_overlay_cb = dlg_draw;

    while (!done) {
        struct IntuiMessage *im;
        /* Vorschau beim Ziehen nur, wenn sie schnell ist; sonst beim Loslassen.
         * Intuition schickt je Bewegung eine Nachricht - eine langsame
         * Vorschau pro Schritt wuerde den Rechner minutenlang beschaeftigen. */
        if (need_preview && preview && (!drag || preview_us < 80000)) {
            ULONG t = now_us();
            preview();
            preview_us = now_us() - t;
            view_flush();       /* zeichnet den Dialog ueber view_overlay_cb mit */
            need_preview = FALSE;
        }
        /* nur zeichnen, wenn sich etwas geaendert hat (nicht bei jedem Takt) */
        if (changed) {
            dlg_draw();
            blit(dlg_x0, dlg_y0, dlg_w, dlg_h);
        }
        changed = FALSE;
        if (Wait((1UL << win->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            /* Abbruch von aussen: Dialog schliessen, Signal fuer die Hauptschleife erneut setzen */
            SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
            done = TRUE;
        }
        while ((im = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG class = im->Class;
            UWORD code = im->Code;
            LONG mx = im->MouseX, my = im->MouseY;
            ReplyMsg((struct Message *)im);
            switch (class) {
            case IDCMP_MOUSEBUTTONS:
                if (code == SELECTDOWN) {
                    for (i = 0; i < n; i++)
                        if (dlg_kind[i] && dlg_kind[i] != DLG_PALETTE && my >= dlg_iy[i] - 4 && my < dlg_iy[i] + 22 &&
                            mx >= dlg_ix[i] - 4 && mx < dlg_ix[i] + dlg_colw - 8) {
                            if (dlg_kind[i] == DLG_CHECK)
                                *dlg_sl[i].val = !*dlg_sl[i].val;
                            else if (dlg_kind[i] == DLG_CHOICE) {
                                LONG k;
                                for (k = 0; k < dlg_cn[i]; k++)
                                    if (mx >= dlg_cx[i][k] && mx < dlg_cx[i][k] + dlg_cw[i][k])
                                        *dlg_sl[i].val = k;
                            } else if (mx >= dlg_sl[i].tx && mx < dlg_sl[i].tx + 40) {
                                /* Farbfeld: Farbwaehler, danach Dialog wieder zeigen */
                                void (*cb)(void) = view_overlay_cb;
                                ui_color_picker("Color Picker", (ULONG *)dlg_sl[i].val);
                                view_overlay_cb = cb;
                                view_inval_all();
                                view_flush();
                            }
                            need_preview = TRUE;
                        }
                    for (i = 0; i < n; i++)
                        if (!dlg_kind[i] && value_hit(&dlg_sl[i], mx, my)) {
                            if (slider_edit_value(&dlg_sl[i]))
                                need_preview = TRUE;
                            changed = TRUE;
                        }
                    for (i = 0; i < n; i++)
                        if (!dlg_kind[i] && slider_hit(&dlg_sl[i], mx, my)) {
                            LONG old = *dlg_sl[i].val;
                            drag = &dlg_sl[i];
                            slider_set(drag, mx);
                            need_preview |= old != *drag->val;
                        }
                    if (my >= dlg_by && my < dlg_by + 26) {
                        if (mx >= dlg_okx && mx < dlg_okx + 86)
                            ok = done = TRUE;
                        else if (mx >= dlg_cax && mx < dlg_cax + 92)
                            done = TRUE;
                    }
                    changed = TRUE;     /* Knopfzustand neu zeichnen */
                } else if (code == SELECTUP && drag) {
                    /* Endposition steht nur hier sicher drin: Intuition
                     * verschluckt Bewegungen, solange die Vorschau rechnet */
                    LONG old = *drag->val;
                    slider_set(drag, mx);
                    need_preview |= old != *drag->val;
                    drag = NULL;
                    changed = TRUE;
                }
                break;
            case IDCMP_MOUSEMOVE:
                if (drag) {
                    LONG old = *drag->val;
                    slider_set(drag, mx);
                    if (old != *drag->val)
                        changed = need_preview = TRUE;
                }
                break;
            case IDCMP_RAWKEY:
                if (code == 0x44 || code == 0x43)
                    ok = done = TRUE;
                else if (code == 0x45)
                    done = TRUE;
                else if ((code == 0x4E || code == 0x4F) && n > 0 && !dlg_kind[0]) {
                    LONG v = *dlg_sl[0].val + (code == 0x4E ? 1 : -1);
                    if (v >= dlg_sl[0].min && v <= dlg_sl[0].max) {
                        *dlg_sl[0].val = v;
                        changed = need_preview = TRUE;
                    }
                }
                break;
            }
        }
    }
    view_overlay_cb = NULL;
    ui_draw_all();
    view_inval_all();
    view_flush();
    return ok;
}

/* ---- Groessen-Dialog (New, Image Size, Canvas Size) ---- */

static LONG sd_x0, sd_y0, sd_w, sd_h, sd_by, sd_fx, sd_ow, sd_oh;
static LONG *sd_wv, *sd_hv, *sd_anchor;
static BOOL *sd_lock;
static const char *sd_title;

static void sd_field(LONG y, const char *label, LONG v)
{
    char buf[16];
    text(sd_x0 + 16, y + 3, label, COL_TEXT);
    fill(sd_fx, y, 90, 22, COL_FIELD);
    frame(sd_fx, y, 90, 22, COL_LINE);
    sprintf(buf, "%ld", (long)v);
    text(sd_fx + 6, y + 3, buf, COL_TEXT);
    text(sd_fx + 98, y + 3, "px", COL_DIM);
}

static void sd_draw(void)
{
    char buf[64];
    LONG y = sd_y0 + 42;
    gfx_noclip();
    fill(sd_x0, sd_y0, sd_w, sd_h, COL_BAR);
    frame(sd_x0, sd_y0, sd_w, sd_h, COL_LINE);
    fill(sd_x0 + 1, sd_y0 + 1, sd_w - 2, 26, COL_PANELHD);
    text_bold(sd_x0 + 12, sd_y0 + 5, sd_title, COL_TEXT);
    if (sd_ow) {
        sprintf(buf, "Current size: %ld x %ld px", (long)sd_ow, (long)sd_oh);
        text(sd_x0 + 16, y, buf, COL_DIM);
        y += 28;
    }
    sd_field(y, "Width:", *sd_wv);
    sd_field(y + 32, "Height:", *sd_hv);
    y += 70;
    if (sd_lock) {
        fill(sd_x0 + 16, y + 2, 16, 16, COL_FIELD);
        frame(sd_x0 + 16, y + 2, 16, 16, COL_LINE);
        if (*sd_lock)
            fill(sd_x0 + 20, y + 6, 8, 8, COL_TEXT);
        text(sd_x0 + 40, y + 1, "Constrain Proportions", COL_TEXT);
    }
    if (sd_anchor) {
        LONG i;
        text(sd_x0 + 16, y + 1, "Anchor:", COL_TEXT);
        for (i = 0; i < 9; i++) {
            LONG ax = sd_fx + (i % 3) * 24, ay = y + (i / 3) * 24;
            fill(ax, ay, 22, 22, i == *sd_anchor ? COL_ACCENT : COL_FIELD);
            frame(ax, ay, 22, 22, COL_LINE);
        }
    }
    fill(sd_x0 + sd_w - 200, sd_by, 86, 26, COL_ACCENT);
    frame(sd_x0 + sd_w - 200, sd_by, 86, 26, COL_LINE);
    text(sd_x0 + sd_w - 200 + (86 - text_width("OK")) / 2, sd_by + 4, "OK", 0xFFFFFFFF);
    button(sd_x0 + sd_w - 106, sd_by, 92, 26, "Cancel", FALSE);
}

/* Zahl eintippen; bei Seitenverhaeltnis-Sperre die andere Seite nachziehen */
static void sd_edit(LONG y, LONG *v, LONG *other, LONG base, LONG obase)
{
    char buf[16];
    LONG n = 0;
    const char *b = buf;
    sprintf(buf, "%ld", (long)*v);
    if (!ui_edit_text(sd_fx, y, 90, buf, sizeof(buf)))
        return;
    while (*b == ' ')
        b++;
    while (*b >= '0' && *b <= '9')
        n = n * 10 + (*b++ - '0');
    if (n < 1 || n > 16000)
        return;
    *v = n;
    if (sd_lock && *sd_lock && base > 0) {
        *other = (n * obase + base / 2) / base;
        if (*other < 1)
            *other = 1;
    }
}

/* ow/oh: aktuelle Groesse (0 = keine Anzeige/Sperre), lock/anchor optional */
BOOL size_dialog(const char *title, LONG *w, LONG *h, LONG ow, LONG oh, BOOL *lock, LONG *anchor)
{
    BOOL done = FALSE, ok = FALSE, changed = TRUE;
    sd_title = title;
    sd_wv = w;
    sd_hv = h;
    sd_ow = ow;
    sd_oh = oh;
    sd_lock = lock;
    sd_anchor = anchor;
    sd_w = 360;
    sd_h = 42 + (ow ? 28 : 0) + 70 + (anchor ? 76 : lock ? 30 : 0) + 52;
    sd_x0 = cvx + (cvw - sd_w) / 2;
    sd_y0 = cvy + 60;
    sd_fx = sd_x0 + 100;
    sd_by = sd_y0 + sd_h - 40;
    view_overlay_cb = sd_draw;

    while (!done) {
        struct IntuiMessage *im;
        LONG fy = sd_y0 + 42 + (ow ? 28 : 0), oy = fy + 70;
        if (changed) {
            sd_draw();
            blit(sd_x0, sd_y0, sd_w, sd_h);
        }
        changed = FALSE;
        if (Wait((1UL << win->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
            done = TRUE;
        }
        while (!done && (im = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG class = im->Class;
            UWORD code = im->Code;
            LONG mx = im->MouseX, my = im->MouseY;
            ReplyMsg((struct Message *)im);
            if (class == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                if (mx >= sd_fx && mx < sd_fx + 90 && my >= fy && my < fy + 22)
                    sd_edit(fy, w, h, ow, oh);
                else if (mx >= sd_fx && mx < sd_fx + 90 && my >= fy + 32 && my < fy + 54)
                    sd_edit(fy + 32, h, w, oh, ow);
                else if (lock && !anchor && my >= oy && my < oy + 20 && mx >= sd_x0 + 16 && mx < sd_x0 + 200) {
                    *lock = !*lock;
                    if (*lock && ow)
                        *h = (*w * oh + ow / 2) / ow;
                } else if (anchor && mx >= sd_fx && mx < sd_fx + 72 && my >= oy && my < oy + 72)
                    *anchor = ((my - oy) / 24) * 3 + (mx - sd_fx) / 24;
                else if (my >= sd_by && my < sd_by + 26) {
                    if (mx >= sd_x0 + sd_w - 200 && mx < sd_x0 + sd_w - 114)
                        ok = done = TRUE;
                    else if (mx >= sd_x0 + sd_w - 106 && mx < sd_x0 + sd_w - 14)
                        done = TRUE;
                }
                changed = TRUE;
            } else if (class == IDCMP_RAWKEY) {
                if (code == 0x44 || code == 0x43)
                    ok = done = TRUE;
                else if (code == 0x45)
                    done = TRUE;
            }
        }
    }
    view_overlay_cb = NULL;
    ui_draw_all();
    view_inval_all();
    view_flush();
    return ok;
}

/* ---- Farbwaehler (Color Picker) ---- */

#define CP_N 256                /* Groesse des Saettigung/Helligkeit-Feldes */
static LONG cp_x0, cp_y0, cp_w, cp_h, cp_fx, cp_fy, cp_hx, cp_rx, cp_bx, cp_by;
static float cp_hue, cp_sat, cp_val;    /* 0..360, 0..1, 0..1 */
static ULONG cp_old;
static const char *cp_title;
static const char *cp_lab[7] = { "H:", "S:", "B:", "R:", "G:", "B:", "#" };

static ULONG hsv_rgb(float h, float s, float v)
{
    float r, g, b, f, p, q, t;
    LONG i;
    h /= 60.0f;
    while (h >= 6.0f)
        h -= 6.0f;
    if (h < 0.0f)
        h = 0.0f;
    i = (LONG)h;
    f = h - i;
    p = v * (1.0f - s);
    q = v * (1.0f - s * f);
    t = v * (1.0f - s * (1.0f - f));
    switch (i) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return ((ULONG)(r * 255.0f + 0.5f) << 16) | ((ULONG)(g * 255.0f + 0.5f) << 8) | (ULONG)(b * 255.0f + 0.5f);
}

/* Farbton bleibt bei Grau erhalten (Ganzzahl-Vergleiche: 68881 rechnet 80 Bit) */
static void rgb_hsv(ULONG c, float *h, float *s, float *v)
{
    LONG r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    LONG mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    LONG mn = r < g ? (r < b ? r : b) : (g < b ? g : b), d = mx - mn;
    *v = mx / 255.0f;
    *s = mx ? (float)d / mx : 0.0f;
    if (d) {
        float hh;
        if (mx == r)
            hh = (float)(g - b) / d;
        else if (mx == g)
            hh = 2.0f + (float)(b - r) / d;
        else
            hh = 4.0f + (float)(r - g) / d;
        hh *= 60.0f;
        if (hh < 0.0f)
            hh += 360.0f;
        *h = hh;
    }
}

static void hex6(char *buf, ULONG c)
{
    static const char hx[] = "0123456789ABCDEF";   /* libnix: %lX gibt Kleinbuchstaben */
    LONG i;
    for (i = 0; i < 6; i++)
        buf[i] = hx[(c >> (20 - 4 * i)) & 0xF];
    buf[6] = 0;
}

static ULONG cp_color(void)
{
    return hsv_rgb(cp_hue, cp_sat, cp_val);
}

static LONG cp_value(LONG i)
{
    ULONG c = cp_color();
    switch (i) {
    case 0: return (LONG)(cp_hue + 0.5f) % 360;
    case 1: return (LONG)(cp_sat * 100.0f + 0.5f);
    case 2: return (LONG)(cp_val * 100.0f + 0.5f);
    case 3: return (c >> 16) & 0xFF;
    case 4: return (c >> 8) & 0xFF;
    case 5: return c & 0xFF;
    }
    return c;
}

static void cp_draw(void)
{
    ULONG hc = hsv_rgb(cp_hue, 1.0f, 1.0f), c = cp_color();
    LONG hr = (hc >> 16) & 0xFF, hg = (hc >> 8) & 0xFF, hb = hc & 0xFF, x, y, i;
    char buf[16];

    gfx_noclip();
    fill(cp_x0, cp_y0, cp_w, cp_h, COL_BAR);
    frame(cp_x0, cp_y0, cp_w, cp_h, COL_LINE);
    fill(cp_x0 + 1, cp_y0 + 1, cp_w - 2, 26, COL_PANELHD);
    text_bold(cp_x0 + 12, cp_y0 + 5, cp_title, COL_TEXT);
    /* Feld: waagerecht Saettigung, senkrecht Helligkeit */
    for (y = 0; y < CP_N; y++) {
        ULONG *row = bb + (cp_fy + y) * SW + cp_fx;
        LONG v = 255 - y;
        for (x = 0; x < CP_N; x++) {
            LONG r = (255 * (255 - x) + hr * x) / 255, g = (255 * (255 - x) + hg * x) / 255,
                 b = (255 * (255 - x) + hb * x) / 255;
            row[x] = 0xFF000000 | ((r * v / 255) << 16) | ((g * v / 255) << 8) | (b * v / 255);
        }
    }
    frame(cp_fx - 1, cp_fy - 1, CP_N + 2, CP_N + 2, COL_LINE);
    /* Kreis an der aktuellen Stelle */
    {
        LONG mx = cp_fx + (LONG)(cp_sat * 255.0f + 0.5f), my = cp_fy + (LONG)((1.0f - cp_val) * 255.0f + 0.5f);
        ULONG rc = cp_val > 0.6f && cp_sat < 0.5f ? 0xFF000000 : 0xFFFFFFFF;
        gfx_clip(cp_fx, cp_fy, cp_fx + CP_N, cp_fy + CP_N);
        frame(mx - 5, my - 5, 11, 11, rc);
        gfx_noclip();
    }
    /* Farbton-Streifen, rot oben und unten */
    for (y = 0; y < CP_N; y++)
        fill(cp_hx, cp_fy + y, 20, 1, 0xFF000000 | hsv_rgb((255 - y) * 360.0f / 256.0f, 1.0f, 1.0f));
    frame(cp_hx - 1, cp_fy - 1, 22, CP_N + 2, COL_LINE);
    y = cp_fy + 255 - (LONG)(cp_hue * 256.0f / 360.0f);
    for (i = 0; i < 4; i++) {
        fill(cp_hx - 6 + i, y - 3 + i, 1, 7 - 2 * i, COL_TEXT);
        fill(cp_hx + 25 - i, y - 3 + i, 1, 7 - 2 * i, COL_TEXT);
    }
    /* neu / aktuell */
    text(cp_rx + 18, cp_fy - 2, "new", COL_DIM);
    fill(cp_rx, cp_fy + 16, 64, 34, 0xFF000000 | c);
    fill(cp_rx, cp_fy + 50, 64, 34, 0xFF000000 | cp_old);
    frame(cp_rx - 1, cp_fy + 15, 66, 70, COL_LINE);
    text(cp_rx + 8, cp_fy + 86, "current", COL_DIM);
    /* Zahlenfelder */
    for (i = 0; i < 7; i++) {
        LONG fy = cp_fy + 116 + i * 26 + (i >= 3 ? 6 : 0) + (i == 6 ? 6 : 0);
        text(cp_rx, fy + 3, cp_lab[i], COL_TEXT);
        fill(cp_rx + 24, fy, i == 6 ? 70 : 46, 22, COL_FIELD);
        frame(cp_rx + 24, fy, i == 6 ? 70 : 46, 22, COL_LINE);
        if (i == 6)
            hex6(buf, c);
        else
            sprintf(buf, "%ld", (long)cp_value(i));
        text(cp_rx + 30, fy + 3, buf, COL_TEXT);
        if (i < 3)
            text(cp_rx + 76, fy + 3, i ? "%" : "°", COL_DIM);
    }
    /* Knoepfe */
    fill(cp_bx, cp_by, 100, 26, COL_ACCENT);
    frame(cp_bx, cp_by, 100, 26, COL_LINE);
    text(cp_bx + (100 - text_width("OK")) / 2, cp_by + 4, "OK", 0xFFFFFFFF);
    button(cp_bx, cp_by + 34, 100, 26, "Cancel", FALSE);
}

static LONG cp_field_y(LONG i)
{
    return cp_fy + 116 + i * 26 + (i >= 3 ? 6 : 0) + (i == 6 ? 6 : 0);
}

static void cp_edit(LONG i)
{
    char buf[16];
    ULONG c = cp_color();
    LONG v = 0, n = 0;
    const char *b;
    if (i == 6)
        hex6(buf, c);
    else
        sprintf(buf, "%ld", (long)cp_value(i));
    if (!ui_edit_text(cp_rx + 24, cp_field_y(i), i == 6 ? 70 : 46, buf, i == 6 ? 7 : 4))
        return;
    for (b = buf; *b == ' ' || *b == '#'; b++)
        ;
    if (i == 6) {
        for (; *b; b++) {
            LONG d = *b >= '0' && *b <= '9' ? *b - '0' : *b >= 'a' && *b <= 'f' ? *b - 'a' + 10 :
                     *b >= 'A' && *b <= 'F' ? *b - 'A' + 10 : -1;
            if (d < 0)
                break;
            v = v * 16 + d;
            n++;
        }
        if (n == 6)
            rgb_hsv(v, &cp_hue, &cp_sat, &cp_val);
        return;
    }
    for (; *b >= '0' && *b <= '9'; b++, n++)
        v = v * 10 + (*b - '0');
    if (!n)
        return;
    switch (i) {
    case 0: cp_hue = (float)(v % 360); break;
    case 1: cp_sat = (v > 100 ? 100 : v) / 100.0f; break;
    case 2: cp_val = (v > 100 ? 100 : v) / 100.0f; break;
    default: {
        LONG sh = (5 - i) * 8;
        if (v > 255) v = 255;
        c = (c & ~(0xFFUL << sh)) | ((ULONG)v << sh);
        rgb_hsv(c, &cp_hue, &cp_sat, &cp_val);
        break; }
    }
}

/* Modaler Farbwaehler; TRUE und *color gesetzt bei OK */
BOOL ui_color_picker(const char *title, ULONG *color)
{
    BOOL done = FALSE, ok = FALSE, changed = TRUE;
    LONG drag = 0;              /* 1 = Feld, 2 = Farbton */

    cp_title = title;
    cp_old = *color & 0xFFFFFF;
    cp_hue = 0.0f;
    rgb_hsv(cp_old, &cp_hue, &cp_sat, &cp_val);
    cp_w = 16 + CP_N + 16 + 22 + 24 + 110 + 120;
    cp_h = 40 + CP_N + 60;
    cp_x0 = cvx + (cvw - cp_w) / 2;
    cp_y0 = cvy + 40;
    cp_fx = cp_x0 + 16;
    cp_fy = cp_y0 + 44;
    cp_hx = cp_fx + CP_N + 16;
    cp_rx = cp_hx + 22 + 24;
    cp_bx = cp_x0 + cp_w - 116;
    cp_by = cp_fy + 14;
    view_overlay_cb = cp_draw;

    while (!done) {
        struct IntuiMessage *im;
        if (changed) {
            cp_draw();
            blit(cp_x0, cp_y0, cp_w, cp_h);
        }
        changed = FALSE;
        if (Wait((1UL << win->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
            done = TRUE;
        }
        while (!done && (im = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG class = im->Class;
            UWORD code = im->Code;
            LONG mx = im->MouseX, my = im->MouseY, i;
            ReplyMsg((struct Message *)im);
            if (class == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                if (mx >= cp_fx && mx < cp_fx + CP_N && my >= cp_fy && my < cp_fy + CP_N)
                    drag = 1;
                else if (mx >= cp_hx - 6 && mx < cp_hx + 28 && my >= cp_fy && my < cp_fy + CP_N)
                    drag = 2;
                else if (mx >= cp_bx && mx < cp_bx + 100 && my >= cp_by && my < cp_by + 26)
                    ok = done = TRUE;
                else if (mx >= cp_bx && mx < cp_bx + 100 && my >= cp_by + 34 && my < cp_by + 60)
                    done = TRUE;
                else
                    for (i = 0; i < 7; i++)
                        if (mx >= cp_rx + 24 && mx < cp_rx + 24 + (i == 6 ? 70 : 46) &&
                            my >= cp_field_y(i) && my < cp_field_y(i) + 22)
                            cp_edit(i);
                changed = TRUE;
            } else if (class == IDCMP_MOUSEBUTTONS && code == SELECTUP)
                drag = 0;
            else if (class == IDCMP_RAWKEY) {
                if (code == 0x44 || code == 0x43)
                    ok = done = TRUE;
                else if (code == 0x45)
                    done = TRUE;
            }
            if (drag && (class == IDCMP_MOUSEMOVE || class == IDCMP_MOUSEBUTTONS)) {
                LONG px = mx - cp_fx, py = my - cp_fy;
                if (px < 0) px = 0;
                if (px > 255) px = 255;
                if (py < 0) py = 0;
                if (py > 255) py = 255;
                if (drag == 1) {
                    cp_sat = px / 255.0f;
                    cp_val = (255 - py) / 255.0f;
                } else
                    cp_hue = (255 - py) * 360.0f / 256.0f;
                changed = TRUE;
            }
        }
    }
    view_overlay_cb = NULL;
    if (ok)
        *color = cp_color();
    ui_draw_all();
    view_inval_all();
    view_flush();
    return ok;
}

/* ---- Histogramm zeichnen: Saeulen ausserhalb lo..hi abgedunkelt ---- */
static void hist_draw(const ULONG *h, LONG x0, LONG y0, LONG w, LONG ht, ULONG col, ULONG dim, LONG lo, LONG hi)
{
    ULONG mx = 0;
    LONG i;
    /* Spitzen an den Enden (reines Schwarz/Weiss) nicht als Massstab */
    for (i = 1; i < 255; i++)
        if (h[i] > mx)
            mx = h[i];
    if (!mx)
        mx = h[0] > h[255] ? h[0] : h[255];
    if (!mx)
        return;
    for (i = 0; i < 256; i++) {
        LONG bh = (LONG)((h[i] * (ULONG)ht + mx - 1) / mx);
        if (h[i] > mx)
            bh = ht;
        if (bh > 0)
            fill(x0 + i * w / 256, y0 + ht - bh, (i + 1) * w / 256 - i * w / 256, bh,
                 i < lo || i > hi ? dim : col);
    }
}

/* ---- Tonwertkorrektur-Dialog ---- */

#define LV_HH 128               /* Hoehe des Histogramms */
static LONG lv_x0, lv_y0, lv_w, lv_h, lv_gx, lv_gy, lv_ty, lv_oy, lv_by, lv_drag = -1;
static const char *cd_names[4];         /* Kanalnamen/-farben (beim Curves-Dialog definiert) */
static const ULONG cd_cols[4];
static LONG *lv_v;              /* Werte des gewaehlten Kanals */
static LONG *lv_master, (*lv_ch)[5], lv_c; /* RGB, R/G/B, gewaehlter Kanal 0..3 */

/* Position des Gamma-Dreiecks: dort ergibt die Kurve 50 % */
static LONG lv_gamma_x(void)
{
    float g = lv_v[1] / 100.0f;
    return lv_v[0] + (LONG)((lv_v[2] - lv_v[0]) * powf(0.5f, g) + 0.5f);
}

static void lv_tri(LONG cx, LONG ty, ULONG col, BOOL hot)
{
    LONG r;
    for (r = 0; r < 9; r++)
        fill(cx - r, ty + r, 2 * r + 1, 1, hot ? COL_ACCENT : 0xFF0A0A0A);
    for (r = 2; r < 8; r++)
        fill(cx - r + 1, ty + r, 2 * r - 1, 1, col);
}

static void lv_gradient(LONG y, LONG ht)
{
    LONG i;
    for (i = 0; i < 256; i++)
        fill(lv_gx + i, y, 1, ht, 0xFF000000 | (ULONG)i * 0x010101);
}

static void lv_field(LONG x, LONG y, LONG v, BOOL gamma)
{
    char buf[16];
    fill(x, y, 52, 22, COL_FIELD);
    frame(x, y, 52, 22, COL_LINE);
    if (gamma)
        sprintf(buf, "%ld.%02ld", (long)(v / 100), (long)(v % 100));
    else
        sprintf(buf, "%ld", (long)v);
    text(x + (52 - text_width(buf)) / 2, y + 3, buf, COL_TEXT);
}

/* x der fuenf Zahlenfelder */
static LONG lv_fx(LONG i)
{
    return i == 0 || i == 3 ? lv_gx - 4 : i == 1 ? lv_gx + 128 - 26 : lv_gx + 256 - 48;
}

static LONG lv_fy(LONG i)
{
    return i < 3 ? lv_ty + 14 : lv_oy + 24;
}

static void lv_draw(void)
{
    LONG i;
    gfx_noclip();
    fill(lv_x0, lv_y0, lv_w, lv_h, COL_BAR);
    frame(lv_x0, lv_y0, lv_w, lv_h, COL_LINE);
    fill(lv_x0 + 1, lv_y0 + 1, lv_w - 2, 26, COL_PANELHD);
    text_bold(lv_x0 + 12, lv_y0 + 5, "Levels", COL_TEXT);
    text(lv_x0 + 16, lv_y0 + 38, "Channel:", COL_DIM);
    for (i = 0; i < 4; i++)
        button(lv_x0 + 80 + i * 60, lv_y0 + 35, 56, 22, cd_names[i], i == lv_c);
    text(lv_x0 + 16, lv_y0 + 68, "Input Levels:", COL_DIM);
    fill(lv_gx - 1, lv_gy - 1, 258, LV_HH + 2, COL_LINE);
    fill(lv_gx, lv_gy, 256, LV_HH, COL_FIELD);
    hist_draw(hist[lv_c], lv_gx, lv_gy, 256, LV_HH, lv_c ? cd_cols[lv_c] : 0xFFB4B4B4, 0xFF5A5A5A, lv_v[0], lv_v[2]);
    lv_gradient(lv_gy + LV_HH + 3, 8);
    fill(lv_gx - 10, lv_ty, 276, 10, COL_BAR);
    lv_tri(lv_gx + lv_v[0], lv_ty, 0xFF000000, lv_drag == 0);
    lv_tri(lv_gx + lv_gamma_x(), lv_ty, 0xFF808080, lv_drag == 1);
    lv_tri(lv_gx + lv_v[2], lv_ty, 0xFFFFFFFF, lv_drag == 2);
    text(lv_x0 + 16, lv_oy - 22, "Output Levels:", COL_DIM);
    lv_gradient(lv_oy, 8);
    fill(lv_gx - 10, lv_oy + 10, 276, 10, COL_BAR);
    lv_tri(lv_gx + lv_v[3], lv_oy + 10, 0xFF000000, lv_drag == 3);
    lv_tri(lv_gx + lv_v[4], lv_oy + 10, 0xFFFFFFFF, lv_drag == 4);
    for (i = 0; i < 5; i++)
        lv_field(lv_fx(i), lv_fy(i), lv_v[i], i == 1);
    button(lv_x0 + 16, lv_by, 64, 26, "Auto", FALSE);
    button(lv_x0 + 86, lv_by, 64, 26, "Reset", FALSE);
    fill(lv_x0 + lv_w - 176, lv_by, 76, 26, COL_ACCENT);
    frame(lv_x0 + lv_w - 176, lv_by, 76, 26, COL_LINE);
    text(lv_x0 + lv_w - 176 + (76 - text_width("OK")) / 2, lv_by + 4, "OK", 0xFFFFFFFF);
    button(lv_x0 + lv_w - 94, lv_by, 80, 26, "Cancel", FALSE);
}

/* Schwarz-/Weisspunkt so, dass je 0,1 % der Pixel abgeschnitten werden */
static void lv_auto(void)
{
    ULONG total = 0, clip, sum;
    LONG lo, hi;
    for (lo = 0; lo < 256; lo++)
        total += hist[lv_c][lo];
    if (!total)
        return;
    clip = total / 1000;
    for (lo = 0, sum = 0; lo < 255 && (sum += hist[lv_c][lo]) <= clip; lo++)
        ;
    for (hi = 255, sum = 0; hi > 0 && (sum += hist[lv_c][hi]) <= clip; hi--)
        ;
    if (hi - lo < 2) {
        lo = lo > 253 ? 253 : lo;
        hi = lo + 2;
    }
    lv_v[0] = lo;
    lv_v[1] = 100;
    lv_v[2] = hi;
    lv_v[3] = 0;
    lv_v[4] = 255;
}

/* Gamma aus der Position des mittleren Dreiecks: 0,5^g = t (Bisektion) */
static void lv_set_gamma(LONG px)
{
    float t = (float)(px - lv_v[0]) / (float)(lv_v[2] - lv_v[0]), lo = 0.10f, hi = 9.99f;
    LONG i;
    if (t < 0.002f) t = 0.002f;
    if (t > 0.93f) t = 0.93f;
    for (i = 0; i < 24; i++) {
        float m = (lo + hi) * 0.5f;
        if (powf(0.5f, m) > t)
            lo = m;
        else
            hi = m;
    }
    lv_v[1] = (LONG)((lo + hi) * 50.0f + 0.5f);
    if (lv_v[1] < 10) lv_v[1] = 10;
    if (lv_v[1] > 999) lv_v[1] = 999;
}

static void lv_drag_to(LONG px)
{
    if (px < 0) px = 0;
    if (px > 255) px = 255;
    switch (lv_drag) {
    case 0: lv_v[0] = px > lv_v[2] - 2 ? lv_v[2] - 2 : px; break;
    case 1: lv_set_gamma(px); break;
    case 2: lv_v[2] = px < lv_v[0] + 2 ? lv_v[0] + 2 : px; break;
    case 3: lv_v[3] = px; break;
    case 4: lv_v[4] = px; break;
    }
}

/* Zahlenfeld i bearbeiten; Gamma mit Nachkommastellen */
static void lv_edit(LONG i)
{
    char buf[16];
    const char *b;
    LONG v = 0, n = 0, frac = 0, fd = 0;
    if (i == 1)
        sprintf(buf, "%ld.%02ld", (long)(lv_v[1] / 100), (long)(lv_v[1] % 100));
    else
        sprintf(buf, "%ld", (long)lv_v[i]);
    if (!ui_edit_text(lv_fx(i), lv_fy(i), 52, buf, 5))
        return;
    for (b = buf; *b == ' '; b++)
        ;
    for (; *b >= '0' && *b <= '9'; b++, n++)
        v = v * 10 + (*b - '0');
    if (i == 1 && (*b == '.' || *b == ','))
        for (b++; *b >= '0' && *b <= '9' && fd < 2; b++, fd++)
            frac = frac * 10 + (*b - '0');
    if (!n && !fd)
        return;
    if (i == 1) {
        v = v * 100 + (fd == 1 ? frac * 10 : frac);
        lv_v[1] = v < 10 ? 10 : v > 999 ? 999 : v;
        return;
    }
    if (v > 255)
        v = 255;
    if (i == 0)
        lv_v[0] = v > lv_v[2] - 2 ? lv_v[2] - 2 : v;
    else if (i == 2)
        lv_v[2] = v < lv_v[0] + 2 ? lv_v[0] + 2 : v;
    else
        lv_v[i] = v;
}

/* Kanal waehlen; neutraler (unbenutzter) Kanal bekommt Standardwerte */
static void lv_select(LONG c)
{
    static const LONG defs[5] = { 0, 100, 255, 0, 255 };
    lv_c = c;
    lv_v = c ? lv_ch[c - 1] : lv_master;
    if (!lv_v[1])
        memcpy(lv_v, defs, sizeof(defs));
}

BOOL levels_dialog(LONG *v, LONG (*ch)[5], void (*preview)(void))
{
    LONG saved[5], savedch[3][5], i;
    BOOL done = FALSE, ok = FALSE, changed = TRUE, need_preview = TRUE;
    ULONG preview_us = 0;

    memcpy(saved, v, sizeof(saved));
    memcpy(savedch, ch, sizeof(savedch));
    lv_master = v;
    lv_ch = ch;
    lv_select(0);
    lv_w = 256 + 84;
    lv_x0 = cvx + cvw - lv_w - 16;
    lv_y0 = cvy + 16;
    lv_gx = lv_x0 + 42;
    lv_gy = lv_y0 + 90;
    lv_ty = lv_gy + LV_HH + 12;
    lv_oy = lv_ty + 14 + 22 + 34;
    lv_by = lv_oy + 24 + 22 + 18;
    lv_h = lv_by - lv_y0 + 40;
    lv_drag = -1;
    view_overlay_cb = lv_draw;

    while (!done) {
        struct IntuiMessage *im;
        if (need_preview && (lv_drag < 0 || preview_us < 80000)) {
            ULONG t = now_us();
            preview();
            preview_us = now_us() - t;
            view_flush();
            need_preview = FALSE;
        }
        if (changed) {
            lv_draw();
            blit(lv_x0, lv_y0, lv_w, lv_h);
            changed = FALSE;
        }
        if (Wait((1UL << win->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
            done = TRUE;
        }
        while ((im = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG class = im->Class;
            UWORD code = im->Code;
            LONG mx = im->MouseX, my = im->MouseY;
            ReplyMsg((struct Message *)im);

            if (class == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                LONG px = mx - lv_gx;
                lv_drag = -1;
                if (my >= lv_ty - 4 && my < lv_ty + 12 && px >= -10 && px < 266) {
                    /* naechstes Dreieck greifen; bei Gleichstand das, das sich wegziehen laesst */
                    LONG xs[3], best = 1000;
                    xs[0] = lv_v[0]; xs[1] = lv_gamma_x(); xs[2] = lv_v[2];
                    for (i = 0; i < 3; i++) {
                        LONG d = px - xs[i];
                        if (d < 0) d = -d;
                        if (d < best || (d == best && ((i == 2 && px > xs[i]) || (i == 0 && px < xs[i])))) {
                            best = d;
                            lv_drag = i;
                        }
                    }
                } else if (my >= lv_oy + 6 && my < lv_oy + 22 && px >= -10 && px < 266) {
                    LONG d3 = px - lv_v[3], d4 = px - lv_v[4];
                    if (d3 < 0) d3 = -d3;
                    if (d4 < 0) d4 = -d4;
                    lv_drag = d3 < d4 || (d3 == d4 && px < lv_v[3]) ? 3 : 4;
                } else if (my >= lv_y0 + 35 && my < lv_y0 + 57 && mx >= lv_x0 + 80 && mx < lv_x0 + 80 + 4 * 60) {
                    lv_select((mx - lv_x0 - 80) / 60);
                    changed = TRUE;
                } else if (my >= lv_by && my < lv_by + 26) {
                    if (mx >= lv_x0 + 16 && mx < lv_x0 + 80) {
                        lv_auto();
                        changed = need_preview = TRUE;
                    } else if (mx >= lv_x0 + 86 && mx < lv_x0 + 150) {
                        static const LONG defs[5] = { 0, 100, 255, 0, 255 };
                        memcpy(lv_v, defs, sizeof(defs));
                        changed = need_preview = TRUE;
                    } else if (mx >= lv_x0 + lv_w - 176 && mx < lv_x0 + lv_w - 100)
                        ok = done = TRUE;
                    else if (mx >= lv_x0 + lv_w - 94 && mx < lv_x0 + lv_w - 14)
                        done = TRUE;
                } else
                    for (i = 0; i < 5; i++)
                        if (mx >= lv_fx(i) && mx < lv_fx(i) + 52 && my >= lv_fy(i) && my < lv_fy(i) + 22) {
                            lv_edit(i);
                            changed = need_preview = TRUE;
                        }
                if (lv_drag >= 0)
                    changed = TRUE;
            } else if (class == IDCMP_MOUSEMOVE && lv_drag >= 0) {
                lv_drag_to(mx - lv_gx);
                changed = need_preview = TRUE;
            } else if (class == IDCMP_MOUSEBUTTONS && code == SELECTUP && lv_drag >= 0) {
                lv_drag_to(mx - lv_gx);
                lv_drag = -1;
                changed = need_preview = TRUE;
            } else if (class == IDCMP_RAWKEY) {
                if (code == 0x44 || code == 0x43)
                    ok = done = TRUE;
                else if (code == 0x45)
                    done = TRUE;
            }
        }
    }
    view_overlay_cb = NULL;
    if (!ok) {
        memcpy(v, saved, sizeof(saved));
        memcpy(ch, savedch, sizeof(savedch));
    } else
        for (i = 0; i < 3; i++)         /* unveraenderte Kanaele bleiben "neutral" (0) */
            if (ch[i][0] == 0 && ch[i][1] == 100 && ch[i][2] == 255 && ch[i][3] == 0 && ch[i][4] == 255)
                memset(ch[i], 0, sizeof(ch[i]));
    ui_draw_all();
    view_inval_all();
    view_flush();
    return ok;
}

/* ---- Gradationskurven-Dialog ---- */

#define CG 256                  /* Groesse des Diagramms */
static LONG cd_x0, cd_y0, cd_w, cd_h, cd_gx, cd_gy, cd_ch, cd_drag = -1, cd_hx = -1, cd_hy;
static const char *cd_names[4] = { "RGB", "Red", "Green", "Blue" };
static const ULONG cd_cols[4] = { 0xFFE8E8E8, 0xFFFF5050, 0xFF50E050, 0xFF6090FF };

static void cd_draw(void)
{
    UBYTE t[256];
    LONG i, x, by = cd_y0 + cd_h - 40;
    char buf[48];
    Curve *c = &curves[cd_ch];

    gfx_noclip();
    fill(cd_x0, cd_y0, cd_w, cd_h, COL_BAR);
    frame(cd_x0, cd_y0, cd_w, cd_h, COL_LINE);
    fill(cd_x0 + 1, cd_y0 + 1, cd_w - 2, 26, COL_PANELHD);
    text_bold(cd_x0 + 12, cd_y0 + 5, "Curves", COL_TEXT);
    /* Kanalwahl */
    text(cd_x0 + 16, cd_y0 + 38, "Channel:", COL_DIM);
    for (i = 0; i < 4; i++)
        button(cd_x0 + 80 + i * 60, cd_y0 + 35, 56, 22, cd_names[i], i == cd_ch);
    /* Diagramm */
    fill(cd_gx - 1, cd_gy - 1, CG + 2, CG + 2, COL_LINE);
    fill(cd_gx, cd_gy, CG, CG, COL_FIELD);
    hist_draw(hist[cd_ch], cd_gx, cd_gy, CG, CG, 0xFF3C3C3C, 0xFF3C3C3C, 0, 255);
    for (i = 1; i < 4; i++) {
        fill(cd_gx + i * CG / 4, cd_gy, 1, CG, 0xFF383838);
        fill(cd_gx, cd_gy + i * CG / 4, CG, 1, 0xFF383838);
    }
    gfx_clip(cd_gx, cd_gy, cd_gx + CG, cd_gy + CG);
    line(cd_gx, cd_gy + CG - 1, cd_gx + CG - 1, cd_gy, 0xFF505050);   /* Diagonale */
    curve_table(c, t);
    for (x = 1; x < 256; x++)
        line(cd_gx + x - 1, cd_gy + 255 - t[x - 1], cd_gx + x, cd_gy + 255 - t[x], cd_cols[cd_ch]);
    for (i = 0; i < c->n; i++) {
        LONG px = cd_gx + c->x[i], py = cd_gy + 255 - c->y[i];
        fill(px - 3, py - 3, 7, 7, i == cd_drag ? cd_cols[cd_ch] : 0xFF000000);
        frame(px - 3, py - 3, 7, 7, cd_cols[cd_ch]);
    }
    gfx_noclip();
    /* Input / Output */
    fill(cd_x0 + 16, by - 30, 300, FONT_H, COL_BAR);
    if (cd_hx >= 0) {
        sprintf(buf, "Input: %ld    Output: %ld", (long)cd_hx, (long)cd_hy);
        text(cd_x0 + 16, by - 30, buf, COL_TEXT);
    }
    button(cd_x0 + 16, by, 80, 26, "Reset", FALSE);
    fill(cd_x0 + cd_w - 200, by, 86, 26, COL_ACCENT);
    frame(cd_x0 + cd_w - 200, by, 86, 26, COL_LINE);
    text(cd_x0 + cd_w - 200 + (86 - text_width("OK")) / 2, by + 4, "OK", 0xFFFFFFFF);
    button(cd_x0 + cd_w - 106, by, 92, 26, "Cancel", FALSE);
}

BOOL curves_dialog(void (*preview)(void))
{
    Curve saved[4];
    BOOL done = FALSE, ok = FALSE, changed = TRUE, need_preview = TRUE;
    ULONG preview_us = 0;
    LONG by, i;

    if (curves[0].n < 2)
        curves_reset();
    memcpy(saved, curves, sizeof(saved));
    cd_w = CG + 84;
    cd_h = 70 + CG + 90;
    cd_x0 = cvx + cvw - cd_w - 16;
    cd_y0 = cvy + 16;
    cd_gx = cd_x0 + (cd_w - CG) / 2;
    cd_gy = cd_y0 + 70;
    cd_drag = cd_hx = -1;
    by = cd_y0 + cd_h - 40;
    view_overlay_cb = cd_draw;

    while (!done) {
        struct IntuiMessage *im;
        if (need_preview && (cd_drag < 0 || preview_us < 80000)) {
            ULONG t = now_us();
            preview();
            preview_us = now_us() - t;
            view_flush();
            need_preview = FALSE;
        }
        if (changed) {
            cd_draw();
            blit(cd_x0, cd_y0, cd_w, cd_h);
            changed = FALSE;
        }
        if (Wait((1UL << win->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
            done = TRUE;
        }
        while ((im = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG class = im->Class;
            UWORD code = im->Code;
            LONG mx = im->MouseX, my = im->MouseY;
            Curve *c = &curves[cd_ch];
            LONG gx = mx - cd_gx, gy = 255 - (my - cd_gy);
            ReplyMsg((struct Message *)im);
            if (gx < 0) gx = 0;
            if (gx > 255) gx = 255;
            if (gy < 0) gy = 0;
            if (gy > 255) gy = 255;

            if (class == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                if (mx >= cd_gx - 4 && mx < cd_gx + CG + 4 && my >= cd_gy - 4 && my < cd_gy + CG + 4) {
                    /* vorhandenen Punkt greifen, sonst neuen setzen */
                    cd_drag = -1;
                    for (i = 0; i < c->n; i++)
                        if (mx >= cd_gx + c->x[i] - 5 && mx <= cd_gx + c->x[i] + 5 &&
                            my >= cd_gy + 255 - c->y[i] - 5 && my <= cd_gy + 255 - c->y[i] + 5)
                            cd_drag = i;
                    if (cd_drag < 0 && c->n < CURVE_MAXPTS) {
                        for (i = 0; i < c->n && c->x[i] < gx; i++)
                            ;
                        if (i < c->n && c->x[i] == gx) {
                            cd_drag = i;
                        } else {
                            memmove(&c->x[i + 1], &c->x[i], (c->n - i) * sizeof(LONG));
                            memmove(&c->y[i + 1], &c->y[i], (c->n - i) * sizeof(LONG));
                            c->x[i] = gx;
                            c->y[i] = gy;
                            c->n++;
                            cd_drag = i;
                            need_preview = TRUE;
                        }
                    }
                    cd_hx = gx;
                    cd_hy = gy;
                    changed = TRUE;
                } else if (my >= cd_y0 + 35 && my < cd_y0 + 57 && mx >= cd_x0 + 80 && mx < cd_x0 + 80 + 4 * 60) {
                    cd_ch = (mx - cd_x0 - 80) / 60;
                    cd_hx = -1;
                    changed = TRUE;
                } else if (my >= by && my < by + 26) {
                    if (mx >= cd_x0 + 16 && mx < cd_x0 + 96) {
                        curves_reset();
                        changed = need_preview = TRUE;
                    } else if (mx >= cd_x0 + cd_w - 200 && mx < cd_x0 + cd_w - 114)
                        ok = done = TRUE;
                    else if (mx >= cd_x0 + cd_w - 106 && mx < cd_x0 + cd_w - 14)
                        done = TRUE;
                }
            } else if ((class == IDCMP_MOUSEMOVE || (class == IDCMP_MOUSEBUTTONS && code == SELECTUP)) &&
                       cd_drag >= 0) {
                LONG k = cd_drag;
                /* weit nach oben/unten aus dem Diagramm ziehen: Punkt loeschen */
                if ((my < cd_gy - 24 || my > cd_gy + CG + 24) && k > 0 && k < c->n - 1) {
                    memmove(&c->x[k], &c->x[k + 1], (c->n - k - 1) * sizeof(LONG));
                    memmove(&c->y[k], &c->y[k + 1], (c->n - k - 1) * sizeof(LONG));
                    c->n--;
                    cd_drag = -1;
                    cd_hx = -1;
                } else {
                    /* x zwischen den Nachbarn halten */
                    LONG lo = k > 0 ? c->x[k - 1] + 1 : 0, hi = k < c->n - 1 ? c->x[k + 1] - 1 : 255;
                    c->x[k] = gx < lo ? lo : gx > hi ? hi : gx;
                    c->y[k] = gy;
                    cd_hx = c->x[k];
                    cd_hy = gy;
                }
                if (class == IDCMP_MOUSEBUTTONS)
                    cd_drag = -1;
                changed = need_preview = TRUE;
            } else if (class == IDCMP_RAWKEY) {
                if (code == 0x44 || code == 0x43)
                    ok = done = TRUE;
                else if (code == 0x45)
                    done = TRUE;
            }
        }
    }
    view_overlay_cb = NULL;
    if (!ok)
        memcpy(curves, saved, sizeof(saved));
    ui_draw_all();
    view_inval_all();
    view_flush();
    return ok;
}

/* ---- Aufklappliste (modal), NULL-Eintraege = Trennlinie ----
 * Passt die Liste nicht unter den Startpunkt, wird sie in Spalten umbrochen. */

#define POP_MAX 300
static LONG pop_x[POP_MAX], pop_y[POP_MAX];
static LONG pop_colw;

static LONG popup_item_at(const char **items, LONG n, LONG mx, LONG my)
{
    LONG i;
    for (i = 0; i < n; i++)
        if (items[i] && mx >= pop_x[i] && mx < pop_x[i] + pop_colw && my >= pop_y[i] && my < pop_y[i] + 22)
            return i;
    return -1;
}

LONG ui_popup(LONG x0, LONG y0, const char **items, LONG n, LONG cur)
{
    LONG i, w = 120, h, hover = cur, res = -1, avail, col = 0, y, maxy = 0, ncols;
    BOOL done = FALSE, redraw = TRUE;

    if (n > POP_MAX)
        n = POP_MAX;
    for (i = 0; i < n; i++)
        if (items[i] && text_width(items[i]) + 44 > w)
            w = text_width(items[i]) + 44;
    pop_colw = w;
    avail = SH - y0 - 12;
    /* Positionen: spaltenweise fuellen */
    y = y0 + 4;
    for (i = 0; i < n; i++) {
        LONG ih = items[i] ? 22 : 9;
        if (y + ih > y0 + avail && y > y0 + 4) {
            col++;
            y = y0 + 4;
            if (!items[i]) {        /* keine Trennlinie oben in einer Spalte */
                pop_x[i] = pop_y[i] = -1000;
                continue;
            }
        }
        pop_x[i] = x0 + col * w;
        pop_y[i] = y;
        y += ih;
        if (y > maxy)
            maxy = y;
    }
    ncols = col + 1;
    h = maxy - y0 + 4;
    if (x0 + ncols * w > SW) {
        LONG dx = x0 + ncols * w - SW;
        x0 -= dx;
        for (i = 0; i < n; i++)
            pop_x[i] -= dx;
    }
    w *= ncols;

    while (!done) {
        struct IntuiMessage *im;
        if (redraw) {
            gfx_noclip();
            fill(x0, y0, w, h, 0xFF424242);
            frame(x0, y0, w, h, COL_LINE);
            for (i = 0; i < n; i++) {
                if (pop_y[i] < 0)
                    continue;
                if (!items[i]) {
                    fill(pop_x[i] + 6, pop_y[i] + 4, pop_colw - 12, 1, 0xFF2C2C2C);
                    continue;
                }
                if (i == hover)
                    fill(pop_x[i] + 1, pop_y[i], pop_colw - 2, 22, COL_ACCENT);
                if (i == cur)
                    fill(pop_x[i] + 9, pop_y[i] + 8, 6, 6, COL_TEXT);  /* aktueller Eintrag */
                text(pop_x[i] + 24, pop_y[i] + 2, items[i], COL_TEXT);
            }
            blit(x0, y0, w, h);
            redraw = FALSE;
        }
        if (Wait((1UL << win->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
            done = TRUE;
        }
        while ((im = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG class = im->Class;
            UWORD code = im->Code;
            LONG mx = im->MouseX, my = im->MouseY, it;
            ReplyMsg((struct Message *)im);
            switch (class) {
            case IDCMP_MOUSEMOVE:
                it = popup_item_at(items, n, mx, my);
                if (it >= 0 && it != hover) {
                    hover = it;
                    redraw = TRUE;
                }
                break;
            case IDCMP_MOUSEBUTTONS:
                if (code == SELECTDOWN) {
                    res = popup_item_at(items, n, mx, my);
                    done = TRUE;        /* Klick daneben schliesst */
                }
                break;
            case IDCMP_RAWKEY:
                if (code == 0x45)
                    done = TRUE;
                else if (code == 0x44 || code == 0x43) {
                    res = hover;
                    done = TRUE;
                } else if (code == 0x4C || code == 0x4D) {      /* Pfeil hoch/runter */
                    LONG d = code == 0x4D ? 1 : -1, k = hover;
                    do
                        k += d;
                    while (k >= 0 && k < n && !items[k]);
                    if (k >= 0 && k < n) {
                        hover = k;
                        redraw = TRUE;
                    }
                }
                break;
            }
        }
    }
    ui_draw_all();
    view_inval_all();
    view_flush();
    return res;
}

/* ---- Textfeld (modal) ----
 * buf ist UTF-8; Eingaben kommen ueber die Tastaturbelegung als Latin-1. */

BOOL ui_edit_enter;             /* letzte Eingabe mit Return beendet */

BOOL ui_edit_text(LONG x, LONG y, LONG w, char *buf, LONG max)
{
    BOOL done = FALSE, ok = FALSE, all = buf[0] != 0;  /* all: Inhalt markiert */
    LONG h = 22;

    ui_edit_enter = FALSE;

    while (!done) {
        struct IntuiMessage *im;
        gfx_noclip();
        fill(x, y, w, h, COL_FIELD);
        frame(x, y, w, h, 0xFF6A8AC0);
        gfx_clip(x + 1, y + 1, x + w - 1, y + h - 1);
        if (all)
            fill(x + 3, y + 3, text_width(buf) + 2, h - 6, COL_ACCENT);
        text(x + 4, y + 2, buf, COL_TEXT);
        if (!all)
            fill(x + 4 + text_width(buf), y + 4, 1, 15, COL_TEXT);     /* Schreibmarke */
        gfx_noclip();
        blit(x, y, w, h);

        if (Wait((1UL << win->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
            done = TRUE;
        }
        while ((im = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG class = im->Class;
            UWORD code = im->Code, qual = im->Qualifier;
            LONG mx = im->MouseX, my = im->MouseY;
            ReplyMsg((struct Message *)im);
            if (class == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                if (mx < x || mx >= x + w || my < y || my >= y + h)
                    ok = done = TRUE;       /* Klick daneben uebernimmt */
            } else if (class == IDCMP_RAWKEY && !(code & 0x80)) {
                LONG len = strlen(buf);
                if (code == 0x44 || code == 0x43)
                    ok = done = ui_edit_enter = TRUE;
                else if (code == 0x45)
                    done = TRUE;
                else if (code == 0x41 && all) {             /* Markiertes loeschen */
                    buf[0] = 0;
                    all = FALSE;
                } else if (code == 0x41) {                  /* Backspace: ein UTF-8-Zeichen */
                    while (len > 0 && ((UBYTE)buf[len - 1] & 0xC0) == 0x80)
                        len--;
                    if (len > 0)
                        len--;
                    buf[len] = 0;
                } else if (code == 0x4E || code == 0x4F) { /* Pfeile: Markierung aufheben */
                    all = FALSE;
                } else {
                    struct InputEvent ie;
                    UBYTE c[4];
                    memset(&ie, 0, sizeof(ie));
                    ie.ie_Class = IECLASS_RAWKEY;
                    ie.ie_Code = code;
                    ie.ie_Qualifier = qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT |
                                              IEQUALIFIER_LALT | IEQUALIFIER_RALT);
                    if (MapRawKey(&ie, (STRPTR)c, sizeof(c), NULL) == 1 && c[0] >= 32 && c[0] != 127) {
                        if (all) {              /* erstes Zeichen ersetzt die Markierung */
                            buf[0] = 0;
                            len = 0;
                            all = FALSE;
                        }
                        if (c[0] < 128 && len + 1 < max) {
                            buf[len] = c[0];
                            buf[len + 1] = 0;
                        } else if (c[0] >= 160 && len + 2 < max) {
                            buf[len] = (char)(0xC0 | (c[0] >> 6));
                            buf[len + 1] = (char)(0x80 | (c[0] & 0x3F));
                            buf[len + 2] = 0;
                        }
                    }
                }
            }
        }
    }
    return ok;
}

const char *ui_tool_name(LONG t)
{
    return tool_names[t];
}

/* Geometrie */
static LONG nd_x0, nd_y0, nd_w, nd_h, nd_tab, nd_sel, nd_cols, nd_rows, nd_px;
static LONG nd_tabx[9];
static NewDocSpec *nd_s;
static char nd_preset[40];      /* Name der gewaehlten Vorlage (fuer Recent) */
#define ND_CW 132
#define ND_CH 92

static void nd_field(LONG x, LONG y, LONG w, const char *s, BOOL dropdown)
{
    fill(x, y, w, 24, COL_FIELD);
    frame(x, y, w, 24, COL_LINE);
    gfx_clip(x, y, x + w - (dropdown ? 18 : 4), y + 24);
    text(x + 8, y + 3, s, COL_TEXT);
    gfx_noclip();
    if (dropdown)
        down_arrow(x + w - 13, y + 10, COL_DIM);
}

static void nd_draw(void)
{
    LONG i, n, x, y, px = nd_px;
    const NPreset *pl = np_list(nd_tab, &n);
    char buf[64];
    gfx_noclip();
    fill(nd_x0, nd_y0, nd_w, nd_h, COL_BAR);
    frame(nd_x0, nd_y0, nd_w, nd_h, COL_LINE);
    text_bold(nd_x0 + 16, nd_y0 + 10, "New Document", COL_TEXT);
    fill(nd_x0 + 12, nd_y0 + 34, nd_w - 24, 1, COL_LINE);
    /* Reiter */
    x = nd_x0 + 16;
    for (i = 0; i < 8; i++) {
        LONG w = text_width(np_tabs[i]);
        nd_tabx[i] = x;
        text(x, nd_y0 + 46, np_tabs[i], i == nd_tab ? COL_TEXT : COL_DIM);
        if (i == nd_tab)
            fill(x, nd_y0 + 46 + FONT_H + 3, w, 2, COL_TEXT);
        x += w + 22;
    }
    nd_tabx[8] = x;
    fill(nd_x0 + 12, nd_y0 + 74, nd_w - 24, 1, COL_LINE);
    /* Vorlagen */
    sprintf(buf, "%s PRESETS (%ld)", nd_tab == 6 ? "AMIGA" : nd_tab == 0 ? "RECENT" : nd_tab == 7 ? "SAVED" : "BLANK DOCUMENT",
            (long)n);
    text(nd_x0 + 16, nd_y0 + 84, buf, COL_DIM);
    if (!n)
        text(nd_x0 + 16, nd_y0 + 112, nd_tab == 7 ? "No saved presets yet - use Save Preset."
                                                  : "No recent documents yet.", COL_DIM);
    for (i = 0; i < n && i < nd_cols * nd_rows; i++) {
        LONG cx = nd_x0 + 16 + (i % nd_cols) * (ND_CW + 8), cy = nd_y0 + 108 + (i / nd_cols) * (ND_CH + 8);
        LONG iw, ih;
        BOOL on = i == nd_sel;
        fill(cx, cy, ND_CW, ND_CH, on ? 0xFF5A5A5A : COL_PANEL);
        frame(cx, cy, ND_CW, ND_CH, on ? COL_ACCENT : COL_LINE);
        if (on)
            frame(cx + 1, cy + 1, ND_CW - 2, ND_CH - 2, COL_ACCENT);
        /* Seitenverhaeltnis */
        if (pl[i].w >= pl[i].h) {
            iw = 30;
            ih = 30 * pl[i].h / pl[i].w;
        } else {
            ih = 30;
            iw = 30 * pl[i].w / pl[i].h;
        }
        if (ih < 4) ih = 4;
        if (iw < 4) iw = 4;
        frame(cx + (ND_CW - iw) / 2, cy + 10 + (30 - ih) / 2, iw, ih, 0xFFD0D0D0);
        gfx_clip(cx + 4, cy, cx + ND_CW - 4, cy + ND_CH);
        text(cx + (ND_CW - text_width(pl[i].name)) / 2, cy + 46, pl[i].name, COL_TEXT);
        if (pl[i].colors) {
            char cd[24];
            color_depth_name(pl[i].colors, cd);
            sprintf(buf, "%ld x %ld, %s", (long)pl[i].w, (long)pl[i].h, cd);
        } else
            sprintf(buf, "%ld x %ld px", (long)pl[i].w, (long)pl[i].h);
        text(cx + (ND_CW - text_width(buf)) / 2, cy + 66, buf, COL_DIM);
        gfx_noclip();
    }
    /* Details */
    y = nd_y0 + 84;
    text(px, y, "PRESET DETAILS", COL_DIM);
    nd_field(px, y + 22, 268, nd_s->name, FALSE);
    text(px, y + 58, "Width", COL_DIM);
    sprintf(buf, "%ld", (long)nd_s->w);
    nd_field(px, y + 78, 110, buf, FALSE);
    text(px + 120, y + 81, "Pixels", COL_DIM);
    text(px, y + 112, "Height", COL_DIM);
    sprintf(buf, "%ld", (long)nd_s->h);
    nd_field(px, y + 132, 110, buf, FALSE);
    text(px + 120, y + 135, "Orientation", COL_DIM);
    /* Hoch / Quer */
    button(px + 206, y + 132, 26, 24, "", nd_s->h > nd_s->w);
    frame(px + 214, y + 137, 10, 14, COL_TEXT);
    button(px + 236, y + 132, 26, 24, "", nd_s->w >= nd_s->h);
    frame(px + 242, y + 140, 14, 10, COL_TEXT);
    text(px, y + 166, "Resolution", COL_DIM);
    sprintf(buf, "%ld", (long)nd_s->ppi);
    nd_field(px, y + 186, 110, buf, FALSE);
    text(px + 120, y + 189, "Pixels/Inch", COL_DIM);
    text(px, y + 220, "Color Mode", COL_DIM);
    np_label(nd_s->colors, buf);
    nd_field(px, y + 240, 268, buf, TRUE);
    text(px, y + 274, "Background Contents", COL_DIM);
    nd_field(px, y + 294, 232, np_bgs[nd_s->bg], TRUE);
    if (nd_s->bg == 3) {
        LONG k, j;
        for (j = 0; j < 4; j++)
            for (k = 0; k < 4; k++)
                fill(px + 242 + k * 6, y + 294 + j * 6, 6, 6, (j + k) & 1 ? 0xFFCCCCCC : 0xFFFFFFFF);
    } else
        fill(px + 242, y + 294, 24, 24, nd_s->bg == 0 ? 0xFFFFFFFF : nd_s->bg == 1 ? 0xFF000000 : 0xFF000000 | bg_color);
    frame(px + 242, y + 294, 24, 24, COL_LINE);
    button(px, y + 336, 268, 26, "Save Preset...", FALSE);
    /* Knoepfe */
    button(nd_x0 + nd_w - 216, nd_y0 + nd_h - 42, 96, 28, "Close", FALSE);
    fill(nd_x0 + nd_w - 112, nd_y0 + nd_h - 42, 96, 28, COL_ACCENT);
    frame(nd_x0 + nd_w - 112, nd_y0 + nd_h - 42, 96, 28, COL_LINE);
    text(nd_x0 + nd_w - 112 + (96 - text_width("Create")) / 2, nd_y0 + nd_h - 37, "Create", 0xFFFFFFFF);
}

static void nd_pick(const NPreset *p)
{
    strncpy(nd_preset, p->name, sizeof(nd_preset) - 1);
    nd_s->w = p->w;
    nd_s->h = p->h;
    nd_s->colors = p->colors;
    if (p->colors && nd_s->ppi > 100)
        nd_s->ppi = 72;
}

/* Zahl in einem Feld bearbeiten */
static void nd_edit_num(LONG x, LONG y, LONG *v, LONG lo, LONG hi)
{
    char buf[16];
    BOOL ok;
    LONG n;
    sprintf(buf, "%ld", (long)*v);
    if (!ui_edit_text(x, y, 110, buf, sizeof(buf)))
        return;
    n = parse_value(buf, 1, &ok);
    if (ok)
        *v = n < lo ? lo : n > hi ? hi : n;
}

BOOL ui_new_document(NewDocSpec *s)
{
    BOOL done = FALSE, ok = FALSE, changed = TRUE;
    char prefs[4096];
    BPTR fh;
    LONG n;
    nd_s = s;
    /* Recent/Saved aus den Prefs */
    if ((fh = Open("ENV:AmiPixelShop.prefs", MODE_OLDFILE))) {
        n = Read(fh, prefs, sizeof(prefs) - 1);
        Close(fh);
        prefs[n > 0 ? n : 0] = 0;
        np_load(prefs);
    }
    if (!s->ppi)
        s->ppi = 72;
    sprintf(s->name, "Untitled-%ld", (long)doc_next_untitled());
    strcpy(nd_preset, "Custom");
    nd_tab = np_nrecent ? 0 : 6;
    nd_sel = -1;
    nd_cols = 4;
    nd_w = 16 + nd_cols * (ND_CW + 8) + 16 + 268 + 16;
    nd_h = 108 + 4 * (ND_CH + 8) + 60;
    if (nd_h > SH - MENU_H - 8) {
        nd_rows = (SH - MENU_H - 8 - 168) / (ND_CH + 8);
        nd_h = 108 + nd_rows * (ND_CH + 8) + 60;
    } else
        nd_rows = 4;
    if (nd_h < 470)
        nd_h = 470;
    nd_x0 = (SW - nd_w) / 2;
    nd_y0 = MENU_H + (SH - MENU_H - nd_h) / 2;
    nd_px = nd_x0 + 16 + nd_cols * (ND_CW + 8) + 8;
    view_overlay_cb = nd_draw;

    while (!done) {
        struct IntuiMessage *im;
        if (changed) {
            nd_draw();
            blit(nd_x0, nd_y0, nd_w, nd_h);
        }
        changed = FALSE;
        if (Wait((1UL << win->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
            done = TRUE;
        }
        while (!done && (im = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG class = im->Class;
            UWORD code = im->Code;
            LONG mx = im->MouseX, my = im->MouseY, i, px = nd_px, y = nd_y0 + 84;
            ReplyMsg((struct Message *)im);
            if (class == IDCMP_RAWKEY) {
                if (code == 0x44 || code == 0x43)
                    ok = done = TRUE;
                else if (code == 0x45)
                    done = TRUE;
                continue;
            }
            if (class != IDCMP_MOUSEBUTTONS || code != SELECTDOWN)
                continue;
            changed = TRUE;
            /* Reiter */
            if (my >= nd_y0 + 40 && my < nd_y0 + 72) {
                for (i = 0; i < 8; i++)
                    if (mx >= nd_tabx[i] && mx < nd_tabx[i] + text_width(np_tabs[i]) + 12) {
                        nd_tab = i;
                        nd_sel = -1;
                    }
                continue;
            }
            /* Vorlagen */
            if (mx >= nd_x0 + 16 && mx < nd_px - 8 && my >= nd_y0 + 108) {
                LONG c = (mx - nd_x0 - 16) / (ND_CW + 8), r = (my - nd_y0 - 108) / (ND_CH + 8), k;
                const NPreset *pl = np_list(nd_tab, &n);
                k = r * nd_cols + c;
                if (c < nd_cols && r < nd_rows && k < n) {
                    nd_sel = k;
                    nd_pick(&pl[k]);
                }
                continue;
            }
            /* Details */
            if (mx >= px && mx < px + 268 && my < y + 366) {
                if (my >= y + 22 && my < y + 46)
                    ui_edit_text(px, y + 22, 268, s->name, sizeof(s->name));
                else if (my >= y + 78 && my < y + 102 && mx < px + 110) {
                    nd_edit_num(px, y + 78, &s->w, 1, 8000);
                    nd_sel = -1;
                    strcpy(nd_preset, "Custom");
                } else if (my >= y + 132 && my < y + 156 && mx < px + 110) {
                    nd_edit_num(px, y + 132, &s->h, 1, 8000);
                    nd_sel = -1;
                    strcpy(nd_preset, "Custom");
                } else if (my >= y + 132 && my < y + 156 && mx >= px + 206) {
                    BOOL portrait = mx < px + 234;
                    if ((portrait && s->w > s->h) || (!portrait && s->h > s->w)) {
                        LONG t = s->w;
                        s->w = s->h;
                        s->h = t;
                    }
                } else if (my >= y + 186 && my < y + 210 && mx < px + 110)
                    nd_edit_num(px, y + 186, &s->ppi, 1, 1200);
                else if (my >= y + 240 && my < y + 264) {
                    const char *items[11];
                    static char lab[11][40];
                    LONG cur = 0, r;
                    for (i = 0; i < 11; i++) {
                        np_label(np_depths[i], lab[i]);
                        items[i] = lab[i];
                        if (np_depths[i] == s->colors)
                            cur = i;
                    }
                    r = ui_popup(px, y + 264, items, 11, cur);
                    if (r >= 0)
                        s->colors = np_depths[r];
                } else if (my >= y + 294 && my < y + 318 && mx < px + 232) {
                    LONG r = ui_popup(px, y + 318, np_bgs, 4, s->bg);
                    if (r >= 0)
                        s->bg = r;
                } else if (my >= y + 336 && my < y + 362) {
                    np_push(np_saved, np_sname, &np_nsaved, s->name, s->w, s->h, s->colors);
                    ui_prefs_save();
                    nd_tab = 7;
                    nd_sel = 0;
                }
                continue;
            }
            /* Close / Create */
            if (my >= nd_y0 + nd_h - 42 && my < nd_y0 + nd_h - 14) {
                if (mx >= nd_x0 + nd_w - 216 && mx < nd_x0 + nd_w - 120)
                    done = TRUE;
                else if (mx >= nd_x0 + nd_w - 112 && mx < nd_x0 + nd_w - 16)
                    ok = done = TRUE;
            }
        }
    }
    if (ok) {
        np_push(np_recent, np_rname, &np_nrecent, nd_preset, s->w, s->h, s->colors);
        ui_prefs_save();
    }
    view_overlay_cb = NULL;
    ui_draw_all();
    view_inval_all();
    view_flush();
    return ok;
}
