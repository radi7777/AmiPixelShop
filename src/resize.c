/*
 * resize.c - Bildgroesse und Arbeitsflaeche
 *
 * Beide erzeugen ein neues Dokument (die Kachelzahl aendert sich), das im
 * selben Tab das alte ersetzt. Der Undo-Verlauf beginnt danach neu.
 * Skalieren: verkleinern per Flaechenmittel (premultiplied), vergroessern
 * bilinear. Masken werden mitskaliert, Textebenen bleiben editierbar.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "apx.h"

/* ganze Kachelmenge als ARGB-Bild (straight) auslesen */
static ULONG *tiles_to_image(Doc *d, Tile **tiles, ULONG empty)
{
    ULONG *img = AllocVec(d->w * d->h * 4, MEMF_ANY);
    LONG x, y;
    if (!img)
        return NULL;
    for (y = 0; y < d->h; y++)
        for (x = 0; x < d->w; x++) {
            Tile *t = tiles[(y >> TILE_SHIFT) * d->tw + (x >> TILE_SHIFT)];
            img[y * d->w + x] = t ? t->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)] : empty;
        }
    return img;
}

/* ARGB-Bild in Kacheln schreiben; leere Kacheln bleiben NULL (ausser keep_all) */
static BOOL image_to_tiles(Doc *d, const ULONG *img, Tile **tiles, BOOL keep_all)
{
    LONG tx, ty, y;
    for (ty = 0; ty < d->th; ty++)
        for (tx = 0; tx < d->tw; tx++) {
            LONG x0 = tx << TILE_SHIFT, y0 = ty << TILE_SHIFT;
            LONG vw = d->w - x0, vh = d->h - y0;
            Tile *t = keep_all ? tile_new_gray(255) : tile_new();
            if (!t)
                return FALSE;
            if (vw > TILE_SIZE) vw = TILE_SIZE;
            if (vh > TILE_SIZE) vh = TILE_SIZE;
            for (y = 0; y < vh; y++)
                CopyMem((APTR)(img + (y0 + y) * d->w + x0), t->px + (y << TILE_SHIFT), vw * 4);
            tile_update_flag(d, t, tx, ty);
            if (!keep_all && t->flag == TF_EMPTY)
                FreeVec(t);
            else
                tiles[ty * d->tw + tx] = t;
        }
    return TRUE;
}

static ULONG premul(ULONG p)
{
    ULONG a = p >> 24;
    return (a << 24) | ((((p >> 16) & 0xFF) * a / 255) << 16) | ((((p >> 8) & 0xFF) * a / 255) << 8) | ((p & 0xFF) * a / 255);
}

static ULONG unpremul(ULONG a, ULONG r, ULONG g, ULONG b)
{
    if (!a)
        return 0;
    if (a < 255) {
        r = r * 255 / a; g = g * 255 / a; b = b * 255 / a;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
    }
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* Skalieren src (sw x sh) -> dst (dw x dh) */
static void resample(const ULONG *src, LONG sw, LONG sh, ULONG *dst, LONG dw, LONG dh)
{
    float fx = (float)sw / dw, fy = (float)sh / dh;
    LONG x, y;
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++) {
            float a = 0, r = 0, g = 0, b = 0, wsum = 0;
            if (fx > 1.0f || fy > 1.0f) {
                /* Flaechenmittel ueber den Quellbereich */
                float x0 = x * fx, x1 = x0 + fx, y0 = y * fy, y1 = y0 + fy;
                LONG ix, iy;
                for (iy = (LONG)y0; iy < y1 && iy < sh; iy++) {
                    float wy = (iy + 1 < y1 ? iy + 1 : y1) - (iy > y0 ? iy : y0);
                    for (ix = (LONG)x0; ix < x1 && ix < sw; ix++) {
                        float wx = (ix + 1 < x1 ? ix + 1 : x1) - (ix > x0 ? ix : x0), w = wx * wy;
                        ULONG p = premul(src[iy * sw + ix]);
                        a += (p >> 24) * w; r += ((p >> 16) & 0xFF) * w;
                        g += ((p >> 8) & 0xFF) * w; b += (p & 0xFF) * w;
                        wsum += w;
                    }
                }
            } else {
                /* bilinear */
                float u = (x + 0.5f) * fx - 0.5f, v = (y + 0.5f) * fy - 0.5f;
                LONG iu = (LONG)floorf(u), iv = (LONG)floorf(v), k;
                float du = u - iu, dv = v - iv;
                for (k = 0; k < 4; k++) {
                    LONG sx = iu + (k & 1), sy = iv + (k >> 1);
                    float w = ((k & 1) ? du : 1.0f - du) * ((k >> 1) ? dv : 1.0f - dv);
                    ULONG p;
                    if (sx < 0) sx = 0;
                    if (sy < 0) sy = 0;
                    if (sx >= sw) sx = sw - 1;
                    if (sy >= sh) sy = sh - 1;
                    p = premul(src[sy * sw + sx]);
                    a += (p >> 24) * w; r += ((p >> 16) & 0xFF) * w;
                    g += ((p >> 8) & 0xFF) * w; b += (p & 0xFF) * w;
                    wsum += w;
                }
            }
            if (wsum > 0.0f) {
                a /= wsum; r /= wsum; g /= wsum; b /= wsum;
            }
            dst[y * dw + x] = unpremul((ULONG)(a + 0.5f), (ULONG)(r + 0.5f), (ULONG)(g + 0.5f), (ULONG)(b + 0.5f));
        }
}

/* Inhalt einer Ebene der alten Flaeche (Bild + Ueberhang), versetzt */
static const ULONG *cs_img;
static const Ovf *cs_ovf;
static LONG cs_w, cs_h, cs_ox, cs_oy;

static ULONG cs_get(LONG x, LONG y)
{
    x -= cs_ox;
    y -= cs_oy;
    if (x >= 0 && y >= 0 && x < cs_w && y < cs_h)
        return cs_img[y * cs_w + x];
    return cs_ovf ? ovf_get(cs_ovf, x, y) : 0;
}

static LONG build_fresh;

/* Maske der alten Flaeche samt Ueberhang (Grau; 0 = unbekannt) */
static const ULONG *cm_img;
static const Ovf *cm_ovf;

static ULONG cm_get(LONG x, LONG y)
{
    x -= cs_ox;
    y -= cs_oy;
    if (x >= 0 && y >= 0 && x < cs_w && y < cs_h)
        return cm_img[y * cs_w + x];
    return cm_ovf ? ovf_get(cm_ovf, x, y) : 0;
}

/* Drehen/Spiegeln der ganzen Flaeche (build mit rot_on) */
static BOOL rot_on, rot_fh, rot_fv, rot_exact;
static float rot_c, rot_s;

/* Ziel (x, y) -> Quelle: Mittelpunkte aufeinander, inverse Drehung */
static void rotate_img(const ULONG *src, LONG sw, LONG sh, ULONG *dst, LONG dw, LONG dh, ULONG empty)
{
    LONG x, y;
    float scx = sw * 0.5f, scy = sh * 0.5f, dcx = dw * 0.5f, dcy = dh * 0.5f;
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++) {
            float dx = x + 0.5f - dcx, dy = y + 0.5f - dcy, u, v;
            if (rot_fh) dx = -dx;
            if (rot_fv) dy = -dy;
            u = rot_c * dx + rot_s * dy + scx - 0.5f;
            v = -rot_s * dx + rot_c * dy + scy - 0.5f;
            if (rot_exact) {
                LONG sx = (LONG)floorf(u + 0.5f), sy = (LONG)floorf(v + 0.5f);
                dst[y * dw + x] = sx >= 0 && sy >= 0 && sx < sw && sy < sh ? src[sy * sw + sx] : empty;
            } else {
                LONG iu = (LONG)floorf(u), iv = (LONG)floorf(v), k;
                float du = u - iu, dv = v - iv, a = 0, r = 0, g = 0, b = 0;
                for (k = 0; k < 4; k++) {
                    LONG sx = iu + (k & 1), sy = iv + (k >> 1);
                    float w = ((k & 1) ? du : 1.0f - du) * ((k >> 1) ? dv : 1.0f - dv);
                    ULONG p = premul(sx >= 0 && sy >= 0 && sx < sw && sy < sh ? src[sy * sw + sx] : empty);
                    a += (p >> 24) * w; r += ((p >> 16) & 0xFF) * w;
                    g += ((p >> 8) & 0xFF) * w; b += (p & 0xFF) * w;
                }
                dst[y * dw + x] = unpremul((ULONG)(a + 0.5f), (ULONG)(r + 0.5f), (ULONG)(g + 0.5f), (ULONG)(b + 0.5f));
            }
        }
}

/* scale = TRUE: Bildgroesse, sonst Arbeitsflaeche mit Versatz (ox, oy) */
static Doc *build(Doc *s, LONG nw, LONG nh, BOOL scale, LONG ox, LONG oy)
{
    Doc *d = doc_new(nw, nh, DOC_EMPTY);
    ULONG *src = NULL, *dst = NULL;
    LONG i;
    float kx = (float)nw / s->w, ky = (float)nh / s->h;
    if (!d)
        return NULL;
    layer_free(d->layers[0], d);
    d->nlayers = 0;
    if (!(dst = AllocVec(nw * nh * 4, MEMF_ANY)))
        goto fail;
    for (i = 0; i < s->nlayers; i++) {
        Layer *sl = s->layers[i], *l = layer_new(d, sl->name);
        if (!l)
            goto fail;
        d->layers[d->nlayers++] = l;
        l->opacity = sl->opacity;
        l->visible = sl->visible;
        l->mode = sl->mode;
        l->mask_on = sl->mask_on;
        l->fill = sl->fill;
        l->lock = sl->lock;
        l->kind = sl->kind;
        l->depth = sl->depth;
        l->pass = sl->pass;
        l->collapsed = sl->collapsed;
        l->clip = sl->clip;
        if (sl->adj && (l->adj = AllocVec(sizeof(AdjInfo), MEMF_ANY)))
            *l->adj = *sl->adj;
        /* Pixel */
        if (!(src = tiles_to_image(s, sl->tiles, 0)))
            goto fail;
        if (rot_on)     /* Hintergrund: neue Ecken in Hintergrundfarbe */
            rotate_img(src, s->w, s->h, dst, nw, nh,
                       i == 0 && !strcmp(sl->name, "Background") ? 0xFF000000 | bg_color : 0);
        else if (scale)
            resample(src, s->w, s->h, dst, nw, nh);
        else {
            /* Ueberhang: kommt wieder zum Vorschein bzw. nimmt Abgeschnittenes auf */
            LONG x, y, x0 = 0, y0 = 0, x1 = s->w, y1 = s->h;
            cs_img = src; cs_ovf = sl->ovf;
            cs_w = s->w; cs_h = s->h; cs_ox = ox; cs_oy = oy;
            for (y = 0; y < nh; y++)
                for (x = 0; x < nw; x++)
                    dst[y * nw + x] = cs_get(x, y);
            if (sl->ovf) {
                if (sl->ovf->x < x0) x0 = sl->ovf->x;
                if (sl->ovf->y < y0) y0 = sl->ovf->y;
                if (sl->ovf->x + sl->ovf->w > x1) x1 = sl->ovf->x + sl->ovf->w;
                if (sl->ovf->y + sl->ovf->h > y1) y1 = sl->ovf->y + sl->ovf->h;
            }
            l->ovf = ovf_build(x0 + ox, y0 + oy, x1 + ox, y1 + oy, nw, nh, cs_get);
        }
        FreeVec(src);
        src = NULL;
        if (!image_to_tiles(d, dst, l->tiles, FALSE))
            goto fail;
        /* Maske (ausserhalb: zeigen) */
        if (sl->mask) {
            if (!(l->mask = AllocVec(d->tw * d->th * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR)) ||
                !(src = tiles_to_image(s, sl->mask, 0xFFFFFFFF)))
                goto fail;
            if (rot_on)
                rotate_img(src, s->w, s->h, dst, nw, nh, 0xFFFFFFFF);
            else if (scale)
                resample(src, s->w, s->h, dst, nw, nh);
            else {
                /* Arbeitsflaeche: Ueberhang der Maske kommt zurueck bzw. nimmt auf */
                LONG x, y, x0 = 0, y0 = 0, x1 = s->w, y1 = s->h;
                cm_img = src; cm_ovf = sl->movf;
                cs_w = s->w; cs_h = s->h; cs_ox = ox; cs_oy = oy;
                for (y = 0; y < nh; y++)
                    for (x = 0; x < nw; x++) {
                        ULONG v = cm_get(x, y);
                        dst[y * nw + x] = v ? v : 0xFFFFFFFF;
                    }
                if (sl->movf) {
                    if (sl->movf->x < x0) x0 = sl->movf->x;
                    if (sl->movf->y < y0) y0 = sl->movf->y;
                    if (sl->movf->x + sl->movf->w > x1) x1 = sl->movf->x + sl->movf->w;
                    if (sl->movf->y + sl->movf->h > y1) y1 = sl->movf->y + sl->movf->h;
                }
                l->movf = ovf_build(x0 + ox, y0 + oy, x1 + ox, y1 + oy, nw, nh, cm_get);
            }
            FreeVec(src);
            src = NULL;
            if (!image_to_tiles(d, dst, l->mask, TRUE))
                goto fail;
            mask_compact(d, l);
        }
        /* Ebenenstil: Groessen mitskalieren */
        if (sl->fx && (l->fx = AllocVec(sizeof(LayerFx), MEMF_ANY))) {
            float k = scale ? (kx < ky ? kx : ky) : 1.0f;
            *l->fx = *sl->fx;
            l->fx->sh_dist = (LONG)(l->fx->sh_dist * k + 0.5f);
            l->fx->sh_size = (LONG)(l->fx->sh_size * k + 0.5f);
            l->fx->gl_size = (LONG)(l->fx->gl_size * k + 0.5f);
            l->fx->st_size = (LONG)(l->fx->st_size * k + 0.5f);
            if (l->fx->st_size < 1) l->fx->st_size = 1;
        }
        /* Text bleibt editierbar; gedreht/gespiegelt: Anker und Matrix mitnehmen */
        if (sl->text && (l->text = AllocVec(sizeof(TextInfo), MEMF_ANY))) {
            TextInfo *t = l->text;
            *t = *sl->text;
            if (rot_on) {
                /* vorwaerts (Quelle -> Ziel) = F * R, R = (c -s; s c), F = Spiegelung */
                float fx = rot_fh ? -1.0f : 1.0f, fy = rot_fv ? -1.0f : 1.0f;
                float L[4], M[4], bx[4], by[4];
                LONG k;
                L[0] = fx * rot_c; L[1] = -fx * rot_s; L[2] = fy * rot_s; L[3] = fy * rot_c;
                if (t->m[0] || t->m[1] || t->m[2] || t->m[3])
                    for (k = 0; k < 4; k++)
                        M[k] = t->m[k] / 65536.0f;
                else {
                    M[0] = M[3] = 1.0f;
                    M[1] = M[2] = 0.0f;
                }
                t->m[0] = (LONG)floorf((L[0] * M[0] + L[1] * M[2]) * 65536.0f + 0.5f);
                t->m[1] = (LONG)floorf((L[0] * M[1] + L[1] * M[3]) * 65536.0f + 0.5f);
                t->m[2] = (LONG)floorf((L[2] * M[0] + L[3] * M[2]) * 65536.0f + 0.5f);
                t->m[3] = (LONG)floorf((L[2] * M[1] + L[3] * M[3]) * 65536.0f + 0.5f);
                /* Anker und Umriss (Ecken) mitnehmen */
                {
                    float dx = sl->text->x - s->w * 0.5f, dy = sl->text->y - s->h * 0.5f;
                    t->x = (LONG)floorf(nw * 0.5f + L[0] * dx + L[1] * dy + 0.5f);
                    t->y = (LONG)floorf(nh * 0.5f + L[2] * dx + L[3] * dy + 0.5f);
                }
                bx[0] = bx[2] = (float)sl->text->bx0; bx[1] = bx[3] = (float)sl->text->bx1;
                by[0] = by[1] = (float)sl->text->by0; by[2] = by[3] = (float)sl->text->by1;
                t->bx0 = t->by0 = 0x7FFFFFFF;
                t->bx1 = t->by1 = -0x7FFFFFFF;
                for (k = 0; k < 4; k++) {
                    float dx = bx[k] - s->w * 0.5f, dy = by[k] - s->h * 0.5f;
                    LONG X = (LONG)floorf(nw * 0.5f + L[0] * dx + L[1] * dy + 0.5f);
                    LONG Y = (LONG)floorf(nh * 0.5f + L[2] * dx + L[3] * dy + 0.5f);
                    if (X < t->bx0) t->bx0 = X;
                    if (X > t->bx1) t->bx1 = X;
                    if (Y < t->by0) t->by0 = Y;
                    if (Y > t->by1) t->by1 = Y;
                }
            } else if (scale) {
                t->x = (LONG)(t->x * kx); t->y = (LONG)(t->y * ky);
                t->size = (LONG)(t->size * (kx < ky ? kx : ky) + 0.5f);
                if (t->size < 1) t->size = 1;
                t->bx0 = (LONG)(t->bx0 * kx); t->bx1 = (LONG)(t->bx1 * kx);
                t->boxw = (LONG)(t->boxw * kx); t->boxh = (LONG)(t->boxh * ky);
                t->by0 = (LONG)(t->by0 * ky); t->by1 = (LONG)(t->by1 * ky);
            } else {
                t->x += ox; t->y += oy;
                t->bx0 += ox; t->bx1 += ox; t->by0 += oy; t->by1 += oy;
            }
        }
    }
    FreeVec(dst);
    d->active = s->active < d->nlayers ? s->active : d->nlayers - 1;
    d->layer_counter = s->layer_counter;
    strcpy(d->path, s->path);
    build_fresh = d->untitled;  /* frische Nummer (fuer Duplicate) */
    d->untitled = s->untitled;
    d->modified = TRUE;
    return d;
fail:
    if (src) FreeVec(src);
    if (dst) FreeVec(dst);
    doc_free(d);
    return NULL;
}

/* Tab auf einen anderen Groessen-Zustand umschalten (Undo/Redo) */
void doc_size_switch(Doc *to)
{
    LONG i = doc_index(doc);
    if (i < 0 || !to)
        return;
    view_save();
    docs[i] = to;
    doc = to;
    view_restore();
    doc_mark_all_dirty();
}

/* aktives Dokument im selben Tab ersetzen; das alte bleibt fuer Undo */
static BOOL replace(Doc *d, const char *name)
{
    LONG i = doc_index(doc), depth = 0;
    Doc *old = doc, *p;
    if (i < 0 || !d)
        return FALSE;
    if (old->size_next) {               /* alter Redo-Zweig verfaellt */
        Doc *n = old->size_next;
        old->size_next = NULL;
        n->size_prev = NULL;
        doc_free(n);
    }
    d->size_prev = old;
    old->size_next = d;
    strncpy(d->base_name, name, sizeof(d->base_name) - 1);
    /* hoechstens 4 alte Zustaende behalten */
    for (p = d; p->size_prev; p = p->size_prev)
        if (++depth > 4) {
            Doc *x = p->size_prev;
            p->size_prev = NULL;
            x->size_next = NULL;
            doc_free(x);
            break;
        }
    view_save();
    docs[i] = d;
    doc = d;
    doc_mark_all_dirty();
    d->vset = FALSE;
    view_restore();
    return TRUE;
}

BOOL doc_image_size(LONG nw, LONG nh)
{
    if (nw == doc->w && nh == doc->h)
        return TRUE;
    return replace(build(doc, nw, nh, TRUE, 0, 0), "Image Size");
}

/* anchor 0..8: 3x3-Raster (0 = oben links, 4 = Mitte) */
BOOL doc_canvas_size(LONG nw, LONG nh, LONG anchor)
{
    LONG ax = anchor % 3, ay = anchor / 3;
    LONG ox = ax == 0 ? 0 : ax == 1 ? (nw - doc->w) / 2 : nw - doc->w;
    LONG oy = ay == 0 ? 0 : ay == 1 ? (nh - doc->h) / 2 : nh - doc->h;
    if (nw == doc->w && nh == doc->h)
        return TRUE;
    return replace(build(doc, nw, nh, FALSE, ox, oy), "Canvas Size");
}

static const char *cr_name = "Crop";

/* Arbeitsflaeche auf das Rechteck x0..x1/y0..y1 setzen (darf groesser sein) */
BOOL doc_canvas_rect(LONG x0, LONG y0, LONG x1, LONG y1)
{
    if (x1 - x0 < 1 || y1 - y0 < 1)
        return FALSE;
    if (!x0 && !y0 && x1 == doc->w && y1 == doc->h)
        return TRUE;
    return replace(build(doc, x1 - x0, y1 - y0, FALSE, -x0, -y0), cr_name);
}

/* Image > Trim: transparenten Rand abschneiden */
BOOL doc_trim(void)
{
    LONG x, y, x0 = doc->w, y0 = doc->h, x1 = 0, y1 = 0;
    doc_process_dirty();
    for (y = 0; y < doc->h; y++) {
        const ULONG *p = doc->flat + y * doc->w;
        for (x = 0; x < doc->w; x++)
            if (p[x] >> 24) {
                if (x < x0) x0 = x;
                if (x >= x1) x1 = x + 1;
                if (y < y0) y0 = y;
                y1 = y + 1;
            }
    }
    if (x1 <= x0)
        return TRUE;            /* ganz leer: nichts zu tun */
    cr_name = "Trim";
    {
        BOOL r = doc_canvas_rect(x0, y0, x1, y1);
        cr_name = "Crop";
        return r;
    }
}

/* Image > Reveal All: Flaeche um alles Ueberstehende erweitern */
BOOL doc_reveal_all(void)
{
    LONG i, x0 = 0, y0 = 0, x1 = doc->w, y1 = doc->h;
    for (i = 0; i < doc->nlayers; i++) {
        LONG k;
        for (k = 0; k < 2; k++) {
            Ovf *o = k ? doc->layers[i]->movf : doc->layers[i]->ovf;
            if (!o || (k && !doc->layers[i]->ovf))
                continue;       /* Masken-Ueberhang nur, wo auch Pixel ueberstehen */
            if (o->x < x0) x0 = o->x;
            if (o->y < y0) y0 = o->y;
            if (o->x + o->w > x1) x1 = o->x + o->w;
            if (o->y + o->h > y1) y1 = o->y + o->h;
        }
    }
    cr_name = "Reveal All";
    {
        BOOL r = doc_canvas_rect(x0, y0, x1, y1);
        cr_name = "Crop";
        return r;
    }
}

/* Image > Duplicate: Kopie als neues Dokument */
Doc *doc_duplicate(void)
{
    Doc *d = build(doc, doc->w, doc->h, FALSE, 0, 0);
    LONG i;
    if (!d)
        return NULL;
    d->path[0] = 0;
    d->untitled = build_fresh;
    d->modified = TRUE;
    for (i = 0; i < d->tw * d->th; i++)
        d->dirty[i] = 1;
    d->anydirty = TRUE;
    return d;
}

/* Image > Image Rotation: deg im Uhrzeigersinn, dazu Spiegeln der Flaeche */
BOOL doc_rotate(LONG deg, BOOL fh, BOOL fv, const char *name)
{
    float a = deg * 3.14159265f / 180.0f;
    LONG nw, nh, d = ((deg % 360) + 360) % 360;
    BOOL r;
    rot_exact = d % 90 == 0;
    if (rot_exact) {            /* exakt, ohne Rundungsfehler */
        rot_c = d == 0 ? 1.0f : d == 180 ? -1.0f : 0.0f;
        rot_s = d == 90 ? 1.0f : d == 270 ? -1.0f : 0.0f;
    } else {
        rot_c = cosf(a);
        rot_s = sinf(a);
    }
    nw = (LONG)ceilf(fabsf(rot_c) * doc->w + fabsf(rot_s) * doc->h - 0.01f);
    nh = (LONG)ceilf(fabsf(rot_s) * doc->w + fabsf(rot_c) * doc->h - 0.01f);
    rot_fh = fh;
    rot_fv = fv;
    rot_on = TRUE;
    r = replace(build(doc, nw, nh, FALSE, 0, 0), name);
    rot_on = FALSE;
    return r;
}
