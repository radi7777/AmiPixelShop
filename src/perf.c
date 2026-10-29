/*
 * perf.c - "AmiPixelShop PERF": typische Ablaeufe auf 1920x1080 messen
 *
 * Laeuft mit echtem Bildschirm, schreibt die Zeiten nach RAM:perf.txt.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include "apx.h"

ULONG filter_perf(LONG cmd);

static BPTR pf;

static void out(const char *name, ULONG us, LONG n)
{
    char buf[100];
    if (n > 1)
        sprintf(buf, "%-34s %6ld.%ld ms  (je Schritt, %ld Schritte)\n", name,
                (long)(us / n / 1000), (long)(us / n / 100 % 10), (long)n);
    else
        sprintf(buf, "%-34s %6ld.%ld ms\n", name, (long)(us / 1000), (long)(us / 100 % 10));
    if (pf)
        Write(pf, buf, strlen(buf));
}

/* Ebene mit Inhalt fuellen: fn(x, y) liefert ARGB straight */
static void fill_layer(Layer *l, ULONG (*fn)(LONG x, LONG y))
{
    LONG tx, ty, x, y;
    for (ty = 0; ty < doc->th; ty++)
        for (tx = 0; tx < doc->tw; tx++) {
            Tile *t = l->tiles[ty * doc->tw + tx];
            if (!t && !(t = l->tiles[ty * doc->tw + tx] = tile_new()))
                continue;
            for (y = 0; y < TILE_SIZE; y++)
                for (x = 0; x < TILE_SIZE; x++) {
                    LONG gx = (tx << TILE_SHIFT) + x, gy = (ty << TILE_SHIFT) + y;
                    t->px[(y << TILE_SHIFT) | x] = gx < doc->w && gy < doc->h ? fn(gx, gy) : 0;
                }
            tile_update_flag(doc, t, tx, ty);
            if (t->flag == TF_EMPTY) {
                FreeVec(t);
                l->tiles[ty * doc->tw + tx] = NULL;
            }
        }
}

static ULONG seed = 1;
static ULONG photo(LONG x, LONG y)
{
    ULONG n;
    seed = seed * 1103515245 + 12345;
    n = (seed >> 16) & 15;
    return 0xFF000000 | (((x * 255 / 1920 + n) & 0xFF) << 16) | (((y * 255 / 1080 + n) & 0xFF) << 8) |
           (((x + y) / 12 + n) & 0xFF);
}
static ULONG blobs(LONG x, LONG y)
{
    LONG dx = (x % 400) - 200, dy = (y % 300) - 150;
    return dx * dx + dy * dy < 120 * 120 ? 0xB0E04020 : 0;
}
static ULONG band(LONG x, LONG y)
{
    return y > 400 && y < 700 ? 0x80305080 : 0;
}

void perf_run(void)
{
    Doc *d = doc_new(1920, 1080, DOC_WHITE);
    Layer *l1, *l2;
    ULONG t;
    LONG i;
    if (!d || !doc_open(d))
        return;
    pf = Open("RAM:perf.txt", MODE_NEWFILE);
    ui_draw_all();
    /* Hintergrund wie ein Foto, zwei Ebenen darueber (eine Multiplizieren) */
    fill_layer(doc->layers[0], photo);
    l1 = doc->layers[1];
    fill_layer(l1, blobs);
    doc_add_layer();
    l2 = doc->layers[doc->active];
    fill_layer(l2, band);
    l2->mode = BM_MULTIPLY;

    l1->visible = l2->visible = FALSE;
    doc_mark_all_dirty();
    t = now_us();
    doc_process_dirty();
    out("Komposit nur Hintergrund (deckend)", now_us() - t, 1);
    l1->visible = TRUE;
    doc_mark_all_dirty();
    t = now_us();
    doc_process_dirty();
    out("Komposit + Normal-Ebene (Kreise)", now_us() - t, 1);
    l1->visible = FALSE;
    l2->visible = TRUE;
    doc_mark_all_dirty();
    t = now_us();
    doc_process_dirty();
    out("Komposit + Multiply-Ebene (Band)", now_us() - t, 1);
    l1->visible = TRUE;
    doc_mark_all_dirty();
    t = now_us();
    doc_process_dirty();
    out("Komposit komplett (3 Ebenen)", now_us() - t, 1);

    view_set_zoom(1.0f, cvx + cvw / 2, cvy + cvh / 2);
    view_inval_all();
    t = now_us();
    view_flush();
    out("Anzeige 100% komplett", now_us() - t, 1);

    view_fit();
    view_inval_all();
    t = now_us();
    view_flush();
    out("Anzeige Fit komplett", now_us() - t, 1);

    t = now_us();
    for (i = 0; i < 10; i++) {
        view_pan(20, 10);
        view_flush();
    }
    out("Pan (Fit)", now_us() - t, 10);

    /* Pinselstrich interaktiv: je Schritt Tupfer + Komposit + Anzeige */
    doc->active = 1;
    brush_size = 60;
    brush_rebuild();
    t = now_us();
    stroke_begin(200.0f, 200.0f, FALSE);
    for (i = 1; i <= 40; i++) {
        stroke_to(200.0f + i * 30.0f, 200.0f + i * 12.0f);
        doc_process_dirty();
        view_flush();
    }
    out("Pinsel 60px (Tupfer+Komposit+Anzeige)", now_us() - t, 40);
    t = now_us();
    stroke_end();
    out("Pinsel Strichende (Undo)", now_us() - t, 1);

    /* Ebene ziehen: Teile einzeln */
    doc->active = 2;
    t = now_us();
    move_begin();
    out("  Ziehen Start (Kopien+Unterbau)", now_us() - t, 1);
    {
        ULONG tu = 0, tc = 0, td = 0, t0;
        for (i = 1; i <= 10; i++) {
            t0 = now_us();
            move_update(i * 7, i * 4);
            tu += now_us() - t0;
            t0 = now_us();
            doc_process_dirty();
            tc += now_us() - t0;
            t0 = now_us();
            view_flush();
            td += now_us() - t0;
        }
        out("  Ziehen: Ebene neu aufbauen", tu, 10);
        out("  Ziehen: Komposit", tc, 10);
        out("  Ziehen: Anzeige", td, 10);
        out("Ebene ziehen (Neuaufbau+Komposit+Anz.)", tu + tc + td, 10);
    }
    t = now_us();
    move_end();
    out("Ebene ziehen Ende", now_us() - t, 1);

    doc->active = 0;
    out("Filter Invert (Hintergrund)", filter_perf(CMD_ADJ_INVERT), 1);
    out("Filter Gaussian Blur 2px", filter_perf(CMD_FLT_GAUSS), 1);
    out("Filter Hue/Saturation (0)", filter_perf(CMD_ADJ_HUESAT), 1);
    t = now_us();
    doc_process_dirty();
    out("  danach Komposit", now_us() - t, 1);

    /* Korrekturebene Farbton */
    doc->active = doc->nlayers - 1;
    if (doc_new_adjust(CMD_ADJ_INVERT, NULL)) {
        Layer *a = doc->layers[doc->active];
        a->adj->cmd = CMD_ADJ_HUESAT;
        a->adj->v[0] = 40;
        doc_mark_all_dirty();
        t = now_us();
        doc_process_dirty();
        out("Komposit mit Hue/Sat-Korrekturebene", now_us() - t, 1);
        a->adj->cmd = CMD_ADJ_CURVES;
        doc_mark_all_dirty();
        t = now_us();
        doc_process_dirty();
        out("Komposit mit Curves-Korrekturebene", now_us() - t, 1);
    }

    /* Auswahl und Ameisen */
    t = now_us();
    sel_ellipse(100.0f, 100.0f, 1800.0f, 1000.0f, SEL_NEW);
    out("Auswahl Ellipse", now_us() - t, 1);
    t = now_us();
    view_ants_rebuild();
    out("Ameisen neu aufbauen", now_us() - t, 1);

    t = now_us();
    undo_step();
    doc_process_dirty();
    out("Undo + Komposit", now_us() - t, 1);

    if (pf)
        Close(pf);
    pf = 0;
}

/* "AmiPixelShop FILTERTEST ein.png ausdir": jede Korrektur und jeder Filter mit
 * Standardwerten, einmal aufs ganze Bild, einmal auf eine Auswahl an der linken
 * oberen Ecke (Rand + Kachelgrenzen). tools/filtertest.py prueft, dass ausserhalb
 * der Auswahl nichts veraendert wurde. Kein Bildschirm noetig. */
void filter_test(const char *in, const char *outdir)
{
    static const LONG cmds[] = {
        CMD_ADJ_BRICON, CMD_ADJ_LEVELS, CMD_ADJ_HUESAT, CMD_ADJ_DESAT, CMD_ADJ_INVERT,
        CMD_ADJ_THRESH, CMD_ADJ_POSTER, CMD_ADJ_CURVES,
        CMD_FLT_GAUSS, CMD_FLT_UNSHARP, CMD_FLT_NOISE, CMD_FLT_EMBOSS, CMD_FLT_MOSAIC,
        CMD_FLT_MOTION, CMD_FLT_BOX, CMD_FLT_MEDIAN, CMD_FLT_SHARPEN, CMD_FLT_SHARPEN_MORE,
        CMD_FLT_HIGHPASS, CMD_FLT_EDGES };
    LONG i, s;
    char path[256], err[80], line[120];
    for (i = 0; i < (LONG)(sizeof(cmds) / sizeof(cmds[0])); i++)
        for (s = 0; s < 2; s++) {
            ULONG t;
            if (!(doc = file_load(in, err))) {
                Printf("Load failed: %s\n", (ULONG)err);
                return;
            }
            curves_reset();
            if (s)          /* Auswahl ueber Kachelgrenzen, an der Ecke anliegend */
                sel_rect(0, 0, doc->w * 2 / 5 + 3, doc->h / 2 + 5, SEL_NEW);
            t = filter_perf(cmds[i]);
            sprintf(path, "%s/f%02ld_%s.png", outdir, (long)i, s ? "sel" : "all");
            if (!file_save(doc, path, err))
                Printf("Save failed: %s\n", (ULONG)err);
            sprintf(line, "%-24s %s %6ld ms\n", adjust_name(cmds[i]), s ? "sel" : "all", (long)(t / 1000));
            PutStr(line);
            undo_clear();
            doc_free(doc);
            doc = NULL;
        }
}

/* "AmiPixelShop LAYERTEST ausdir": Dokument mit Masken, Gruppe, Schnittmaske,
 * Mischmodi, Stil und Korrekturebene bauen; zusammengesetzt speichern, als .aps
 * speichern, neu laden und wieder zusammengesetzt speichern, dazu nach
 * Flatten. tools/layertest.py vergleicht: alle drei muessen gleich sein. */
static ULONG disc(LONG x, LONG y)
{
    LONG dx = x - 120, dy = y - 90, d = dx * dx + dy * dy;
    return d < 70 * 70 ? 0xFF000000 | ((ULONG)(x * 3 & 0xFF) << 16) | 0x40 << 8 | (ULONG)(y & 0xFF) :
           d < 80 * 80 ? (ULONG)((80 * 80 - d) * 255 / (80 * 80 - 70 * 70)) << 24 | 0x2050E0 : 0;
}

static ULONG lt_band(LONG x, LONG y)
{
    return (y > 40 && y < 150) ? 0xC0000000 | ((ULONG)(x & 0xFF) << 8) | 0xE04000 : 0;
}

static ULONG stripes(LONG x, LONG y)
{
    return ((x / 9) & 1) ? 0xFFF0F030 : 0x8000A0FF;
}

static char lt_prompt[200];

void layer_test(const char *outdir)
{
    char path[256], err[80];
    Layer *l;
    LayerFx *fx;
    if (!(doc = doc_new(321, 203, DOC_WHITE)))
        return;
    fill_layer(doc->layers[0], photo);
    /* Ebene 1: Scheibe mit Maske aus Auswahl, Multiplizieren, 80 % */
    l = doc->layers[doc->active];
    fill_layer(l, disc);
    sel_rect(30, 20, 260, 180, SEL_NEW);
    doc_mask_add(MASK_REVEAL_SEL);
    l->mode = BM_MULTIPLY;
    l->opacity = 204;
    /* Schnittmaske darueber: Streifen auf die Scheibe beschnitten, Overlay */
    doc_add_layer();
    l = doc->layers[doc->active];
    fill_layer(l, stripes);
    l->mode = BM_OVERLAY;
    doc_clip_toggle();
    /* Ebene mit Stil (Schatten, Kontur) in einer Gruppe, Fuellung 60 % */
    doc_add_layer();
    l = doc->layers[doc->active];
    fill_layer(l, lt_band);
    l->fill = 153;
    if ((fx = AllocVec(sizeof(LayerFx), MEMF_ANY | MEMF_CLEAR))) {
        fx->sh_on = 1; fx->sh_opacity = 75; fx->sh_angle = 120; fx->sh_dist = 6; fx->sh_size = 4;
        fx->sh_color = 0xFF000000;
        fx->st_on = 1; fx->st_size = 2; fx->st_opacity = 100; fx->st_color = 0xFFFF0000;
        l->fx = fx;
        l->fx_valid = FALSE;
    }
    /* generative Ebene: laengster Prompt (199 Zeichen) mit UTF-8-Umlauten */
    {
        GenInfo *g = AllocVec(sizeof(GenInfo), MEMF_ANY | MEMF_CLEAR);
        if (g) {
            LONG k;
            strcpy(g->prompt, "Gr\xC3\xBCne Wiese mit Bl\xC3\xBCmchen, ");
            for (k = strlen(g->prompt); k < 199; k++)
                g->prompt[k] = 'a' + k % 26;
            g->prompt[199] = 0;
            g->mode = GEN_REMOVE;
            l->gen = g;
            strcpy(lt_prompt, g->prompt);
        }
    }
    doc_group_layers();
    doc->layers[doc->active]->mode = BM_SCREEN;
    doc->layers[doc->active]->pass = 0;     /* isoliert, damit der Modus der Gruppe wirkt */
    /* Korrekturebene oben: Hue/Saturation */
    if (doc_new_adjust(CMD_ADJ_HUESAT, NULL)) {
        l = doc->layers[doc->active];
        l->adj->v[0] = 40; l->adj->v[1] = 25; l->adj->v[2] = -10;
    }
    /* Levels mit eigenem Rot- und Blaukanal (Chunk ALVL) */
    if (doc_new_adjust(CMD_ADJ_LEVELS, NULL)) {
        static const LONG r[5] = { 30, 140, 220, 10, 250 }, b[5] = { 0, 70, 255, 40, 255 };
        l = doc->layers[doc->active];
        memcpy(l->adj->lv[0], r, sizeof(r));
        memcpy(l->adj->lv[2], b, sizeof(b));
    }
    doc_mark_all_dirty();
    doc_process_dirty();
    sprintf(path, "%s/before.png", outdir);
    if (!file_save(doc, path, err)) Printf("save: %s\n", (ULONG)err);
    sprintf(path, "%s/t.aps", outdir);
    if (!file_save(doc, path, err)) Printf("save aps: %s\n", (ULONG)err);
    Printf("%ld layers saved\n", doc->nlayers);
    undo_clear();
    doc_free(doc);
    if (!(doc = file_load(path, err))) {
        Printf("load: %s\n", (ULONG)err);
        return;
    }
    Printf("%ld layers loaded\n", doc->nlayers);
    {
        LONG k, found = 0;
        for (k = 0; k < doc->nlayers; k++)
            if (doc->layers[k]->gen)
                found = !strcmp(doc->layers[k]->gen->prompt, lt_prompt) &&
                        doc->layers[k]->gen->mode == GEN_REMOVE ? 1 : -1;
        PutStr(found == 1 ? "GENI ok\n" : found ? "GENI FAIL: prompt/mode differ\n" : "GENI FAIL: missing\n");
    }
    doc_mark_all_dirty();
    doc_process_dirty();
    sprintf(path, "%s/after.png", outdir);
    file_save(doc, path, err);
    doc_flatten();
    doc_process_dirty();
    sprintf(path, "%s/flat.png", outdir);
    file_save(doc, path, err);
    Printf("%ld layers after flatten\n", doc->nlayers);
    undo_clear();
    doc_free(doc);
    doc = NULL;
}
