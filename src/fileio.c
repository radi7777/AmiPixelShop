/*
 * fileio.c - Bilder laden und speichern
 *
 * Laden:     .aps (eigenes Format), sonst stb_image (PNG, JPEG, PSD, GIF,
 *            BMP, TGA), sonst Datatypes (ILBM und alles Installierte)
 * Speichern: .aps (Ebenen), .png (mit Alpha), .jpg/.jpeg (auf Weiss)
 *
 * .aps-Format (big-endian):
 *   "APX1", ULONG version=1, w, h, nlayers, active
 *   je Ebene: char name[32], UBYTE opacity, UBYTE visible, UBYTE mode, UBYTE 0,
 *             UBYTE vorhanden[tw*th], ULONG zlib-Laenge, zlib(Kacheln)
 *   Kachel: 4 Ebenen A, R, G, B zu je 64x64 Bytes, jedes Byte als Differenz
 *           zum linken Nachbarn (erste Spalte: zum oberen), wie PNG "Sub".
 *   ab Version 3 je Ebene danach Abschnitte: ULONG tag, ULONG laenge, Daten;
 *   tag 0 beendet. "MASK": ULONG an, ULONG zlib-Laenge, zlib(Grauwerte aller
 *   Kacheln, je 64x64 Bytes); "TEXT": struct TextInfo.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <datatypes/datatypes.h>
#include <datatypes/pictureclass.h>
#include <intuition/classusr.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/datatypes.h>
#include <clib/alib_protos.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "apx.h"
#include "quant.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include "zlib.h"

extern unsigned char *stbi_zlib_compress(unsigned char *data, int data_len, int *out_len, int quality);

struct Library *DataTypesBase;

#define APS_MAGIC   0x41505831  /* "APX1" */
#define APS_VERSION 3     /* 3: Abschnitte je Ebene (Maske, Text) */
#define TAG_MASK 0x4D41534B /* "MASK" */
#define TAG_TEXT 0x54455854 /* "TEXT" */
#define TAG_LFX  0x4C465820 /* "LFX " */
#define TAG_FLLK 0x464C4C4B /* "FLLK": ULONG fill, ULONG lock */
#define TAG_KIND 0x4B494E44 /* "KIND": ULONG kind, depth, pass, collapsed */
#define TAG_ADJ  0x41444A20 /* "ADJ ": AdjInfo */
#define TAG_CLIP 0x434C4950 /* "CLIP": Schnittmaske (ohne Daten) */
#define TAG_OVF  0x4F564620 /* "OVF ": x, y, w, h, zlib-Laenge, zlib(ARGB) */
#define TAG_MOVF 0x4D4F5646 /* "MOVF": wie OVF, Ueberhang der Maske */
#define TAG_PATH 0x50415448 /* "PATH": nach der letzten Ebene, Arbeitspfad */
#define TAG_AMIG 0x414D4947 /* "AMIG": Farbtiefe und Aufloesung des Dokuments */
#define TAG_QPAL 0x5150414C /* "QPAL": Palette der letzten Farbreduktion */
#define TAG_FPAL 0x4650414C /* "FPAL": vorgegebene Palette (Fixed) */
#define TAG_ALVL 0x414C564C /* "ALVL": Levels je Kanal einer Korrekturebene: LONG lv[3][5] */
#define TAG_TMAT 0x544D4154 /* "TMAT": Transformation einer Textebene: LONG m[4] (16.16) */
#define TAG_GUID 0x47554944 /* "GUID": Hilfslinien: n, dann n * (Lage, senkrecht) */
#define TAG_GENI 0x47454E49 /* "GENI": generative Ebene: ULONG mode, Prompt (UTF-8, 0-terminiert) */

/* Kachel -> 4 Kanalebenen mit Differenzfilter */
static void tile_encode(const ULONG *px, UBYTE *out)
{
    LONG c, x, y;
    for (c = 0; c < 4; c++) {
        LONG sh = 24 - c * 8;
        for (y = 0; y < TILE_SIZE; y++) {
            const ULONG *row = px + y * TILE_SIZE;
            UBYTE prev = y ? (UBYTE)(px[(y - 1) * TILE_SIZE] >> sh) : 0;
            for (x = 0; x < TILE_SIZE; x++) {
                UBYTE v = (UBYTE)(row[x] >> sh);
                *out++ = (UBYTE)(v - prev);
                prev = v;
            }
        }
    }
}

static void tile_decode(const UBYTE *in, ULONG *px)
{
    LONG c, x, y;
    memset(px, 0, TILE_PIX * 4);
    for (c = 0; c < 4; c++) {
        LONG sh = 24 - c * 8;
        for (y = 0; y < TILE_SIZE; y++) {
            ULONG *row = px + y * TILE_SIZE;
            UBYTE prev = y ? (UBYTE)(px[(y - 1) * TILE_SIZE] >> sh) : 0;
            for (x = 0; x < TILE_SIZE; x++) {
                prev = (UBYTE)(prev + *in++);
                row[x] |= (ULONG)prev << sh;
            }
        }
    }
}

static BOOL ext_is(const char *path, const char *e)
{
    LONG lp = strlen(path), le = strlen(e), i;
    if (lp < le)
        return FALSE;
    for (i = 0; i < le; i++)
        if (tolower((unsigned char)path[lp - le + i]) != e[i])
            return FALSE;
    return TRUE;
}

/* ---- Dokument aus ARGB-Puffer (straight) ---- */

static Doc *doc_from_argb(LONG w, LONG h, const ULONG *src)
{
    Doc *d = doc_new(w, h, DOC_EMPTY);
    Layer *l;
    LONG tx, ty, y;
    if (!d)
        return NULL;
    l = d->layers[0];
    for (ty = 0; ty < d->th; ty++)
        for (tx = 0; tx < d->tw; tx++) {
            LONG x0 = tx << TILE_SHIFT, y0 = ty << TILE_SHIFT;
            LONG vw = w - x0, vh = h - y0;
            Tile *t = tile_new();
            if (!t) {
                doc_free(d);
                return NULL;
            }
            if (vw > TILE_SIZE) vw = TILE_SIZE;
            if (vh > TILE_SIZE) vh = TILE_SIZE;
            for (y = 0; y < vh; y++)
                CopyMem((APTR)(src + (y0 + y) * w + x0), t->px + y * TILE_SIZE, vw * 4);
            tile_update_flag(d, t, tx, ty);
            if (t->flag == TF_EMPTY)
                FreeVec(t);
            else
                l->tiles[ty * d->tw + tx] = t;
        }
    return d;
}

/* ---- Laden ueber stb_image ---- */

static Doc *load_stb(const char *path)
{
    int w, h, n;
    unsigned char *data = stbi_load(path, &w, &h, &n, 4);
    ULONG *argb;
    Doc *d;
    LONG i;
    if (!data)
        return NULL;
    /* RGBA-Bytes -> ARGB, in place */
    argb = (ULONG *)data;
    for (i = 0; i < (LONG)w * h; i++) {
        ULONG v = argb[i];      /* R G B A (big-endian gelesen) */
        argb[i] = (v >> 8) | (v << 24);
    }
    d = doc_from_argb(w, h, argb);
    stbi_image_free(data);
    return d;
}

/* ---- Laden ueber Datatypes ---- */

/* Bild ueber Datatypes als ARGB lesen (Datei oder Clipboard).
 * name: Dateiname bzw. Clipboard-Unit, srctype: DTST_FILE / DTST_CLIPBOARD */
ULONG *dt_read_argb(APTR name, ULONG srctype, LONG *pw, LONG *ph, char *err)
{
    Object *o;
    struct BitMapHeader *bmh = NULL;
    ULONG *buf;
    LONG w, h, i;
    struct gpLayout gpl;
    struct pdtBlitPixelArray bpa;

    if (!DataTypesBase && !(DataTypesBase = OpenLibrary("datatypes.library", 43))) {
        strcpy(err, "datatypes.library V43 is missing");
        return NULL;
    }
    o = NewDTObject(name,
                    DTA_SourceType, srctype,
                    DTA_GroupID, GID_PICTURE,
                    PDTA_DestMode, PMODE_V43,
                    PDTA_Remap, FALSE,
                    TAG_DONE);
    if (!o) {
        strcpy(err, "Unknown file format");
        return NULL;
    }
    GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, TAG_DONE);
    if (!bmh) {
        DisposeDTObject(o);
        strcpy(err, "Not a picture");
        return NULL;
    }
    w = bmh->bmh_Width;
    h = bmh->bmh_Height;

    gpl.MethodID = DTM_PROCLAYOUT;
    gpl.gpl_GInfo = NULL;
    gpl.gpl_Initial = 1;
    DoMethodA(o, (Msg)&gpl);

    buf = AllocVec(w * h * 4, MEMF_ANY);
    if (buf) {
        bpa.MethodID = PDTM_READPIXELARRAY;
        bpa.pbpa_PixelData = buf;
        bpa.pbpa_PixelFormat = PBPAFMT_ARGB;
        bpa.pbpa_PixelArrayMod = w * 4;
        bpa.pbpa_Left = 0;
        bpa.pbpa_Top = 0;
        bpa.pbpa_Width = w;
        bpa.pbpa_Height = h;
        DoMethodA(o, (Msg)&bpa);
        if (bmh->bmh_Masking != mskHasAlpha)
            for (i = 0; i < w * h; i++)
                buf[i] |= 0xFF000000;
        *pw = w;
        *ph = h;
    } else
        strcpy(err, "Not enough memory");
    DisposeDTObject(o);
    return buf;
}

static Doc *load_datatypes(const char *path, char *err)
{
    LONG w, h;
    ULONG *buf = dt_read_argb((APTR)path, DTST_FILE, &w, &h, err);
    Doc *d;
    if (!buf)
        return NULL;
    d = doc_from_argb(w, h, buf);
    FreeVec(buf);
    if (!d)
        strcpy(err, "Not enough memory");
    return d;
}

/* oeffentlich fuer die Zwischenablage */
Doc *doc_from_argb_pub(LONG w, LONG h, const ULONG *src)
{
    return doc_from_argb(w, h, src);
}

/* ---- eigenes Format ---- */

static BOOL rd(BPTR fh, APTR p, LONG n) { return Read(fh, p, n) == n; }
static BOOL wr(BPTR fh, CONST_APTR p, LONG n) { return Write(fh, (APTR)p, n) == n; }
static BOOL wr32(BPTR fh, ULONG v) { return wr(fh, &v, 4); }

static BOOL rd32(BPTR fh, ULONG *v) { return Read(fh, v, 4) == 4; }

static BOOL write_chunks(BPTR fh, Doc *d, Layer *l)
{
    LONG nt = d->tw * d->th, i, k;
    if (l->mask) {
        UBYTE *g = malloc(nt * TILE_PIX);
        unsigned char *comp;
        int clen = 0;
        BOOL ok;
        if (!g)
            return FALSE;
        for (i = 0; i < nt; i++)
            for (k = 0; k < TILE_PIX; k++)
                g[i * TILE_PIX + k] = l->mask[i] ? (UBYTE)l->mask[i]->px[k] : 255;
        comp = stbi_zlib_compress(g, nt * TILE_PIX, &clen, 8);
        free(g);
        if (!comp)
            return FALSE;
        ok = wr32(fh, TAG_MASK) && wr32(fh, 8 + clen) && wr32(fh, l->mask_on) &&
             wr32(fh, clen) && wr(fh, comp, clen);
        free(comp);
        if (!ok)
            return FALSE;
    }
    if (l->text && (!wr32(fh, TAG_TEXT) || !wr32(fh, TEXTINFO_V3) || !wr(fh, l->text, TEXTINFO_V3)))
        return FALSE;
    if (l->text && (l->text->m[0] || l->text->m[1] || l->text->m[2] || l->text->m[3]) &&
        (!wr32(fh, TAG_TMAT) || !wr32(fh, 16) || !wr(fh, l->text->m, 16)))
        return FALSE;
    if (l->movf) {
        Ovf *o = l->movf;
        int clen = 0;
        unsigned char *comp = stbi_zlib_compress((unsigned char *)o->px, o->w * o->h * 4, &clen, 8);
        BOOL ok;
        if (!comp)
            return FALSE;
        ok = wr32(fh, TAG_MOVF) && wr32(fh, 20 + clen) && wr32(fh, o->x) && wr32(fh, o->y) &&
             wr32(fh, o->w) && wr32(fh, o->h) && wr32(fh, clen) && wr(fh, comp, clen);
        free(comp);
        if (!ok)
            return FALSE;
    }
    if (l->ovf) {
        Ovf *o = l->ovf;
        int clen = 0;
        unsigned char *comp = stbi_zlib_compress((unsigned char *)o->px, o->w * o->h * 4, &clen, 8);
        BOOL ok;
        if (!comp)
            return FALSE;
        ok = wr32(fh, TAG_OVF) && wr32(fh, 20 + clen) && wr32(fh, o->x) && wr32(fh, o->y) &&
             wr32(fh, o->w) && wr32(fh, o->h) && wr32(fh, clen) && wr(fh, comp, clen);
        free(comp);
        if (!ok)
            return FALSE;
    }
    if ((l->kind || l->depth) &&
        (!wr32(fh, TAG_KIND) || !wr32(fh, 16) || !wr32(fh, l->kind) || !wr32(fh, l->depth) ||
         !wr32(fh, l->pass) || !wr32(fh, l->collapsed)))
        return FALSE;
    if (l->clip && (!wr32(fh, TAG_CLIP) || !wr32(fh, 0)))
        return FALSE;
    if (l->gen) {
        LONG pl = (strlen(l->gen->prompt) + 4) & ~3;    /* mit 0, auf 4 Bytes */
        char pb[204];
        memset(pb, 0, sizeof(pb));
        strcpy(pb, l->gen->prompt);
        if (!wr32(fh, TAG_GENI) || !wr32(fh, 4 + pl) || !wr32(fh, l->gen->mode) || !wr(fh, pb, pl))
            return FALSE;
    }
    /* ADJ in der Groesse bis 0.6 (aeltere Versionen lesen ihn weiter), Kanal-Levels getrennt */
    if (l->adj && (!wr32(fh, TAG_ADJ) || !wr32(fh, ADJINFO_V1) || !wr(fh, l->adj, ADJINFO_V1)))
        return FALSE;
    if (l->adj && l->adj->cmd == CMD_ADJ_LEVELS &&
        (l->adj->lv[0][1] || l->adj->lv[1][1] || l->adj->lv[2][1]) &&
        (!wr32(fh, TAG_ALVL) || !wr32(fh, sizeof(l->adj->lv)) || !wr(fh, l->adj->lv, sizeof(l->adj->lv))))
        return FALSE;
    if ((l->fill != 255 || l->lock) &&
        (!wr32(fh, TAG_FLLK) || !wr32(fh, 8) || !wr32(fh, l->fill) || !wr32(fh, l->lock)))
        return FALSE;
    if (l->fx && (!wr32(fh, TAG_LFX) || !wr32(fh, sizeof(LayerFx)) || !wr(fh, l->fx, sizeof(LayerFx))))
        return FALSE;
    return wr32(fh, 0);
}

static BOOL read_chunks(BPTR fh, Doc *d, Layer *l, char *err)
{
    LONG nt = d->tw * d->th, i, k;
    for (;;) {
        ULONG tag, len;
        if (!rd32(fh, &tag))
            break;
        if (!tag)
            return TRUE;
        if (!rd32(fh, &len))
            break;
        if (tag == TAG_MASK && len >= 8) {
            ULONG on, clen;
            char *comp, *raw;
            int rawlen = 0;
            if (!rd32(fh, &on) || !rd32(fh, &clen) || !(comp = malloc(clen)))
                break;
            if (!rd(fh, comp, clen)) {
                free(comp);
                break;
            }
            raw = stbi_zlib_decode_malloc_guesssize(comp, clen, nt * TILE_PIX, &rawlen);
            free(comp);
            if (!raw || rawlen != nt * TILE_PIX) {
                if (raw) free(raw);
                break;
            }
            if ((l->mask = AllocVec(nt * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR))) {
                for (i = 0; i < nt; i++)
                    if ((l->mask[i] = tile_new_gray(255)))
                        for (k = 0; k < TILE_PIX; k++)
                            l->mask[i]->px[k] = 0xFF000000 | ((UBYTE)raw[i * TILE_PIX + k] * 0x010101);
                l->mask_on = on ? 1 : 0;
                mask_compact(d, l);
            }
            free(raw);
        } else if ((tag == TAG_OVF || tag == TAG_MOVF) && len >= 20) {
            ULONG v[5];
            char *comp, *raw;
            int rawlen = 0;
            if (!rd(fh, v, 20) || !(comp = malloc(v[4])))
                break;
            if (!rd(fh, comp, v[4])) {
                free(comp);
                break;
            }
            raw = stbi_zlib_decode_malloc_guesssize(comp, v[4], v[2] * v[3] * 4, &rawlen);
            free(comp);
            if (!raw || rawlen != (int)(v[2] * v[3] * 4)) {
                if (raw) free(raw);
                break;
            }
            {
                Ovf *o = ovf_new(v[0], v[1], v[2], v[3]);
                if (o) {
                    CopyMem(raw, o->px, rawlen);
                    if (tag == TAG_MOVF)
                        l->movf = o;
                    else
                        l->ovf = o;
                }
            }
            free(raw);
        } else if (tag == TAG_CLIP && len == 0) {
            l->clip = 1;
        } else if (tag == TAG_GENI && len >= 8 && len <= 4 + 200) {
            ULONG mode;
            GenInfo *g = AllocVec(sizeof(GenInfo), MEMF_ANY | MEMF_CLEAR);
            if (!g || !rd32(fh, &mode) || !rd(fh, g->prompt, len - 4)) {
                if (g) FreeVec(g);
                break;
            }
            g->prompt[sizeof(g->prompt) - 1] = 0;
            g->mode = mode <= GEN_EXTEND ? (LONG)mode : GEN_FILL;
            if (l->gen)
                ai_gen_free(l->gen);
            l->gen = g;
        } else if (tag == TAG_KIND && len == 16) {
            ULONG v[4];
            if (!rd(fh, v, 16))
                break;
            l->kind = (UBYTE)(v[0] <= LK_ADJUST ? v[0] : LK_PIXEL);
            l->depth = (UBYTE)(v[1] < MAX_DEPTH ? v[1] : MAX_DEPTH - 1);
            l->pass = (UBYTE)v[2];
            l->collapsed = (UBYTE)v[3];
        } else if (tag == TAG_ADJ && len == ADJINFO_V1) {
            if ((l->adj = AllocVec(sizeof(AdjInfo), MEMF_ANY | MEMF_CLEAR))) {
                if (!rd(fh, l->adj, ADJINFO_V1))
                    break;
            } else
                Seek(fh, len, OFFSET_CURRENT);
        } else if (tag == TAG_ALVL && len == sizeof(((AdjInfo *)0)->lv) && l->adj) {
            if (!rd(fh, l->adj->lv, len))
                break;
        } else if (tag == TAG_FLLK && len == 8) {
            ULONG v[2];
            if (!rd(fh, v, 8))
                break;
            l->fill = (UBYTE)v[0];
            l->lock = (UBYTE)(v[1] & LOCK_ALL);
        } else if (tag == TAG_LFX && len == sizeof(LayerFx)) {
            if ((l->fx = AllocVec(sizeof(LayerFx), MEMF_ANY))) {
                if (!rd(fh, l->fx, sizeof(LayerFx)))
                    break;
            } else
                Seek(fh, len, OFFSET_CURRENT);
        } else if (tag == TAG_TMAT && len == 16 && l->text) {
            if (!rd(fh, l->text->m, 16))
                break;
        } else if (tag == TAG_TEXT && (len == TEXTINFO_V3 || len == TEXTINFO_V1 || len == TEXTINFO_V2)) {
            if ((l->text = AllocVec(sizeof(TextInfo), MEMF_ANY | MEMF_CLEAR))) {
                if (!rd(fh, l->text, len))
                    break;
                l->text->str[sizeof(l->text->str) - 1] = 0;
                l->text->font[sizeof(l->text->font) - 1] = 0;
            } else
                Seek(fh, len, OFFSET_CURRENT);
        } else
            Seek(fh, len, OFFSET_CURRENT);  /* unbekannt: ueberspringen */
    }
    strcpy(err, "The file is damaged");
    return FALSE;
}

static Doc *load_aps(const char *path, char *err)
{
    BPTR fh = Open(path, MODE_OLDFILE);
    ULONG hdr[6];
    Doc *d = NULL;
    UBYTE *present = NULL;
    LONG li, i;

    if (!fh) {
        strcpy(err, "File not found");
        return NULL;
    }
    if (!rd(fh, hdr, sizeof(hdr)) || hdr[0] != APS_MAGIC || hdr[1] < 2 || hdr[1] > APS_VERSION ||
        hdr[2] < 1 || hdr[3] < 1 || hdr[2] > 16000 || hdr[3] > 16000 ||
        hdr[4] < 1 || hdr[4] > MAX_LAYERS) {
        strcpy(err, "Not a valid .aps file");
        goto fail;
    }
    if (!(d = doc_new(hdr[2], hdr[3], DOC_EMPTY)) ||
        !(present = AllocVec(d->tw * d->th, MEMF_ANY))) {
        strcpy(err, "Not enough memory");
        goto fail;
    }
    for (li = 0; li < (LONG)hdr[4]; li++) {
        struct { char name[32]; UBYTE opacity, visible, mode, pad; } lh;
        ULONG clen;
        LONG np = 0, k = 0;
        char *comp, *raw;
        int rawlen = 0;
        Layer *l;

        if (!rd(fh, &lh, sizeof(lh)) || !rd(fh, present, d->tw * d->th) || !rd(fh, &clen, 4)) {
            strcpy(err, "The file is damaged");
            goto fail;
        }
        lh.name[31] = 0;
        if (li == 0) {
            l = d->layers[0];
            strcpy(l->name, lh.name);
        } else {
            if (!(l = layer_new(d, lh.name))) {
                strcpy(err, "Not enough memory");
                goto fail;
            }
            d->layers[d->nlayers++] = l;
        }
        l->opacity = lh.opacity;
        l->visible = lh.visible;
        l->mode = lh.mode < BM_COUNT ? lh.mode : BM_NORMAL;
        for (i = 0; i < d->tw * d->th; i++)
            np += present[i] != 0;
        if (np) {
            if (!(comp = malloc(clen))) {
                strcpy(err, "Not enough memory");
                goto fail;
            }
            if (!rd(fh, comp, clen)) {
                free(comp);
                strcpy(err, "The file is damaged");
                goto fail;
            }
            raw = stbi_zlib_decode_malloc_guesssize(comp, clen, np * TILE_PIX * 4, &rawlen);
            free(comp);
            if (!raw || rawlen != np * TILE_PIX * 4) {
                if (raw)
                    free(raw);
                strcpy(err, "The file is damaged");
                goto fail;
            }
            for (i = 0; i < d->tw * d->th; i++) {
                Tile *t;
                if (!present[i])
                    continue;
                if (!(t = tile_new())) {
                    free(raw);
                    strcpy(err, "Not enough memory");
                    goto fail;
                }
                tile_decode((UBYTE *)raw + k * TILE_PIX * 4, t->px);
                k++;
                tile_update_flag(d, t, i % d->tw, i / d->tw);
                l->tiles[i] = t;
            }
            free(raw);
        }
        if (hdr[1] >= 3 && !read_chunks(fh, d, l, err))
            goto fail;
    }
    d->active = hdr[5] < (ULONG)d->nlayers ? (LONG)hdr[5] : d->nlayers - 1;
    {
        /* Bloecke hinter den Ebenen: Dokument-Angaben, Arbeitspfad */
        ULONG tag, len, np, ns, v[2];
        Path *p;
        while (rd32(fh, &tag) && rd32(fh, &len)) {
            if (tag == TAG_AMIG && len == 8) {
                if (!rd(fh, v, 8))
                    break;
                d->colors = (LONG)v[0];
                d->ppi = (LONG)v[1];
            } else if (tag == TAG_PATH && len >= 8) {
                if (!rd32(fh, &np) || !rd32(fh, &ns))
                    break;
                if (np <= PATH_MAXPTS && ns <= PATH_MAXSUB && len == 8 + ns * 8 + np * sizeof(PathPt) &&
                    (p = AllocVec(sizeof(Path), MEMF_ANY | MEMF_CLEAR))) {
                    BOOL ok = TRUE;
                    p->npts = np;
                    p->nsub = ns;
                    for (i = 0; i < (LONG)ns && ok; i++) {
                        ok = rd(fh, v, 8) && v[0] < np;
                        p->sub[i] = v[0];
                        p->closed[i] = v[1] != 0;
                    }
                    if (ok && rd(fh, p->pt, np * sizeof(PathPt))) {
                        if (d->wpath)
                            FreeVec(d->wpath);
                        d->wpath = p;
                    } else {
                        FreeVec(p);
                        break;
                    }
                } else if (Seek(fh, len - 8, OFFSET_CURRENT) < 0)
                    break;
            } else if ((tag == TAG_QPAL || tag == TAG_FPAL) && len >= 36 && len <= 36 + 256 * 4) {
                QPalette *q = AllocVec(sizeof(QPalette), MEMF_ANY | MEMF_CLEAR);
                ULONG h[9];
                BOOL ok = q && rd(fh, h, 36) && h[6] == (len - 36) / 4 && h[6] >= 1 &&
                          rd(fh, q->col, h[6] * 4);
                if (!ok) {
                    if (q) FreeVec(q);
                    break;
                }
                q->n = (LONG)h[6];
                q->mode = (LONG)h[7];
                q->ocs = (h[8] & 1) != 0;
                q->transp = (h[8] & 2) != 0;
                if (tag == TAG_FPAL) {
                    if (d->fixpal)
                        FreeVec(d->fixpal);
                    d->fixpal = q;
                } else {
                    for (i = 0; i < 6; i++)
                        d->qopt[i] = (LONG)h[i];
                    if (d->qpal)
                        FreeVec(d->qpal);
                    d->qpal = q;
                }
            } else if (tag == TAG_GUID && len >= 4 && rd32(fh, &np)) {
                for (i = 0; i < (LONG)np && (ULONG)(4 + i * 8 + 8) <= len; i++) {
                    if (!rd(fh, v, 8))
                        break;
                    if (d->nguides < MAX_GUIDES) {
                        d->guide[d->nguides] = (LONG)v[0];
                        d->gvert[d->nguides++] = v[1] != 0;
                    }
                }
                if (i < (LONG)np || (4 + np * 8 < len && Seek(fh, len - 4 - np * 8, OFFSET_CURRENT) < 0))
                    break;
            } else if (Seek(fh, len, OFFSET_CURRENT) < 0)
                break;              /* unbekannter Block: ueberspringen */
        }
    }
    d->layer_counter = d->nlayers;
    FreeVec(present);
    Close(fh);
    return d;
fail:
    if (present) FreeVec(present);
    if (d) doc_free(d);
    Close(fh);
    return NULL;
}

static BOOL wr_qpal(BPTR fh, ULONG tag, const QPalette *q, const LONG *opt)
{
    LONG i;
    if (!wr32(fh, tag) || !wr32(fh, 36 + q->n * 4))
        return FALSE;
    for (i = 0; i < 6; i++)
        if (!wr32(fh, opt ? opt[i] : 0))
            return FALSE;
    return wr32(fh, q->n) && wr32(fh, q->mode) && wr32(fh, (q->ocs ? 1 : 0) | (q->transp ? 2 : 0)) &&
           wr(fh, q->col, q->n * 4);
}

static BOOL save_aps(Doc *d, const char *path, char *err)
{
    BPTR fh = Open(path, MODE_NEWFILE);
    UBYTE *present = NULL;
    BOOL ok = FALSE;
    LONG li, i, nt = d->tw * d->th;

    if (!fh) {
        strcpy(err, "Cannot create the file");
        return FALSE;
    }
    if (!(present = AllocVec(nt, MEMF_ANY)))
        goto out;
    if (!wr32(fh, APS_MAGIC) || !wr32(fh, APS_VERSION) || !wr32(fh, d->w) || !wr32(fh, d->h) ||
        !wr32(fh, d->nlayers) || !wr32(fh, d->active))
        goto out;
    for (li = 0; li < d->nlayers; li++) {
        Layer *l = d->layers[li];
        struct { char name[32]; UBYTE opacity, visible, mode, pad; } lh;
        LONG np = 0, k = 0;
        UBYTE *raw;
        unsigned char *comp;
        int clen = 0;

        memset(&lh, 0, sizeof(lh));
        strncpy(lh.name, l->name, 31);
        lh.opacity = l->opacity;
        lh.visible = l->visible;
        lh.mode = l->mode;
        for (i = 0; i < nt; i++) {
            present[i] = l->tiles[i] && l->tiles[i]->flag != TF_EMPTY;
            np += present[i];
        }
        if (!wr(fh, &lh, sizeof(lh)) || !wr(fh, present, nt))
            goto out;
        if (!np) {
            if (!wr32(fh, 0) || !write_chunks(fh, d, l))
                goto out;
            continue;
        }
        if (!(raw = malloc(np * TILE_PIX * 4)))
            goto out;
        for (i = 0; i < nt; i++)
            if (present[i])
                tile_encode(l->tiles[i]->px, raw + (k++) * TILE_PIX * 4);
        comp = stbi_zlib_compress(raw, np * TILE_PIX * 4, &clen, 8);
        free(raw);
        if (!comp)
            goto out;
        if (!wr32(fh, clen) || !wr(fh, comp, clen)) {
            free(comp);
            goto out;
        }
        free(comp);
        if (!write_chunks(fh, d, l))
            goto out;
    }
    /* Dokument-Angaben (aeltere Versionen lesen hinter den Ebenen nichts mehr) */
    if ((d->colors || d->ppi) &&
        (!wr32(fh, TAG_AMIG) || !wr32(fh, 8) || !wr32(fh, d->colors) || !wr32(fh, d->ppi)))
        goto out;
    /* Arbeitspfad */
    if (d->wpath && d->wpath->npts) {
        Path *p = d->wpath;
        LONG len = 8 + p->nsub * 8 + p->npts * sizeof(PathPt);
        if (!wr32(fh, TAG_PATH) || !wr32(fh, len) || !wr32(fh, p->npts) || !wr32(fh, p->nsub))
            goto out;
        for (i = 0; i < p->nsub; i++)
            if (!wr32(fh, p->sub[i]) || !wr32(fh, p->closed[i]))
                goto out;
        if (!wr(fh, p->pt, p->npts * sizeof(PathPt)))
            goto out;
    }
    /* Paletten (nach PATH: 0.3 liest dahinter nichts mehr) */
    if ((d->qpal && !wr_qpal(fh, TAG_QPAL, d->qpal, d->qopt)) ||
        (d->fixpal && !wr_qpal(fh, TAG_FPAL, d->fixpal, NULL)))
        goto out;
    if (d->nguides) {
        if (!wr32(fh, TAG_GUID) || !wr32(fh, 4 + d->nguides * 8) || !wr32(fh, d->nguides))
            goto out;
        for (i = 0; i < d->nguides; i++)
            if (!wr32(fh, d->guide[i]) || !wr32(fh, d->gvert[i]))
                goto out;
    }
    ok = TRUE;
out:
    if (present)
        FreeVec(present);
    Close(fh);
    if (!ok) {
        strcpy(err, "Write error (disk full?)");
        DeleteFile(path);
    }
    return ok;
}

/* ---- Komposit als RGBA-Bytes (straight) ---- */

/* Einzige sichtbare Ebene mit voller Deckkraft? Dann verlustfrei direkt
 * aus den Kacheln exportieren statt aus dem vormultiplizierten Komposit. */
static Layer *single_layer(Doc *d)
{
    Layer *found = NULL;
    LONG i;
    for (i = 0; i < d->nlayers; i++)
        if (d->layers[i]->visible && d->layers[i]->opacity) {
            if (found || d->layers[i]->opacity != 255)
                return NULL;
            found = d->layers[i];
        }
    return found;
}

static UBYTE *flat_rgba(Doc *d, BOOL on_white, BOOL *has_alpha)
{
    UBYTE *buf = malloc(d->w * d->h * 4), *o;
    Layer *l = single_layer(d);
    LONG x, y;
    BOOL alpha = FALSE;
    if (!buf)
        return NULL;
    o = buf;
    for (y = 0; y < d->h; y++)
        for (x = 0; x < d->w; x++) {
            ULONG p, a, r, g, b;
            if (l) {
                Tile *t = l->tiles[(y >> TILE_SHIFT) * d->tw + (x >> TILE_SHIFT)];
                p = t ? t->px[((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK)] : 0;
                a = p >> 24;
                r = (p >> 16) & 0xFF; g = (p >> 8) & 0xFF; b = p & 0xFF;
                if (on_white) {
                    r = (r * a + 255 * (255 - a)) / 255;
                    g = (g * a + 255 * (255 - a)) / 255;
                    b = (b * a + 255 * (255 - a)) / 255;
                    a = 255;
                }
            } else {
                p = d->flat[y * d->w + x];
                a = p >> 24;
                r = (p >> 16) & 0xFF; g = (p >> 8) & 0xFF; b = p & 0xFF;
                if (on_white) {
                    /* premultiplied ueber Weiss */
                    r += 255 - a; g += 255 - a; b += 255 - a;
                    a = 255;
                } else if (a && a != 255) {
                    r = r * 255 / a; g = g * 255 / a; b = b * 255 / a;
                    if (r > 255) r = 255;
                    if (g > 255) g = 255;
                    if (b > 255) b = 255;
                }
            }
            if (a != 255)
                alpha = TRUE;
            o[0] = r; o[1] = g; o[2] = b; o[3] = a;
            o += 4;
        }
    if (has_alpha)
        *has_alpha = alpha;
    return buf;
}

/* RGBA -> RGB im selben Puffer */
static void rgba_to_rgb(UBYTE *buf, LONG n)
{
    UBYTE *s = buf, *d = buf;
    LONG i;
    for (i = 0; i < n; i++) {
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        d += 3;
        s += 4;
    }
}

/* ---- oeffentlich ---- */

/* ---- IFF ILBM ---- */

/* ByteRun1: Laeufe ab 3 gleichen Bytes, sonst woertlich (je hoechstens 128) */
static LONG pack_row(const UBYTE *s, LONG n, UBYTE *d)
{
    LONG i = 0, o = 0;
    while (i < n) {
        LONG run = 1;
        while (i + run < n && run < 128 && s[i + run] == s[i])
            run++;
        if (run >= 3) {
            d[o++] = (UBYTE)(257 - run);
            d[o++] = s[i];
            i += run;
        } else {
            LONG st = i, lit = 0;
            while (i < n && lit < 128) {
                if (i + 2 < n && s[i] == s[i + 1] && s[i] == s[i + 2])
                    break;
                i++;
                lit++;
            }
            d[o++] = (UBYTE)(lit - 1);
            memcpy(d + o, s + st, lit);
            o += lit;
        }
    }
    return o;
}

/* Vorgaben fuer die Farbreduktion aus der Farbtiefe des Dokuments */
static void iff_defaults(Doc *d, QuantOpts *o)
{
    LONG c = d->colors;
    o->palmode = c > 0 && c <= 32 ? QP_OCS : QP_ADAPTIVE;
    o->mode = c == -6 ? QM_HAM6 : c == -8 ? QM_HAM8 : c == 64 ? QM_EHB : QM_NORMAL;
    o->ncolors = c > 0 ? c : 256;
    o->dither = QD_FS;
    o->amount = 100;
    o->optimize = 1;
}

BOOL file_export_iff(Doc *d, const char *path, char *err)
{
    UBYTE *rgba, *idx = NULL, *plane = NULL, *packed = NULL;
    ULONG *px = NULL, camg = 0;
    QuantOpts o;
    QPalette pal;
    LONG w = d->w, h = d->h, i, x, y, p, planes, rb, ncmap, bodylen = 0, bodycap, masking;
    BOOL anytr = FALSE;
    BPTR fh = 0;
    BOOL ok = FALSE;
    doc_process_dirty();
    if (!(rgba = flat_rgba(d, FALSE, NULL))) {
        strcpy(err, "Not enough memory");
        return FALSE;
    }
    px = AllocVec(w * h * 4, MEMF_ANY);
    idx = AllocVec(w * h, MEMF_ANY);
    if (!px || !idx) {
        strcpy(err, "Not enough memory");
        goto out;
    }
    for (i = 0; i < w * h; i++)
        px[i] = ((ULONG)rgba[i * 4 + 3] << 24) | ((ULONG)rgba[i * 4] << 16) |
                ((ULONG)rgba[i * 4 + 1] << 8) | rgba[i * 4 + 2];
    free(rgba);
    rgba = NULL;
    /* Palette: die der letzten Farbreduktion, sonst neu (Vorgaben aus New Document) */
    if (d->qpal) {
        pal = *(QPalette *)d->qpal;
        o.palmode = d->qopt[0]; o.mode = d->qopt[1]; o.ncolors = d->qopt[2];
        o.dither = d->qopt[3]; o.amount = d->qopt[4]; o.optimize = d->qopt[5];
    } else {
        iff_defaults(d, &o);
        if (!quant_palette_img(px, w, h, &o, &pal)) {
            strcpy(err, "Not enough memory");
            goto out;
        }
    }
    if (!quant_map(px, w, h, w, &o, &pal, idx)) {
        strcpy(err, "Not enough memory");
        goto out;
    }
    planes = quant_planes(&pal);
    rb = ((w + 15) >> 4) << 1;
    ncmap = pal.mode == QM_EHB ? 32 : pal.n;
    /* Transparenz: freie Farbe 0 (DPaint-Brush), sonst Maskenebene */
    for (i = 0; i < w * h && !anytr; i++)
        anytr = (px[i] >> 24) == 0;
    masking = !anytr ? 0 : pal.transp ? 2 : 1;
    bodycap = h * (planes + 1) * (rb + rb / 128 + 2);
    plane = AllocVec(rb, MEMF_ANY);
    packed = AllocVec(bodycap, MEMF_ANY);
    if (!plane || !packed) {
        strcpy(err, "Not enough memory");
        goto out;
    }
    /* Bitplanes zeilenweise verschachtelt, je Zeile und Ebene gepackt */
    for (y = 0; y < h; y++)
        for (p = 0; p < planes; p++) {
            const UBYTE *ir = idx + y * w;
            memset(plane, 0, rb);
            for (x = 0; x < w; x++)
                if ((ir[x] >> p) & 1)
                    plane[x >> 3] |= 0x80 >> (x & 7);
            bodylen += pack_row(plane, rb, packed + bodylen);
            if (p == planes - 1 && masking == 1) {
                const ULONG *pr = px + y * w;
                memset(plane, 0, rb);
                for (x = 0; x < w; x++)
                    if (pr[x] >> 24)
                        plane[x >> 3] |= 0x80 >> (x & 7);
                bodylen += pack_row(plane, rb, packed + bodylen);
            }
        }
    if (pal.mode == QM_HAM6 || pal.mode == QM_HAM8)
        camg |= 0x800;
    if (pal.mode == QM_EHB)
        camg |= 0x80;
    if (w > 800)
        camg |= 0x8020;         /* Super-Hires */
    else if (w > 400)
        camg |= 0x8000;         /* Hires */
    if (h > 300)
        camg |= 0x4;            /* Interlace */
    if (!(fh = Open(path, MODE_NEWFILE))) {
        strcpy(err, "Cannot create the file");
        goto out;
    }
    {
        LONG cmaplen = ncmap * 3, formlen = 4 + (8 + 20) + (8 + cmaplen + (cmaplen & 1)) + (8 + 4) +
                       (8 + bodylen + (bodylen & 1));
        UBYTE bmhd[20], pad = 0;
        UBYTE xa = w > 800 ? 5 : w > 400 ? 10 : 20, ya = h > 300 ? 11 : 22;
        bmhd[0] = w >> 8; bmhd[1] = w; bmhd[2] = h >> 8; bmhd[3] = h;
        bmhd[4] = bmhd[5] = bmhd[6] = bmhd[7] = 0;         /* x, y */
        /* Seite: mindestens der passende Bildschirm; kleiner als die Seite = Brush */
        LONG pw = w, ph = h;
        if (anytr) {
            LONG sw = w > 640 ? 1280 : w > 320 ? 640 : 320, sh = h > 256 ? 512 : 256;
            if (pw < sw) pw = sw;
            if (ph < sh) ph = sh;
        }
        bmhd[8] = planes; bmhd[9] = masking;               /* 0 keine, 1 Maskenebene, 2 transparente Farbe */
        bmhd[10] = 1; bmhd[11] = 0;                        /* ByteRun1 */
        bmhd[12] = bmhd[13] = 0;                           /* transparente Farbe 0 */
        bmhd[14] = xa; bmhd[15] = ya;
        bmhd[16] = pw >> 8; bmhd[17] = pw; bmhd[18] = ph >> 8; bmhd[19] = ph;
        ok = wr32(fh, 0x464F524D) && wr32(fh, formlen) && wr32(fh, 0x494C424D) &&
             wr32(fh, 0x424D4844) && wr32(fh, 20) && wr(fh, bmhd, 20) &&
             wr32(fh, 0x434D4150) && wr32(fh, cmaplen);
        for (i = 0; ok && i < ncmap; i++) {
            UBYTE c[3];
            c[0] = pal.col[i] >> 16; c[1] = pal.col[i] >> 8; c[2] = pal.col[i];
            ok = wr(fh, c, 3);
        }
        if (ok && (cmaplen & 1))
            ok = wr(fh, &pad, 1);
        ok = ok && wr32(fh, 0x43414D47) && wr32(fh, 4) && wr32(fh, camg) &&
             wr32(fh, 0x424F4459) && wr32(fh, bodylen) && wr(fh, packed, bodylen);
        if (ok && (bodylen & 1))
            ok = wr(fh, &pad, 1);
    }
    if (!ok)
        strcpy(err, "Write error (disk full?)");
out:
    if (fh) {
        Close(fh);
        if (!ok)
            DeleteFile(path);
    }
    if (rgba) free(rgba);
    if (px) FreeVec(px);
    if (idx) FreeVec(idx);
    if (plane) FreeVec(plane);
    if (packed) FreeVec(packed);
    return ok;
}

/* ByteRun1 entpacken; Rueckgabe: neue Leseposition oder NULL bei Fehler */
static const UBYTE *unpack_row(const UBYTE *s, const UBYTE *end, UBYTE *d, LONG n)
{
    while (n > 0) {
        LONG c;
        if (s >= end)
            return NULL;
        c = (BYTE)*s++;
        if (c >= 0) {
            c++;
            if (c > n || s + c > end)
                return NULL;
            memcpy(d, s, c);
            s += c;
        } else if (c != -128) {
            c = 1 - c;
            if (c > n || s >= end)
                return NULL;
            memset(d, *s++, c);
        } else
            continue;
        d += c;
        n -= c;
    }
    return s;
}

static ULONG be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

/* IFF ILBM laden (1-8 Ebenen, EHB, HAM6/8, 24 Bit; Maskenebene als Alpha).
 * Palette und Modus kommen an das Dokument (fuer den IFF-Export).
 * NULL ohne Fehlertext: keine ILBM-Datei, andere Lader versuchen. */
static Doc *load_iff(const char *path, char *err)
{
    BPTR fh = Open(path, MODE_OLDFILE);
    UBYTE hd[12], *buf = NULL, *bmhd = NULL, *cmap = NULL, *body = NULL, *row = NULL;
    ULONG *px = NULL, col[256], camg = 0;
    LONG size, cmaplen = 0, bodylen = 0, w, h, planes, mask, comp, rb, nplanes, ncol, i, x, y, p;
    BOOL ham, ehb, ocs = TRUE, pbm, tmask;
    LONG tcol, pagew, pageh;
    Doc *d = NULL;
    if (!fh)
        return NULL;
    if (Read(fh, hd, 12) != 12 || be32(hd) != 0x464F524D ||
        (be32(hd + 8) != 0x494C424D && be32(hd + 8) != 0x50424D20)) {
        Close(fh);
        return NULL;
    }
    pbm = be32(hd + 8) == 0x50424D20;     /* DPaint "PBM ": Chunky, 8 Bit */
    size = (LONG)be32(hd + 4) - 4;
    if (size <= 0 || size > 64 * 1024 * 1024 || !(buf = AllocVec(size, MEMF_ANY))) {
        Close(fh);
        strcpy(err, size > 0 ? "Not enough memory" : "Damaged IFF file");
        return NULL;
    }
    size = Read(fh, buf, size);
    Close(fh);
    for (i = 0; i + 8 <= size; ) {
        ULONG id = be32(buf + i), len = be32(buf + i + 4);
        UBYTE *c = buf + i + 8;
        if (len > (ULONG)(size - i - 8))
            len = size - i - 8;     /* abgeschnittene Datei: was da ist */
        if (id == 0x424D4844 && len >= 20) bmhd = c;
        else if (id == 0x434D4150) { cmap = c; cmaplen = len; }
        else if (id == 0x43414D47 && len >= 4) camg = be32(c);
        else if (id == 0x424F4459) { body = c; bodylen = len; }
        i += 8 + len + (len & 1);
    }
    if (!bmhd || !body) {
        strcpy(err, "Damaged IFF file (no BMHD or BODY)");
        goto out;
    }
    w = (bmhd[0] << 8) | bmhd[1];
    h = (bmhd[2] << 8) | bmhd[3];
    planes = bmhd[8];
    mask = bmhd[9];
    comp = bmhd[10];
    tcol = (bmhd[12] << 8) | bmhd[13];
    pagew = (bmhd[16] << 8) | bmhd[17];
    pageh = (bmhd[18] << 8) | bmhd[19];
    if (w < 1 || h < 1 || w > 8192 || h > 8192 || comp > 1 || !((planes >= 1 && planes <= 8) || planes == 24) ||
        (pbm && planes != 8)) {
        strcpy(err, "Unsupported IFF picture");
        goto out;
    }
    if (camg & 0xFFF00000)
        camg = 0;               /* RTG-Modus-ID: HAM/EHB-Bits sind dort keine Flags */
    ham = !pbm && (camg & 0x800) && (planes == 6 || planes == 8);
    ehb = !ham && (camg & 0x80) && planes == 6;
    /* transparente Farbe nur bei Brushes (kleiner als die Seite): ganze Bilder
     * tragen sie oft nur nebenbei (Farbe 0 = Hintergrund) */
    tmask = mask == 2 && !ham && planes <= 8 && tcol < (1 << planes) && (w < pagew || h < pageh);
    /* Palette; alte 4-Bit-Werte (0xX0) auf 8 Bit strecken */
    ncol = cmaplen / 3 > 256 ? 256 : cmaplen / 3;
    {
        BOOL low0 = ncol > 0;
        for (i = 0; i < ncol * 3; i++)
            if (cmap[i] & 0x0F)
                low0 = FALSE;
        for (i = 0; i < 256; i++) {
            LONG r = 0, g = 0, b = 0;
            if (i < ncol) {
                r = cmap[i * 3]; g = cmap[i * 3 + 1]; b = cmap[i * 3 + 2];
                if (low0) {
                    r |= r >> 4; g |= g >> 4; b |= b >> 4;
                }
                if (r % 17 || g % 17 || b % 17)
                    ocs = FALSE;
            } else if (i < 64 && ehb && i >= 32) {
                r = (col[i - 32] >> 17) & 0x7F; g = (col[i - 32] >> 9) & 0x7F; b = (col[i - 32] >> 1) & 0x7F;
            } else if (planes <= 8 && !ncol) {
                r = g = b = i * 255 / ((1 << planes) - 1);  /* ohne Palette: Graustufen */
            }
            col[i] = ((ULONG)r << 16) | ((ULONG)g << 8) | (ULONG)b;
        }
    }
    rb = ((w + 15) >> 4) << 1;
    nplanes = planes + (mask == 1 ? 1 : 0);
    if (pbm) {
        rb = (w + 1) & ~1;      /* eine Chunky-Zeile, auf gerade Laenge aufgefuellt */
        nplanes = 1;
    }
    row = AllocVec(rb * nplanes, MEMF_ANY);
    px = AllocVec(w * h * 4, MEMF_ANY);
    if (!row || !px) {
        strcpy(err, "Not enough memory");
        goto out;
    }
    {
        const UBYTE *s = body, *end = body + bodylen;
        for (y = 0; y < h; y++) {
            ULONG *o = px + y * w;
            LONG pr, pg, pb;
            for (p = 0; p < nplanes; p++) {
                UBYTE *r = row + p * rb;
                if (comp) {
                    s = s ? unpack_row(s, end, r, rb) : NULL;
                    if (!s)
                        memset(r, 0, rb);
                } else if (s && s + rb <= end) {
                    memcpy(r, s, rb);
                    s += rb;
                } else {
                    s = NULL;
                    memset(r, 0, rb);
                }
            }
            if (pbm) {
                for (x = 0; x < w; x++)
                    o[x] = tmask && row[x] == tcol ? 0 : 0xFF000000 | col[row[x]];
                continue;
            }
            pr = (col[0] >> 16) & 0xFF; pg = (col[0] >> 8) & 0xFF; pb = col[0] & 0xFF;
            for (x = 0; x < w; x++) {
                LONG byte = x >> 3, bit = 0x80 >> (x & 7);
                ULONG v = 0, a = 0xFF000000;
                for (p = planes - 1; p >= 0; p--)
                    v = (v << 1) | ((row[p * rb + byte] & bit) ? 1 : 0);
                if (mask == 1 && !(row[planes * rb + byte] & bit))
                    a = 0;
                if (planes == 24) {
                    /* Ebenen 0-7 Rot, 8-15 Gruen, 16-23 Blau (je niederwertigstes Bit zuerst) */
                    o[x] = a | ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF);
                    continue;
                }
                if (ham) {
                    LONG sh = planes - 2, ctl = v >> sh, val = v & ((1 << sh) - 1);
                    if (ctl == 0) {     /* Palettenfarbe */
                        pr = (col[val] >> 16) & 0xFF;
                        pg = (col[val] >> 8) & 0xFF;
                        pb = col[val] & 0xFF;
                    } else if (planes == 6)
                        val *= 17;
                    switch (ctl) {
                    case 0:
                        break;
                    case 1:
                        pb = planes == 6 ? val : (val << 2) | (pb & 3);
                        break;
                    case 2:
                        pr = planes == 6 ? val : (val << 2) | (pr & 3);
                        break;
                    default:
                        pg = planes == 6 ? val : (val << 2) | (pg & 3);
                        break;
                    }
                    o[x] = a | ((ULONG)pr << 16) | ((ULONG)pg << 8) | (ULONG)pb;
                } else
                    o[x] = tmask && v == (ULONG)tcol ? 0 : a | col[v];
            }
        }
    }
    if (!(d = doc_from_argb(w, h, px))) {
        strcpy(err, "Not enough memory");
        goto out;
    }
    if (planes <= 8) {
        QPalette *q = AllocVec(sizeof(QPalette), MEMF_ANY | MEMF_CLEAR);
        LONG n = ham ? (planes == 6 ? 16 : 64) : ehb ? 64 : 1 << planes;
        d->colors = ham ? (planes == 6 ? -6 : -8) : n;
        if (q) {
            for (i = 0; i < n; i++)
                q->col[i] = col[i];
            q->n = n;
            q->mode = ham ? (planes == 6 ? QM_HAM6 : QM_HAM8) : ehb ? QM_EHB : QM_NORMAL;
            q->ocs = ocs;
            /* Brush: transparente Farbe kommt auf Index 0 (beim EHB geht das nicht) */
            if (tmask && (tcol == 0 || !ehb)) {
                ULONG t = q->col[0];
                q->col[0] = q->col[tcol];
                q->col[tcol] = t;
                q->transp = TRUE;
            }
            d->qpal = q;
            d->qopt[0] = ocs ? QP_OCS : QP_ADAPTIVE;
            d->qopt[1] = q->mode;
            d->qopt[2] = n;
            d->qopt[3] = QD_NONE;   /* Pixel liegen schon auf der Palette */
            d->qopt[4] = 100;
            d->qopt[5] = 0;
        }
    }
out:
    if (buf) FreeVec(buf);
    if (row) FreeVec(row);
    if (px) FreeVec(px);
    return d;
}

/* Palette aus einer IFF-Datei (CMAP; ILBM, PBM oder reine Palettendatei) */
BOOL file_load_palette(const char *path, void *qpal, char *err)
{
    QPalette *q = qpal;
    BPTR fh = Open(path, MODE_OLDFILE);
    UBYTE hd[12], c[8];
    ULONG camg = 0;
    LONG i, n = 0, planes = 0;
    BOOL low0 = TRUE, ocs = TRUE, ok = FALSE;
    if (!fh) {
        strcpy(err, "Cannot open the file");
        return FALSE;
    }
    memset(q, 0, sizeof(*q));
    if (Read(fh, hd, 12) != 12 || be32(hd) != 0x464F524D ||
        (be32(hd + 8) != 0x494C424D && be32(hd + 8) != 0x50424D20)) {
        strcpy(err, "Not an IFF picture or palette");
        goto out;
    }
    while (Read(fh, c, 8) == 8) {
        ULONG id = be32(c), len = be32(c + 4);
        if (id == 0x434D4150 && !n) {
            UBYTE rgb[3];
            for (i = 0; i < (LONG)(len / 3) && i < 256; i++) {
                if (Read(fh, rgb, 3) != 3)
                    goto out;
                q->col[i] = ((ULONG)rgb[0] << 16) | ((ULONG)rgb[1] << 8) | rgb[2];
                if ((rgb[0] | rgb[1] | rgb[2]) & 0x0F)
                    low0 = FALSE;
            }
            n = i;
            len -= i * 3;
        } else if (id == 0x43414D47 && len >= 4) {
            if (Read(fh, c, 4) != 4)
                goto out;
            camg = be32(c);
            len -= 4;
        } else if (id == 0x424D4844 && len >= 20) {
            UBYTE b[20];
            if (Read(fh, b, 20) != 20)
                goto out;
            planes = b[8];
            len -= 20;
        } else if (id == 0x424F4459)
            break;
        if (Seek(fh, len + (len & 1), OFFSET_CURRENT) < 0)
            break;
    }
    if (!n) {
        strcpy(err, "The file has no palette (CMAP)");
        goto out;
    }
    for (i = 0; i < n; i++) {
        ULONG v = q->col[i];
        if (low0 && n <= 64)
            v |= (v >> 4) & 0x0F0F0F;       /* alte 4-Bit-Werte */
        if (((v >> 16) & 0xFF) % 17 || ((v >> 8) & 0xFF) % 17 || (v & 0xFF) % 17)
            ocs = FALSE;
        q->col[i] = v;
    }
    if (camg & 0xFFF00000)
        camg = 0;
    /* HAM: nur die Basispalette; EHB: die 32 Grundfarben */
    if ((camg & 0x800) && (planes == 6 || planes == 8) && n > (planes == 6 ? 16 : 64))
        n = planes == 6 ? 16 : 64;
    else if ((camg & 0x80) && planes == 6 && n > 32)
        n = 32;
    q->n = n;
    q->mode = QM_NORMAL;
    q->ocs = ocs;
    ok = TRUE;
out:
    Close(fh);
    return ok;
}

/* Palette als IFF ILBM ohne Bilddaten (wie DPaint-Palettendateien) */
BOOL file_save_palette(const char *path, const void *qpal, char *err)
{
    const QPalette *q = qpal;
    BPTR fh = Open(path, MODE_NEWFILE);
    UBYTE bmhd[20], pad = 0;
    LONG i, n = q->mode == QM_EHB ? 32 : q->n, planes = 1, cl = n * 3;
    BOOL ok;
    if (!fh) {
        strcpy(err, "Cannot create the file");
        return FALSE;
    }
    while ((1 << planes) < n)
        planes++;
    if (q->mode == QM_EHB || q->mode == QM_HAM6)
        planes = 6;
    else if (q->mode == QM_HAM8)
        planes = 8;
    memset(bmhd, 0, sizeof(bmhd));
    bmhd[8] = planes;
    bmhd[14] = 10; bmhd[15] = 11;
    bmhd[16] = 320 >> 8; bmhd[17] = 320 & 0xFF; bmhd[19] = 256 & 0xFF; bmhd[18] = 256 >> 8;
    ok = wr32(fh, 0x464F524D) && wr32(fh, 4 + 28 + 8 + cl + (cl & 1) + 12) && wr32(fh, 0x494C424D) &&
         wr32(fh, 0x424D4844) && wr32(fh, 20) && wr(fh, bmhd, 20) && wr32(fh, 0x434D4150) && wr32(fh, cl);
    for (i = 0; ok && i < n; i++) {
        UBYTE c[3];
        c[0] = q->col[i] >> 16; c[1] = q->col[i] >> 8; c[2] = q->col[i];
        ok = wr(fh, c, 3);
    }
    if (ok && (cl & 1))
        ok = wr(fh, &pad, 1);
    ok = ok && wr32(fh, 0x43414D47) && wr32(fh, 4) &&
         wr32(fh, q->mode == QM_EHB ? 0x80 : q->mode == QM_HAM6 || q->mode == QM_HAM8 ? 0x800 : 0);
    Close(fh);
    if (!ok) {
        strcpy(err, "Write error (disk full?)");
        DeleteFile(path);
    }
    return ok;
}

/* ---- TIFF (eigener Lader; akTIFF.datatype setzt Streifen falsch zusammen) ----
 * Erstes Bild, Streifen, chunky; ungepackt, LZW, PackBits, Deflate; Praediktor 2;
 * 1/2/4/8/16 Bit Grau/Palette, 8/16 Bit RGB(A), CMYK. Alles andere: NULL ohne
 * Fehlertext, dann versuchen es die Datatypes. */

typedef struct Tif {
    const UBYTE *d;
    LONG len;
    BOOL le;                    /* "II": little-endian */
} Tif;

static ULONG tf16(const Tif *t, LONG o)
{
    if (o < 0 || o + 2 > t->len)
        return 0;
    return t->le ? t->d[o] | t->d[o + 1] << 8 : t->d[o] << 8 | t->d[o + 1];
}

static ULONG tf32(const Tif *t, LONG o)
{
    if (o < 0 || o + 4 > t->len)
        return 0;
    return t->le ? (ULONG)t->d[o] | (ULONG)t->d[o + 1] << 8 | (ULONG)t->d[o + 2] << 16 | (ULONG)t->d[o + 3] << 24
                 : (ULONG)t->d[o] << 24 | (ULONG)t->d[o + 1] << 16 | (ULONG)t->d[o + 2] << 8 | t->d[o + 3];
}

/* Wert i eines Tags (SHORT oder LONG, inline oder ab Offset) */
static ULONG tif_val(const Tif *t, LONG ent, LONG i)
{
    ULONG type = tf16(t, ent + 2), cnt = tf32(t, ent + 4);
    LONG sz = type == 3 ? 2 : 4, o = cnt * sz <= 4 ? ent + 8 : (LONG)tf32(t, ent + 8);
    if ((ULONG)i >= cnt)
        return 0;
    if (type == 3)
        return tf16(t, o + i * 2);
    if (type == 1)
        return o + i < t->len ? t->d[o + i] : 0;
    return tf32(t, o + i * 4);
}

/* LZW (TIFF 6: MSB zuerst, fruehes Umschalten); liefert erzeugte Bytes */
static LONG tif_lzw(const UBYTE *src, LONG n, UBYTE *dst, LONG max)
{
    static UWORD prefix[4096];
    static UBYTE suffix[4096], first[4096], stack[4096];
    ULONG bits = 0;
    LONG nbits = 0, width = 9, next = 258, old = -1, out = 0, i = 0;
    for (;;) {
        LONG code, c, sp = 0;
        while (nbits < width) {
            if (i >= n)
                return out;
            bits = bits << 8 | src[i++];
            nbits += 8;
        }
        code = (bits >> (nbits - width)) & ((1 << width) - 1);
        nbits -= width;
        if (code == 257)
            return out;
        if (code == 256) {
            width = 9;
            next = 258;
            old = -1;
            continue;
        }
        if (old < 0) {
            if (code > 255)
                return out;
            if (out < max)
                dst[out++] = code;
            old = code;
            continue;
        }
        if (code < next) {
            c = code;
        } else if (code == next) {
            stack[sp++] = old < 256 ? old : first[old];
            c = old;
        } else
            return out;                     /* kaputt */
        while (c > 255 && sp < 4095) {
            stack[sp++] = suffix[c];
            c = prefix[c];
        }
        stack[sp++] = c;
        if (next < 4096) {
            prefix[next] = old;
            suffix[next] = c;               /* erstes Zeichen des neuen Strings */
            first[next] = old < 256 ? old : first[old];
            next++;
        }
        while (sp > 0) {
            if (out < max)
                dst[out++] = stack[--sp];
            else
                sp--;
        }
        old = code;
        if (next + 1 >= (1 << width) && width < 12)
            width++;
    }
}

static LONG tif_packbits(const UBYTE *src, LONG n, UBYTE *dst, LONG max)
{
    LONG i = 0, out = 0;
    while (i < n && out < max) {
        LONG c = (BYTE)src[i++], k;
        if (c >= 0) {
            for (k = 0; k <= c && i < n && out < max; k++)
                dst[out++] = src[i++];
        } else if (c != -128 && i < n) {
            for (k = 0; k < 1 - c && out < max; k++)
                dst[out++] = src[i];
            i++;
        }
    }
    return out;
}

static Doc *load_tiff(const char *path, char *err)
{
    BPTR fh = Open(path, MODE_OLDFILE);
    UBYTE hd[4], *buf = NULL, *raw = NULL;
    ULONG *px = NULL, pal[256];
    Tif t;
    LONG size, ifd, n, i, e, w = 0, h = 0, bps = 1, spp = 1, comp = 1, photo = -1, rps = 0, planar = 1;
    LONG pred = 1, extra = 0, stripe = 0, stripc = 0, nstrips = 0, cmap = 0, rowb, y, x, s, tiled = 0;
    Doc *d = NULL;

    if (!fh)
        return NULL;
    if (Read(fh, hd, 4) != 4 || !((hd[0] == 'I' && hd[1] == 'I' && hd[2] == 42 && hd[3] == 0) ||
                                  (hd[0] == 'M' && hd[1] == 'M' && hd[2] == 0 && hd[3] == 42))) {
        Close(fh);
        return NULL;
    }
    Seek(fh, 0, OFFSET_END);
    size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size <= 8 || !(buf = malloc(size))) {
        Close(fh);
        if (size > 8)
            strcpy(err, "Not enough memory");
        return NULL;
    }
    size = Read(fh, buf, size);
    Close(fh);
    t.d = buf;
    t.len = size;
    t.le = buf[0] == 'I';
    ifd = tf32(&t, 4);
    n = tf16(&t, ifd);
    for (i = 0; i < n; i++) {
        e = ifd + 2 + i * 12;
        switch (tf16(&t, e)) {
        case 256: w = tif_val(&t, e, 0); break;
        case 257: h = tif_val(&t, e, 0); break;
        case 258: bps = tif_val(&t, e, 0); break;
        case 259: comp = tif_val(&t, e, 0); break;
        case 262: photo = tif_val(&t, e, 0); break;
        case 273: stripe = e; nstrips = tf32(&t, e + 4); break;
        case 277: spp = tif_val(&t, e, 0); break;
        case 278: rps = tif_val(&t, e, 0); break;
        case 279: stripc = e; break;
        case 284: planar = tif_val(&t, e, 0); break;
        case 317: pred = tif_val(&t, e, 0); break;
        case 320: cmap = e; break;
        case 322: tiled = 1; break;
        case 338: extra = tif_val(&t, e, 0); break;
        }
    }
    if (rps <= 0 || rps > h)
        rps = h;
    /* nicht unterstuetzt: den Datatypes ueberlassen */
    if (w <= 0 || h <= 0 || w > 16000 || h > 16000 || !stripe || !stripc || tiled || planar != 1 ||
        (comp != 1 && comp != 5 && comp != 8 && comp != 32946 && comp != 32773) ||
        (bps != 1 && bps != 2 && bps != 4 && bps != 8 && bps != 16) || spp < 1 || spp > 5 ||
        (bps < 8 && spp != 1) || (photo == 3 && (!cmap || bps > 8)) || photo < 0 || photo > 5 || photo == 4 ||
        (photo == 2 && spp < 3) || (photo == 5 && spp < 4) || nstrips < (h + rps - 1) / rps)
        goto out;
    rowb = (w * spp * bps + 7) / 8;
    if (!(raw = malloc(rowb * rps + 16)) || !(px = AllocVec(w * h * 4, MEMF_ANY))) {
        strcpy(err, "Not enough memory");
        goto out;
    }
    if (photo == 3) {
        LONG nc = 1 << bps;
        for (i = 0; i < nc; i++)
            pal[i] = 0xFF000000 | (tif_val(&t, cmap, i) >> 8) << 16 |
                     (tif_val(&t, cmap, nc + i) >> 8) << 8 | tif_val(&t, cmap, 2 * nc + i) >> 8;
    }
    for (s = 0; s * rps < h; s++) {
        LONG off = tif_val(&t, stripe, s), cnt = tif_val(&t, stripc, s), rows = h - s * rps, got;
        LONG need;
        if (rows > rps)
            rows = rps;
        need = rowb * rows;
        if (off < 0 || cnt < 0 || off > size || cnt > size - off)
            goto out;
        if (comp == 1) {
            got = cnt < need ? cnt : need;
            memcpy(raw, buf + off, got);
        } else if (comp == 5)
            got = tif_lzw(buf + off, cnt, raw, need);
        else if (comp == 32773)
            got = tif_packbits(buf + off, cnt, raw, need);
        else {
            uLongf dl = need;
            got = uncompress(raw, &dl, buf + off, cnt) == Z_OK ? (LONG)dl : 0;
        }
        if (got < need)
            memset(raw + got, 0, need - got);   /* abgeschnitten: Rest schwarz */
        for (y = 0; y < rows; y++) {
            UBYTE *r = raw + y * rowb;
            ULONG *o = px + (s * rps + y) * w;
            if (pred == 2 && bps == 8)          /* horizontale Differenzen */
                for (x = spp; x < w * spp; x++)
                    r[x] += r[x - spp];
            else if (pred == 2 && bps == 16)
                for (x = spp; x < w * spp; x++) {
                    UBYTE *a = r + x * 2, *b = a - spp * 2;
                    UWORD v = t.le ? (a[0] | a[1] << 8) + (b[0] | b[1] << 8) : (a[0] << 8 | a[1]) + (b[0] << 8 | b[1]);
                    if (t.le) { a[0] = v; a[1] = v >> 8; } else { a[0] = v >> 8; a[1] = v; }
                }
            for (x = 0; x < w; x++) {
                ULONG c[5], a = 255;
                LONG k;
                for (k = 0; k < spp; k++) {
                    if (bps == 16) {
                        const UBYTE *q = r + (x * spp + k) * 2;
                        c[k] = t.le ? q[1] : q[0];          /* oberes Byte */
                    } else if (bps == 8)
                        c[k] = r[x * spp + k];
                    else {
                        LONG bit = x * bps;
                        c[k] = (r[bit >> 3] >> (8 - bps - (bit & 7))) & ((1 << bps) - 1);
                    }
                }
                if (photo == 3) {
                    o[x] = pal[c[0]];
                    continue;
                }
                if (bps < 8)
                    c[0] = c[0] * 255 / ((1 << bps) - 1);
                if (photo <= 1) {
                    ULONG g = photo == 0 ? 255 - c[0] : c[0];
                    if (spp >= 2)
                        a = c[1];
                    c[0] = c[1] = c[2] = g;
                } else if (photo == 5) {            /* CMYK, naiv */
                    ULONG kk = 255 - c[3];
                    c[0] = (255 - c[0]) * kk / 255;
                    c[1] = (255 - c[1]) * kk / 255;
                    c[2] = (255 - c[2]) * kk / 255;
                    if (spp >= 5)
                        a = c[4];
                } else if (spp >= 4)
                    a = c[3];
                if (a < 255 && extra == 1 && a) {   /* vormultipliziert */
                    for (k = 0; k < 3; k++)
                        c[k] = c[k] * 255 / a > 255 ? 255 : c[k] * 255 / a;
                }
                o[x] = a << 24 | c[0] << 16 | c[1] << 8 | c[2];
            }
        }
    }
    if (!(d = doc_from_argb(w, h, px)))
        strcpy(err, "Not enough memory");
out:
    free(buf);
    free(raw);
    if (px)
        FreeVec(px);
    return d;
}

Doc *file_load(const char *path, char *err)
{
    Doc *d;
    err[0] = 0;
    if (ext_is(path, ".aps"))
        d = load_aps(path, err);
    else if (ext_is(path, ".psd") && ((d = load_psd(path, err)) || err[0])) {
        /* zu gross fuer alle Ebenen: wenigstens das zusammengesetzte Bild */
        if (!d && strstr(err, "memory") && (d = load_stb(path))) {
            err[0] = 0;
            status_message("Not enough memory for all layers - opened the flattened image");
        }
    }
    else if (!(d = load_iff(path, err)) && !err[0] && !(d = load_tiff(path, err)) && !err[0] &&
             !(d = load_stb(path)))
        d = load_datatypes(path, err);
    if (d) {
        strncpy(d->path, path, sizeof(d->path) - 1);
        d->modified = FALSE;
    }
    return d;
}

BOOL file_save(Doc *d, const char *path, char *err)
{
    UBYTE *buf;
    BOOL ok, alpha;
    err[0] = 0;

    if (ext_is(path, ".aps"))
        return save_aps(d, path, err);
    if (ext_is(path, ".iff") || ext_is(path, ".ilbm"))
        return file_export_iff(d, path, err);

    doc_process_dirty();
    if (ext_is(path, ".png")) {
        if (!(buf = flat_rgba(d, FALSE, &alpha))) {
            strcpy(err, "Not enough memory");
            return FALSE;
        }
        if (!alpha)
            rgba_to_rgb(buf, d->w * d->h);
        ok = stbi_write_png(path, d->w, d->h, alpha ? 4 : 3, buf, d->w * (alpha ? 4 : 3));
    } else if (ext_is(path, ".jpg") || ext_is(path, ".jpeg")) {
        if (!(buf = flat_rgba(d, TRUE, NULL))) {
            strcpy(err, "Not enough memory");
            return FALSE;
        }
        rgba_to_rgb(buf, d->w * d->h);
        ok = stbi_write_jpg(path, d->w, d->h, 3, buf, 90);
    } else {
        strcpy(err, "Unknown file extension: use .aps, .png, .jpg or .iff");
        return FALSE;
    }
    free(buf);
    if (!ok)
        strcpy(err, "Write error");
    return ok;
}
