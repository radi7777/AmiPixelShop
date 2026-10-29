/*
 * text.c - Horizontal Type Tool
 *
 * TrueType/OpenType ueber stb_truetype. Klick setzt die Grundlinie, getippt
 * wird direkt ins Bild auf eine neue Ebene (live gerendert, geglaettet, mit
 * Kerning). Uebernehmen legt einen Undo-Eintrag an; die Ebene bleibt eine
 * Textebene (TextInfo). Gedrehter/gespiegelter Text (Matrix m um den Anker)
 * wird ungedreht in einen Deckungspuffer gerendert und dann gedreht gestempelt.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosasl.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "apx.h"
#include "stb_truetype.h"

LONG text_size = 48;
LONG text_font = -1;
LONG text_align, text_aa, text_track, text_lead;

#define MAX_FONTS 256
static char *font_path[MAX_FONTS];
static char *font_name[MAX_FONTS];
static LONG nfonts;
static BOOL scanned;

static LONG loaded = -1;        /* Index der geladenen Schrift */
static UBYTE *font_data;
static stbtt_fontinfo finfo;

static BOOL tx_on;
static BOOL tx_editing;         /* vorhandene Textebene wird bearbeitet */
static BOOL tx_saved;           /* alte Kacheln liegen schon im Undo */
static LONG tx_bx0, tx_by0, tx_bx1, tx_by1;    /* Umriss des gerenderten Textes */
static Layer *tx_layer;
static UndoRec *tx_undo;
static char tx_buf[1024];       /* UTF-8 */
static LONG tx_x, tx_y;         /* Grundlinie der ersten Zeile (Dokument) */
static LONG car_x, car_y0, car_y1;     /* Schreibmarke (Dokument) */
static LONG cv_x0, cv_y0, cv_x1, cv_y1; /* Overlay-Box am Schirm */
static LONG tx_boxw, tx_boxh;   /* Absatztext: Rahmen ab (tx_x, tx_y), 0 = Punkttext */
static float tx_scale;
static float tx_m[4] = { 1.0f, 0.0f, 0.0f, 1.0f };     /* Drehen/Spiegeln um (tx_x, tx_y) */
static BOOL tx_xf;              /* tx_m ist nicht die Einheitsmatrix */
static UBYTE *cov;              /* gedrehter Text: Deckung ungedreht, Umriss cov_x0.. */
static LONG cov_x0, cov_y0, cov_w, cov_h;
static BOOL measuring;          /* erster Durchgang: nur Umriss */

BOOL text_active(void)
{
    return tx_on;
}

/* ---- Schriften finden ---- */

static void scan_dir(const char *pattern)
{
    struct AnchorPath *ap = AllocVec(sizeof(struct AnchorPath) + 256, MEMF_ANY | MEMF_CLEAR);
    LONG err;
    if (!ap)
        return;
    ap->ap_Strlen = 256;
    for (err = MatchFirst((STRPTR)pattern, ap); !err && nfonts < MAX_FONTS; err = MatchNext(ap)) {
        char *p, *n, *dot;
        LONG l = strlen((char *)ap->ap_Buf);
        if (ap->ap_Info.fib_DirEntryType > 0)
            continue;
        if (!(p = malloc(l + 1)) || !(n = malloc(strlen((char *)ap->ap_Info.fib_FileName) + 1))) {
            if (p) free(p);
            break;
        }
        strcpy(p, (char *)ap->ap_Buf);
        strcpy(n, (char *)ap->ap_Info.fib_FileName);
        if ((dot = strrchr(n, '.')))
            *dot = 0;
        font_path[nfonts] = p;
        font_name[nfonts] = n;
        nfonts++;
    }
    MatchEnd(ap);
    FreeVec(ap);
}

static int cmp_font(const void *a, const void *b)
{
    LONG i = *(const LONG *)a, j = *(const LONG *)b;
    return strcasecmp(font_name[i], font_name[j]);
}

static void scan_fonts(void)
{
    LONG idx[MAX_FONTS], i;
    char *p[MAX_FONTS], *n[MAX_FONTS];
    if (scanned)
        return;
    scanned = TRUE;
    scan_dir("PROGDIR:Fonts/#?.(ttf|otf)");
    scan_dir("FONTS:truetype/#?.(ttf|otf)");
    scan_dir("FONTS:_ttf/#?.(ttf|otf)");
    /* alphabetisch sortieren */
    for (i = 0; i < nfonts; i++)
        idx[i] = i;
    qsort(idx, nfonts, sizeof(LONG), cmp_font);
    for (i = 0; i < nfonts; i++) {
        p[i] = font_path[idx[i]];
        n[i] = font_name[idx[i]];
    }
    for (i = 0; i < nfonts; i++) {
        font_path[i] = p[i];
        font_name[i] = n[i];
    }
    /* Vorgabe: eine schlichte Sans */
    for (i = 0; i < nfonts && text_font < 0; i++)
        if (!strcasecmp(font_name[i], "Roboto-Regular") || !strcasecmp(font_name[i], "VeraSans"))
            text_font = i;
    if (text_font < 0 && nfonts)
        text_font = 0;
}

LONG text_font_count(void)
{
    scan_fonts();
    return nfonts;
}

const char *text_font_name(LONG i)
{
    scan_fonts();
    return i >= 0 && i < nfonts ? font_name[i] : "(no font)";
}

static BOOL load_font(LONG i)
{
    BPTR fh;
    LONG size;
    if (i == loaded)
        return font_data != NULL;
    if (font_data) {
        FreeVec(font_data);
        font_data = NULL;
    }
    loaded = i;
    if (i < 0 || i >= nfonts || !(fh = Open(font_path[i], MODE_OLDFILE)))
        return FALSE;
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size > 0 && (font_data = AllocVec(size, MEMF_ANY))) {
        if (Read(fh, font_data, size) != size ||
            !stbtt_InitFont(&finfo, font_data, stbtt_GetFontOffsetForIndex(font_data, 0))) {
            FreeVec(font_data);
            font_data = NULL;
        }
    }
    Close(fh);
    return font_data != NULL;
}

void text_free(void)
{
    LONG i;
    if (font_data)
        FreeVec(font_data);
    font_data = NULL;
    for (i = 0; i < nfonts; i++) {
        free(font_path[i]);
        free(font_name[i]);
    }
    nfonts = 0;
}

/* ---- Rendern ---- */

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

static void put(LONG x, LONG y, ULONG a)
{
    LONG idx;
    Tile *t;
    if (text_aa == 2)           /* None: harte Kanten */
        a = a >= 128 ? 255 : 0;
    else if (text_aa == 1) {    /* Sharp: steilere Kante */
        LONG v = ((LONG)a - 48) * 255 / 160;
        a = v < 0 ? 0 : v > 255 ? 255 : v;
    }
    if (x < 0 || y < 0 || x >= doc->w || y >= doc->h || !a)
        return;
    idx = (y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT);
    if (!(t = tx_layer->tiles[idx])) {
        if (!(t = tile_new()))
            return;
        tx_layer->tiles[idx] = t;
    }
    {
        ULONG *p = &t->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)];
        *p = over(*p, (a << 24) | (fg_color & 0xFFFFFF));
    }
}

/* Glyphenpixel: ungedreht direkt in die Ebene, sonst in den Deckungspuffer */
static void plot(LONG x, LONG y, ULONG a)
{
    UBYTE *c;
    if (!tx_xf) {
        put(x, y, a);
        return;
    }
    x -= cov_x0;
    y -= cov_y0;
    if (x < 0 || y < 0 || x >= cov_w || y >= cov_h || !a)
        return;
    c = &cov[y * cov_w + x];
    *c = (UBYTE)(*c + a - (*c * a + 127) / 255);  /* Vereinigung wie "over" bei gleicher Farbe */
}

/* Punkt (Dokument, ungedreht) -> gedreht um den Anker */
static void xf_point(float x, float y, float *ox, float *oy)
{
    float dx = x - tx_x, dy = y - tx_y;
    *ox = tx_x + tx_m[0] * dx + tx_m[1] * dy;
    *oy = tx_y + tx_m[2] * dx + tx_m[3] * dy;
}

static void set_matrix(const LONG *m)
{
    if (m && (m[0] || m[1] || m[2] || m[3])) {
        LONG i;
        for (i = 0; i < 4; i++)
            tx_m[i] = m[i] / 65536.0f;
    } else {
        tx_m[0] = tx_m[3] = 1.0f;
        tx_m[1] = tx_m[2] = 0.0f;
    }
    tx_xf = !(tx_m[0] == 1.0f && tx_m[1] == 0.0f && tx_m[2] == 0.0f && tx_m[3] == 1.0f);
}

/* Deckungspuffer gedreht in die Ebene; Umriss tx_bx0.. wird der gedrehte */
static void stamp_transformed(void)
{
    float det = tx_m[0] * tx_m[3] - tx_m[1] * tx_m[2], ia, ib, ic, id;
    float xs[4], ys[4], fx0 = 1e9f, fy0 = 1e9f, fx1 = -1e9f, fy1 = -1e9f;
    LONG k, x, y, X0, Y0, X1, Y1;
    if (det > -1e-6f && det < 1e-6f)
        return;
    ia = tx_m[3] / det; ib = -tx_m[1] / det; ic = -tx_m[2] / det; id = tx_m[0] / det;
    xf_point((float)cov_x0, (float)cov_y0, &xs[0], &ys[0]);
    xf_point((float)(cov_x0 + cov_w), (float)cov_y0, &xs[1], &ys[1]);
    xf_point((float)cov_x0, (float)(cov_y0 + cov_h), &xs[2], &ys[2]);
    xf_point((float)(cov_x0 + cov_w), (float)(cov_y0 + cov_h), &xs[3], &ys[3]);
    for (k = 0; k < 4; k++) {
        if (xs[k] < fx0) fx0 = xs[k];
        if (xs[k] > fx1) fx1 = xs[k];
        if (ys[k] < fy0) fy0 = ys[k];
        if (ys[k] > fy1) fy1 = ys[k];
    }
    X0 = (LONG)floorf(fx0) - 1; Y0 = (LONG)floorf(fy0) - 1;
    X1 = (LONG)ceilf(fx1) + 1;  Y1 = (LONG)ceilf(fy1) + 1;
    tx_bx0 = X0; tx_by0 = Y0; tx_bx1 = X1; tx_by1 = Y1;
    if (X0 < 0) X0 = 0;
    if (Y0 < 0) Y0 = 0;
    if (X1 > doc->w) X1 = doc->w;
    if (Y1 > doc->h) Y1 = doc->h;
    for (y = Y0; y < Y1; y++)
        for (x = X0; x < X1; x++) {
            /* Pixelmitte zurueck in den ungedrehten Text, bilinear */
            float dx = x + 0.5f - tx_x, dy = y + 0.5f - tx_y;
            float u = tx_x + ia * dx + ib * dy - cov_x0 - 0.5f, v = tx_y + ic * dx + id * dy - cov_y0 - 0.5f;
            LONG iu = (LONG)floorf(u), iv = (LONG)floorf(v), fu, fv, a;
            ULONG p00, p10, p01, p11;
            if (iu < -1 || iv < -1 || iu >= cov_w || iv >= cov_h)
                continue;
            fu = (LONG)((u - iu) * 256.0f);
            fv = (LONG)((v - iv) * 256.0f);
#define CV(xx, yy) ((xx) >= 0 && (yy) >= 0 && (xx) < cov_w && (yy) < cov_h ? cov[(yy) * cov_w + (xx)] : 0)
            p00 = CV(iu, iv); p10 = CV(iu + 1, iv); p01 = CV(iu, iv + 1); p11 = CV(iu + 1, iv + 1);
#undef CV
            a = ((p00 * (256 - fu) + p10 * fu) * (256 - fv) + (p01 * (256 - fu) + p11 * fu) * fv + 32768) >> 16;
            if (a)
                put(x, y, a);
        }
}

static ULONG next_cp(const UBYTE **sp)
{
    const UBYTE *s = *sp;
    ULONG c = *s++;
    if ((c & 0xE0) == 0xC0 && (*s & 0xC0) == 0x80)
        c = ((c & 0x1F) << 6) | (*s++ & 0x3F);
    *sp = s;
    return c;
}

/* Punkt des ungedrehten Textes -> Bildschirm */
static void scr_point(float x, float y, LONG *sx, LONG *sy)
{
    float ox, oy;
    xf_point(x, y, &ox, &oy);
    *sx = doc2scrx(ox);
    *sy = doc2scry(oy);
}

/* Bildschirm-Umriss der vier Ecken eines (ungedrehten) Rechtecks, um r erweitert */
static void scr_box(float x0, float y0, float x1, float y1, LONG r, LONG *bx0, LONG *by0, LONG *bx1, LONG *by1)
{
    LONG xs[4], ys[4], k;
    scr_point(x0, y0, &xs[0], &ys[0]);
    scr_point(x1, y0, &xs[1], &ys[1]);
    scr_point(x0, y1, &xs[2], &ys[2]);
    scr_point(x1, y1, &xs[3], &ys[3]);
    *bx0 = *bx1 = xs[0];
    *by0 = *by1 = ys[0];
    for (k = 1; k < 4; k++) {
        if (xs[k] < *bx0) *bx0 = xs[k];
        if (xs[k] > *bx1) *bx1 = xs[k];
        if (ys[k] < *by0) *by0 = ys[k];
        if (ys[k] > *by1) *by1 = ys[k];
    }
    *bx0 -= r; *by0 -= r; *bx1 += r + 1; *by1 += r + 1;
}

static void caret_inval(void)
{
    LONG a, b, c, d;
    view_inval(cv_x0, cv_y0, cv_x1, cv_y1);
    if (tx_boxw > 0) {
        scr_box((float)tx_x, (float)tx_y, (float)(tx_x + tx_boxw), (float)(tx_y + tx_boxh), 2, &a, &b, &c, &d);
        view_inval(a, b, c, d);
    }
    scr_box((float)car_x, (float)car_y0, (float)car_x, (float)car_y1, 2, &cv_x0, &cv_y0, &cv_x1, &cv_y1);
    view_inval(cv_x0, cv_y0, cv_x1, cv_y1);
}

/* Breite von s..e wie gezeichnet (Kerning, Tracking) */
static float seg_width(const UBYTE *s, const UBYTE *e)
{
    float w = 0.0f;
    ULONG prev = 0;
    while (s < e) {
        ULONG cp = next_cp(&s);
        int adv, lsb;
        if (prev)
            w += stbtt_GetCodepointKernAdvance(&finfo, prev, cp) * tx_scale + text_track * text_size / 1000.0f;
        stbtt_GetCodepointHMetrics(&finfo, cp, &adv, &lsb);
        w += adv * tx_scale;
        prev = cp;
    }
    return w;
}

/* naechste Zeile ab s: Ende (exklusiv); *next = Anfang der folgenden.
 * Absatztext bricht an Leerzeichen um, ueberlange Woerter zeichenweise. */
static const UBYTE *wrap_line(const UBYTE *s, const UBYTE **next)
{
    const UBYTE *e = s, *p, *best = NULL, *q, *ok;
    while (*e && *e != '\n')
        e++;
    if (tx_boxw <= 0 || seg_width(s, e) <= tx_boxw) {
        *next = *e ? e + 1 : e;
        return e;
    }
    for (p = s; p < e; p++)
        if (*p == ' ') {
            if (seg_width(s, p) <= tx_boxw)
                best = p;
            else
                break;
        }
    if (best && best > s) {
        for (q = best; *q == ' '; q++)
            ;
        *next = q;
        return best;
    }
    q = s;
    next_cp(&q);
    ok = q;                     /* mindestens ein Zeichen je Zeile */
    while (q < e) {
        const UBYTE *r = q;
        next_cp(&r);
        if (seg_width(s, r) > tx_boxw)
            break;
        q = ok = r;
    }
    *next = ok;
    return ok;
}

static void render(void)
{
    LONG i, n = doc->tw * doc->th, lh, pen_y = 0, pass;
    int asc, desc, gap;
    float scale, pen_x = 0.0f;
    const UBYTE *s;
    ULONG prev = 0;
    UBYTE *gbuf = NULL;
    LONG gsize = 0;

    /* alte Darstellung wegraeumen; beim Bearbeiten einer vorhandenen
     * Textebene wandern die Original-Kacheln einmalig ins Undo (alle
     * Indizes, damit Undo auch neu entstandene Kacheln wieder entfernt) */
    if (tx_editing && !tx_saved) {
        for (i = 0; i < n; i++) {
            undo_add(tx_undo, i, tx_layer->tiles[i]);
            if (tx_layer->tiles[i])
                doc_mark_dirty(i % doc->tw, i / doc->tw);
            tx_layer->tiles[i] = NULL;
        }
        tx_saved = TRUE;
    }
    for (i = 0; i < n; i++)
        if (tx_layer->tiles[i]) {
            FreeVec(tx_layer->tiles[i]);
            tx_layer->tiles[i] = NULL;
            doc_mark_dirty(i % doc->tw, i / doc->tw);
        }
    tx_bx0 = tx_by0 = 0x7FFFFFFF;
    tx_bx1 = tx_by1 = -0x7FFFFFFF;
    if (!load_font(text_font)) {
        car_x = tx_x;
        car_y0 = tx_y - text_size;
        car_y1 = tx_y;
        caret_inval();
        return;
    }
    scale = stbtt_ScaleForPixelHeight(&finfo, (float)text_size);
    tx_scale = scale;
    stbtt_GetFontVMetrics(&finfo, &asc, &desc, &gap);
    lh = text_lead > 0 ? text_lead : (LONG)((asc - desc + gap) * scale + 0.5f);
    /* gedreht: erst Umriss messen, dann ungedreht in den Deckungspuffer, dann drehen */
    for (pass = tx_xf ? 0 : 1; pass < 2; pass++) {
    measuring = pass == 0;
    if (pass == 1 && tx_xf) {
        if (cov)
            FreeVec(cov);
        cov_x0 = tx_bx0 - 2;
        cov_y0 = tx_by0 - 2;
        cov_w = tx_bx1 - tx_bx0 + 4;
        cov_h = tx_by1 - tx_by0 + 4;
        if (cov_w <= 4 || cov_h <= 4 || cov_w * cov_h > 16 * 1024 * 1024 ||
            !(cov = AllocVec(cov_w * cov_h, MEMF_ANY | MEMF_CLEAR))) {
            cov = NULL;
            cov_w = cov_h = 0;
        }
    }
    tx_bx0 = tx_by0 = 0x7FFFFFFF;
    tx_bx1 = tx_by1 = -0x7FFFFFFF;
    s = (const UBYTE *)tx_buf;
    /* Punkttext: Anker = Grundlinie; Absatztext: Anker = Rahmen oben links */
    pen_y = tx_boxw > 0 ? tx_y + (LONG)(asc * scale + 0.5f) : tx_y;
    pen_x = (float)tx_x;
    for (;;) {
        const UBYTE *nx, *e = wrap_line(s, &nx), *c;
        float wdt = seg_width(s, e);
        float ax = (float)tx_x + (tx_boxw > 0 ? (text_align == 1 ? tx_boxw * 0.5f : text_align == 2 ? tx_boxw : 0.0f) : 0.0f);
        BOOL vis = tx_boxw <= 0 || tx_boxh <= 0 || pen_y - (LONG)(desc * scale) <= tx_y + tx_boxh;
        pen_x = ax - (text_align == 1 ? wdt * 0.5f : text_align == 2 ? wdt : 0.0f);
        prev = 0;
        for (c = s; c < e; ) {
            ULONG cp = next_cp(&c);
            int adv, lsb, x0, y0, x1, y1, w, h, gx, gy;
            if (prev)
                pen_x += stbtt_GetCodepointKernAdvance(&finfo, prev, cp) * scale + text_track * text_size / 1000.0f;
            stbtt_GetCodepointHMetrics(&finfo, cp, &adv, &lsb);
            stbtt_GetCodepointBitmapBoxSubpixel(&finfo, cp, scale, scale, pen_x - floorf(pen_x), 0.0f,
                                                &x0, &y0, &x1, &y1);
            w = x1 - x0;
            h = y1 - y0;
            if (vis && w > 0 && h > 0) {
                LONG px0 = (LONG)floorf(pen_x), gx0 = px0 + x0, gy0 = pen_y + y0;
                if (gx0 < tx_bx0) tx_bx0 = gx0;
                if (gy0 < tx_by0) tx_by0 = gy0;
                if (gx0 + w > tx_bx1) tx_bx1 = gx0 + w;
                if (gy0 + h > tx_by1) tx_by1 = gy0 + h;
                if (w * h > gsize) {
                    if (gbuf)
                        FreeVec(gbuf);
                    gsize = w * h;
                    gbuf = AllocVec(gsize, MEMF_ANY);
                }
                if (gbuf && !measuring) {
                    stbtt_MakeCodepointBitmapSubpixel(&finfo, gbuf, w, h, w, scale, scale,
                                                      pen_x - floorf(pen_x), 0.0f, cp);
                    for (gy = 0; gy < h; gy++)
                        for (gx = 0; gx < w; gx++)
                            plot(px0 + x0 + gx, pen_y + y0 + gy, gbuf[gy * w + gx]);
                }
            }
            pen_x += adv * scale;
            prev = cp;
        }
        if (!*e)
            break;
        s = nx;
        pen_y += lh;
    }
    if (tx_boxw > 0) {          /* Rahmen gehoert zum Umriss (Klick zum Bearbeiten) */
        if (tx_x < tx_bx0) tx_bx0 = tx_x;
        if (tx_y < tx_by0) tx_by0 = tx_y;
        if (tx_x + tx_boxw > tx_bx1) tx_bx1 = tx_x + tx_boxw;
        if (tx_y + tx_boxh > tx_by1) tx_by1 = tx_y + tx_boxh;
    }
    }                           /* Durchgaenge */
    measuring = FALSE;
    if (tx_xf && cov) {
        stamp_transformed();
        FreeVec(cov);
        cov = NULL;
    }
    if (gbuf)
        FreeVec(gbuf);
    for (i = 0; i < n; i++)
        if (tx_layer->tiles[i]) {
            tile_update_flag(doc, tx_layer->tiles[i], i % doc->tw, i / doc->tw);
            doc_mark_dirty(i % doc->tw, i / doc->tw);
        }
    car_x = (LONG)pen_x;
    car_y0 = pen_y - (LONG)(asc * scale);
    car_y1 = pen_y - (LONG)(desc * scale);
    caret_inval();
}

/* Schreibmarke als Overlay */
static void text_overlay(void)
{
    if (tx_xf) {                /* gedreht: Rahmen und Schreibmarke als Linien */
        LONG ax, ay, bx, by, k;
        if (tx_boxw > 0) {
            static const BYTE cx[5] = { 0, 1, 1, 0, 0 }, cy[5] = { 0, 0, 1, 1, 0 };
            for (k = 0; k < 4; k++) {
                scr_point((float)(tx_x + cx[k] * tx_boxw), (float)(tx_y + cy[k] * tx_boxh), &ax, &ay);
                scr_point((float)(tx_x + cx[k + 1] * tx_boxw), (float)(tx_y + cy[k + 1] * tx_boxh), &bx, &by);
                line(ax, ay, bx, by, 0xFFFFFFFF);
            }
        }
        scr_point((float)car_x, (float)car_y0, &ax, &ay);
        scr_point((float)car_x, (float)car_y1, &bx, &by);
        line(ax, ay, bx, by, 0xFF000000);
        line(ax + 1, ay, bx + 1, by, 0xFFFFFFFF);
        return;
    }
    if (tx_boxw > 0) {
        LONG x0 = doc2scrx((float)tx_x), y0 = doc2scry((float)tx_y);
        LONG x1 = doc2scrx((float)(tx_x + tx_boxw)), y1 = doc2scry((float)(tx_y + tx_boxh));
        frame(x0 - 1, y0 - 1, x1 - x0 + 2, y1 - y0 + 2, 0xFF000000);
        frame(x0, y0, x1 - x0, y1 - y0, 0xFFFFFFFF);
    }
    LONG x = doc2scrx((float)car_x), y0 = doc2scry((float)car_y0), y1 = doc2scry((float)car_y1);
    fill(x, y0, 1, y1 - y0, 0xFF000000);
    fill(x + 1, y0, 1, y1 - y0, 0xFFFFFFFF);
}

/* ---- Sitzung ---- */

BOOL text_begin(LONG x, LONG y)
{
    LONG i;
    scan_fonts();
    if (doc->nlayers >= MAX_LAYERS) {
        status_message("Too many layers");
        return FALSE;
    }
    if (!nfonts) {
        status_message("No TrueType fonts found in FONTS:truetype");
        return FALSE;
    }
    if (!(tx_layer = layer_new(doc, "Text")))
        return FALSE;
    tx_undo = undo_begin_struct("Type Tool");
    for (i = doc->nlayers; i > doc->active + 1; i--)
        doc->layers[i] = doc->layers[i - 1];
    doc->layers[doc->active + 1] = tx_layer;
    doc->nlayers++;
    doc->active++;
    tx_buf[0] = 0;
    tx_x = x;
    tx_y = y;
    tx_boxw = tx_boxh = 0;
    set_matrix(NULL);
    tx_on = TRUE;
    tx_editing = FALSE;
    tx_saved = FALSE;
    cv_x0 = cv_x1 = 0;
    view_text_overlay = text_overlay;
    render();
    return TRUE;
}

static LONG font_index(const char *name)
{
    LONG i;
    scan_fonts();
    for (i = 0; i < nfonts; i++)
        if (!strcasecmp(font_name[i], name))
            return i;
    return -1;
}

/* vorhandene Textebene erneut bearbeiten */
BOOL text_begin_edit(Layer *l)
{
    LONG i, fi;
    if (!l || !l->text)
        return FALSE;
    for (i = 0; i < doc->nlayers && doc->layers[i] != l; i++)
        ;
    if (i == doc->nlayers)
        return FALSE;
    doc->active = i;
    doc->edit_mask = FALSE;
    if (!(tx_undo = undo_begin_struct("Edit Type Layer")))
        return FALSE;
    tx_undo->layer = l;
    tx_layer = l;
    strcpy(tx_buf, l->text->str);
    tx_x = l->text->x;
    tx_y = l->text->y;
    tx_boxw = l->text->boxw;
    tx_boxh = l->text->boxh;
    set_matrix(l->text->m);
    text_size = l->text->size;
    fg_color = l->text->color;
    text_align = l->text->align;
    text_aa = l->text->aa;
    text_track = l->text->tracking;
    text_lead = l->text->leading;
    if ((fi = font_index(l->text->font)) >= 0)
        text_font = fi;
    tx_on = TRUE;
    tx_editing = TRUE;
    tx_saved = FALSE;
    cv_x0 = cv_x1 = 0;
    view_text_overlay = text_overlay;
    render();
    return TRUE;
}

/* Textebene affin abbilden (Free Transform, Spiegeln, Drehen): Punkt p ->
 * (dx, dy) + L * (p - (sx, sy)). Der Text bleibt editierbar und wird neu
 * gerendert; gleichmaessiges Skalieren geht in die Schriftgroesse (scharf). */
BOOL text_transform(Layer *l, const float *L, float sx, float sy, float dx, float dy, const char *undo_name)
{
    float M[4], det = L[0] * L[3] - L[1] * L[2], sc, ax, ay;
    LONG nsize, k;
    if (det > -1e-6f && det < 1e-6f)
        return FALSE;
    if (!text_begin_edit(l))
        return FALSE;
    if (undo_name) {
        strncpy(tx_undo->name, undo_name, sizeof(tx_undo->name) - 1);
        tx_undo->name[sizeof(tx_undo->name) - 1] = 0;
    }
    /* Anker mitnehmen */
    ax = dx + L[0] * (tx_x - sx) + L[1] * (tx_y - sy);
    ay = dy + L[2] * (tx_x - sx) + L[3] * (tx_y - sy);
    tx_x = (LONG)floorf(ax + 0.5f);
    tx_y = (LONG)floorf(ay + 0.5f);
    /* neue Matrix = L * alte */
    M[0] = L[0] * tx_m[0] + L[1] * tx_m[2];
    M[1] = L[0] * tx_m[1] + L[1] * tx_m[3];
    M[2] = L[2] * tx_m[0] + L[3] * tx_m[2];
    M[3] = L[2] * tx_m[1] + L[3] * tx_m[3];
    /* gleichmaessiger Anteil (Spalten gleich lang und senkrecht) -> Schriftgroesse */
    {
        float c0 = sqrtf(M[0] * M[0] + M[2] * M[2]), c1 = sqrtf(M[1] * M[1] + M[3] * M[3]);
        float dot = M[0] * M[1] + M[2] * M[3];
        sc = c0;
        if (c0 > 0.0f && fabsf(c0 - c1) < 0.01f * c0 && fabsf(dot) < 0.01f * c0 * c1 && fabsf(sc - 1.0f) > 0.001f) {
            nsize = (LONG)floorf(text_size * sc + 0.5f);
            if (nsize < 1)
                nsize = 1;
            sc = (float)nsize / text_size;         /* tatsaechlicher Faktor */
            text_size = nsize;
            if (text_lead > 0)
                text_lead = (LONG)floorf(text_lead * sc + 0.5f);
            tx_boxw = (LONG)floorf(tx_boxw * sc + 0.5f);
            tx_boxh = (LONG)floorf(tx_boxh * sc + 0.5f);
            for (k = 0; k < 4; k++)
                M[k] /= sc;
        }
    }
    for (k = 0; k < 4; k++)
        tx_m[k] = M[k];
    /* fast Einheitsmatrix: genau setzen (keine Glaettung durch Resampling) */
    if (fabsf(M[0] - 1.0f) < 1e-4f && fabsf(M[3] - 1.0f) < 1e-4f && fabsf(M[1]) < 1e-4f && fabsf(M[2]) < 1e-4f) {
        tx_m[0] = tx_m[3] = 1.0f;
        tx_m[1] = tx_m[2] = 0.0f;
    }
    tx_xf = !(tx_m[0] == 1.0f && tx_m[1] == 0.0f && tx_m[2] == 0.0f && tx_m[3] == 1.0f);
    render();
    text_commit();
    return TRUE;
}

/* oberste sichtbare Textebene unter dem Punkt */
Layer *text_layer_at(LONG x, LONG y)
{
    LONG i;
    for (i = doc->nlayers - 1; i >= 0; i--) {
        Layer *l = doc->layers[i];
        TextInfo *t = l->text;
        if (l->visible && t && x >= t->bx0 - 4 && x < t->bx1 + 4 && y >= t->by0 - 4 && y < t->by1 + 4)
            return l;
    }
    return NULL;
}

/* Ebene wieder aus der Liste nehmen und freigeben */
static void drop_layer(void)
{
    LONG i;
    for (i = 0; i < doc->nlayers; i++)
        if (doc->layers[i] == tx_layer)
            break;
    if (i < doc->nlayers) {
        for (; i < doc->nlayers - 1; i++)
            doc->layers[i] = doc->layers[i + 1];
        doc->nlayers--;
        if (doc->active >= doc->nlayers)
            doc->active = doc->nlayers - 1;
    }
    layer_free(tx_layer, doc);
    doc_mark_all_dirty();
}

static void end_session(void)
{
    view_inval(cv_x0, cv_y0, cv_x1, cv_y1);
    if (tx_boxw > 0) {          /* Rahmen wegraeumen */
        LONG a, b, c, d;
        scr_box((float)tx_x, (float)tx_y, (float)(tx_x + tx_boxw), (float)(tx_y + tx_boxh), 2, &a, &b, &c, &d);
        view_inval(a, b, c, d);
    }
    view_text_overlay = NULL;
    tx_on = FALSE;
    tx_editing = FALSE;
    tx_layer = NULL;
    tx_undo = NULL;
}

void text_commit(void)
{
    LONG k;
    if (!tx_on)
        return;
    if (!tx_buf[0] && !tx_editing) {
        /* leerer Text: nichts anlegen (Zustand wie vorher, Eintrag verfaellt) */
        text_cancel();
        return;
    }
    if (!tx_layer->text)
        tx_layer->text = AllocVec(sizeof(TextInfo), MEMF_ANY | MEMF_CLEAR);
    if (tx_layer->text) {
        TextInfo *t = tx_layer->text;
        strcpy(t->str, tx_buf);
        strncpy(t->font, text_font_name(text_font), sizeof(t->font) - 1);
        t->font[sizeof(t->font) - 1] = 0;
        t->size = text_size;
        t->color = fg_color;
        t->x = tx_x;
        t->y = tx_y;
        t->boxw = tx_boxw;
        t->boxh = tx_boxh;
        t->align = text_align;
        t->aa = text_aa;
        t->tracking = text_track;
        t->leading = text_lead;
        t->bx0 = tx_bx0; t->by0 = tx_by0;
        t->bx1 = tx_bx1; t->by1 = tx_by1;
        for (k = 0; k < 4; k++)
            t->m[k] = tx_xf ? (LONG)floorf(tx_m[k] * 65536.0f + 0.5f) : 0;
    }
    {
        /* Ebenenname = Anfang des Textes (erste Zeile) */
        char *nl;
        strncpy(tx_layer->name, tx_buf, sizeof(tx_layer->name) - 1);
        tx_layer->name[sizeof(tx_layer->name) - 1] = 0;
        if ((nl = strchr(tx_layer->name, '\n')))
            *nl = 0;
    }
    undo_commit(tx_undo);
    end_session();
}

void text_cancel(void)
{
    if (!tx_on)
        return;
    if (tx_editing) {
        /* Original-Kacheln zuruecklegen, Eintrag verwerfen */
        LONG k;
        if (tx_undo && tx_saved)
            for (k = 0; k < tx_undo->n; k++) {
                LONG idx = tx_undo->idx[k];
                if (tx_layer->tiles[idx])
                    FreeVec(tx_layer->tiles[idx]);
                tx_layer->tiles[idx] = tx_undo->tiles[k];
                tx_undo->tiles[k] = NULL;
                doc_mark_dirty(idx % doc->tw, idx / doc->tw);
            }
        undo_discard(tx_undo);
        end_session();
        return;
    }
    drop_layer();
    if (tx_undo)
        undo_commit(tx_undo);   /* Zustand wie vorher: Eintrag wird verworfen */
    end_session();
}

/* Tastatur: Zeichen (Latin-1) oder Steuerung */
void text_key_char(UBYTE c)
{
    LONG len = strlen(tx_buf);
    if (!tx_on)
        return;
    if (c == '\b') {
        while (len > 0 && ((UBYTE)tx_buf[len - 1] & 0xC0) == 0x80)
            len--;
        if (len > 0)
            len--;
        tx_buf[len] = 0;
    } else if (c == '\n' || (c >= 32 && c < 127)) {
        if (len + 1 < (LONG)sizeof(tx_buf)) {
            tx_buf[len] = c;
            tx_buf[len + 1] = 0;
        }
    } else if (c >= 160 && len + 2 < (LONG)sizeof(tx_buf)) {
        tx_buf[len] = (char)(0xC0 | (c >> 6));
        tx_buf[len + 1] = (char)(0x80 | (c & 0x3F));
        tx_buf[len + 2] = 0;
    }
    render();
}

/* Schrift oder Groesse geaendert */
void text_refresh(void)
{
    if (tx_on)
        render();
}

BOOL text_layer_active(void)
{
    return tx_on || doc->layers[doc->active]->text != NULL;
}

/* Stil geaendert: laufenden Text neu zeichnen bzw. aktive Textebene neu setzen */
void text_restyle(void)
{
    Layer *l = doc->layers[doc->active];
    LONG al = text_align, aa = text_aa, tr = text_track, ld = text_lead, sz = text_size, fo = text_font;
    ULONG fg = fg_color;
    if (tx_on) {
        render();
        return;
    }
    if (!l->text || !text_begin_edit(l))
        return;
    strcpy(tx_undo->name, "Change Type Style");
    text_align = al; text_aa = aa; text_track = tr; text_lead = ld;
    render();
    text_commit();
    text_size = sz;
    text_font = fo;
    fg_color = fg;
}

/* Zeichenkette anhaengen (Paste Lorem Ipsum) */
void text_insert(const char *s)
{
    LONG len = strlen(tx_buf), n = strlen(s);
    if (!tx_on)
        return;
    if (len + n >= (LONG)sizeof(tx_buf))
        n = sizeof(tx_buf) - 1 - len;
    memcpy(tx_buf + len, s, n);
    tx_buf[len + n] = 0;
    render();
}

/* Properties-Panel: alle Textwerte der aktiven Textebene setzen */
void text_set_props(LONG size, LONG align, LONG track, LONG lead)
{
    Layer *l = doc->layers[doc->active];
    LONG sz = text_size, al = text_align, aa = text_aa, tr = text_track, ld = text_lead, fo = text_font;
    ULONG fg = fg_color;
    if (tx_on) {
        text_size = size; text_align = align; text_track = track; text_lead = lead;
        render();
        return;
    }
    if (!l->text || !text_begin_edit(l))
        return;
    strcpy(tx_undo->name, "Character Change");
    text_size = size; text_align = align; text_track = track; text_lead = lead;
    render();
    text_commit();
    text_size = sz; text_align = al; text_aa = aa; text_track = tr; text_lead = ld;
    text_font = fo;
    fg_color = fg;
}

/* Absatztext im Rahmen (x, y = oben links) */
BOOL text_begin_box(LONG x, LONG y, LONG w, LONG h)
{
    if (!text_begin(x, y))
        return FALSE;
    tx_boxw = w;
    tx_boxh = h;
    render();
    view_inval_all();
    return TRUE;
}

/* Rahmen beim Aufziehen zeigen (x1 < 0: beenden) */
static LONG pv_x0, pv_y0, pv_x1, pv_y1;

static void box_overlay(void)
{
    LONG x0 = doc2scrx((float)pv_x0), y0 = doc2scry((float)pv_y0);
    LONG x1 = doc2scrx((float)pv_x1), y1 = doc2scry((float)pv_y1);
    frame(x0 - 1, y0 - 1, x1 - x0 + 2, y1 - y0 + 2, 0xFF000000);
    frame(x0, y0, x1 - x0, y1 - y0, 0xFFFFFFFF);
}

void text_box_preview(LONG x0, LONG y0, LONG x1, LONG y1)
{
    view_inval_all();
    if (x1 < 0) {
        if (view_text_overlay == box_overlay)
            view_text_overlay = NULL;
        return;
    }
    pv_x0 = x0 < x1 ? x0 : x1; pv_x1 = x0 < x1 ? x1 : x0;
    pv_y0 = y0 < y1 ? y0 : y1; pv_y1 = y0 < y1 ? y1 : y0;
    view_text_overlay = box_overlay;
}
