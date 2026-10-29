/*
 * filter.c - Korrekturen (Bild-Menue) und Filter
 *
 * Gemeinsamer Ablauf fuer alle:
 *   begin   Kacheln im betroffenen Bereich sichern (Auswahl-Box oder Ebene)
 *   apply   Bereich + Rand aus den Originalen in einen Puffer holen
 *           (Nachbarschaftsfilter vormultipliziert), Filter rechnen,
 *           mit der Auswahl als weicher Maske zurueckschreiben
 *   commit  Originale ins Undo   /   cancel  Originale zurueckspielen
 * Die Vorschau im Dialog ruft apply bei jeder Reglerbewegung erneut auf.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "apx.h"
#include "quant.h"

typedef struct Filter {
    LONG cmd;
    const char *name;           /* Dialogtitel und Undo-Name */
    LONG n;                     /* Anzahl Regler (0 = ohne Dialog) */
    DlgSlider sl[6];
    LONG val[6];
    BOOL premul;                /* Nachbarschaftsfilter: vormultipliziert rechnen */
    LONG (*margin)(const LONG *v);
    void (*run)(ULONG *buf, LONG w, LONG h, const LONG *v, LONG ox, LONG oy);
    BOOL (*custom)(void (*preview)(void));     /* eigener Dialog statt Reglern */
} Filter;

/* ---- Sicherung / Anwenden ---- */

static Layer *fl_layer;
static Tile **fl_tiles;         /* Ebenen- oder Maskenkacheln */
static Tile **fl_orig;
static UBYTE *fl_has;
static LONG fl_x0, fl_y0, fl_x1, fl_y1;

static Tile *src_tile(LONG idx)
{
    return fl_has[idx] ? fl_orig[idx] : fl_tiles[idx];
}

static BOOL fl_begin(void)
{
    LONG n = doc->tw * doc->th, tx, ty;
    fl_layer = doc->layers[doc->active];
    doc_cache_below(doc->active);
    if (!doc_target_is_mask())
        doc_rasterize_text(fl_layer);
    fl_tiles = doc_target();
    if (doc->hassel) {
        fl_x0 = doc->selx0; fl_y0 = doc->sely0;
        fl_x1 = doc->selx1; fl_y1 = doc->sely1;
    } else {
        fl_x0 = fl_y0 = 0;
        fl_x1 = doc->w; fl_y1 = doc->h;
    }
    fl_orig = AllocVec(n * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR);
    fl_has = AllocVec(n, MEMF_ANY | MEMF_CLEAR);
    if (!fl_orig || !fl_has)
        goto fail;
    for (ty = fl_y0 >> TILE_SHIFT; ty <= (fl_y1 - 1) >> TILE_SHIFT; ty++)
        for (tx = fl_x0 >> TILE_SHIFT; tx <= (fl_x1 - 1) >> TILE_SHIFT; tx++) {
            LONG idx = ty * doc->tw + tx;
            Tile *t = fl_tiles[idx];
            if (t && !(fl_orig[idx] = tile_clone(t)))
                goto fail;
            fl_has[idx] = 1;
        }
    return TRUE;
fail:
    if (fl_orig) {
        LONG i;
        for (i = 0; i < n; i++)
            if (fl_orig[i])
                FreeVec(fl_orig[i]);
        FreeVec(fl_orig);
    }
    if (fl_has)
        FreeVec(fl_has);
    fl_orig = NULL;
    fl_has = NULL;
    return FALSE;
}

static void fl_free(void)
{
    if (fl_orig) FreeVec(fl_orig);
    if (fl_has) FreeVec(fl_has);
    fl_orig = NULL;
    fl_has = NULL;
}

static void fl_commit(const char *name)
{
    LONG i, n = doc->tw * doc->th;
    doc_cache_drop();
    UndoRec *u = undo_begin_target(name);
    for (i = 0; i < n; i++)
        if (fl_has[i])
            undo_add(u, i, fl_orig[i]);     /* gehoert jetzt dem Undo */
    undo_commit(u);
    fl_free();
}

static void fl_cancel(void)
{
    LONG i, n = doc->tw * doc->th;
    doc_cache_drop();
    for (i = 0; i < n; i++)
        if (fl_has[i]) {
            if (fl_tiles[i])
                FreeVec(fl_tiles[i]);
            fl_tiles[i] = fl_orig[i];
            doc_mark_dirty(i % doc->tw, i / doc->tw);
        }
    fl_free();
}

/* Zeile aus den Originalen lesen, ausserhalb des Dokuments = 0 */
static void read_row(LONG sx, LONG sy, LONG n, ULONG *dst)
{
    while (n > 0) {
        Tile *t = src_tile((sy >> TILE_SHIFT) * doc->tw + (sx >> TILE_SHIFT));
        LONG off = sx & TILE_MASK, k = TILE_SIZE - off;
        if (k > n)
            k = n;
        if (t)
            memcpy(dst, t->px + ((sy & TILE_MASK) << TILE_SHIFT) + off, k * 4);
        else
            memset(dst, 0, k * 4);
        dst += k;
        sx += k;
        n -= k;
    }
}

static void to_premul(ULONG *p, LONG n)
{
    LONG i;
    for (i = 0; i < n; i++) {
        ULONG v = p[i], a = v >> 24, a2;
        if (a == 255)
            continue;
        a2 = a + (a >> 7);
        p[i] = (a << 24) | ((((v & 0xFF00FF) * a2) >> 8) & 0xFF00FF)
                         | ((((v & 0x00FF00) * a2) >> 8) & 0x00FF00);
    }
}

static void from_premul(ULONG *p, LONG n)
{
    LONG i;
    for (i = 0; i < n; i++) {
        ULONG v = p[i], a = v >> 24, inv, r, g, b;
        if (a == 255)
            continue;
        if (!a) {
            p[i] = 0;
            continue;
        }
        inv = (255 * 65536 + a / 2) / a;
        r = (((v >> 16) & 0xFF) * inv) >> 16;
        g = (((v >> 8) & 0xFF) * inv) >> 16;
        b = ((v & 0xFF) * inv) >> 16;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        p[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

/* o + (r - o) * s / 255, je Kanal */
static ULONG lerp_px(ULONG o, ULONG r, ULONG s)
{
    ULONG out = 0;
    LONG sh;
    for (sh = 0; sh < 32; sh += 8) {
        LONG a = (o >> sh) & 0xFF, b = (r >> sh) & 0xFF;
        out |= (ULONG)(a + ((b - a) * (LONG)s + 127) / 255) << sh;
    }
    return out;
}

static BOOL fl_apply(Filter *f)
{
    LONG m = f->margin ? f->margin(f->val) : 0;
    LONG rx0 = fl_x0 - m, ry0 = fl_y0 - m, rx1 = fl_x1 + m, ry1 = fl_y1 + m;
    LONG rw, rh, y, tx, ty;
    ULONG *buf;

    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rx1 > doc->w) rx1 = doc->w;
    if (ry1 > doc->h) ry1 = doc->h;
    rw = rx1 - rx0;
    rh = ry1 - ry0;
    if (!(buf = AllocVec(rw * rh * 4, MEMF_ANY)))
        return FALSE;
    for (y = 0; y < rh; y++)
        read_row(rx0, ry0 + y, rw, buf + y * rw);
    if (f->premul)
        to_premul(buf, rw * rh);
    f->run(buf, rw, rh, f->val, rx0, ry0);
    if (f->premul)
        from_premul(buf, rw * rh);

    /* zurueckschreiben, nur innerhalb der Box, Auswahl als Maske */
    for (ty = fl_y0 >> TILE_SHIFT; ty <= (fl_y1 - 1) >> TILE_SHIFT; ty++)
        for (tx = fl_x0 >> TILE_SHIFT; tx <= (fl_x1 - 1) >> TILE_SHIFT; tx++) {
            LONG idx = ty * doc->tw + tx, x, yy;
            LONG ox0 = tx << TILE_SHIFT, oy0 = ty << TILE_SHIFT;
            LONG ax0 = fl_x0 > ox0 ? fl_x0 : ox0, ay0 = fl_y0 > oy0 ? fl_y0 : oy0;
            LONG ax1 = fl_x1 < ox0 + TILE_SIZE ? fl_x1 : ox0 + TILE_SIZE;
            LONG ay1 = fl_y1 < oy0 + TILE_SIZE ? fl_y1 : oy0 + TILE_SIZE;
            Tile *o = fl_orig[idx], *t = fl_tiles[idx];
            if (!doc->hassel) {
                /* ohne Auswahl: zeilenweise kopieren */
                if (!t) {
                    ULONG any = 0;
                    for (yy = ay0; yy < ay1 && !any; yy++) {
                        const ULONG *r = buf + (yy - ry0) * rw - rx0;
                        for (x = ax0; x < ax1; x++)
                            any |= r[x];
                    }
                    if (!any || !(t = tile_new())) {
                        doc_mark_dirty(tx, ty);
                        continue;
                    }
                    fl_tiles[idx] = t;
                }
                for (yy = ay0; yy < ay1; yy++)
                    CopyMem((APTR)(buf + (yy - ry0) * rw + (ax0 - rx0)),
                            t->px + ((yy & TILE_MASK) << TILE_SHIFT) + (ax0 & TILE_MASK), (ax1 - ax0) * 4);
                tile_update_flag(doc, t, tx, ty);
                if (t->flag == TF_EMPTY) {
                    FreeVec(t);
                    fl_tiles[idx] = NULL;
                }
                doc_mark_dirty(tx, ty);
                continue;
            }
            for (yy = ay0; yy < ay1; yy++) {
                const ULONG *r = buf + (yy - ry0) * rw - rx0;
                const UBYTE *sel = doc->hassel ? doc->sel + yy * doc->w : NULL;
                for (x = ax0; x < ax1; x++) {
                    LONG ti = ((yy & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK);
                    ULONG op = o ? o->px[ti] : 0, v = r[x];
                    if (sel)
                        v = sel[x] == 255 ? v : sel[x] ? lerp_px(op, v, sel[x]) : op;
                    if (!t) {
                        if (!v)
                            continue;
                        if (!(t = o ? tile_clone(o) : tile_new()))
                            continue;
                        fl_tiles[idx] = t;
                    }
                    t->px[ti] = v;
                }
            }
            if (t) {
                tile_update_flag(doc, t, tx, ty);
                if (t->flag == TF_EMPTY) {
                    FreeVec(t);
                    fl_tiles[idx] = NULL;
                }
            }
            doc_mark_dirty(tx, ty);
        }
    FreeVec(buf);
    return TRUE;
}

/* ---- Punktoperationen (Tabellen) ---- */

static UBYTE lut_r[256], lut_g[256], lut_b[256];

static void run_lut(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    LONG i, n = w * h;
    for (i = 0; i < n; i++) {
        ULONG c = p[i];
        p[i] = (c & 0xFF000000) | ((ULONG)lut_r[(c >> 16) & 0xFF] << 16)
             | ((ULONG)lut_g[(c >> 8) & 0xFF] << 8) | lut_b[c & 0xFF];
    }
}

static UBYTE clamp255(LONG v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (UBYTE)v;
}

static void lut_all(void)
{
    memcpy(lut_g, lut_r, 256);
    memcpy(lut_b, lut_r, 256);
}

static void run_bricon(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    LONG i;
    float c = v[1] >= 0 ? 1.0f + v[1] / 50.0f : 1.0f + v[1] / 100.0f;
    for (i = 0; i < 256; i++)
        lut_r[i] = clamp255((LONG)((i - 127.5f) * c + 127.5f + v[0] * 0.66f + 0.5f));
    lut_all();
    run_lut(p, w, h, v, ox, oy);
}

LONG levels_ch[3][5];
static const LONG (*lv_cur)[5] = levels_ch;    /* Kanalwerte fuer run_levels */

/* Tonwerttabelle aus Input Black, Gamma*100, Input White, Output Black, Output White */
static void level_table(const LONG *v, UBYTE *out)
{
    LONG i, ib = v[0], iw = v[2] > v[0] ? v[2] : v[0] + 1;
    float g = v[1] / 100.0f;
    for (i = 0; i < 256; i++) {
        float t = (float)(i - ib) / (float)(iw - ib);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        t = powf(t, 1.0f / g);
        out[i] = clamp255((LONG)(v[3] + t * (v[4] - v[3]) + 0.5f));
    }
}

static BOOL level_neutral(const LONG *v)
{
    return v[1] == 0 || (v[0] == 0 && v[1] == 100 && v[2] == 255 && v[3] == 0 && v[4] == 255);
}

/* erst der Kanal, dann der Gesamtwert (RGB) */
static void run_levels(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    UBYTE m[256], t[256];
    UBYTE *dst[3];
    LONG i, c;
    dst[0] = lut_r; dst[1] = lut_g; dst[2] = lut_b;
    level_table(v, m);
    for (c = 0; c < 3; c++) {
        if (lv_cur && !level_neutral(lv_cur[c])) {
            level_table(lv_cur[c], t);
            for (i = 0; i < 256; i++)
                dst[c][i] = m[t[i]];
        } else
            memcpy(dst[c], m, 256);
    }
    run_lut(p, w, h, v, ox, oy);
}

static void run_invert(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    LONG i, n = w * h;
    for (i = 0; i < n; i++)
        p[i] ^= 0x00FFFFFF;
}

static void run_poster(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    LONG i, n = v[0] < 2 ? 2 : v[0];
    for (i = 0; i < 256; i++)
        lut_r[i] = clamp255(((i * (n - 1) + 127) / 255) * 255 / (n - 1));
    lut_all();
    run_lut(p, w, h, v, ox, oy);
}

/* Helligkeit wie Photoshop: (max + min) / 2 */
static ULONG lightness(ULONG c)
{
    ULONG r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    ULONG mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    ULONG mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return (mx + mn + 1) >> 1;
}

static ULONG luma(ULONG c)
{
    return (((c >> 16) & 0xFF) * 77 + ((c >> 8) & 0xFF) * 150 + (c & 0xFF) * 29) >> 8;
}

static void run_desat(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    LONG i, n = w * h;
    for (i = 0; i < n; i++) {
        ULONG l = lightness(p[i]);
        p[i] = (p[i] & 0xFF000000) | (l * 0x010101);
    }
}

static void run_thresh(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    LONG i, n = w * h;
    for (i = 0; i < n; i++)
        p[i] = (p[i] & 0xFF000000) | ((LONG)luma(p[i]) >= v[0] ? 0xFFFFFF : 0);
}

/* Farbton/Saettigung ueber HSL */
static float hue2rgb(float p, float q, float t)
{
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

/* exakt: ein Pixel ueber HSL */
static ULONG huesat_px(ULONG c, float dh, float sf, float lf)
{
    /* Maximum/Minimum ganzzahlig bestimmen: die 68881 rechnet intern mit
     * 80 Bit, ein float-Vergleich "mx == r" kann daher falsch ausgehen */
    LONG ri = (c >> 16) & 0xFF, gi = (c >> 8) & 0xFF, bi = c & 0xFF;
    LONG mxi = ri > gi ? (ri > bi ? ri : bi) : (gi > bi ? gi : bi);
    LONG mni = ri < gi ? (ri < bi ? ri : bi) : (gi < bi ? gi : bi);
    float r = ri / 255.0f, g = gi / 255.0f, b = bi / 255.0f;
    float mx = mxi / 255.0f, mn = mni / 255.0f;
    float hh = 0.0f, ss = 0.0f, ll = (mx + mn) * 0.5f, d = (mxi - mni) / 255.0f;
    if (mxi > mni) {
        ss = mxi + mni > 255 ? d / (2.0f - mx - mn) : d / (mx + mn);
        if (mxi == ri) hh = (g - b) / d + (gi < bi ? 6.0f : 0.0f);
        else if (mxi == gi) hh = (b - r) / d + 2.0f;
        else hh = (r - g) / d + 4.0f;
        hh *= 1.0f / 6.0f;
    }
    hh += dh;
    if (hh < 0.0f) hh += 1.0f;
    if (hh >= 1.0f) hh -= 1.0f;
    ss *= sf;
    if (ss > 1.0f) ss = 1.0f;
    if (ss <= 0.0f) {
        r = g = b = ll;
    } else {
        float q = ll < 0.5f ? ll * (1.0f + ss) : ll + ss - ll * ss, pp = 2.0f * ll - q;
        r = hue2rgb(pp, q, hh + 1.0f / 3.0f);
        g = hue2rgb(pp, q, hh);
        b = hue2rgb(pp, q, hh - 1.0f / 3.0f);
    }
    if (lf < 0.0f) {
        r *= 1.0f + lf; g *= 1.0f + lf; b *= 1.0f + lf;
    } else if (lf > 0.0f) {
        r += (1.0f - r) * lf; g += (1.0f - g) * lf; b += (1.0f - b) * lf;
    }
    return (c & 0xFF000000) | ((ULONG)clamp255((LONG)(r * 255.0f + 0.5f)) << 16)
         | ((ULONG)clamp255((LONG)(g * 255.0f + 0.5f)) << 8) | clamp255((LONG)(b * 255.0f + 0.5f));
}

/* Schnell: Ergebnis fuer ein 33^3-Gitter vorrechnen (Schritt 8), jedes Pixel
 * trilinear aus 8 Gitterpunkten - ohne Verzweigungen (teuer auf Emu68) */
#define HS_N   33
#define HS_N2  (HS_N * HS_N)
#define HS_N3  (HS_N * HS_N * HS_N)

/* Gitter bleibt erhalten, solange sich die Werte nicht aendern (Korrekturebenen
 * rufen das je Kachel auf) */
static LONG *hs_grid;
static ULONG *hs_pack;          /* Gitterpunkte gepackt 0x00RRGGBB (fuer Tetraeder) */

void filter_free(void)
{
    if (hs_grid)
        FreeVec(hs_grid);
    if (hs_pack)
        FreeVec(hs_pack);
    hs_grid = NULL;
    hs_pack = NULL;
}
static float hs_key[3] = { -999.0f, 0, 0 };

static void huesat_lut(ULONG *p, LONG n, float dh, float sf, float lf)
{
    LONG *lr, *lg, *lb;
    LONG i, ri, gi, bi;
    BOOL fresh = !hs_grid || (LONG)(hs_key[0] * 1e4f) != (LONG)(dh * 1e4f) ||
                 (LONG)(hs_key[1] * 1e4f) != (LONG)(sf * 1e4f) || (LONG)(hs_key[2] * 1e4f) != (LONG)(lf * 1e4f);
    if (!hs_grid && !(hs_grid = AllocVec(HS_N3 * 3 * sizeof(LONG), MEMF_ANY)))
        return;
    lr = hs_grid;
    lg = lr + HS_N3;
    lb = lg + HS_N3;
    hs_key[0] = dh; hs_key[1] = sf; hs_key[2] = lf;
    if (fresh)
    for (ri = 0, i = 0; ri < HS_N; ri++)
        for (gi = 0; gi < HS_N; gi++)
            for (bi = 0; bi < HS_N; bi++, i++) {
                ULONG v = huesat_px(0xFF000000 | ((ULONG)(ri * 8 > 255 ? 255 : ri * 8) << 16)
                                   | ((ULONG)(gi * 8 > 255 ? 255 : gi * 8) << 8)
                                   | (ULONG)(bi * 8 > 255 ? 255 : bi * 8), dh, sf, lf);
                lr[i] = ((v >> 16) & 0xFF) << 4;    /* 4 Nachkommabits */
                lg[i] = ((v >> 8) & 0xFF) << 4;
                lb[i] = (v & 0xFF) << 4;
            }
    if (fresh || !hs_pack) {
        if (!hs_pack && !(hs_pack = AllocVec(HS_N3 * 4, MEMF_ANY)))
            return;
        for (i = 0; i < HS_N3; i++)
            hs_pack[i] = ((ULONG)clamp255((lr[i] + 8) >> 4) << 16) |
                         ((ULONG)clamp255((lg[i] + 8) >> 4) << 8) | clamp255((lb[i] + 8) >> 4);
    }
    /* Tetraeder-Interpolation: 4 Eckpunkte, alle drei Kanaele gemeinsam
     * (R und B in den Feldern 0xFF00FF, G getrennt; Gewichte summieren zu 8) */
    for (i = 0; i < n; i++) {
        ULONG c = p[i], c0, c1, c2, c3, rb, gg;
        LONG r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        LONG fr = r & 7, fg = g & 7, fb = b & 7, w0, w1, w2, w3;
        const ULONG *P = hs_pack + (r >> 3) * HS_N2 + (g >> 3) * HS_N + (b >> 3);
        c0 = P[0];
        c3 = P[HS_N2 + HS_N + 1];
        if (fr >= fg) {
            if (fg >= fb) {             /* r >= g >= b */
                c1 = P[HS_N2]; c2 = P[HS_N2 + HS_N];
                w0 = 8 - fr; w1 = fr - fg; w2 = fg - fb; w3 = fb;
            } else if (fr >= fb) {      /* r >= b > g */
                c1 = P[HS_N2]; c2 = P[HS_N2 + 1];
                w0 = 8 - fr; w1 = fr - fb; w2 = fb - fg; w3 = fg;
            } else {                    /* b > r >= g */
                c1 = P[1]; c2 = P[HS_N2 + 1];
                w0 = 8 - fb; w1 = fb - fr; w2 = fr - fg; w3 = fg;
            }
        } else {
            if (fr >= fb) {             /* g > r >= b */
                c1 = P[HS_N]; c2 = P[HS_N2 + HS_N];
                w0 = 8 - fg; w1 = fg - fr; w2 = fr - fb; w3 = fb;
            } else if (fg >= fb) {      /* g >= b > r */
                c1 = P[HS_N]; c2 = P[HS_N + 1];
                w0 = 8 - fg; w1 = fg - fb; w2 = fb - fr; w3 = fr;
            } else {                    /* b > g > r */
                c1 = P[1]; c2 = P[HS_N + 1];
                w0 = 8 - fb; w1 = fb - fg; w2 = fg - fr; w3 = fr;
            }
        }
        rb = (c0 & 0xFF00FF) * w0 + (c1 & 0xFF00FF) * w1 + (c2 & 0xFF00FF) * w2 + (c3 & 0xFF00FF) * w3;
        gg = (c0 & 0x00FF00) * w0 + (c1 & 0x00FF00) * w1 + (c2 & 0x00FF00) * w2 + (c3 & 0x00FF00) * w3;
        p[i] = (c & 0xFF000000) | (((rb + 0x040004) >> 3) & 0xFF00FF) | (((gg + 0x000400) >> 3) & 0x00FF00);
    }
}

static void run_huesat(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    float dh = v[0] / 360.0f, sf = 1.0f + v[1] / 100.0f, lf = v[2] / 100.0f;
    LONG i, n = w * h;
    if (!v[0] && !v[1] && !v[2])
        return;                 /* neutral: exakt unveraendert (das Gitter weicht um 1 ab) */
    /* Gitter (zwischengespeichert): ab einer Kachel lohnt es immer, weil es
     * fuer weitere Kacheln mit denselben Werten wiederverwendet wird */
    if (n >= 4096 || (hs_grid && (LONG)(hs_key[0] * 1e4f) == (LONG)(dh * 1e4f) &&
                      (LONG)(hs_key[1] * 1e4f) == (LONG)(sf * 1e4f) &&
                      (LONG)(hs_key[2] * 1e4f) == (LONG)(lf * 1e4f))) {
        huesat_lut(p, n, dh, sf, lf);
        return;
    }
    for (i = 0; i < n; i++)
        if (p[i] >> 24)
            p[i] = huesat_px(p[i], dh, sf, lf);
}

/* fuer BENCH: exakt gegen Gitter, Zeit und groesste Abweichung */
void bench_huesat(ULONG *buf, ULONG *tmp, LONG n, ULONG *t_exact, ULONG *t_lut, LONG *maxdev)
{
    LONG i, dev = 0;
    ULONG t;
    CopyMem(buf, tmp, n * 4);
    t = now_us();
    for (i = 0; i < n; i++)
        tmp[i] = huesat_px(tmp[i], 40 / 360.0f, 1.3f, 0.1f);
    *t_exact = now_us() - t;
    t = now_us();
    huesat_lut(buf, n, 40 / 360.0f, 1.3f, 0.1f);
    *t_lut = now_us() - t;
    for (i = 0; i < n; i++) {
        LONG sh;
        for (sh = 0; sh < 24; sh += 8) {
            LONG d = (LONG)((buf[i] >> sh) & 0xFF) - (LONG)((tmp[i] >> sh) & 0xFF);
            if (d < 0) d = -d;
            if (d > dev) dev = d;
        }
    }
    *maxdev = dev;
}

/* ---- Gradationskurven ---- */

Curve curves[4];

void curves_reset(void)
{
    LONG k;
    for (k = 0; k < 4; k++) {
        curves[k].n = 2;
        curves[k].x[0] = 0;   curves[k].y[0] = 0;
        curves[k].x[1] = 255; curves[k].y[1] = 255;
    }
}

/* natuerlicher kubischer Spline durch die Punkte -> Tabelle 0..255 */
void curve_table(const Curve *c, UBYTE *out)
{
    float m[CURVE_MAXPTS], u[CURVE_MAXPTS];
    LONG n = c->n, i, k;
    if (n < 2) {
        for (i = 0; i < 256; i++)
            out[i] = (UBYTE)i;
        return;
    }
    /* zweite Ableitungen (Tridiagonal-System, Raender = 0) */
    m[0] = u[0] = 0.0f;
    for (i = 1; i < n - 1; i++) {
        float hl = (float)(c->x[i] - c->x[i - 1]), hr = (float)(c->x[i + 1] - c->x[i]);
        float sig = hl / (hl + hr), p = sig * m[i - 1] + 2.0f;
        float d = (c->y[i + 1] - c->y[i]) / hr - (c->y[i] - c->y[i - 1]) / hl;
        m[i] = (sig - 1.0f) / p;
        u[i] = (6.0f * d / (hl + hr) - sig * u[i - 1]) / p;
    }
    m[n - 1] = 0.0f;
    for (i = n - 2; i >= 0; i--)
        m[i] = m[i] * m[i + 1] + u[i];
    k = 0;
    for (i = 0; i < 256; i++) {
        float v;
        if (i <= c->x[0])
            v = (float)c->y[0];
        else if (i >= c->x[n - 1])
            v = (float)c->y[n - 1];
        else {
            float h, a, b;
            while (k < n - 2 && i > c->x[k + 1])
                k++;
            h = (float)(c->x[k + 1] - c->x[k]);
            a = (c->x[k + 1] - i) / h;
            b = (i - c->x[k]) / h;
            v = a * c->y[k] + b * c->y[k + 1] +
                ((a * a * a - a) * m[k] + (b * b * b - b) * m[k + 1]) * h * h / 6.0f;
        }
        out[i] = clamp255((LONG)(v + 0.5f));
    }
}

static void run_curves(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    UBYTE t[4][256];
    LONG k, i;
    for (k = 0; k < 4; k++)
        curve_table(&curves[k], t[k]);
    /* erst der Kanal, dann die RGB-Gesamtkurve */
    for (i = 0; i < 256; i++) {
        lut_r[i] = t[0][t[1][i]];
        lut_g[i] = t[0][t[2][i]];
        lut_b[i] = t[0][t[3][i]];
    }
    run_lut(p, w, h, v, ox, oy);
}

/* ---- Unschaerfe (3 Box-Durchgaenge ~ Gauss), vormultipliziert ---- */

/* Box-Radien fuer Gauss mit sigma (Wells / Kovesi) */
static void gauss_boxes(float sigma, LONG *r)
{
    float wi = sqrtf(12.0f * sigma * sigma / 3.0f + 1.0f);
    LONG wl = (LONG)wi, wu, m, i;
    if (!(wl & 1)) wl--;
    wu = wl + 2;
    m = (LONG)((12.0f * sigma * sigma - 3 * wl * wl - 4 * 3 * wl - 3 * 3) / (-4.0f * wl - 4.0f) + 0.5f);
    for (i = 0; i < 3; i++)
        r[i] = ((i < m ? wl : wu) - 1) / 2;
}

/* Gauss in place; sigma in Pixeln */
static void gauss(ULONG *p, LONG w, LONG h, float sigma)
{
    LONG r[3], k;
    ULONG *tmp, *sums, *pad;
    if (sigma < 0.3f)
        return;
    gauss_boxes(sigma, r);
    tmp = AllocVec(w * h * 4, MEMF_ANY);
    sums = AllocVec(4 * w * sizeof(ULONG), MEMF_ANY);
    pad = AllocVec((w + 2 * (r[2] > r[0] ? r[2] : r[0]) + 2) * sizeof(ULONG), MEMF_ANY);
    if (tmp && sums && pad)
        for (k = 0; k < 3; k++) {
            if (r[k] < 1)
                continue;
            blur_h(p, tmp, w, h, r[k], pad);
            blur_v(tmp, p, w, h, r[k], sums);
        }
    if (tmp) FreeVec(tmp);
    if (sums) FreeVec(sums);
    if (pad) FreeVec(pad);
}

static LONG margin_gauss(const LONG *v) { return v[0] * 3 / 10 + 2; }

static void run_gauss(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    gauss(p, w, h, v[0] / 10.0f);
}

static LONG margin_unsharp(const LONG *v) { return v[1] * 3 / 10 + 2; }

static void run_unsharp(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    ULONG *bl = AllocVec(w * h * 4, MEMF_ANY);
    LONG i, n = w * h, amt = v[0] * 256 / 100, th = v[2];
    if (!bl)
        return;
    CopyMem(p, bl, n * 4);
    gauss(bl, w, h, v[1] / 10.0f);
    for (i = 0; i < n; i++) {
        ULONG o = p[i], b = bl[i], a = o >> 24, out = o & 0xFF000000;
        LONG sh;
        for (sh = 0; sh < 24; sh += 8) {
            LONG c = (o >> sh) & 0xFF, d = c - (LONG)((b >> sh) & 0xFF);
            if ((d < 0 ? -d : d) >= th)
                c += (d * amt) >> 8;
            if (c < 0) c = 0;
            if (c > (LONG)a) c = a;     /* vormultipliziert: Kanal <= Alpha */
            out |= (ULONG)c << sh;
        }
        p[i] = out;
    }
    FreeVec(bl);
}

/* ---- Rauschen ---- */

static ULONG rng;

static ULONG xorshift(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void run_noise(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    LONG x, y, a = v[0] * 255 / 100;
    for (y = 0; y < h; y++) {
        /* je Dokumentzeile gleicher Startwert: Vorschau bleibt ruhig */
        rng = 0x9E3779B9u ^ (ULONG)(oy + y) * 2654435761u;
        for (x = 0; x < w; x++) {
            ULONG c = p[y * w + x], out = c & 0xFF000000;
            LONG sh;
            if (!(c >> 24))
                continue;
            for (sh = 0; sh < 24; sh += 8) {
                LONG n = (LONG)(xorshift() % (ULONG)(2 * a + 1)) - a;
                out |= (ULONG)clamp255((LONG)((c >> sh) & 0xFF) + n) << sh;
            }
            p[y * w + x] = out;
        }
    }
}

/* ---- Relief ---- */

static LONG margin_emboss(const LONG *v) { return v[1] + 1; }

static void run_emboss(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    float ang = v[0] * 3.14159265f / 180.0f;
    LONG dx = (LONG)floorf(cosf(ang) * v[1] + 0.5f), dy = (LONG)floorf(-sinf(ang) * v[1] + 0.5f);
    LONG x, y, amt = v[2];
    UBYTE *l = AllocVec(w * h, MEMF_ANY);
    if (!l)
        return;
    for (x = 0; x < w * h; x++)
        l[x] = (UBYTE)luma(p[x]);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            LONG x1 = x + dx, y1 = y + dy, x2 = x - dx, y2 = y - dy;
            LONG a, b, g;
            if (x1 < 0) x1 = 0;
            if (x1 >= w) x1 = w - 1;
            if (y1 < 0) y1 = 0;
            if (y1 >= h) y1 = h - 1;
            if (x2 < 0) x2 = 0;
            if (x2 >= w) x2 = w - 1;
            if (y2 < 0) y2 = 0;
            if (y2 >= h) y2 = h - 1;
            a = l[y1 * w + x1];
            b = l[y2 * w + x2];
            g = clamp255(128 + (a - b) * amt / 100);
            p[y * w + x] = (p[y * w + x] & 0xFF000000) | ((ULONG)g * 0x010101);
        }
    FreeVec(l);
}

/* ---- Mosaik (vormultipliziert, Zellen am Dokumentursprung ausgerichtet) ---- */

static LONG margin_mosaic(const LONG *v) { return v[0]; }

static void run_mosaic(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    LONG c = v[0], cy0, cx0;
    for (cy0 = -((oy % c + c) % c); cy0 < h; cy0 += c)
        for (cx0 = -((ox % c + c) % c); cx0 < w; cx0 += c) {
            LONG x0 = cx0 < 0 ? 0 : cx0, y0 = cy0 < 0 ? 0 : cy0;
            LONG x1 = cx0 + c > w ? w : cx0 + c, y1 = cy0 + c > h ? h : cy0 + c;
            ULONG sa = 0, sr = 0, sg = 0, sb = 0, n = (x1 - x0) * (y1 - y0), avg;
            LONG x, y;
            for (y = y0; y < y1; y++)
                for (x = x0; x < x1; x++) {
                    ULONG q = p[y * w + x];
                    sa += q >> 24; sr += (q >> 16) & 0xFF; sg += (q >> 8) & 0xFF; sb += q & 0xFF;
                }
            avg = ((sa / n) << 24) | ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
            for (y = y0; y < y1; y++)
                for (x = x0; x < x1; x++)
                    p[y * w + x] = avg;
        }
}

/* ---- Bewegungsunschaerfe: Mittel entlang einer Linie (vormultipliziert) ---- */

static LONG margin_motion(const LONG *v) { return v[1] / 2 + 2; }

static void run_motion(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    float ang = v[0] * 3.14159265f / 180.0f;
    LONG d = v[1], n = d + 1, k, x, y;
    LONG *ofx = AllocVec(n * sizeof(LONG), MEMF_ANY), *ofy = AllocVec(n * sizeof(LONG), MEMF_ANY);
    ULONG *src = AllocVec(w * h * 4, MEMF_ANY);
    if (ofx && ofy && src) {
        for (k = 0; k < n; k++) {
            float t = k - d * 0.5f;
            ofx[k] = (LONG)floorf(cosf(ang) * t + 0.5f);
            ofy[k] = (LONG)floorf(-sinf(ang) * t + 0.5f);
        }
        CopyMem(p, src, w * h * 4);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                ULONG sa = 0, sr = 0, sg = 0, sb = 0;
                for (k = 0; k < n; k++) {
                    LONG sx = x + ofx[k], sy = y + ofy[k];
                    ULONG q;
                    if (sx < 0) sx = 0;
                    if (sx >= w) sx = w - 1;
                    if (sy < 0) sy = 0;
                    if (sy >= h) sy = h - 1;
                    q = src[sy * w + sx];
                    sa += q >> 24; sr += (q >> 16) & 0xFF; sg += (q >> 8) & 0xFF; sb += q & 0xFF;
                }
                p[y * w + x] = ((sa / n) << 24) | ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
            }
    }
    if (ofx) FreeVec(ofx);
    if (ofy) FreeVec(ofy);
    if (src) FreeVec(src);
}

/* ---- Box Blur ---- */

static LONG margin_box(const LONG *v) { return v[0] + 1; }

static void run_box(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    LONG r = v[0];
    ULONG *tmp = AllocVec(w * h * 4, MEMF_ANY), *sums = AllocVec(4 * w * sizeof(ULONG), MEMF_ANY);
    ULONG *pad = AllocVec((w + 2 * r + 2) * sizeof(ULONG), MEMF_ANY);
    if (tmp && sums && pad) {
        blur_h(p, tmp, w, h, r, pad);
        blur_v(tmp, p, w, h, r, sums);
    }
    if (tmp) FreeVec(tmp);
    if (sums) FreeVec(sums);
    if (pad) FreeVec(pad);
}

/* ---- Median je Kanal (Histogramm, gleitend je Zeile) ---- */

static LONG margin_median(const LONG *v) { return v[0] + 1; }

static void run_median(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    LONG r = v[0], x, y, sh, half = ((2 * r + 1) * (2 * r + 1)) / 2;
    ULONG *src = AllocVec(w * h * 4, MEMF_ANY);
    if (!src)
        return;
    CopyMem(p, src, w * h * 4);
    for (sh = 0; sh < 32; sh += 8)
        for (y = 0; y < h; y++) {
            UWORD hist[256];
            LONG i, j, med = 0, below = 0;     /* below = Anzahl Werte < med */
            memset(hist, 0, sizeof(hist));
            /* Fenster fuer x = 0 (Rand wiederholt) */
            for (j = -r; j <= r; j++) {
                LONG yy = y + j < 0 ? 0 : y + j >= h ? h - 1 : y + j;
                for (i = -r; i <= r; i++) {
                    LONG xx = i < 0 ? 0 : i >= w ? w - 1 : i;
                    hist[(src[yy * w + xx] >> sh) & 0xFF]++;
                }
            }
            for (x = 0; x < w; x++) {
                if (x > 0) {
                    LONG xo = x - r - 1 < 0 ? 0 : x - r - 1, xn = x + r >= w ? w - 1 : x + r;
                    for (j = -r; j <= r; j++) {
                        LONG yy = y + j < 0 ? 0 : y + j >= h ? h - 1 : y + j;
                        LONG a = (src[yy * w + xo] >> sh) & 0xFF, b = (src[yy * w + xn] >> sh) & 0xFF;
                        hist[a]--;
                        if (a < med) below--;
                        hist[b]++;
                        if (b < med) below++;
                    }
                }
                /* Median nachfuehren */
                while (below > half) {
                    med--;
                    below -= hist[med];
                }
                while (below + hist[med] <= half) {
                    below += hist[med];
                    med++;
                }
                p[y * w + x] = (p[y * w + x] & ~(0xFFUL << sh)) | ((ULONG)med << sh);
            }
        }
    FreeVec(src);
    /* vormultipliziert: Kanaele duerfen Alpha nicht uebersteigen */
    for (x = 0; x < w * h; x++) {
        ULONG q = p[x], a = q >> 24, r2 = (q >> 16) & 0xFF, g = (q >> 8) & 0xFF, b = q & 0xFF;
        if (r2 > a) r2 = a;
        if (g > a) g = a;
        if (b > a) b = a;
        p[x] = (a << 24) | (r2 << 16) | (g << 8) | b;
    }
}

/* ---- Scharfzeichnen (3x3) ---- */

static LONG margin_one(const LONG *v) { return 1; }

static void sharpen3(ULONG *p, LONG w, LONG h, LONG k)
{
    /* Kern in Sechzehnteln: Nachbarn -k, Mitte 16 + 4k (Summe 1) */
    ULONG *src = AllocVec(w * h * 4, MEMF_ANY);
    LONG x, y;
    if (!src)
        return;
    CopyMem(p, src, w * h * 4);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            ULONG c = src[y * w + x], u = src[(y > 0 ? y - 1 : y) * w + x], d = src[(y < h - 1 ? y + 1 : y) * w + x];
            ULONG l = src[y * w + (x > 0 ? x - 1 : x)], r = src[y * w + (x < w - 1 ? x + 1 : x)];
            ULONG a = c >> 24, out = c & 0xFF000000;
            LONG sh;
            for (sh = 0; sh < 24; sh += 8) {
                LONG v = ((LONG)((c >> sh) & 0xFF) * (16 + 4 * k) -
                          k * ((LONG)((u >> sh) & 0xFF) + (LONG)((d >> sh) & 0xFF) +
                               (LONG)((l >> sh) & 0xFF) + (LONG)((r >> sh) & 0xFF))) / 16;
                if (v < 0) v = 0;
                if (v > (LONG)a) v = a;
                out |= (ULONG)v << sh;
            }
            p[y * w + x] = out;
        }
    FreeVec(src);
}

static void run_sharpen(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    sharpen3(p, w, h, v[0]);
}

/* ---- Hochpass: 128 + (Bild - Weichzeichnung), Alpha bleibt ---- */

static LONG margin_highpass(const LONG *v) { return v[0] * 3 / 10 + 2; }

static void run_highpass(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    ULONG *bl = AllocVec(w * h * 4, MEMF_ANY);
    LONG i, n = w * h;
    if (!bl)
        return;
    CopyMem(p, bl, n * 4);
    to_premul(bl, n);
    gauss(bl, w, h, v[0] / 10.0f);
    from_premul(bl, n);
    for (i = 0; i < n; i++) {
        ULONG o = p[i], b = bl[i], out = o & 0xFF000000;
        LONG sh;
        for (sh = 0; sh < 24; sh += 8)
            out |= (ULONG)clamp255(128 + (LONG)((o >> sh) & 0xFF) - (LONG)((b >> sh) & 0xFF)) << sh;
        p[i] = out;
    }
    FreeVec(bl);
}

/* ---- Konturen finden: Sobel je Kanal, invertiert ---- */

static void run_edges(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    ULONG *src = AllocVec(w * h * 4, MEMF_ANY);
    LONG x, y;
    if (!src)
        return;
    CopyMem(p, src, w * h * 4);
    for (y = 0; y < h; y++) {
        LONG ym = y > 0 ? y - 1 : y, yp = y < h - 1 ? y + 1 : y;
        for (x = 0; x < w; x++) {
            LONG xm = x > 0 ? x - 1 : x, xp = x < w - 1 ? x + 1 : x, sh;
            ULONG out = src[y * w + x] & 0xFF000000;
            for (sh = 0; sh < 24; sh += 8) {
#define C(xx, yy) ((LONG)((src[(yy) * w + (xx)] >> sh) & 0xFF))
                LONG gx = C(xp, ym) + 2 * C(xp, y) + C(xp, yp) - C(xm, ym) - 2 * C(xm, y) - C(xm, yp);
                LONG gy = C(xm, yp) + 2 * C(x, yp) + C(xp, yp) - C(xm, ym) - 2 * C(x, ym) - C(xp, ym);
#undef C
                LONG m = ((gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy)) / 2;
                out |= (ULONG)clamp255(255 - m) << sh;
            }
            p[y * w + x] = out;
        }
    }
    FreeVec(src);
}

/* ---- Amiga: Farben reduzieren ---- */

static QPalette q_last;
static LONG q_shown;            /* Anzahl in der Anzeige */
static BOOL q_force_fixed;      /* naechster Dialog: Palette "Fixed" */

static void opts_from(const LONG *v, QuantOpts *o)
{
    o->palmode = v[0];
    o->mode = v[1];
    o->ncolors = v[2];
    o->dither = v[3];
    o->amount = v[4];
    o->optimize = v[5];
    o->fixed = NULL;
    if (o->palmode == QP_FIXED) {
        /* vorgegebene Palette, sonst die aktuelle des Dokuments */
        o->fixed = doc->fixpal ? doc->fixpal : doc->qpal;
        if (!o->fixed)
            o->palmode = QP_ADAPTIVE;
    }
}

static void run_quant(ULONG *p, LONG w, LONG h, const LONG *v, LONG ox, LONG oy)
{
    QuantOpts o;
    opts_from(v, &o);
    if (!quant_palette_img(p, w, h, &o, &q_last))
        return;
    quant_map(p, w, h, w, &o, &q_last, NULL);
    q_shown = q_last.mode == QM_EHB ? 64 : q_last.n;
}

static Filter *cur;

static BOOL quant_dialog(void (*prev)(void))
{
    LONG *v = cur->val;
    DlgSlider ds[7] = {
        { "Palette:", 0, DLG_CHOICE, DLG_CHOICE, "Adaptive|OCS 12-bit|MagicWB|Grayscale|Fixed", 1 },
        { "Mode:", 0, DLG_CHOICE, DLG_CHOICE, "Normal|EHB|HAM6|HAM8", 1 },
        { "Colors:", 0, 2, 256, "", 1 },
        { "Dither:", 0, DLG_CHOICE, DLG_CHOICE, "None|Floyd-Steinberg|Atkinson|Ordered", 1 },
        { "Amount:", 0, 0, 100, "%", 1 },
        { "Optimize Palette", 0, DLG_CHECK, DLG_CHECK, "", 1 },
        { "Palette", 0, DLG_PALETTE, DLG_PALETTE, "", 1 },
    };
    LONG i;
    BOOL ok;
    /* Vorgabe: letzte Reduktion dieses Dokuments, sonst Farbtiefe aus New Document */
    if (doc->qpal) {
        for (i = 0; i < 6; i++)
            v[i] = doc->qopt[i];
    } else if (doc->colors) {
        v[1] = doc->colors == -6 ? QM_HAM6 : doc->colors == -8 ? QM_HAM8 : doc->colors == 64 ? QM_EHB : QM_NORMAL;
        if (doc->colors > 0)
            v[2] = doc->colors;
        v[0] = doc->colors > 0 && doc->colors <= 32 ? QP_OCS : QP_ADAPTIVE;
    }
    if (q_force_fixed) {
        v[0] = QP_FIXED;
        v[1] = QM_NORMAL;
        q_force_fixed = FALSE;
    }
    for (i = 0; i < 6; i++)
        ds[i].val = &v[i];
    ds[6].val = &q_shown;
    ui_dialog_palette(q_last.col, &q_shown);
    ok = dialog_run("Reduce Colors", ds, 7, prev);
    ui_dialog_palette(NULL, NULL);
    return ok;
}

/* nach dem Anwenden: Palette am Dokument (fuer den IFF-Export) */
static void quant_store(void)
{
    LONG i;
    if (!doc->qpal && !(doc->qpal = AllocVec(sizeof(QPalette), MEMF_ANY)))
        return;
    *(QPalette *)doc->qpal = q_last;
    for (i = 0; i < 6; i++)
        doc->qopt[i] = cur->val[i];
    doc->colors = q_last.mode == QM_HAM6 ? -6 : q_last.mode == QM_HAM8 ? -8 :
                  q_last.mode == QM_EHB ? 64 : q_last.n;
}

/* Reduce Colors mit der eben geladenen Palette oeffnen */
void filter_reduce_fixed(void)
{
    q_force_fixed = TRUE;
    filter_command(CMD_FLT_QUANT);
    q_force_fixed = FALSE;
}

/* ---- Tabelle aller Korrekturen und Filter ---- */

/* ---- Histogramm fuer Levels/Curves: 0 = RGB (alle drei Kanaele), 1..3 = R, G, B ---- */

ULONG hist[4][256];

static void hist_clear(void)
{
    memset(hist, 0, sizeof(hist));
}

/* n Pixel zaehlen; premul: Komposit (vorher entmultiplizieren) */
static void hist_add(const ULONG *p, const UBYTE *sel, LONG n, BOOL premul, BOOL any_alpha)
{
    LONG i;
    for (i = 0; i < n; i++) {
        ULONG c = p[i], a = c >> 24, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        if ((sel && !sel[i]) || (!a && !any_alpha))
            continue;
        if (premul && a && a < 255) {
            r = r * 255 / a; g = g * 255 / a; b = b * 255 / a;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
        }
        hist[0][r]++; hist[0][g]++; hist[0][b]++;
        hist[1][r]++; hist[2][g]++; hist[3][b]++;
    }
}

/* Filter: Originalpixel der aktiven Ebene (bzw. Maske) im Bearbeitungsbereich */
static void hist_layer(void)
{
    ULONG *row = AllocVec((fl_x1 - fl_x0) * 4, MEMF_ANY);
    LONG y;
    hist_clear();
    if (!row)
        return;
    for (y = fl_y0; y < fl_y1; y++) {
        read_row(fl_x0, y, fl_x1 - fl_x0, row);
        hist_add(row, doc->hassel ? doc->sel + y * doc->w + fl_x0 : NULL, fl_x1 - fl_x0, FALSE,
                 doc_target_is_mask());
    }
    FreeVec(row);
}

/* Korrekturebene: Komposit darunter (sonst das ganze Bild) */
static void hist_below(void)
{
    const ULONG *src = doc->bcache && doc->bcache_upto == doc->active ? doc->bcache : doc->flat;
    LONG y;
    hist_clear();
    if (!src)
        return;
    for (y = 0; y < doc->h; y++)
        hist_add(src + y * doc->w, doc->hassel ? doc->sel + y * doc->w : NULL, doc->w, TRUE, FALSE);
}

static BOOL levels_custom(void (*prev)(void))
{
    hist_layer();
    return levels_dialog(cur->val, levels_ch, prev);
}

static BOOL curves_custom(void (*prev)(void))
{
    hist_layer();
    return curves_dialog(prev);
}

static Filter filters[] = {
    { CMD_ADJ_BRICON, "Brightness/Contrast", 2,
      { { "Brightness:", 0, -150, 150, "", 1 }, { "Contrast:", 0, -50, 100, "", 1 } },
      { 0, 0 }, FALSE, NULL, run_bricon, NULL },
    { CMD_ADJ_LEVELS, "Levels", 5,
      { { "Input Black:", 0, 0, 253, "", 1 }, { "Gamma:", 0, 10, 999, "", 100 },
        { "Input White:", 0, 2, 255, "", 1 }, { "Output Black:", 0, 0, 255, "", 1 },
        { "Output White:", 0, 0, 255, "", 1 } },
      { 0, 100, 255, 0, 255 }, FALSE, NULL, run_levels, levels_custom },
    { CMD_ADJ_CURVES, "Curves", 0, { { 0 } }, { 0 }, FALSE, NULL, run_curves, curves_custom },
    { CMD_ADJ_HUESAT, "Hue/Saturation", 3,
      { { "Hue:", 0, -180, 180, "°", 1 }, { "Saturation:", 0, -100, 100, "", 1 },
        { "Lightness:", 0, -100, 100, "", 1 } },
      { 0, 0, 0 }, FALSE, NULL, run_huesat, NULL },
    { CMD_ADJ_DESAT, "Desaturate", 0, { { 0 } }, { 0 }, FALSE, NULL, run_desat, NULL },
    { CMD_ADJ_INVERT, "Invert", 0, { { 0 } }, { 0 }, FALSE, NULL, run_invert, NULL },
    { CMD_ADJ_THRESH, "Threshold", 1,
      { { "Threshold Level:", 0, 1, 255, "", 1 } }, { 128 }, FALSE, NULL, run_thresh, NULL },
    { CMD_ADJ_POSTER, "Posterize", 1,
      { { "Levels:", 0, 2, 255, "", 1 } }, { 4 }, FALSE, NULL, run_poster, NULL },
    { CMD_FLT_GAUSS, "Gaussian Blur", 1,
      { { "Radius:", 0, 1, 1000, "px", 10 } }, { 20 }, TRUE, margin_gauss, run_gauss, NULL },
    { CMD_FLT_UNSHARP, "Unsharp Mask", 3,
      { { "Amount:", 0, 1, 500, "%", 1 }, { "Radius:", 0, 1, 1000, "px", 10 },
        { "Threshold:", 0, 0, 255, "", 1 } },
      { 100, 10, 0 }, TRUE, margin_unsharp, run_unsharp, NULL },
    { CMD_FLT_NOISE, "Add Noise", 1,
      { { "Amount:", 0, 1, 100, "%", 1 } }, { 12 }, FALSE, NULL, run_noise, NULL },
    { CMD_FLT_EMBOSS, "Emboss", 3,
      { { "Angle:", 0, -180, 180, "°", 1 }, { "Height:", 0, 1, 10, "px", 1 },
        { "Amount:", 0, 1, 500, "%", 1 } },
      { 135, 3, 100 }, FALSE, margin_emboss, run_emboss, NULL },
    { CMD_FLT_MOSAIC, "Mosaic", 1,
      { { "Cell Size:", 0, 2, 200, "px", 1 } }, { 10 }, TRUE, margin_mosaic, run_mosaic, NULL },
    { CMD_FLT_MOTION, "Motion Blur", 2,
      { { "Angle:", 0, -90, 90, "°", 1 }, { "Distance:", 0, 1, 200, "px", 1 } },
      { 0, 10 }, TRUE, margin_motion, run_motion, NULL },
    { CMD_FLT_BOX, "Box Blur", 1,
      { { "Radius:", 0, 1, 200, "px", 1 } }, { 5 }, TRUE, margin_box, run_box, NULL },
    { CMD_FLT_MEDIAN, "Median", 1,
      { { "Radius:", 0, 1, 20, "px", 1 } }, { 1 }, TRUE, margin_median, run_median, NULL },
    { CMD_FLT_SHARPEN, "Sharpen", 0, { { 0 } }, { 4 }, TRUE, margin_one, run_sharpen, NULL },
    { CMD_FLT_SHARPEN_MORE, "Sharpen More", 0, { { 0 } }, { 12 }, TRUE, margin_one, run_sharpen, NULL },
    { CMD_FLT_HIGHPASS, "High Pass", 1,
      { { "Radius:", 0, 1, 2500, "px", 10 } }, { 100 }, FALSE, margin_highpass, run_highpass, NULL },
    { CMD_FLT_EDGES, "Find Edges", 0, { { 0 } }, { 0 }, FALSE, margin_one, run_edges, NULL },
    { CMD_FLT_QUANT, "Reduce Colors", 0, { { 0 } }, { QP_ADAPTIVE, QM_NORMAL, 32, QD_FS, 100, 1 },
      FALSE, NULL, run_quant, quant_dialog },
};
#define NFILTERS (sizeof(filters) / sizeof(filters[0]))

static Filter *last_filter;
AdjInfo *adj_curves_target;     /* Curves-Dialog fuer eine Korrekturebene: Kurven live dorthin */

static void preview(void)
{
    char msg[48];
    ULONG t = now_us();
    LONG oldpri = SetTaskPri(FindTask(NULL), -1);   /* System bleibt bedienbar */
    fl_apply(cur);
    SetTaskPri(FindTask(NULL), oldpri);
    t = now_us() - t;
    sprintf(msg, "Preview: %ld ms", (long)(t / 1000));
    status_message(msg);
}

static void busy_ptr(BOOL on)
{
    if (on)
        SetWindowPointer(win, WA_BusyPointer, TRUE, TAG_DONE);
    else
        ClearPointer(win);
}

const char *filter_last_name(void)
{
    return last_filter ? last_filter->name : NULL;
}

void filter_command(LONG cmd)
{
    Filter *f = NULL;
    LONG i;
    BOOL ok = TRUE;

    if (cmd == CMD_FLT_LAST) {
        if (!(f = last_filter))
            return;
    } else
        for (i = 0; i < (LONG)NFILTERS; i++)
            if (filters[i].cmd == cmd)
                f = &filters[i];
    if (!f)
        return;
    if (!doc->layers[doc->active]->visible) {
        status_message("The layer is hidden");
        return;
    }
    if (!fl_begin()) {
        status_message("Not enough memory");
        return;
    }
    cur = f;
    for (i = 0; i < f->n; i++)
        f->sl[i].val = &f->val[i];

    if (f->custom && cmd != CMD_FLT_LAST) {
        ok = f->custom(preview);
    } else if (f->n && cmd != CMD_FLT_LAST) {
        ok = dialog_run(f->name, f->sl, f->n, preview);
    } else {
        LONG oldpri;
        busy_ptr(TRUE);
        oldpri = SetTaskPri(FindTask(NULL), -1);
        ok = fl_apply(f);
        SetTaskPri(FindTask(NULL), oldpri);
        busy_ptr(FALSE);
    }
    if (ok) {
        fl_commit(f->name);
        if (f->cmd == CMD_FLT_QUANT)
            quant_store();
        if (f->cmd >= CMD_FLT_GAUSS)
            last_filter = f;
    } else
        fl_cancel();
    cur = NULL;
}

/* ---- Korrekturebenen ---- */

static Filter *adj_filter(LONG cmd)
{
    LONG i;
    for (i = 0; i < (LONG)NFILTERS; i++)
        if (filters[i].cmd == cmd)
            return &filters[i];
    return NULL;
}

const char *adjust_name(LONG cmd)
{
    Filter *f = adj_filter(cmd);
    return f ? f->name : "Adjustment";
}

void adjust_defaults(AdjInfo *a, LONG cmd)
{
    static const LONG defs[][6] = {
        { 0, 0 }, { 0, 100, 255, 0, 255 }, { 0 }, { 0, 0, 0 }, { 0 }, { 0 }, { 128 }, { 4 }
    };
    static const LONG cmds[8] = { CMD_ADJ_BRICON, CMD_ADJ_LEVELS, CMD_ADJ_CURVES, CMD_ADJ_HUESAT,
                                  CMD_ADJ_DESAT, CMD_ADJ_INVERT, CMD_ADJ_THRESH, CMD_ADJ_POSTER };
    Curve save[4];
    LONG i;
    memset(a, 0, sizeof(*a));
    a->cmd = cmd;
    for (i = 0; i < 8; i++)
        if (cmds[i] == cmd)
            memcpy(a->v, defs[i], sizeof(a->v));
    memcpy(save, curves, sizeof(save));
    curves_reset();
    memcpy(a->curves, curves, sizeof(a->curves));
    memcpy(curves, save, sizeof(save));
    memset(a->lv, 0, sizeof(a->lv));
}

/* Korrektur auf n Pixel (ARGB straight) anwenden; Tabellen-Korrekturen werden
 * einmal je Einstellung ueber eine Graurampe berechnet */
void adjust_apply(const AdjInfo *a, ULONG *p, LONG n)
{
    static AdjInfo cached;
    static UBYTE tr[256], tg[256], tb[256];
    static BOOL valid;
    Filter *f = adj_filter(a->cmd);
    LONG i;
    if (!f)
        return;
    if (a->cmd == CMD_ADJ_BRICON || a->cmd == CMD_ADJ_LEVELS || a->cmd == CMD_ADJ_CURVES ||
        a->cmd == CMD_ADJ_POSTER) {
        if (!valid || memcmp(a, &cached, sizeof(cached))) {
            ULONG ramp[256];
            for (i = 0; i < 256; i++)
                ramp[i] = 0xFF000000 | ((ULONG)i * 0x010101);
            if (a->cmd == CMD_ADJ_CURVES) {
                Curve save[4];
                memcpy(save, curves, sizeof(save));
                memcpy(curves, a->curves, sizeof(save));
                f->run(ramp, 256, 1, a->v, 0, 0);
                memcpy(curves, save, sizeof(save));
            } else if (a->cmd == CMD_ADJ_LEVELS) {
                lv_cur = (const LONG (*)[5])a->lv;
                f->run(ramp, 256, 1, a->v, 0, 0);
                lv_cur = levels_ch;
            } else
                f->run(ramp, 256, 1, a->v, 0, 0);
            for (i = 0; i < 256; i++) {
                tr[i] = (UBYTE)(ramp[i] >> 16);
                tg[i] = (UBYTE)(ramp[i] >> 8);
                tb[i] = (UBYTE)ramp[i];
            }
            cached = *a;
            valid = TRUE;
        }
        for (i = 0; i < n; i++) {
            ULONG c = p[i];
            p[i] = (c & 0xFF000000) | ((ULONG)tr[(c >> 16) & 0xFF] << 16) |
                   ((ULONG)tg[(c >> 8) & 0xFF] << 8) | tb[c & 0xFF];
        }
        return;
    }
    if (a->cmd == CMD_ADJ_CURVES) {
        Curve save[4];
        memcpy(save, curves, sizeof(save));
        memcpy(curves, a->curves, sizeof(save));
        f->run(p, n, 1, a->v, 0, 0);
        memcpy(curves, save, sizeof(save));
    } else
        f->run(p, n, 1, a->v, 0, 0);
}

/* Dialog der Korrektur; Werte live in *a (Vorschau zeigt sie) */
BOOL adjust_dialog(AdjInfo *a, void (*prev)(void))
{
    Filter *f = adj_filter(a->cmd);
    DlgSlider sl[6];
    LONG i;
    BOOL ok;
    if (!f)
        return FALSE;
    if (a->cmd == CMD_ADJ_LEVELS) {
        hist_below();
        return levels_dialog(a->v, a->lv, prev);
    }
    if (a->cmd == CMD_ADJ_CURVES) {
        Curve save[4];
        memcpy(save, curves, sizeof(save));
        memcpy(curves, a->curves, sizeof(save));
        adj_curves_target = a;
        hist_below();
        ok = curves_dialog(prev);
        adj_curves_target = NULL;
        if (ok)
            memcpy(a->curves, curves, sizeof(save));
        memcpy(curves, save, sizeof(save));
        return ok;
    }
    if (!f->n)
        return TRUE;            /* ohne Regler (Invert, Desaturate) */
    for (i = 0; i < f->n; i++) {
        sl[i] = f->sl[i];
        sl[i].val = &a->v[i];
    }
    return dialog_run(f->name, sl, f->n, prev);
}

/* fuer PERF: Filter mit Standardwerten ohne Dialog auf die aktive Ebene */
ULONG filter_perf(LONG cmd)
{
    Filter *f = adj_filter(cmd);
    ULONG t;
    LONG i;
    if (!f || !fl_begin())
        return 0;
    for (i = 0; i < f->n; i++)
        f->sl[i].val = &f->val[i];
    t = now_us();
    fl_apply(f);
    fl_commit(f->name);
    return now_us() - t;
}

/* Regler einer Korrektur (fuer das Properties-Panel); Rueckgabe: Anzahl */
LONG adjust_sliders(LONG cmd, DlgSlider *out)
{
    Filter *f = adj_filter(cmd);
    LONG i;
    if (!f || (f->custom && cmd != CMD_ADJ_LEVELS))
        return 0;
    for (i = 0; i < f->n; i++)
        out[i] = f->sl[i];
    return f->n;
}
