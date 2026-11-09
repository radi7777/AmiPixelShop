/*
 * ai.c - KI-Funktionen ueber OpenRouter, direkt Google Gemini oder ComfyUI
 * im lokalen Netz: Generative Fill und Remove
 *
 * Ablauf: Ausschnitt um die Auswahl (mit Umgebung) als JPEG und die Auswahl
 * als Schwarz-Weiss-Maske an ein Bildmodell schicken. Die Anfrage laeuft in
 * einem eigenen Prozess; das Ergebnis wird auf den Ausschnitt skaliert, in
 * der Farbe an die Umgebung angeglichen und als neue Ebene mit der Auswahl
 * als Ebenenmaske eingesetzt. Das Original bleibt unveraendert.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include "apx.h"
#include "net.h"
#include "stb_image.h"
#include "stb_image_write.h"

void main_activate_doc(Doc *d);
BOOL crop_active(void);

#define AI_HOST     "openrouter.ai"
#define GEM_HOST    "generativelanguage.googleapis.com"
#define COMFY_PORT  8188
#define COMFY_IN    "amipixelshop_in.png"   /* Name des hochgeladenen Bildes in ComfyUI */
#define AI_MINSIDE  768         /* lange Seite des gesendeten Ausschnitts */
#define AI_MAXSIDE  1024

typedef struct AiJob {
    NetCtl ctl;
    char *body;                 /* JSON-Anfrage (net_alloc) */
    LONG blen;
    char hdr[320];              /* Kopfzeilen mit Schluessel */
    LONG prov;                  /* PROV_* */
    char path[160];             /* Anfragepfad (Gemini: mit Modellname) */
    char host[64];              /* ComfyUI: Rechner und Port */
    LONG port;
    char *up;                   /* ComfyUI: Upload (multipart, net_alloc) */
    LONG uplen;
    char uphdr[96];
    struct Task *parent;
    LONG sig;
    volatile LONG done;
    BOOL ok;
    char err[160];
    UBYTE *img;                 /* Ergebnisbild, PNG/JPEG (net_alloc) */
    LONG imglen;
    char cost[24];
    /* Platzierung */
    Doc *doc;
    LONG x, y, w, h;            /* Ausschnitt im Dokument */
    UBYTE *sel;                 /* Auswahl im Ausschnitt, w*h */
    BOOL grow;                  /* Entfernen: Maske um vom Modell geaenderte Raender erweitern */
    LONG kind;                  /* AI_FILL, AI_SUBJECT, AI_RMBG */
    Layer *target;              /* AI_FILL: neue Variante fuer diese generative Ebene, sonst NULL */
    char prompt[200];
    LONG mode;                  /* GEN_* */
    Layer *layer;               /* AI_RMBG: Zielebene */
    LONG ix0, iy0, iw, ih, sw, sh;  /* Segmentierung: Bild im gesendeten Rahmen (mit Graubalken) */
    char name[32];              /* Ebenenname */
    ULONG t0, t_up, t_first, t_done;    /* Start, Upload fertig, erstes Byte, Antwort da */
} AiJob;

enum { AI_FILL, AI_SUBJECT, AI_RMBG };
static AiJob *job;              /* laufende Anfrage oder NULL */

/* Einstellungen (Edit > AI Settings..., gespeichert in den Prefs) */
static const char *fill_models[] = { "google/gemini-nano-banana-2.1", "google/gemini-3-pro-image",
                                     "google/gemini-3.1-flash-lite-image" };
static const char *fast_models[] = { "google/gemini-3.1-flash-lite-image", "google/gemini-nano-banana-2.1" };
LONG ai_fill_model, ai_fast_model, ai_variants = 1, ai_provider;
enum { PROV_OPENROUTER, PROV_GEMINI, PROV_COMFY };
static const char *prov_name[] = { "OpenRouter", "Google Gemini", "ComfyUI" };
/* Schluessel; bei ComfyUI die Serveradresse */
static const char *key_files[][2] = {
    { "ENV:AmiPixelShop/openrouter.key", "ENVARC:AmiPixelShop/openrouter.key" },
    { "ENV:AmiPixelShop/gemini.key", "ENVARC:AmiPixelShop/gemini.key" },
    { "ENV:AmiPixelShop/comfyui.server", "ENVARC:AmiPixelShop/comfyui.server" } };
static LONG session_mc;         /* Kosten dieser Sitzung, 1/100 Cent */
static LONG regen_left;         /* noch zu erzeugende Varianten */
static Layer *regen_layer;
static LONG ai_sigbit = -1;
static char ai_model[96];

/* ---------- Hilfen ---------- */

static BOOL read_small(const char *path, char *buf, LONG max)
{
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    LONG n;
    buf[0] = 0;
    if (!fh)
        return FALSE;
    n = Read(fh, buf, max - 1);
    Close(fh);
    if (n < 0)
        n = 0;
    buf[n] = 0;
    while (n > 0 && (UBYTE)buf[n - 1] <= ' ')
        buf[--n] = 0;
    return n > 0;
}

static BOOL read_key(char *key, LONG max)
{
    return read_small(key_files[ai_provider][0], key, max) ||
           read_small(key_files[ai_provider][1], key, max);
}

static const char *model_name(BOOL fill)
{
    if (!read_small("ENV:AmiPixelShop/ai_model", ai_model, sizeof(ai_model)) &&
        !read_small("ENVARC:AmiPixelShop/ai_model", ai_model, sizeof(ai_model)))
        strcpy(ai_model, fill ? fill_models[ai_fill_model] : fast_models[ai_fast_model]);
    /* Gemini direkt: gleiche Modelle, ohne den OpenRouter-Praefix */
    if (ai_provider == PROV_GEMINI && !strncmp(ai_model, "google/", 7))
        memmove(ai_model, ai_model + 7, strlen(ai_model + 7) + 1);
    return ai_model;
}

/* waechst beim JPEG-Schreiben mit */
typedef struct { UBYTE *p; LONG n, cap; } MemBuf;

static void mem_write(void *ctx, void *data, int size)
{
    MemBuf *m = ctx;
    if (!m->p || m->n + size > m->cap) {
        LONG nc = (m->cap ? m->cap * 2 : 65536) + size;
        UBYTE *q = malloc(nc);
        if (!q)
            return;
        if (m->p) {
            memcpy(q, m->p, m->n);
            free(m->p);
        }
        m->p = q;
        m->cap = nc;
    }
    memcpy(m->p + m->n, data, size);
    m->n += size;
}

/* Seitenverhaeltnisse, die die Bildmodelle liefern */
static const UBYTE ratios[][2] = {
    { 1, 1 }, { 5, 4 }, { 4, 5 }, { 4, 3 }, { 3, 4 }, { 3, 2 }, { 2, 3 },
    { 16, 9 }, { 9, 16 }, { 21, 9 }
};

/* Anfrage-JSON in zwei Formaten: OpenRouter (OpenAI-Chat) und Gemini generateContent.
 * body_start schreibt bis zum Beginn des Prompt-Textes, body_image bis zum Beginn
 * der Base64-JPEG-Daten; danach schliesst BODY_END beide Formate. */
#define BODY_END "\"}}]}]}"

static char *body_start(AiJob *j, char *p, BOOL fill, LONG ri)
{
    const char *m = model_name(fill);
    if (j->prov == PROV_GEMINI) {
        sprintf(j->path, "/v1beta/models/%.100s:generateContent", m);
        sprintf(p, "{\"generationConfig\":{\"responseModalities\":[\"TEXT\",\"IMAGE\"],"
                   "\"imageConfig\":{\"aspectRatio\":\"%ld:%ld\"}},"
                   "\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"",
                (long)ratios[ri][0], (long)ratios[ri][1]);
    } else {
        strcpy(j->path, "/api/v1/chat/completions");
        sprintf(p, "{\"model\":\"%s\",\"modalities\":[\"image\",\"text\"],"
                   "\"image_config\":{\"aspect_ratio\":\"%ld:%ld\"},\"usage\":{\"include\":true},"
                   "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"",
                m, (long)ratios[ri][0], (long)ratios[ri][1]);
    }
    return p + strlen(p);
}

/* "http://rechner:port/" -> host, port */
static void comfy_addr(const char *s, char *host, LONG max, LONG *port)
{
    LONG n = 0;
    if (!strncmp(s, "http://", 7))
        s += 7;
    while (*s && *s != ':' && *s != '/' && n < max - 1)
        host[n++] = *s++;
    host[n] = 0;
    *port = *s == ':' ? atol(s + 1) : COMFY_PORT;
    if (*port <= 0 || *port > 65535)
        *port = COMFY_PORT;
}

/* Workflow aus ENV:, ENVARC: oder PROGDIR:ComfyUI/ ganz lesen (malloc, nullterminiert) */
static char *read_workflow(const char *name)
{
    static const char *dirs[] = { "ENV:AmiPixelShop/", "ENVARC:AmiPixelShop/", "PROGDIR:ComfyUI/" };
    LONG d, n, cap = 262144;
    char path[96], *b;
    BPTR fh;
    for (d = 0; d < 3; d++) {
        strcpy(path, dirs[d]);
        strcat(path, name);
        if (!(fh = Open((STRPTR)path, MODE_OLDFILE)))
            continue;
        b = malloc(cap);
        n = b ? Read(fh, b, cap - 1) : 0;
        Close(fh);
        if (n > 0 && n < cap - 1) {
            b[n] = 0;
            return b;
        }
        free(b);
    }
    return NULL;
}

static BOOL have_workflow(const char *name)
{
    char *t = read_workflow(name);
    free(t);
    return t != NULL;
}

/* /prompt-Anfrage aus dem Workflow (API-Format) mit den Platzhaltern %image%, %seed%,
 * %prompt% (Text fuer Inpaint-Modelle), %task% (vollstaendige Anweisung fuer
 * Edit-Modelle wie FLUX 3, Vorgabe: prompt) und %box% (Bereich als [y_min, x_min,
 * y_max, x_max] in 0..1, Vorgabe: ganzes Bild); meldet Fehler selbst */
static BOOL comfy_body(AiJob *j, const char *name, const char *prompt, const char *task, const char *box)
{
    static ULONG cnt;
    char *t, *esc, *etask, *p, seed[12], msg[300];
    const char *s;
    LONG pl, tl, n = 0;

    if (!task)
        task = prompt;
    if (!box)
        box = "[0.000, 0.000, 1.000, 1.000]";
    pl = strlen(prompt) * 2 + 8;
    tl = strlen(task) * 2 + 8;

    if (!(t = read_workflow(name))) {
        sprintf(msg, "No ComfyUI workflow %s found.\n\nIn ComfyUI use Workflow > Export (API) and save\n"
                     "the file as ENVARC:AmiPixelShop/%s\n(examples: ComfyUI drawer of AmiPixelShop)", name, name);
        request(msg, "OK");
        return FALSE;
    }
    if (!strstr(t, "\"class_type\"") || !strstr(t, "%image%")) {
        free(t);
        sprintf(msg, "The ComfyUI workflow %s cannot be used.\n\nExport it with Workflow > Export (API), and put\n"
                     "%%image%% as the image name of the Load Image node\nand %%prompt%% (or %%task%%) into the prompt text.", name);
        request(msg, "OK");
        return FALSE;
    }
    for (s = t; (s = strchr(s, '%')); s++)     /* obere Grenze fuer die Zahl der Platzhalter */
        n++;
    sprintf(seed, "%lu", (unsigned long)(((now_us() ^ (++cnt * 2654435761UL)) & 0x7FFFFFFF) | 1));   /* nie 0 */
    etask = NULL;
    if (!(esc = malloc(pl)) || !(etask = malloc(tl)) ||
        !(j->body = net_alloc(strlen(t) + n * ((pl > tl ? pl : tl) + strlen(box)) + 256))) {
        free(esc);
        free(etask);
        free(t);
        status_message("Not enough memory");
        return FALSE;
    }
    json_esc(prompt, esc, pl);
    json_esc(task, etask, tl);
    strcpy(j->body, "{\"client_id\":\"amipixelshop\",\"prompt\":");
    for (p = j->body + strlen(j->body), s = t; *s;) {
        if (!strncmp(s, "\"%seed%\"", 8) || !strncmp(s, "%seed%", 6)) {      /* Zahl, ohne Anfuehrungszeichen */
            s += *s == '"' ? 8 : 6;
            strcpy(p, seed);
        } else if (!strncmp(s, "%prompt%", 8)) {
            s += 8;
            strcpy(p, esc);
        } else if (!strncmp(s, "%task%", 6)) {
            s += 6;
            strcpy(p, etask);
        } else if (!strncmp(s, "%box%", 5)) {
            s += 5;
            strcpy(p, box);
        } else if (!strncmp(s, "%image%", 7)) {
            s += 7;
            strcpy(p, COMFY_IN);
        } else {
            *p++ = *s++;
            continue;
        }
        p += strlen(p);
    }
    strcpy(p, "}");
    j->blen = p + 1 - j->body;
    free(esc);
    free(etask);
    free(t);
    return TRUE;
}

/* Bild als PNG (mit Alpha: durchsichtig = zu fuellen, wird in ComfyUI zur Maske) */
static BOOL comfy_upload(AiJob *j, const UBYTE *px, LONG w, LONG h, LONG comp)
{
    static const char bnd[] = "AmiPixelShop-7d3f19c2";
    MemBuf png = { NULL, 0, 0 };
    char *p;
    if (!stbi_write_png_to_func(mem_write, &png, w, h, comp, px, w * comp) || !png.p ||
        !(j->up = net_alloc(png.n + 512))) {
        free(png.p);
        status_message("Not enough memory");
        return FALSE;
    }
    p = j->up;
    sprintf(p, "--%s\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\ntrue\r\n"
               "--%s\r\nContent-Disposition: form-data; name=\"image\"; filename=\"" COMFY_IN "\"\r\n"
               "Content-Type: image/png\r\n\r\n", bnd, bnd);
    p += strlen(p);
    memcpy(p, png.p, png.n);
    p += png.n;
    sprintf(p, "\r\n--%s--\r\n", bnd);
    p += strlen(p);
    j->uplen = p - j->up;
    free(png.p);
    sprintf(j->uphdr, "Content-Type: multipart/form-data; boundary=%s\r\n", bnd);
    return TRUE;
}

static char *body_image(AiJob *j, char *p)
{
    strcpy(p, j->prov == PROV_GEMINI ? "\"},{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"" :
              "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,");
    return p + strlen(p);
}

/* Ausschnitt: Auswahl plus Umgebung, auf ein unterstuetztes Seitenverhaeltnis
 * erweitert; liefert den Index in ratios */
static LONG crop_box(LONG *bx, LONG *by, LONG *bw, LONG *bh)
{
    LONG sw = doc->selx1 - doc->selx0, sh = doc->sely1 - doc->sely0;
    LONG pad = (sw > sh ? sw : sh) * 35 / 100 + 16, best = 0, i;
    /* kleine Bereiche: genug Umgebung, damit das Modell die Szene versteht */
    if (pad < (512 - (sw > sh ? sw : sh)) / 2)
        pad = (512 - (sw > sh ? sw : sh)) / 2;
    LONG x0 = doc->selx0 - pad, y0 = doc->sely0 - pad, x1 = doc->selx1 + pad, y1 = doc->sely1 + pad;
    float a, bd = 1e9f;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > doc->w) x1 = doc->w;
    if (y1 > doc->h) y1 = doc->h;
    a = (float)(x1 - x0) / (float)(y1 - y0);
    for (i = 0; i < (LONG)(sizeof(ratios) / 2); i++) {
        float r = (float)ratios[i][0] / ratios[i][1];
        float d = r > a ? r / a : a / r;
        /* nur erweitern, wenn das Bild dafuer Platz hat */
        if ((r > a && (y1 - y0) * r > doc->w + 0.5f) || (r < a && (x1 - x0) / r > doc->h + 0.5f))
            continue;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    {
        float r = (float)ratios[best][0] / ratios[best][1];
        LONG w = x1 - x0, h = y1 - y0, nw = w, nh = h;
        if (r > a)
            nw = (LONG)(h * r + 0.5f);
        else
            nh = (LONG)(w / r + 0.5f);
        if (nw > doc->w) nw = doc->w;
        if (nh > doc->h) nh = doc->h;
        x0 -= (nw - w) / 2;
        y0 -= (nh - h) / 2;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x0 + nw > doc->w) x0 = doc->w - nw;
        if (y0 + nh > doc->h) y0 = doc->h - nh;
        *bx = x0;
        *by = y0;
        *bw = nw;
        *bh = nh;
    }
    return best;
}

/* Komposit (premultiplied) bilinear in RGB abtasten; Durchsichtiges wird mittelgrau
 * (das Modell soll es nicht fuer weisses Bild halten, es ist ohnehin zu fuellen) */
static void sample_rgb(UBYTE *dst, LONG dw, LONG dh, LONG bx, LONG by, LONG bw, LONG bh)
{
    LONG x, y, sx = (bw << 16) / dw, sy = (bh << 16) / dh;
    for (y = 0; y < dh; y++) {
        LONG fy = y * sy + (sy >> 1) - 32768, y0, y1, wy;
        if (fy < 0) fy = 0;
        y0 = fy >> 16;
        wy = (fy >> 8) & 255;
        y1 = y0 + 1 < bh ? y0 + 1 : bh - 1;
        for (x = 0; x < dw; x++) {
            LONG fx = x * sx + (sx >> 1) - 32768, x0, x1, wx, c;
            ULONG p[4];
            if (fx < 0) fx = 0;
            x0 = fx >> 16;
            wx = (fx >> 8) & 255;
            x1 = x0 + 1 < bw ? x0 + 1 : bw - 1;
            p[0] = doc->flat[(by + y0) * doc->w + bx + x0];
            p[1] = doc->flat[(by + y0) * doc->w + bx + x1];
            p[2] = doc->flat[(by + y1) * doc->w + bx + x0];
            p[3] = doc->flat[(by + y1) * doc->w + bx + x1];
            for (c = 0; c < 3; c++) {
                LONG sh = 16 - c * 8, v[4], k, r;
                for (k = 0; k < 4; k++)
                    v[k] = ((p[k] >> sh) & 0xFF) + ((255 - (p[k] >> 24)) >> 1);
                r = ((v[0] * (256 - wx) + v[1] * wx) * (256 - wy) + (v[2] * (256 - wx) + v[3] * wx) * wy + 32768) >> 16;
                dst[(y * dw + x) * 3 + c] = r > 255 ? 255 : r;
            }
        }
    }
}

/* ---------- Hintergrundprozess ----------
 * Hier keine stdio-Funktionen von libnix (sprintf usw.): im fremden Prozess
 * zerstoerten sie den Auftrag. Nur exec/dos und net.c. */

static BOOL strstr_n(const char *s, LONG n, const char *w)
{
    LONG l = strlen(w), i;
    for (i = 0; i + l <= n; i++)
        if (!memcmp(s + i, w, l))
            return TRUE;
    return FALSE;
}

static void refused(AiJob *j, const char *v, LONG vl)
{
    strcpy(j->err, strstr_n(v, vl, "RECITATION") ?
           "The model refused: the result would be too similar to an existing\n"
           "(published) image. Try a larger selection or add a prompt." :
           "The request was blocked by the model's safety filter.");
}

/* Gemini generateContent: Bild im letzten inlineData-Teil (ohne Denk-Zwischenbilder) */
static void gemini_answer(AiJob *j, const char *js, LONG status)
{
    char path[64];
    const char *v, *img = NULL;
    LONG vl, il = 0, k, n;

    if (status != 200) {
        if ((v = json_get(js, "error.message", &vl)))
            json_str(v, vl, j->err, sizeof(j->err));
        else
            strcpy(j->err, "Server error");
        return;
    }
    for (k = 0; k < 10; k++) {
        strcpy(path, "candidates.0.content.parts.");
        n = strlen(path);
        path[n++] = '0' + k;
        path[n] = 0;
        if (!json_get(js, path, &vl))   /* Teil k vorhanden? */
            break;
        path[n++] = '.';
        strcpy(path + n, "thought");
        if ((v = json_get(js, path, &vl)) && *v == 't')
            continue;
        strcpy(path + n, "inlineData.data");
        if ((v = json_get(js, path, &vl)) && vl > 2) {
            img = v;
            il = vl;
        }
    }
    if (img) {
        if ((j->img = net_alloc(il)))
            j->imglen = b64_decode(img + 1, il - 2, j->img);
        if (j->imglen > 0)
            j->ok = TRUE;
        else
            strcpy(j->err, "Invalid image in the answer");
    } else if (((v = json_get(js, "candidates.0.finishReason", &vl)) ||
                (v = json_get(js, "promptFeedback.blockReason", &vl))) && vl > 2 &&
               (strstr_n(v, vl, "RECITATION") || strstr_n(v, vl, "SAFETY") ||
                strstr_n(v, vl, "PROHIBITED") || strstr_n(v, vl, "BLOCK"))) {
        refused(j, v, vl);
    } else if ((v = json_get(js, "candidates.0.content.parts.0.text", &vl)) && *v == '"') {
        strcpy(j->err, "No image: ");
        json_str(v, vl, j->err + 10, sizeof(j->err) - 10);
    } else {
        strcpy(j->err, "The model returned no image");
    }
}

/* ab p (bis e) den naechsten nicht leeren String-Wert zu "key" entschluesseln;
 * *after zeigt dann hinter den Wert */
static BOOL find_str(const char *p, const char *e, const char *key, char *dst, LONG max, const char **after)
{
    LONG kl = strlen(key);
    for (; p + kl + 2 < e; p++) {
        const char *v, *ve;
        if (*p != '"' || memcmp(p + 1, key, kl) || p[kl + 1] != '"')
            continue;
        for (v = p + kl + 2; v < e && (*v == ':' || (UBYTE)*v <= ' '); v++)
            ;
        if (v >= e || *v != '"')
            continue;
        for (ve = v + 1; ve < e && *ve != '"'; ve++)
            if (*ve == '\\')
                ve++;
        if (ve >= e)
            return FALSE;
        if (ve - v > 1) {
            json_str(v, ve + 1 - v, dst, max);
            if (after)
                *after = ve + 1;
            return TRUE;
        }
    }
    return FALSE;
}

static void cat_limit(char *d, const char *s, LONG max)
{
    LONG n = strlen(d);
    while (*s && n < max - 1)
        d[n++] = *s++;
    d[n] = 0;
}

/* Ablauf der letzten Anfrage fuer die Fehlersuche: T:AmiPixelShop-AI.log.
 * Nur dos.library, daher auch im Hintergrundprozess nutzbar; n < 0: keine Zahl */
#define AI_LOG "T:AmiPixelShop-AI.log"

static void ai_log(const char *what, const char *s, LONG n)
{
    struct DateStamp ds;
    char line[220], num[16];
    LONG k = 15, t;
    BPTR fh = Open((STRPTR)AI_LOG, MODE_READWRITE);
    if (!fh)
        return;
    Seek(fh, 0, OFFSET_END);
    DateStamp(&ds);
    t = (ds.ds_Minute % 60) * 6000 + ds.ds_Tick * 2;        /* Hundertstel in der Stunde */
    num[k] = 0;
    num[--k] = ' ';
    num[--k] = '0' + t % 10;
    num[--k] = '0' + t / 10 % 10;
    num[--k] = '.';
    for (t /= 100; k > 7; t /= 10)
        num[--k] = '0' + t % 10;
    strcpy(line, num + k);
    cat_limit(line, what, sizeof(line));
    if (s) {
        cat_limit(line, " ", sizeof(line));
        cat_limit(line, s, sizeof(line));
    }
    if (n >= 0) {
        ULONG v = n;
        k = 15;
        num[k] = 0;
        do {
            num[--k] = '0' + v % 10;
            v /= 10;
        } while (v);
        num[--k] = ' ';
        cat_limit(line, num + k, sizeof(line));
    }
    cat_limit(line, "\n", sizeof(line));
    Write(fh, line, strlen(line));
    Close(fh);
}

/* Fehlertext aus einer ComfyUI-Antwort; die "details" nennen meist das Problem
 * (z. B. ein Modell, das auf dem PC fehlt) */
static void comfy_err(AiJob *j, NetResp *r, const char *what)
{
    const char *js = (const char *)r->body, *v;
    char det[120];
    LONG vl;
    strcpy(j->err, what);
    if (!js)
        return;
    if ((v = json_get(js, "error.message", &vl)) && *v == '"') {
        cat_limit(j->err, ":\n", sizeof(j->err));
        json_str(v, vl, j->err + strlen(j->err), sizeof(j->err) - strlen(j->err));
    }
    if (find_str(js, js + r->len, "details", det, sizeof(det), NULL) ||
        find_str(js, js + r->len, "exception_message", det, sizeof(det), NULL)) {
        cat_limit(j->err, "\n", sizeof(j->err));
        cat_limit(j->err, det, sizeof(j->err));
    }
}

static void url_cat(char *d, const char *s, LONG max)
{
    static const char hx[] = "0123456789ABCDEF";
    LONG n = strlen(d);
    for (; *s && n < max - 4; s++) {
        UBYTE c = *s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.') {
            d[n++] = c;
        } else {
            d[n++] = '%';
            d[n++] = hx[c >> 4];
            d[n++] = hx[c & 15];
        }
    }
    d[n] = 0;
}

/* ComfyUI: Bild hochladen, Workflow einreihen, auf das Ergebnis warten, Bild holen */
static void comfy_run(AiJob *j)
{
    NetResp r;
    const char *v, *js, *a;
    LONG vl, t, k;
    char id[64], kp[96], path[320], fn[100], sub[100], type[16];

    ai_log("ComfyUI upload to", j->host, j->uplen);
    if (!net_http(j->host, j->port, FALSE, "POST", "/upload/image", j->uphdr, j->up, j->uplen, &r, &j->ctl)) {
        strcpy(j->err, r.err);
        return;
    }
    if (r.status != 200) {
        comfy_err(j, &r, "Upload to ComfyUI failed");
        net_free(&r);
        return;
    }
    net_free(&r);
    if (!net_http(j->host, j->port, FALSE, "POST", "/prompt", j->hdr, j->body, j->blen, &r, &j->ctl)) {
        strcpy(j->err, r.err);
        return;
    }
    if (r.status != 200 || !(v = json_get((const char *)r.body, "prompt_id", &vl)) || *v != '"') {
        comfy_err(j, &r, "ComfyUI rejected the workflow");
        net_free(&r);
        return;
    }
    json_str(v, vl, id, sizeof(id));
    net_free(&r);
    ai_log("ComfyUI queued", id, -1);

    /* fertig, sobald der Auftrag in der History steht (beim ersten Mal laedt der PC die Modelle) */
    strcpy(path, "/history/");
    cat_limit(path, id, sizeof(path));
    for (t = 0;; t++) {
        for (k = 0; k < 5; k++) {
            if (j->ctl.cancel) {
                strcpy(j->err, "Cancelled");
                return;
            }
            Delay(10);
        }
        if (t >= 900) {                 /* 1 s je Runde */
            strcpy(j->err, "ComfyUI did not finish within 15 minutes");
            return;
        }
        if (!net_http(j->host, j->port, FALSE, "GET", path, NULL, NULL, 0, &r, &j->ctl)) {
            strcpy(j->err, r.err);
            return;
        }
        if (r.status == 200 && json_get((const char *)r.body, id, &vl))
            break;
        net_free(&r);
    }
    js = (const char *)r.body;
    ai_log("ComfyUI finished, seconds waited:", NULL, t + 1);
    strcpy(kp, id);
    strcat(kp, ".status.status_str");
    if ((v = json_get(js, kp, &vl)) && strstr_n(v, vl, "error")) {
        comfy_err(j, &r, "The ComfyUI workflow failed");
        net_free(&r);
        return;
    }
    strcpy(kp, id);
    strcat(kp, ".outputs");
    if (!(v = json_get(js, kp, &vl)) || !find_str(v, v + vl, "filename", fn, sizeof(fn), &a)) {
        strcpy(j->err, "The workflow returned no image (it needs a Save Image node)");
        net_free(&r);
        return;
    }
    sub[0] = 0;
    strcpy(type, "output");
    find_str(a, v + vl, "subfolder", sub, sizeof(sub), NULL);
    find_str(a, v + vl, "type", type, sizeof(type), NULL);
    net_free(&r);

    strcpy(path, "/view?filename=");
    url_cat(path, fn, sizeof(path));
    cat_limit(path, "&subfolder=", sizeof(path));
    url_cat(path, sub, sizeof(path));
    cat_limit(path, "&type=", sizeof(path));
    url_cat(path, type, sizeof(path));
    if (!net_http(j->host, j->port, FALSE, "GET", path, NULL, NULL, 0, &r, &j->ctl)) {
        strcpy(j->err, r.err);
        return;
    }
    ai_log("ComfyUI result", fn, r.len);
    if (r.status == 200 && r.len > 0) {
        j->img = r.body;                /* PNG, wie geliefert */
        j->imglen = r.len;
        j->ok = TRUE;
    } else {
        strcpy(j->err, "Cannot download the result from ComfyUI");
        net_free(&r);
    }
}

static void ai_worker(void)
{
    AiJob *j = job;
    NetResp r;
    const char *v;
    LONG vl;

    ai_log("worker: request", j->path, j->blen);
    if (j->prov == PROV_COMFY) {
        comfy_run(j);
    } else if (!net_https(j->prov == PROV_GEMINI ? GEM_HOST : AI_HOST, "POST", j->path, j->hdr, j->body,
                          j->blen, &r, &j->ctl)) {
        strcpy(j->err, r.err);
    } else if (j->prov == PROV_GEMINI) {
        ai_log("worker: HTTP status", NULL, r.status);
        ai_log("worker: answer bytes", NULL, r.len);
        gemini_answer(j, (const char *)r.body, r.status);
        net_free(&r);
    } else {
        ai_log("worker: HTTP status", NULL, r.status);
        ai_log("worker: answer bytes", NULL, r.len);
        const char *js = (const char *)r.body;
        if (r.status != 200) {
            if ((v = json_get(js, "error.message", &vl)))
                json_str(v, vl, j->err, sizeof(j->err));
            else
                strcpy(j->err, "Server error");
        } else if ((v = json_get(js, "choices.0.message.images.0.image_url.url", &vl))) {
            const char *e = v + vl, *b = v;
            while (b < e && *b != ',')
                b++;
            if (b < e && (j->img = net_alloc(vl)))
                j->imglen = b64_decode(b + 1, e - b - 1, j->img);
            if (j->imglen > 0)
                j->ok = TRUE;
            else
                strcpy(j->err, "Invalid image in the answer");
            if ((v = json_get(js, "usage.cost", &vl)) && vl < (LONG)sizeof(j->cost)) {
                memcpy(j->cost, v, vl);
                j->cost[vl] = 0;
            }
        } else if ((v = json_get(js, "choices.0.native_finish_reason", &vl)) && vl > 2 &&
                   (strstr_n(v, vl, "RECITATION") || strstr_n(v, vl, "SAFETY") || strstr_n(v, vl, "PROHIBITED"))) {
            refused(j, v, vl);
        } else if ((v = json_get(js, "choices.0.message.content", &vl)) && *v == '"') {
            strcpy(j->err, "No image: ");
            json_str(v, vl, j->err + 10, sizeof(j->err) - 10);
        } else if ((v = json_get(js, "error.message", &vl))) {
            json_str(v, vl, j->err, sizeof(j->err));
        } else {
            strcpy(j->err, "The model returned no image");
        }
        net_free(&r);
    }
    if (j->ok)
        ai_log("worker: image bytes", NULL, j->imglen);
    else
        ai_log("worker: error", j->err, -1);
    Forbid();                   /* Prozess endet, bevor der Hauptprozess weiterlaeuft */
    j->done = 1;
    Signal(j->parent, 1UL << j->sig);
}

static void job_free(AiJob *j)
{
    if (!j)
        return;
    net_freemem(j->body);
    net_freemem(j->up);
    net_freemem(j->img);
    free(j->sel);
    FreeVec(j);
}

/* ---------- Ergebnis einsetzen ---------- */

static BOOL doc_alive(Doc *d)
{
    LONG i;
    for (i = 0; i < ndocs; i++)
        if (docs[i] == d)
            return TRUE;
    return FALSE;
}

/* Maskenrand leicht weich (Kastenfilter, zwei Durchgaenge) */
static void soften(UBYTE *m, LONG w, LONG h, LONG r)
{
    UBYTE *t = malloc(w > h ? w : h);
    LONG pass, x, y;
    if (!t || r < 1) {
        free(t);
        return;
    }
    for (pass = 0; pass < 2; pass++) {
        for (y = 0; y < h; y++) {
            UBYTE *row = m + y * w;
            LONG s = 0, n = 0;
            for (x = -r; x < w + r; x++) {
                if (x + r < w) { s += row[x + r < 0 ? 0 : x + r]; n++; }
                if (x - r - 1 >= 0) { s -= row[x - r - 1]; n--; }
                if (x >= 0 && x < w)
                    t[x] = (s + n / 2) / n;
            }
            memcpy(row, t, w);
        }
        for (x = 0; x < w; x++) {
            LONG s = 0, n = 0;
            for (y = -r; y < h + r; y++) {
                if (y + r < h) { s += m[(y + r < 0 ? 0 : y + r) * w + x]; n++; }
                if (y - r - 1 >= 0) { s -= m[(y - r - 1) * w + x]; n--; }
                if (y >= 0 && y < h)
                    t[y] = (s + n / 2) / n;
            }
            for (y = 0; y < h; y++)
                m[y * w + x] = t[y];
        }
    }
    free(t);
}

/* Graustufen-Kleinbild (Blockmittel) des Ergebnisses */
static UBYTE *gen_thumb(const UBYTE *rgb, LONG iw, LONG ih, LONG tw, LONG th)
{
    UBYTE *t = malloc(tw * th);
    LONG u, v, x, y;
    if (!t)
        return NULL;
    for (v = 0; v < th; v++)
        for (u = 0; u < tw; u++) {
            LONG x0 = u * iw / tw, x1 = (u + 1) * iw / tw, y0 = v * ih / th, y1 = (v + 1) * ih / th, s = 0, n = 0;
            for (y = y0; y < y1; y++)
                for (x = x0; x < x1; x++) {
                    const UBYTE *q = rgb + (y * iw + x) * 3;
                    s += q[0] * 77 + q[1] * 151 + q[2] * 28;
                    n++;
                }
            t[v * tw + u] = n ? s / (n * 256) : 0;
        }
    return t;
}

/* ... und vom Original im Ausschnitt; ok[] = Block ganz deckend und ausserhalb der Auswahl */
static UBYTE *orig_thumb(AiJob *j, LONG tw, LONG th, UBYTE *ok)
{
    UBYTE *t = malloc(tw * th);
    LONG u, v, x, y;
    if (!t)
        return NULL;
    for (v = 0; v < th; v++)
        for (u = 0; u < tw; u++) {
            LONG x0 = u * j->w / tw, x1 = (u + 1) * j->w / tw, y0 = v * j->h / th, y1 = (v + 1) * j->h / th;
            LONG s = 0, n = 0;
            BOOL good = TRUE;
            for (y = y0; y < y1; y++)
                for (x = x0; x < x1; x++) {
                    ULONG q = doc->flat[(j->y + y) * doc->w + j->x + x];
                    if ((q >> 24) != 255 || j->sel[y * j->w + x])
                        good = FALSE;
                    s += ((q >> 16) & 0xFF) * 77 + ((q >> 8) & 0xFF) * 151 + (q & 0xFF) * 28;
                    n++;
                }
            t[v * tw + u] = n ? s / (n * 256) : 0;
            ok[v * tw + u] = good && n;
        }
    return t;
}

/* Summe der Abweichungen fuer Massstab s und Versatz (dx, dy), alles 16.16 in
 * Kleinbild-Pixeln; ganzzahlig (float->int ist auf dem 68040 sehr langsam) */
static LONG align_cost(const UBYTE *o, const UBYTE *ok, const UBYTE *g, LONG tw, LONG th,
                       LONG bias, LONG s, LONG dx, LONG dy, LONG limit)
{
    LONG u, v, sum = 0, maxx = (tw - 1) << 16, maxy = (th - 1) << 16;
    LONG fx0 = (tw << 15) + dx - 32768 - (((tw - 1) * s) >> 1);
    LONG fy = (th << 15) + dy - 32768 - (((th - 1) * s) >> 1);
    for (v = 0; v < th; v++, fy += s) {
        LONG cy = fy < 0 ? 0 : fy > maxy ? maxy : fy, y0 = cy >> 16, wy = (cy >> 8) & 255;
        const UBYTE *r0 = g + y0 * tw, *r1 = y0 + 1 < th ? r0 + tw : r0;
        const UBYTE *orow = o + v * tw, *krow = ok + v * tw;
        LONG fx = fx0;
        for (u = 0; u < tw; u++, fx += s) {
            LONG cx, x0, x1, wx, a, b, d;
            if (!krow[u])
                continue;
            cx = fx < 0 ? 0 : fx > maxx ? maxx : fx;
            x0 = cx >> 16;
            x1 = x0 + 1 < tw ? x0 + 1 : x0;
            wx = (cx >> 8) & 255;
            a = r0[x0] * (256 - wx) + r0[x1] * wx;
            b = r1[x0] * (256 - wx) + r1[x1] * wx;
            d = (((LONG)orow[u] << 16) - (a * (256 - wy) + b * wy) - bias) / 256;
            sum += d < 0 ? -d : d;
        }
        if (sum > limit)            /* schon schlechter als der beste: abbrechen */
            return sum;
    }
    return sum;
}

/* Bildmodelle verschieben und zoomen den Inhalt oft etwas. Massstab und Versatz
 * suchen, mit denen das Ergebnis am besten auf der unveraenderten Umgebung
 * liegt: grob auf 64 Pixel Breite, dann feiner auf 128 und 256. Liefert den
 * Versatz in Ausschnitt-Pixeln. */
static void align_result(AiJob *j, const UBYTE *rgb, LONG iw, LONG ih, float *ps, float *pdx, float *pdy)
{
    LONG level, levels = j->w >= 400 ? 3 : 2;
    LONG bs = 65536, bx = 0, by = 0;            /* 16.16 */
    *ps = 1.0f;
    *pdx = *pdy = 0;
    for (level = 0; level < levels; level++) {
        LONG tw = 64 << level, th = tw * j->h / j->w, i, n = 0, mo = 0, mg = 0, bias;
        LONG s, dx, dy, s0, s1, ss, d0, d1, ds, best, c0, cbx, cby;
        UBYTE *ok, *o, *g;
        if (th < 16)
            return;
        ok = malloc(tw * th);
        o = ok ? orig_thumb(j, tw, th, ok) : NULL;
        g = o ? gen_thumb(rgb, iw, ih, tw, th) : NULL;
        if (!g) {
            free(ok);
            free(o);
            return;
        }
        for (i = 0; i < tw * th; i++) {
            mg += g[i];
            if (ok[i]) {
                mo += o[i];
                n++;
            }
        }
        if (n < tw * th / 20) {     /* zu wenig Umgebung zum Vergleichen */
            free(ok);
            free(o);
            free(g);
            return;
        }
        bias = ((mo << 4) / n - (mg << 4) / (tw * th)) * 4096;  /* Helligkeitsunterschied, 16.16 */
        if (level == 0) {
            s0 = 60293; s1 = 70779; ss = 1311;          /* 0.92 .. 1.08, 0.02 */
            d0 = -(6 << 16); d1 = 6 << 16; ds = 65536;
        } else {                    /* bisheriges Ergebnis auf doppelte Aufloesung */
            LONG r = level == 1 ? 1311 : 393;           /* 0.02 / 0.006 */
            bx *= 2;
            by *= 2;
            s0 = bs - r; s1 = bs + r; ss = r / 2;
            d0 = -(3 << 15); d1 = 3 << 15; ds = 32768;
        }
        for (i = 0; i < tw * th; i++)       /* nur jedes zweite Pixel vergleichen */
            if (((i / tw) | i) & 1)
                ok[i] = 0;
        c0 = align_cost(o, ok, g, tw, th, bias, 65536, 0, 0, 0x7FFFFFFF);
        best = 0x7FFFFFFF;
        cbx = bx;
        cby = by;
        for (s = s0; s <= s1; s += ss)
            for (dy = d0; dy <= d1; dy += ds)
                for (dx = d0; dx <= d1; dx += ds) {
                    LONG c = align_cost(o, ok, g, tw, th, bias, s, cbx + dx, cby + dy, best);
                    if (c < best) {
                        best = c;
                        bs = s;
                        bx = cbx + dx;
                        by = cby + dy;
                    }
                }
        free(ok);
        free(o);
        free(g);
        if (level == levels - 1 && best < c0 - c0 / 20) {
            *ps = bs / 65536.0f;
            *pdx = (float)bx / 65536.0f * j->w / tw;
            *pdy = (float)by / 65536.0f * j->h / th;
        }
    }
}

/* Beim Entfernen nimmt das Modell das ganze Objekt weg, auch Teile neben dem
 * Strich. Maske von der Auswahl aus in deutlich veraenderte Pixel wachsen
 * lassen (Breitensuche, begrenzter Abstand), damit keine Reste stehen bleiben. */
static void grow_mask(AiJob *j, const ULONG *gen)
{
    LONG n = j->w * j->h, *q, qh = 0, qt = 0, i, maxd;
    LONG sx0 = j->w, sy0 = j->h, sx1 = 0, sy1 = 0, x, y;
    UBYTE *dist;
    for (y = 0; y < j->h; y++)
        for (x = 0; x < j->w; x++)
            if (j->sel[y * j->w + x] >= 128) {
                if (x < sx0) sx0 = x;
                if (x > sx1) sx1 = x;
                if (y < sy0) sy0 = y;
                if (y > sy1) sy1 = y;
            }
    if (sx1 < sx0)
        return;
    maxd = ((sx1 - sx0 > sy1 - sy0 ? sx1 - sx0 : sy1 - sy0) * 15) / 100 + 12;
    if (maxd > 80)
        maxd = 80;
    if (!(q = malloc(n * sizeof(LONG))) || !(dist = malloc(n))) {
        free(q);
        return;
    }
    memset(dist, 255, n);
    for (i = 0; i < n; i++)
        if (j->sel[i] >= 128) {
            dist[i] = 0;
            q[qt++] = i;
        }
    while (qh < qt) {
        LONG c = q[qh++], cx = c % j->w, cy = c / j->w, k;
        static const BYTE nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        if (dist[c] >= maxd)
            continue;
        for (k = 0; k < 4; k++) {
            LONG nx = cx + nb[k][0], ny = cy + nb[k][1], ni, d;
            ULONG o, g;
            if (nx < 0 || ny < 0 || nx >= j->w || ny >= j->h)
                continue;
            ni = ny * j->w + nx;
            if (dist[ni] != 255)
                continue;
            o = doc->flat[(j->y + ny) * doc->w + j->x + nx];
            g = gen[ni];
            d = abs((LONG)((o >> 16) & 0xFF) - (LONG)((g >> 16) & 0xFF)) +
                abs((LONG)((o >> 8) & 0xFF) - (LONG)((g >> 8) & 0xFF)) +
                abs((LONG)(o & 0xFF) - (LONG)(g & 0xFF));
            if (d < 70) {
                dist[ni] = 254;         /* geprueft, bleibt draussen */
                continue;
            }
            dist[ni] = dist[c] + 1;
            j->sel[ni] = 255;
            q[qt++] = ni;
        }
    }
    free(q);
    free(dist);
}

void ai_gen_free(GenInfo *g)
{
    LONG i;
    if (!g)
        return;
    for (i = 0; i < g->nvar; i++)
        free(g->var[i].px);
    FreeVec(g);
}

/* Variante k einer generativen Ebene in deren Pixel schreiben (Maske bleibt) */
static void gen_show(Layer *l, LONG k)
{
    GenInfo *g = l->gen;
    GenVar *o, *v;
    LONG x, y, tx, ty;
    if (!g || k < 0 || k >= g->nvar)
        return;
    o = g->cur < g->nvar ? &g->var[g->cur] : NULL;
    v = &g->var[k];
    if (o && o->px)                     /* alte Variante leeren */
        for (ty = o->y >> TILE_SHIFT; ty <= (o->y + o->h - 1) >> TILE_SHIFT; ty++)
            for (tx = o->x >> TILE_SHIFT; tx <= (o->x + o->w - 1) >> TILE_SHIFT; tx++) {
                Tile *t = l->tiles[ty * doc->tw + tx];
                if (!t)
                    continue;
                for (y = 0; y < TILE_SIZE; y++) {
                    LONG dy = (ty << TILE_SHIFT) + y - o->y;
                    if (dy < 0 || dy >= o->h)
                        continue;
                    for (x = 0; x < TILE_SIZE; x++) {
                        LONG dx = (tx << TILE_SHIFT) + x - o->x;
                        if (dx >= 0 && dx < o->w)
                            t->px[(y << TILE_SHIFT) | x] = 0;
                    }
                }
                tile_update_flag(doc, t, tx, ty);
            }
    if (v->px)
        for (ty = v->y >> TILE_SHIFT; ty <= (v->y + v->h - 1) >> TILE_SHIFT; ty++)
            for (tx = v->x >> TILE_SHIFT; tx <= (v->x + v->w - 1) >> TILE_SHIFT; tx++) {
                LONG idx = ty * doc->tw + tx;
                Tile *t = l->tiles[idx];
                if (!t && !(t = l->tiles[idx] = tile_new()))
                    continue;
                for (y = 0; y < TILE_SIZE; y++) {
                    LONG dy = (ty << TILE_SHIFT) + y - v->y;
                    if (dy < 0 || dy >= v->h)
                        continue;
                    for (x = 0; x < TILE_SIZE; x++) {
                        LONG dx = (tx << TILE_SHIFT) + x - v->x;
                        if (dx >= 0 && dx < v->w)
                            t->px[(y << TILE_SHIFT) | x] = v->px[dy * v->w + dx];
                    }
                }
                tile_update_flag(doc, t, tx, ty);
            }
    g->cur = k;
    doc->modified = TRUE;
    doc_mark_all_dirty();
    view_inval_all();
    ui_draw_panels();
}

/* Variante blaettern (dir -1/+1) */
void ai_gen_step(LONG dir)
{
    Layer *l = doc->layers[doc->active];
    GenInfo *g = l->gen;
    if (!g || g->nvar < 2)
        return;
    gen_show(l, (g->cur + dir + g->nvar) % g->nvar);
}

static void place_variant(AiJob *j, ULONG *gen)
{
    Layer *l = NULL;
    GenInfo *g;
    LONG i;
    for (i = 0; i < doc->nlayers; i++)
        if (doc->layers[i] == j->target)
            l = j->target;
    if (!l || !(g = l->gen)) {
        free(gen);
        return;
    }
    if (g->nvar == GEN_MAXVAR) {        /* voll: aelteste (nicht angezeigte) verwerfen */
        LONG k = g->cur == 0 ? 1 : 0;
        free(g->var[k].px);
        for (i = k; i < GEN_MAXVAR - 1; i++)
            g->var[i] = g->var[i + 1];
        g->nvar--;
        if (g->cur > k)
            g->cur--;
    }
    g->var[g->nvar].x = j->x;
    g->var[g->nvar].y = j->y;
    g->var[g->nvar].w = j->w;
    g->var[g->nvar].h = j->h;
    g->var[g->nvar].px = gen;
    g->nvar++;
    gen_show(l, g->nvar - 1);
}

static void place_result(AiJob *j)
{
    UBYTE *rgb;
    ULONG *gen;
    int iw, ih, n;
    LONG x, y, tx, ty, i, ntiles;
    LONG sum[3] = { 0, 0, 0 }, cnt = 0, off[3] = { 0, 0, 0 };
    Layer *src, *l;
    Tile **m;
    UndoRec *u;
    char msg[200];
    float as, adx, ady;
    ULONG talign;

    doc_process_dirty();
    if (!(rgb = stbi_load_from_memory(j->img, j->imglen, &iw, &ih, &n, 3))) {
        status_message("AI: cannot read the returned image");
        return;
    }
    if (!(gen = malloc(j->w * j->h * 4))) {
        stbi_image_free(rgb);
        status_message("Not enough memory");
        return;
    }
    /* ausrichten, dann auf den Ausschnitt skalieren (bilinear, 16.16; float nur je Zeile/Spalte) */
    talign = now_us();
    align_result(j, rgb, iw, ih, &as, &adx, &ady);
    talign = now_us() - talign;
    {
        LONG *xs = malloc(j->w * sizeof(LONG));
        if (!xs) {
            stbi_image_free(rgb);
            free(gen);
            return;
        }
        for (x = 0; x < j->w; x++) {
            float fx = (((x + 0.5f) - j->w * 0.5f) * as + j->w * 0.5f + adx) * iw / j->w - 0.5f;
            xs[x] = fx < 0 ? 0 : fx > iw - 1 ? (iw - 1) << 16 : (LONG)(fx * 65536.0f);
        }
        for (y = 0; y < j->h; y++) {
            float fy = (((y + 0.5f) - j->h * 0.5f) * as + j->h * 0.5f + ady) * ih / j->h - 0.5f;
            LONG cy = fy < 0 ? 0 : fy > ih - 1 ? (ih - 1) << 16 : (LONG)(fy * 65536.0f);
            LONG y0 = cy >> 16, wy = (cy >> 8) & 255;
            const UBYTE *r0 = rgb + y0 * iw * 3, *r1 = y0 + 1 < ih ? r0 + iw * 3 : r0;
            ULONG *d = gen + y * j->w;
            for (x = 0; x < j->w; x++) {
                LONG x0 = xs[x] >> 16, wx = (xs[x] >> 8) & 255, x1 = x0 + 1 < iw ? x0 + 1 : x0, c;
                ULONG p = 0xFF000000;
                for (c = 0; c < 3; c++) {
                    LONG t = r0[x0 * 3 + c] * (256 - wx) + r0[x1 * 3 + c] * wx;
                    LONG b = r1[x0 * 3 + c] * (256 - wx) + r1[x1 * 3 + c] * wx;
                    p |= (ULONG)(((t * (256 - wy) + b * wy) + 32768) >> 16) << (16 - c * 8);
                }
                d[x] = p;
            }
        }
        free(xs);
    }
    stbi_image_free(rgb);

    /* Farbangleich: mittlere Abweichung ausserhalb der Auswahl, wo das Modell
     * kaum etwas veraendert hat (stark veraenderte Stellen zaehlen nicht) */
    for (y = 0; y < j->h; y += 2)
        for (x = 0; x < j->w; x += 2) {
            ULONG o = doc->flat[(j->y + y) * doc->w + j->x + x], g = gen[y * j->w + x];
            LONG a = o >> 24, d[3], c;
            if (j->sel[y * j->w + x] || a < 255)
                continue;
            for (c = 0; c < 3; c++)
                d[c] = (LONG)((o >> (16 - c * 8)) & 0xFF) - (LONG)((g >> (16 - c * 8)) & 0xFF);
            if (abs(d[0]) + abs(d[1]) + abs(d[2]) > 60)
                continue;
            sum[0] += d[0];
            sum[1] += d[1];
            sum[2] += d[2];
            cnt++;
        }
    if (cnt > 64)
        for (i = 0; i < 3; i++)
            off[i] = sum[i] / cnt;
    if (off[0] || off[1] || off[2])
        for (i = 0; i < j->w * j->h; i++) {
            ULONG p = gen[i], q = 0xFF000000;
            LONG c;
            for (c = 0; c < 3; c++) {
                LONG v = (LONG)((p >> (16 - c * 8)) & 0xFF) + off[c];
                q |= (ULONG)(v < 0 ? 0 : v > 255 ? 255 : v) << (16 - c * 8);
            }
            gen[i] = q;
        }

    if (j->grow)
        grow_mask(j, gen);

    if (j->target) {            /* "Generate again": neue Variante der Ebene */
        place_variant(j, gen);
    } else {
        GenInfo *g;
        /* neue Ebene ueber der aktiven */
        if (doc->nlayers >= MAX_LAYERS || !(l = layer_new(doc, j->name))) {
            free(gen);
            status_message("Too many layers");
            return;
        }
        ntiles = doc->tw * doc->th;
        if (!(m = AllocVec(ntiles * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR))) {
            layer_free(l, doc);
            free(gen);
            return;
        }
        /* weiche Kante nur nach aussen: innerhalb der Auswahl volle Deckung */
        {
            UBYTE *t = malloc(j->w * j->h);
            if (t) {
                memcpy(t, j->sel, j->w * j->h);
                soften(t, j->w, j->h, (j->w > j->h ? j->w : j->h) / 250 + 1);
                for (i = 0; i < j->w * j->h; i++)
                    if (t[i] > j->sel[i])
                        j->sel[i] = t[i];
                free(t);
            }
        }
        for (ty = j->y >> TILE_SHIFT; ty <= (j->y + j->h - 1) >> TILE_SHIFT; ty++)
            for (tx = j->x >> TILE_SHIFT; tx <= (j->x + j->w - 1) >> TILE_SHIFT; tx++) {
                LONG idx = ty * doc->tw + tx, x0 = tx << TILE_SHIFT, y0 = ty << TILE_SHIFT;
                Tile *t = tile_new(), *mt = tile_new_gray(0);
                if (!t || !mt) {
                    if (t) FreeVec(t);
                    if (mt) FreeVec(mt);
                    continue;
                }
                for (y = 0; y < TILE_SIZE; y++) {
                    LONG dy = y0 + y - j->y;
                    if (dy < 0 || dy >= j->h)
                        continue;
                    for (x = 0; x < TILE_SIZE; x++) {
                        LONG dx = x0 + x - j->x;
                        if (dx < 0 || dx >= j->w)
                            continue;
                        t->px[(y << TILE_SHIFT) | x] = gen[dy * j->w + dx];
                        mt->px[(y << TILE_SHIFT) | x] = 0xFF000000 | (j->sel[dy * j->w + dx] * 0x010101);
                    }
                }
                tile_update_flag(doc, t, tx, ty);
                l->tiles[idx] = t;
                m[idx] = mt;
            }
        /* Prompt und erste Variante merken */
        if ((g = AllocVec(sizeof(GenInfo), MEMF_ANY | MEMF_CLEAR))) {
            strcpy(g->prompt, j->prompt);
            g->mode = j->mode;
            g->nvar = 1;
            g->var[0].x = j->x;
            g->var[0].y = j->y;
            g->var[0].w = j->w;
            g->var[0].h = j->h;
            g->var[0].px = gen;
            l->gen = g;
        } else
            free(gen);

        src = doc->layers[doc->active];
        l->depth = src->depth;
        u = undo_begin_struct("Generative Fill");
        l->mask = m;
        l->mask_on = 1;
        mask_compact(doc, l);
        for (x = doc->nlayers; x > doc->active + 1; x--)
            doc->layers[x] = doc->layers[x - 1];
        doc->layers[doc->active + 1] = l;
        doc->nlayers++;
        doc->active++;
        doc->edit_mask = FALSE;
        doc->modified = TRUE;
        doc_mark_all_dirty();
        undo_commit(u);
        if (ai_variants > 1 && l->gen) {
            regen_left = ai_variants - 1;
            regen_layer = l;
        }
    }
    ui_draw_panels();
    view_inval_all();
    {
        ULONG t = now_us();
        LONG mc = j->cost[0] ? (LONG)(atof(j->cost) * 10000.0 + 0.5) : -1;     /* 1/100 Cent */
        if (!j->t_up)
            j->t_up = j->t0;
        if (!j->t_first || j->t_first < j->t_up)
            j->t_first = j->t_up;
        sprintf(msg, "Generative Fill: %ld.%ld s (upload %ld.%ld, server %ld.%ld, download %ld.%ld, insert %ld.%ld)",
                (long)((t - j->t0) / 1000000), (long)((t - j->t0) / 100000 % 10),
                (long)((j->t_up - j->t0) / 1000000), (long)((j->t_up - j->t0) / 100000 % 10),
                (long)((j->t_first - j->t_up) / 1000000), (long)((j->t_first - j->t_up) / 100000 % 10),
                (long)((j->t_done - j->t_first) / 1000000), (long)((j->t_done - j->t_first) / 100000 % 10),
                (long)((t - j->t_done) / 1000000), (long)((t - j->t_done) / 100000 % 10));
        if (mc >= 0) {
            session_mc += mc;
            sprintf(msg + strlen(msg), ", %ld.%02ld cents (session %ld.%02ld)", (long)(mc / 100),
                    (long)(mc % 100), (long)(session_mc / 100), (long)(session_mc % 100));
        }
        (void)talign;
    }
    status_message(msg);
}

/* ---------- Start / Abbruch / Abfrage ---------- */

static void bar_inval(void);
BOOL ai_segment(LONG kind);

BOOL ai_busy(void)
{
    return job != NULL;
}

ULONG ai_sigmask(void)
{
    return ai_sigbit >= 0 ? 1UL << ai_sigbit : 0;
}

void ai_cancel(void)
{
    if (job)
        job->ctl.cancel = 1;
    regen_left = 0;
}

/* Auftrag anlegen: Schluessel lesen, Signal holen */
static AiJob *job_new(void)
{
    AiJob *j;
    char key[160];
    if (job) {
        status_message("An AI request is already running");
        return NULL;
    }
    if (!(j = AllocVec(sizeof(AiJob), MEMF_ANY | MEMF_CLEAR)))
        return NULL;
    if (!read_key(key, sizeof(key))) {
        char msg[160];
        FreeVec(j);
        sprintf(msg, "No %s %s found.\n\nEnter it with Edit > AI API Key...\n(stored in %s)",
                prov_name[ai_provider], ai_provider == PROV_COMFY ? "server address" : "API key",
                key_files[ai_provider][1]);
        request(msg, "OK");
        return NULL;
    }
    if (ai_sigbit < 0 && (ai_sigbit = AllocSignal(-1)) < 0) {
        FreeVec(j);
        return NULL;
    }
    j->prov = ai_provider;
    if (j->prov == PROV_COMFY) {
        comfy_addr(key, j->host, sizeof(j->host), &j->port);
        strcpy(j->hdr, "Content-Type: application/json\r\n");
    } else if (j->prov == PROV_GEMINI)
        sprintf(j->hdr, "x-goog-api-key: %s\r\nContent-Type: application/json\r\n", key);
    else
        sprintf(j->hdr, "Authorization: Bearer %s\r\nContent-Type: application/json\r\n"
                        "X-Title: AmiPixelShop\r\n", key);
    DeleteFile((STRPTR)AI_LOG);
    ai_log("start:", prov_name[j->prov], -1);
    j->doc = doc;
    j->parent = FindTask(NULL);
    j->sig = ai_sigbit;
    j->ctl.timeout = 180;
    return j;
}

/* Hintergrundprozess starten; bei Fehler wird j freigegeben */
static BOOL job_launch(AiJob *j)
{
    ai_log("crop", NULL, j->w);
    ai_log("  x", NULL, j->h);
    if (j->up)
        ai_log("upload bytes", NULL, j->uplen);
    j->t0 = now_us();
    job = j;
    if (!CreateNewProcTags(NP_Entry, (ULONG)ai_worker, NP_Name, (ULONG)"AmiPixelShop AI",
                           NP_StackSize, 65536, NP_Priority, -1, NP_Output, 0, NP_Input, 0,
                           NP_CloseOutput, FALSE, NP_CloseInput, FALSE, TAG_DONE)) {
        job = NULL;
        job_free(j);
        status_message("Cannot start the AI process");
        return FALSE;
    }
    bar_inval();
    return TRUE;
}

/* prompt NULL/leer: Inhalt entfernen bzw. leere Flaeche passend fortsetzen.
 * Der zu fuellende Bereich (Auswahl und Durchsichtiges) wird im gesendeten Bild
 * grau gemacht, ohne eigenes Maskenbild: mit Maske uebernahmen die Modelle
 * teils die weisse Maske, liessen Grau stehen oder zoomten ins Original. */
/* Leere Pixel (empty[] != 0) von aussen nach innen mit dem Mittel der schon
 * bekannten Nachbarn fuellen (Ring fuer Ring, Breitensuche), ergibt verschmierte
 * Fortsetzung der Randfarben */
static void smear_fill(UBYTE *rgb, UBYTE *empty, LONG w, LONG h)
{
    LONG n = w * h, *cur, *nxt, nc = 0, nn, i;
    static const BYTE nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    if (!(cur = malloc(n * sizeof(LONG))) || !(nxt = malloc(n * sizeof(LONG)))) {
        free(cur);
        return;
    }
    /* erster Ring: leere Pixel neben bekannten */
    for (i = 0; i < n; i++)
        if (empty[i]) {
            LONG x = i % w, y = i / w, k;
            for (k = 0; k < 4; k++) {
                LONG nx = x + nb[k][0], ny = y + nb[k][1];
                if (nx >= 0 && ny >= 0 && nx < w && ny < h && !empty[ny * w + nx]) {
                    cur[nc++] = i;
                    empty[i] = 2;       /* in der Warteschlange */
                    break;
                }
            }
        }
    while (nc) {
        for (i = 0; i < nc; i++) {      /* Ring aus bekannten Nachbarn berechnen */
            LONG c = cur[i], x = c % w, y = c / w, k, s[3] = { 0, 0, 0 }, cnt = 0;
            for (k = 0; k < 4; k++) {
                LONG nx = x + nb[k][0], ny = y + nb[k][1], q;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h || empty[q = ny * w + nx])
                    continue;
                s[0] += rgb[q * 3];
                s[1] += rgb[q * 3 + 1];
                s[2] += rgb[q * 3 + 2];
                cnt++;
            }
            if (cnt) {
                rgb[c * 3] = s[0] / cnt;
                rgb[c * 3 + 1] = s[1] / cnt;
                rgb[c * 3 + 2] = s[2] / cnt;
            }
        }
        nn = 0;
        for (i = 0; i < nc; i++)        /* Ring wird bekannt, naechsten sammeln */
            empty[cur[i]] = 0;
        for (i = 0; i < nc; i++) {
            LONG c = cur[i], x = c % w, y = c / w, k;
            for (k = 0; k < 4; k++) {
                LONG nx = x + nb[k][0], ny = y + nb[k][1], q;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h || empty[q = ny * w + nx] != 1)
                    continue;
                empty[q] = 2;
                nxt[nn++] = q;
            }
        }
        {
            LONG *t = cur;
            cur = nxt;
            nxt = t;
        }
        nc = nn;
    }
    free(cur);
    free(nxt);
}

/* ComfyUI: Ausschnitt als PNG, zu fuellender Bereich (Auswahl und Leeres) durchsichtig;
 * der Workflow malt nur dort. Gibt rgb/rgb_empty frei. */
/* Anteil n/d als "0.123" */
static char *frac3(char *d, LONG n, LONG den)
{
    LONG k = (n * 1000 + den / 2) / den;
    sprintf(d, "%ld.%03ld", (long)(k / 1000), (long)(k % 1000));
    return d;
}

static BOOL comfy_fill(AiJob *j, UBYTE *rgb, UBYTE *rgb_empty, LONG sw, LONG sh, BOOL remove, const char *prompt)
{
    static const char where[] = "the region %s (bounding box [y_min, x_min, y_max, x_max], 0 to 1 from the top left)";
    UBYTE *rgba = malloc(sw * sh * 4);
    LONG x, y, i, bx0 = sw, by0 = sh, bx1 = 0, by1 = 0;
    char box[80], f[4][16], reg[192], task[760];
    BOOL ok;

    if (rgba)
        for (y = 0; y < sh; y++)
            for (x = 0; x < sw; x++) {
                LONG ox = x * j->w / sw, oy = y * j->h / sh;
                BOOL empty = (doc->flat[(j->y + oy) * doc->w + j->x + ox] >> 24) < 128;
                rgba[(y * sw + x) * 4 + 3] = empty || j->sel[oy * j->w + ox] >= 128 ? 0 : 255;
                if (rgb_empty)
                    rgb_empty[y * sw + x] = empty;
                if (!rgba[(y * sw + x) * 4 + 3]) {
                    if (x < bx0) bx0 = x;
                    if (x >= bx1) bx1 = x + 1;
                    if (y < by0) by0 = y;
                    if (y >= by1) by1 = y + 1;
                }
            }
    if (bx1 <= bx0 || by1 <= by0) {
        bx0 = by0 = 0;
        bx1 = sw;
        by1 = sh;
    }
    sprintf(box, "[%s, %s, %s, %s]", frac3(f[0], by0, sh), frac3(f[1], bx0, sw), frac3(f[2], by1, sh),
            frac3(f[3], bx1, sw));
    sprintf(reg, where, box);
    if (rgb_empty)                      /* Leeres mit Randfarben vorfuellen (fuer Workflows mit denoise < 1) */
        smear_fill(rgb, rgb_empty, sw, sh);
    if (rgba)
        for (i = 0; i < sw * sh; i++) {
            rgba[i * 4] = rgb[i * 3];
            rgba[i * 4 + 1] = rgb[i * 3 + 1];
            rgba[i * 4 + 2] = rgb[i * 3 + 2];
        }
    free(rgb);
    free(rgb_empty);
    if (!remove)
        sprintf(j->name, "Gen: %.26s", prompt);
    else
        strcpy(j->name, j->grow ? "Gen: Remove" : "Gen: Extend");
    j->mode = !remove ? GEN_FILL : j->grow ? GEN_REMOVE : GEN_EXTEND;
    /* %task% fuer Edit-Modelle (FLUX 3 usw.): sie sehen das ganze Bild und brauchen den Bereich */
    if (!remove)
        sprintf(task, "Edit this photo only inside %s: there, paint %.200s. Blend it naturally into the scene "
                      "with matching perspective, lighting and colors, and keep everything outside that region "
                      "exactly unchanged.", reg, prompt);
    else if (j->grow)
        sprintf(task, "Remove everything inside %s, including shadows and reflections, and fill the area so it "
                      "seamlessly continues the surrounding scene. Do not add new objects. Keep everything "
                      "outside that region exactly unchanged.", reg);
    else
        sprintf(task, "The blurry smeared areas in %s are placeholders for missing parts of the photo: repaint "
                      "them as a sharp, seamless continuation of the scene with the same perspective, lighting, "
                      "colors and grain. Keep the sharp original content exactly unchanged.", reg);
    /* %prompt% fuer Inpaint-Modelle (Flux Fill, SD): sie malen nur in der Maske */
    if (!remove)
        ;
    else if (j->grow)
        prompt = "empty background, seamless continuation of the surroundings, no objects";
    else
        prompt = "seamless continuation of the scene";
    j->grow = FALSE;                    /* ComfyUI aendert nur die Maske, nichts nachzuziehen */
    ai_log("region", box, -1);
    ok = rgba && comfy_body(j, remove && have_workflow("comfy_remove.json") ? "comfy_remove.json" :
                            "comfy_fill.json", prompt, task, box) && comfy_upload(j, rgba, sw, sh, 4);
    if (!rgba)
        status_message("Not enough memory");
    free(rgba);
    if (!ok) {
        job_free(j);
        return FALSE;
    }
    return job_launch(j);
}

static BOOL start_fill(const char *prompt, Layer *target);

BOOL ai_start(const char *prompt)
{
    return start_fill(prompt, NULL);
}

static BOOL start_fill(const char *prompt, Layer *target)
{
    static const char mark[] =
        "Image editing task. In this photo, one or more objects are marked with a translucent pink brush "
        "stroke. Remove every marked object completely, including its parts that the pink stroke does not "
        "cover (edges, shadow, reflection), and fill the area so it seamlessly continues the surrounding "
        "scene. Remove the pink marking as well. Do not add new objects. Keep everything else identical and "
        "at exactly the same position and scale (do not zoom, crop, shift or reframe). Return the full photo "
        "with the same size and aspect ratio.";
    static const char smeared[] =
        "Image completion task. Parts of this photo are missing: they were roughly pre-filled with blurry "
        "smeared colors as a placeholder. Repaint those blurry smeared areas with sharp, realistic detail so "
        "the result is one complete, seamless photograph: continue the landscape, sky, terrain and every "
        "structure with the same perspective, lighting, colors, sharpness and grain. Flat mid-gray areas, if "
        "any, are missing too. Keep the sharp original photo content unchanged and at exactly the same "
        "position and scale (do not zoom, crop, shift or reframe). Return the full image with the same size "
        "and aspect ratio. ";
    static const char task[] =
        "Image completion task. Parts of this photo are missing: they are filled with perfectly flat, "
        "uniform mid-gray. Paint the missing parts so the result is one complete, seamless photograph: "
        "continue everything that touches the gray areas with the same perspective, lighting, colors, "
        "sharpness and grain. No gray may remain. Keep all other content unchanged and at exactly the same "
        "position and scale (do not zoom, crop, shift or reframe). Return the full image with the same size "
        "and aspect ratio. ";
    AiJob *j;
    LONG bx, by, bw, bh, sw, sh, lng, x, y, ri, plen, l64, jl, nfill = 0, nempty = 0;
    UBYTE *rgb = NULL;
    MemBuf jpg = { NULL, 0, 0 };
    char *e64 = NULL, *esc = NULL, *p;
    BOOL remove = !prompt || !prompt[0], smear;
    UBYTE *rgb_empty = NULL;

    if (!doc->hassel && !job) {
        status_message("Make a selection first");
        return FALSE;
    }
    if (!(j = job_new()))
        return FALSE;

    doc_process_dirty();
    ri = crop_box(&bx, &by, &bw, &bh);
    lng = bw > bh ? bw : bh;
    x = lng < AI_MINSIDE ? AI_MINSIDE : lng > AI_MAXSIDE ? AI_MAXSIDE : lng;
    sw = (bw * x + lng / 2) / lng;
    sh = (bh * x + lng / 2) / lng;
    if (j->prov == PROV_COMFY) {        /* VAE: Vielfache von 16, sonst schneidet ComfyUI ab */
        sw = (sw + 8) & ~15;
        sh = (sh + 8) & ~15;
    }

    j->kind = AI_FILL;
    j->target = target;
    if (prompt) {
        strncpy(j->prompt, prompt, sizeof(j->prompt) - 1);
        j->prompt[sizeof(j->prompt) - 1] = 0;
    }
    j->x = bx;
    j->y = by;
    j->w = bw;
    j->h = bh;

    /* Auswahl im Ausschnitt merken (darf sich waehrenddessen aendern) */
    if (!(j->sel = malloc(bw * bh)) || !(rgb = malloc(sw * sh * 3)))
        goto fail;
    for (y = 0; y < bh; y++)
        memcpy(j->sel + y * bw, doc->sel + (by + y) * doc->w + bx, bw);
    sample_rgb(rgb, sw, sh, bx, by, bw, bh);
    for (y = 0; y < sh; y++)
        for (x = 0; x < sw; x++) {
            LONG ox = x * bw / sw, oy = y * bh / sh;
            BOOL empty = (doc->flat[(by + oy) * doc->w + bx + ox] >> 24) < 128;
            if (j->sel[oy * bw + ox] >= 128 || empty) {
                nfill++;
                nempty += empty;
            }
        }
    /* Entfernen: Objekt rosa markieren (das Modell soll es sehen und ganz entfernen).
     * Fuellen mit Prompt: Bereich grau, das Modell malt ihn neu.
     * Leere (durchsichtige) Flaeche: mit verschmierten Randfarben vorfuellen - bei
     * Grau liessen die Modelle schmale Streifen als "Balken" stehen. */
    j->grow = remove && nempty * 4 <= nfill;
    smear = nempty > 0 && (rgb_empty = malloc(sw * sh)) != NULL;
    if (j->prov == PROV_COMFY)
        return comfy_fill(j, rgb, rgb_empty, sw, sh, remove, prompt);
    for (y = 0; y < sh; y++)
        for (x = 0; x < sw; x++) {
            LONG ox = x * bw / sw, oy = y * bh / sh;
            BOOL empty = (doc->flat[(by + oy) * doc->w + bx + ox] >> 24) < 128;
            UBYTE *q = rgb + (y * sw + x) * 3;
            if (smear)
                rgb_empty[y * sw + x] = empty;
            if (empty || (!j->grow && !(smear && remove) && j->sel[oy * bw + ox] >= 128)) {
                q[0] = q[1] = q[2] = 128;
            } else if (j->grow && j->sel[oy * bw + ox] >= 128) {
                q[0] = (q[0] + 2 * 255) / 3;
                q[1] = (q[1] + 2 * 64) / 3;
                q[2] = (q[2] + 2 * 160) / 3;
            }
        }
    if (smear) {
        smear_fill(rgb, rgb_empty, sw, sh);
        free(rgb_empty);
        rgb_empty = NULL;
    }
    if (!remove)
        sprintf(j->name, "Gen: %.26s", prompt);
    else
        strcpy(j->name, j->grow ? "Gen: Remove" : "Gen: Extend");
    j->mode = !remove ? GEN_FILL : j->grow ? GEN_REMOVE : GEN_EXTEND;

    if (!stbi_write_jpg_to_func(mem_write, &jpg, sw, sh, 3, rgb, 92) || !jpg.p)
        goto fail;
    free(rgb);
    rgb = NULL;
    ai_log("JPEG bytes", NULL, jpg.n);
    if (!(e64 = b64_encode(jpg.p, jpg.n, &l64)))
        goto fail;
    free(jpg.p);
    jpg.p = NULL;

    plen = remove ? 0 : strlen(prompt);
    if (!(esc = malloc(plen * 2 + 8)))
        goto fail;
    json_esc(remove ? "" : prompt, esc, plen * 2 + 8);

    jl = l64 + strlen(esc) + sizeof(task) + sizeof(mark) + sizeof(smeared) + 1024;
    if (!(j->body = net_alloc(jl)))
        goto fail;
    p = body_start(j, j->body, !remove, ri);
    strcpy(p, j->grow ? mark : smear ? smeared : task);
    p += strlen(p);
    if (j->grow)
        ;
    else if (remove)
        strcpy(p, "Do not add new objects.");
    else if (smear)
        sprintf(p, "In the new areas, include: %s. Blend it naturally into the scene.", esc);
    else
        sprintf(p, "In the gray areas, paint: %s. Blend it naturally into the scene.", esc);
    p += strlen(p);
    p = body_image(j, p);
    memcpy(p, e64, l64);
    p += l64;
    strcpy(p, BODY_END);
    p += strlen(p);
    j->blen = p - j->body;
    net_freemem(e64);
    free(esc);
    return job_launch(j);

fail:
    free(rgb);
    free(rgb_empty);
    free(jpg.p);
    net_freemem(e64);
    free(esc);
    job_free(j);
    status_message("Not enough memory");
    return FALSE;
}

/* Generate again: neue Variante fuer die aktive generative Ebene. Gesendet wird
 * das Bild ohne diese Ebene; Bereich = Ebenenmaske, soweit die Ebene Pixel hat. */
BOOL ai_regenerate(void)
{
    Layer *l = doc->layers[doc->active];
    GenInfo *g = l->gen;
    UBYTE *m, *osel = doc->sel;
    BOOL ohas = doc->hassel, ok;
    LONG o0 = doc->selx0, o1 = doc->sely0, o2 = doc->selx1, o3 = doc->sely1, x, y;
    LONG x0 = doc->w, y0 = doc->h, x1 = 0, y1 = 0;
    if (!g || !l->mask) {
        status_message("Not a generative layer");
        return FALSE;
    }
    if (job) {
        status_message("An AI request is already running");
        return FALSE;
    }
    if (!(m = AllocVec(doc->w * doc->h, MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    for (y = 0; y < doc->h; y++)
        for (x = 0; x < doc->w; x++) {
            LONG idx = (y >> TILE_SHIFT) * doc->tw + (x >> TILE_SHIFT), k = ((y & TILE_MASK) << TILE_SHIFT) | (x & TILE_MASK);
            Tile *t = l->tiles[idx], *mt = l->mask[idx];
            UBYTE v;
            if (!t || !(t->px[k] >> 24))
                continue;
            v = mt ? (UBYTE)mt->px[k] : 255;
            m[y * doc->w + x] = v;
            if (v >= 128) {
                if (x < x0) x0 = x;
                if (x >= x1) x1 = x + 1;
                if (y < y0) y0 = y;
                if (y >= y1) y1 = y + 1;
            }
        }
    if (x1 <= x0) {
        FreeVec(m);
        status_message("The generative layer is empty");
        return FALSE;
    }
    l->visible = 0;                     /* Bild ohne diese Ebene senden */
    doc_mark_all_dirty();
    doc_process_dirty();
    doc->sel = m;
    doc->hassel = TRUE;
    doc->selx0 = x0;
    doc->sely0 = y0;
    doc->selx1 = x1;
    doc->sely1 = y1;
    ok = start_fill(g->mode == GEN_FILL ? g->prompt : NULL, l);
    doc->sel = osel;
    doc->hassel = ohas;
    doc->selx0 = o0;
    doc->sely0 = o1;
    doc->selx1 = o2;
    doc->sely1 = o3;
    l->visible = 1;
    doc_mark_all_dirty();
    FreeVec(m);
    return ok;
}

/* Entfernen mit eigener Maske (KI-Reparaturpinsel): etwas vergroessern, damit
 * Raender und Schatten mitgehen, und als voruebergehende Auswahl verwenden */
BOOL ai_start_mask(UBYTE *m)
{
    UBYTE *osel = doc->sel, *t;
    BOOL ohas = doc->hassel, ok;
    LONG o0 = doc->selx0, o1 = doc->sely0, o2 = doc->selx1, o3 = doc->sely1;
    LONG x0 = doc->w, y0 = doc->h, x1 = 0, y1 = 0, x, y, pad = 8, w, h;
    for (y = 0; y < doc->h; y++)
        for (x = 0; x < doc->w; x++)
            if (m[y * doc->w + x]) {
                if (x < x0) x0 = x;
                if (x >= x1) x1 = x + 1;
                if (y < y0) y0 = y;
                if (y >= y1) y1 = y + 1;
            }
    if (x1 <= x0)
        return FALSE;
    x0 = x0 - pad < 0 ? 0 : x0 - pad;
    y0 = y0 - pad < 0 ? 0 : y0 - pad;
    x1 = x1 + pad > doc->w ? doc->w : x1 + pad;
    y1 = y1 + pad > doc->h ? doc->h : y1 + pad;
    w = x1 - x0;
    h = y1 - y0;
    if ((t = malloc(w * h))) {
        for (y = 0; y < h; y++)
            memcpy(t + y * w, m + (y0 + y) * doc->w + x0, w);
        soften(t, w, h, 3);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                m[(y0 + y) * doc->w + x0 + x] = t[y * w + x] ? 255 : 0;
        free(t);
    }
    doc->sel = m;
    doc->hassel = TRUE;
    doc->selx0 = x0;
    doc->sely0 = y0;
    doc->selx1 = x1;
    doc->sely1 = y1;
    ok = ai_start(NULL);
    doc->sel = osel;
    doc->hassel = ohas;
    doc->selx0 = o0;
    doc->sely0 = o1;
    doc->selx1 = o2;
    doc->sely1 = o3;
    return ok;
}

/* ---------- Motiv: Auswahl bzw. Hintergrund entfernen ---------- */

/* Kastenmittel (Radius r) von src nach dst; tmp: w*h LONG */
static void box_mean(LONG *src, LONG *dst, LONG w, LONG h, LONG r, LONG *tmp)
{
    LONG x, y;
    for (y = 0; y < h; y++) {           /* waagerecht: Summen */
        LONG *s = src + y * w;
        LONG *t = tmp + y * w, sum = 0;
        for (x = 0; x < r && x < w; x++)
            sum += s[x];
        for (x = 0; x < w; x++) {
            if (x + r < w) sum += s[x + r];
            if (x - r - 1 >= 0) sum -= s[x - r - 1];
            t[x] = sum;
        }
    }
    for (x = 0; x < w; x++) {           /* senkrecht, durch Anzahl teilen */
        LONG sum = 0, cx = (x + r < w ? x + r : w - 1) - (x - r > 0 ? x - r : 0) + 1;
        for (y = 0; y < r && y < h; y++)
            sum += tmp[y * w + x];
        for (y = 0; y < h; y++) {
            LONG cy;
            if (y + r < h) sum += tmp[(y + r) * w + x];
            if (y - r - 1 >= 0) sum -= tmp[(y - r - 1) * w + x];
            cy = (y + r < h ? y + r : h - 1) - (y - r > 0 ? y - r : 0) + 1;
            dst[y * w + x] = sum / (cx * cy);
        }
    }
}

/* Kante der hochskalierten Maske an die Bildkanten anpassen (Guided Filter,
 * He et al.; Fuehrung = Helligkeit des Komposits). Ganzzahlig. */
static void refine_mask(UBYTE *m, LONG w, LONG h)
{
    LONG n = w * h, i, r = w / 320 + 3, eps = 400;
    LONG *I = NULL, *P = NULL, *a = NULL, *b = NULL, *t = NULL;
    if (n > 12000000)
        return;
    I = malloc(n * 4);
    P = malloc(n * 4);
    a = malloc(n * 4);
    b = malloc(n * 4);
    t = malloc(n * 4);
    if (!I || !P || !a || !b || !t)
        goto out;
    for (i = 0; i < n; i++) {
        ULONG q = doc->flat[i];
        LONG l = (((q >> 16) & 0xFF) * 77 + ((q >> 8) & 0xFF) * 151 + (q & 0xFF) * 28) >> 8;
        I[i] = l + ((255 - (q >> 24)) >> 1);        /* Durchsichtiges: grau */
        if (I[i] > 255) I[i] = 255;
    }
    for (i = 0; i < n; i++) {
        a[i] = I[i] * I[i];
        P[i] = I[i] * m[i];
    }
    box_mean(a, a, w, h, r, t);         /* mean(I*I) */
    box_mean(P, P, w, h, r, t);         /* mean(I*p) */
    for (i = 0; i < n; i++)
        b[i] = m[i];
    box_mean(b, b, w, h, r, t);         /* mean(p) */
    for (i = 0; i < n; i++)             /* Kantenband merken: mean(p) weder 0 noch 255 */
        if (b[i] < 4 || b[i] > 251)
            m[i] = m[i] >= 128 ? 255 : 0;
        else
            m[i] = m[i] >= 128 ? 254 : 1;
    box_mean(I, I, w, h, r, t);         /* mean(I); I selbst wird unten neu berechnet */
    for (i = 0; i < n; i++) {
        LONG mi = I[i], var = a[i] - mi * mi, cov = P[i] - mi * b[i], av;
        if (var < 0) var = 0;
        av = (cov * 256) / (var + eps);             /* a * 256 */
        if (av > 4096) av = 4096;                   /* gegen Ueberlauf in box_mean */
        if (av < -4096) av = -4096;
        a[i] = av;
        b[i] = b[i] * 256 - av * mi;                /* b * 256 */
    }
    box_mean(a, a, w, h, r, t);
    box_mean(b, b, w, h, r, t);
    for (i = 0; i < n; i++) {
        ULONG q = doc->flat[i];
        LONG l = (((q >> 16) & 0xFF) * 77 + ((q >> 8) & 0xFF) * 151 + (q & 0xFF) * 28) >> 8, v;
        l += (255 - (q >> 24)) >> 1;
        if (l > 255) l = 255;
        if (m[i] == 0 || m[i] == 255)  /* ausserhalb des Kantenbands: Maske des Modells */
            continue;
        v = (a[i] * l + b[i] + 128) >> 8;
        v = (v - 128) * 3 / 2 + 128;    /* etwas haerter: weniger Schleier */
        m[i] = v < 0 ? 0 : v > 255 ? 255 : v;
    }
out:
    free(I);
    free(P);
    free(a);
    free(b);
    free(t);
}

static void place_segment(AiJob *j)
{
    UBYTE *rgb, *m;
    int iw, ih, n;
    LONG x, y, *xs;
    ULONG t;
    char msg[120];

    doc_process_dirty();
    if (!(rgb = stbi_load_from_memory(j->img, j->imglen, &iw, &ih, &n, 3))) {
        status_message("AI: cannot read the returned image");
        return;
    }
    if (!(m = malloc(doc->w * doc->h)) || !(xs = malloc(doc->w * sizeof(LONG)))) {
        free(m);
        stbi_image_free(rgb);
        status_message("Not enough memory");
        return;
    }
    /* Dokument -> gesendeter Rahmen (sw x sh, Bild bei ix0/iy0) -> Antwortbild; bilinear */
    for (x = 0; x < doc->w; x++) {
        LONG f = (LONG)((((x + 0.5f) * j->iw / doc->w + j->ix0) * iw / j->sw - 0.5f) * 65536.0f);
        xs[x] = f < 0 ? 0 : f > (iw - 1) << 16 ? (iw - 1) << 16 : f;
    }
    for (y = 0; y < doc->h; y++) {
        LONG f = (LONG)((((y + 0.5f) * j->ih / doc->h + j->iy0) * ih / j->sh - 0.5f) * 65536.0f);
        LONG cy = f < 0 ? 0 : f > (ih - 1) << 16 ? (ih - 1) << 16 : f, y0 = cy >> 16, wy = (cy >> 8) & 255;
        const UBYTE *r0 = rgb + y0 * iw * 3, *r1 = y0 + 1 < ih ? r0 + iw * 3 : r0;
        UBYTE *d = m + y * doc->w;
        for (x = 0; x < doc->w; x++) {
            LONG x0 = xs[x] >> 16, wx = (xs[x] >> 8) & 255, x1 = x0 + 1 < iw ? x0 + 1 : x0;
            LONG top = (r0[x0 * 3] + r0[x0 * 3 + 1] + r0[x0 * 3 + 2]) * (256 - wx) +
                       (r0[x1 * 3] + r0[x1 * 3 + 1] + r0[x1 * 3 + 2]) * wx;
            LONG bot = (r1[x0 * 3] + r1[x0 * 3 + 1] + r1[x0 * 3 + 2]) * (256 - wx) +
                       (r1[x1 * 3] + r1[x1 * 3 + 1] + r1[x1 * 3 + 2]) * wx;
            d[x] = (top * (256 - wy) + bot * wy) / (3 << 16);
        }
    }
    free(xs);
    stbi_image_free(rgb);
    t = now_us();
    refine_mask(m, doc->w, doc->h);
    t = now_us() - t;

    if (j->kind == AI_SUBJECT) {
        CopyMem(m, doc->sel, doc->w * doc->h);
        sel_update_bounds();
        view_ants_rebuild();
        view_inval_all();
    } else {
        /* Ebene noch da? */
        Layer *l = NULL;
        LONG i, k, ntiles = doc->tw * doc->th;
        Tile **mt;
        UndoRec *u;
        for (i = 0; i < doc->nlayers; i++)
            if (doc->layers[i] == j->layer)
                l = j->layer;
        if (!l || l->mask || !(mt = AllocVec(ntiles * sizeof(Tile *), MEMF_ANY | MEMF_CLEAR))) {
            free(m);
            status_message(!l ? "Remove Background: the layer is gone" :
                           l->mask ? "Remove Background: the layer already has a mask" : "Not enough memory");
            return;
        }
        for (i = 0; i < ntiles; i++) {
            LONG x0 = (i % doc->tw) << TILE_SHIFT, y0 = (i / doc->tw) << TILE_SHIFT;
            if (!(mt[i] = tile_new_gray(0)))
                continue;
            for (y = 0; y < TILE_SIZE && y0 + y < doc->h; y++)
                for (k = 0; k < TILE_SIZE && x0 + k < doc->w; k++)
                    mt[i]->px[(y << TILE_SHIFT) | k] = 0xFF000000 | (m[(y0 + y) * doc->w + x0 + k] * 0x010101);
        }
        u = undo_begin_struct("Remove Background");
        if (l->movf) {
            FreeVec(l->movf);
            l->movf = NULL;
        }
        l->mask = mt;
        l->mask_on = 1;
        mask_compact(doc, l);
        doc->edit_mask = FALSE;
        doc->modified = TRUE;
        doc_mark_all_dirty();
        undo_commit(u);
        ui_draw_panels();
        view_inval_all();
    }
    free(m);
    sprintf(msg, "%s: %ld.%ld s (server %ld.%ld, edges %ld.%ld)", j->kind == AI_SUBJECT ? "Select Subject" : "Remove Background",
            (long)((now_us() - j->t0) / 1000000), (long)((now_us() - j->t0) / 100000 % 10),
            (long)((j->t_done - j->t0) / 1000000), (long)((j->t_done - j->t0) / 100000 % 10),
            (long)(t / 1000000), (long)(t / 100000 % 10));
    status_message(msg);
}

/* Select > Subject (AI_SUBJECT) bzw. Layer > Remove Background (AI_RMBG):
 * das Modell liefert eine Schwarz-Weiss-Maske des Hauptmotivs */
BOOL ai_segment(LONG kind)
{
    static const char task[] =
        "Segmentation task. Output a black-and-white mask of this photo with exactly the same size and "
        "framing: the main subject (including fine details like hair and fur) pure white, everything else "
        "pure black. Flat gray bars at the edges are padding and belong to the black background. No other "
        "content, no gray except at soft edges.";
    AiJob *j;
    Layer *l = doc->layers[doc->active];
    LONG ri = 0, i, pw, ph, lng, sw, sh, y, l64, best;
    float a = (float)doc->w / doc->h, bd = 1e9f;
    UBYTE *rgb = NULL, *in = NULL;
    MemBuf jpg = { NULL, 0, 0 };
    char *e64 = NULL, *p;

    if (kind == AI_RMBG && (l->kind != LK_PIXEL || l->text)) {
        status_message("Remove Background works on pixel layers");
        return FALSE;
    }
    if (kind == AI_RMBG && l->mask) {
        status_message("The layer already has a mask - delete it first");
        return FALSE;
    }
    if (!(j = job_new()))
        return FALSE;
    j->kind = kind;
    j->layer = l;
    j->x = 0;
    j->y = 0;
    j->w = doc->w;
    j->h = doc->h;
    doc_process_dirty();

    /* naechstes Seitenverhaeltnis; das Bild wird mit Graubalken darauf erweitert */
    for (i = 0; i < (LONG)(sizeof(ratios) / 2); i++) {
        float r = (float)ratios[i][0] / ratios[i][1], d = r > a ? r / a : a / r;
        if (d < bd) {
            bd = d;
            ri = i;
        }
    }
    {
        float r = (float)ratios[ri][0] / ratios[ri][1];
        pw = r > a ? (LONG)(doc->h * r + 0.5f) : doc->w;
        ph = r > a ? doc->h : (LONG)(doc->w / r + 0.5f);
    }
    lng = pw > ph ? pw : ph;
    best = lng > AI_MAXSIDE ? AI_MAXSIDE : lng;
    sw = (pw * best + lng / 2) / lng;
    sh = (ph * best + lng / 2) / lng;
    j->sw = sw;
    j->sh = sh;
    j->iw = (doc->w * best + lng / 2) / lng;
    j->ih = (doc->h * best + lng / 2) / lng;
    if (j->iw > sw) j->iw = sw;
    if (j->ih > sh) j->ih = sh;
    j->ix0 = (sw - j->iw) / 2;
    j->iy0 = (sh - j->ih) / 2;

    if (!(rgb = malloc(sw * sh * 3)) || !(in = malloc(j->iw * j->ih * 3)))
        goto fail;
    memset(rgb, 128, sw * sh * 3);
    sample_rgb(in, j->iw, j->ih, 0, 0, doc->w, doc->h);
    for (y = 0; y < j->ih; y++)
        memcpy(rgb + ((j->iy0 + y) * sw + j->ix0) * 3, in + y * j->iw * 3, j->iw * 3);
    free(in);
    in = NULL;
    if (j->prov == PROV_COMFY) {        /* Workflow liefert die Maske als Schwarz-Weiss-Bild */
        BOOL ok = comfy_body(j, "comfy_segment.json", "main subject", NULL, NULL) && comfy_upload(j, rgb, sw, sh, 3);
        free(rgb);
        if (!ok) {
            job_free(j);
            return FALSE;
        }
        return job_launch(j);
    }
    if (!stbi_write_jpg_to_func(mem_write, &jpg, sw, sh, 3, rgb, 92) || !jpg.p)
        goto fail;
    free(rgb);
    rgb = NULL;
    ai_log("JPEG bytes", NULL, jpg.n);
    if (!(e64 = b64_encode(jpg.p, jpg.n, &l64)))
        goto fail;
    free(jpg.p);
    jpg.p = NULL;
    if (!(j->body = net_alloc(l64 + sizeof(task) + 1024)))
        goto fail;
    p = body_start(j, j->body, FALSE, ri);
    strcpy(p, task);
    p = body_image(j, p + strlen(p));
    memcpy(p, e64, l64);
    p += l64;
    strcpy(p, BODY_END);
    p += strlen(p);
    j->blen = p - j->body;
    net_freemem(e64);
    return job_launch(j);

fail:
    free(rgb);
    free(in);
    free(jpg.p);
    net_freemem(e64);
    job_free(j);
    status_message("Not enough memory");
    return FALSE;
}

/* ---------- Einstellungen ---------- */

/* Edit > AI Settings... */
void ai_settings(void)
{
    LONG pv = ai_provider, fm = ai_fill_model, rm = ai_fast_model, nv = ai_variants - 1;
    DlgSlider ds[4] = {
        { "Provider:", &pv, DLG_CHOICE, DLG_CHOICE, "OpenRouter|Google Gemini|ComfyUI (LAN)", 1 },
        { "Fill model:", &fm, DLG_CHOICE, DLG_CHOICE, "Nano Banana 2.1|Nano Banana Pro|Flash Lite (fast)", 1 },
        { "Remove/Extend:", &rm, DLG_CHOICE, DLG_CHOICE, "Flash Lite (fast)|Nano Banana 2.1", 1 },
        { "Variants:", &nv, DLG_CHOICE, DLG_CHOICE, "1|2|3", 1 },
    };
    if (!dialog_run("AI Settings", ds, 4, NULL))
        return;
    ai_provider = pv;
    ai_fill_model = fm;
    ai_fast_model = rm;
    ai_variants = nv + 1;
    ui_prefs_save();
    if (session_mc) {
        char msg[80];
        sprintf(msg, "AI costs this session: %ld.%02ld cents", (long)(session_mc / 100), (long)(session_mc % 100));
        status_message(msg);
    }
}

/* Edit > AI API Key...: Schluessel des gewaehlten Anbieters eingeben, speichern und pruefen */
void ai_key_dialog(void)
{
    const char **files = key_files[ai_provider], *pn = prov_name[ai_provider];
    BOOL gem = ai_provider == PROV_GEMINI, comfy = ai_provider == PROV_COMFY;
    char key[160], hdr[220], msg[320], val[40], host[64];
    LONG port;
    LONG bw = 560, bh = 118, bx = cvx + (cvw - bw) / 2, by = cvy + (cvh - bh) / 2, k;
    NetResp r;
    NetCtl ctl;
    const char *v;
    LONG vl;

    if (job) {
        status_message("An AI request is running");
        return;
    }
    read_key(key, sizeof(key));
    fill(bx, by, bw, bh, COL_PANEL);
    frame(bx, by, bw, bh, COL_LINE);
    fill(bx, by, bw, 26, COL_PANELHD);
    sprintf(msg, comfy ? "%s Server" : "%s API Key", pn);
    text_bold(bx + 12, by + 4, msg, COL_TEXT);
    text(bx + 12, by + 36, comfy ? "Address of the ComfyUI PC (e.g. 192.168.1.20:8188):" :
                           gem ? "Key (aistudio.google.com/apikey), Return to save:" :
                                 "Key (openrouter.ai/keys), Return to save:", COL_DIM);
    sprintf(msg, "Stored in %s", files[1]);
    text(bx + 12, by + 92, msg, COL_DIM);
    blit(bx, by, bw, bh);
    if (!ui_edit_text(bx + 12, by + 58, bw - 24, key, sizeof(key)) || !key[0]) {
        view_inval_all();
        view_flush();
        return;
    }
    view_inval_all();
    view_flush();
    UnLock(CreateDir("ENV:AmiPixelShop"));
    UnLock(CreateDir("ENVARC:AmiPixelShop"));
    for (k = 0; k < 2; k++) {
        BPTR fh = Open((STRPTR)files[k], MODE_NEWFILE);
        if (fh) {
            Write(fh, key, strlen(key));
            Close(fh);
        }
    }
    /* pruefen: Guthaben abfragen */
    status_message(comfy ? "Connecting to ComfyUI..." : "Checking the key...");
    memset(&ctl, 0, sizeof(ctl));
    ctl.timeout = 20;
    if (comfy) {
        comfy_addr(key, host, sizeof(host), &port);
        if (!net_http(host, port, FALSE, "GET", "/system_stats", NULL, NULL, 0, &r, &ctl)) {
            sprintf(msg, "The address was saved, but ComfyUI could not be reached:\n\n%s\n\n"
                         "Start ComfyUI with --listen so other computers can connect.", r.err);
        } else {
            if (r.status != 200)
                sprintf(msg, "The address was saved, but the server answered HTTP %ld.", (long)r.status);
            else if ((v = json_get((char *)r.body, "devices.0.name", &vl)) && *v == '"' &&
                     json_str(v, vl, val, sizeof(val)) > 0)
                sprintf(msg, "ComfyUI is reachable.\n\nGPU: %s", val);
            else
                strcpy(msg, "ComfyUI is reachable.");
            net_free(&r);
        }
        status_message("");
        request(msg, "OK");
        return;
    }
    sprintf(hdr, gem ? "x-goog-api-key: %s\r\n" : "Authorization: Bearer %s\r\n", key);
    if (!net_https(gem ? GEM_HOST : AI_HOST, "GET", gem ? "/v1beta/models?pageSize=1" : "/api/v1/key",
                   hdr, NULL, 0, &r, &ctl)) {
        sprintf(msg, "The key was saved, but %s could not be reached:\n\n%s", pn, r.err);
    } else if (r.status != 200) {
        sprintf(msg, "The key was saved, but %s rejected it (HTTP %ld).", pn, (long)r.status);
        net_free(&r);
    } else if (gem) {
        strcpy(msg, "The key works.");
        net_free(&r);
    } else {
        strcpy(msg, "The key works.");
        if ((v = json_get((char *)r.body, "data.usage", &vl)) && vl < 30) {
            memcpy(val, v, vl);
            val[vl] = 0;
            sprintf(msg + strlen(msg), "\n\nUsed so far: $%s", val);
        }
        if ((v = json_get((char *)r.body, "data.limit_remaining", &vl)) && vl < 30 && *v != 'n') {
            memcpy(val, v, vl);
            val[vl] = 0;
            sprintf(msg + strlen(msg), "\nRemaining limit: $%s", val);
        }
        net_free(&r);
    }
    status_message("");
    request(msg, "OK");
}

/* nach einem Signal oder zyklisch: fertige Anfrage auswerten */
void ai_poll(void)
{
    AiJob *j = job;
    if (!j || !j->done)
        return;
    if (!j->t_done)
        j->t_done = now_us();
    if (j->ok && !j->ctl.cancel) {
        /* nicht mitten in eine andere Aktion hinein */
        if (transform_active() || text_active() || liquify_active() || crop_active() || ui_dragging() ||
            ui_menu_open())
            return;
        if (doc_alive(j->doc)) {
            if (j->doc != doc)
                main_activate_doc(j->doc);
            ai_log("placing result", NULL, j->imglen);
            if (j->kind == AI_FILL)
                place_result(j);
            else
                place_segment(j);
            ai_log("done", NULL, -1);
        }
    } else if (!j->ctl.cancel) {
        char msg[200];
        regen_left = 0;
        sprintf(msg, "%s failed:\n\n%s", j->kind == AI_SUBJECT ? "Select Subject" :
                j->kind == AI_RMBG ? "Remove Background" : "Generative Fill", j->err);
        job = NULL;
        bar_inval();
        job_free(j);
        request(msg, "OK");
        return;
    } else {
        status_message("AI request cancelled");
    }
    job = NULL;
    bar_inval();
    job_free(j);
    if (regen_left > 0) {               /* weitere Varianten (AI Settings: Variants) */
        BOOL found = FALSE;
        LONG i;
        for (i = 0; i < doc->nlayers; i++)
            if (doc->layers[i] == regen_layer)
                found = TRUE;
        regen_left--;
        if (found && doc->layers[doc->active] == regen_layer)
            ai_regenerate();
        else
            regen_left = 0;
    }
}

/* beim Beenden: laufende Anfrage abbrechen und abwarten */
void ai_shutdown(void)
{
    if (job) {
        job->ctl.cancel = 1;
        while (!job->done)
            Wait(1UL << ai_sigbit);
        job_free(job);
        job = NULL;
    }
    if (ai_sigbit >= 0) {
        FreeSignal(ai_sigbit);
        ai_sigbit = -1;
    }
}

/* ---------- schwebende Taskleiste unter der Auswahl ---------- */

enum { BAR_IDLE, BAR_PROMPT };
#define BAR_NOSEL (!job && !doc->hassel)        /* Leiste ohne Auswahl: Select Subject / Remove Background */
static LONG bar_mode = BAR_IDLE;
static char bar_prompt[200];
static LONG bar_x, bar_y, bar_w, bar_h;         /* zuletzt gezeichnet, w = 0: keine */
static LONG bar_drawn_mode = -1;                /* Inhalt der zuletzt gezeichneten Leiste */
static BOOL bar_recheck;                        /* Inhalt gewechselt: im naechsten Tick ganz neu */
static LONG bar_secs = -1;
static LONG bar_sel[4];                         /* Auswahl, fuer die die Leiste offen ist */
#define BAR_H 34

BOOL ai_taskbar = TRUE;         /* Window > Contextual Task Bar */

static BOOL bar_visible(void)
{
    if (job)
        return TRUE;
    if (!doc || transform_active() || text_active() || liquify_active() || crop_active() || ui_menu_open())
        return FALSE;
    if (!ai_taskbar)            /* ausgeblendet: nur das ueber das Menue geoeffnete Eingabefeld */
        return bar_mode == BAR_PROMPT && doc->hassel;
    /* ohne Auswahl nur bei Auswahl-/Verschiebe-Werkzeugen, wie die Taskleiste in Photoshop */
    return doc->hassel || (tool <= T_WAND && !ui_dragging());
}

static void bar_rect(LONG *x, LONG *y, LONG *w, LONG *h)
{
    LONG ww = job ? 250 : BAR_NOSEL ? 300 : bar_mode == BAR_PROMPT ? 480 : 252, lx, ly;
    LONG it = doc2scry(0), ib = doc2scry(doc->h), cx;
    *w = ww;
    *h = BAR_H;
    /* waagerecht unter der Auswahl (bzw. Bildmitte), senkrecht ausserhalb des Bildes:
     * darunter, sonst darueber, sonst am unteren Rand der Arbeitsflaeche */
    if (doc->hassel)
        cx = (doc2scrx(doc->selx0) + doc2scrx(doc->selx1)) / 2;
    else if (job && job->doc == doc && job->kind == AI_FILL)
        cx = doc2scrx(job->x + job->w / 2);
    else
        cx = (doc2scrx(0) + doc2scrx(doc->w)) / 2;
    lx = cx - ww / 2;
    if (ib + 10 + BAR_H <= cvy + cvh - 6)
        ly = ib + 10;
    else if (it - 10 - BAR_H >= cvy + 6)
        ly = it - 10 - BAR_H;
    else
        ly = cvy + cvh - BAR_H - 8;
    if (lx + ww > cvx + cvw - 8)
        lx = cvx + cvw - ww - 8;
    if (lx < cvx + 8)
        lx = cvx + 8;
    *x = lx;
    *y = ly;
}

static void bar_button(LONG x, LONG y, LONG w, const char *s, BOOL accent)
{
    fill(x, y, w, 24, accent ? COL_ACCENT : COL_LIGHT);
    frame(x, y, w, 24, COL_LINE);
    text(x + (w - text_width(s)) / 2, y + 3, s, accent ? 0xFFFFFFFF : COL_TEXT);
}

/* Felder: liefert die x-Positionen der Teile */
#define FIELD_X(bx)   ((bx) + 8)
#define FIELD_W(bw)   ((bw) - 8 - 96 - 8 - 28 - 8)

void ai_overlay(void)
{
    LONG x, y, w, h;
    if (!bar_visible())
        return;
    bar_rect(&x, &y, &w, &h);
    /* Wechselt der Inhalt bei einem Teil-Neuzeichnen (z. B. Paste hebt die Auswahl
     * auf, nur die Ebene wird neu gezeichnet), kaeme er nicht auf den Schirm */
    {
        LONG mode = job ? 0 : bar_mode == BAR_PROMPT ? 1 : BAR_NOSEL ? 2 : 3;
        if (mode != bar_drawn_mode) {
            bar_drawn_mode = mode;
            bar_recheck = TRUE;
        }
    }
    if (!bar_w) {                       /* gezeichneten Umriss merken */
        bar_x = x;
        bar_y = y;
        bar_w = w;
        bar_h = h;
    } else {
        LONG x1 = bar_x + bar_w > x + w ? bar_x + bar_w : x + w;
        LONG y1 = bar_y + bar_h > y + h ? bar_y + bar_h : y + h;
        if (x < bar_x) bar_x = x;
        if (y < bar_y) bar_y = y;
        bar_w = x1 - bar_x;
        bar_h = y1 - bar_y;
    }
    fill(x + 3, y + 3, w, h, 0xFF151515);    /* Schatten */
    fill(x, y, w, h, COL_PANEL);
    frame(x, y, w, h, COL_LINE);
    if (job) {
        char s[48];
        LONG secs = (now_us() - job->t0) / 1000000;
        const char *what = job->kind == AI_FILL ? "Generating..." : "Finding subject...";
        if (job->ctl.recvd > 0)
            sprintf(s, "%s %ld s  %ld KB", what, (long)secs, (long)(job->ctl.recvd >> 10));
        else
            sprintf(s, "%s %ld s", what, (long)secs);
        text(x + 10, y + 8, s, COL_TEXT);
        bar_button(x + w - 78, y + 5, 70, "Cancel", FALSE);
    } else if (bar_mode == BAR_PROMPT) {
        LONG fx = FIELD_X(x), fw = FIELD_W(w);
        fill(fx, y + 5, fw, 24, COL_FIELD);
        frame(fx, y + 5, fw, 24, COL_LINE);
        gfx_clip(fx + 2, y + 6, fx + fw - 2, y + 28);
        if (bar_prompt[0])
            text(fx + 5, y + 8, bar_prompt, COL_TEXT);
        else
            text(fx + 5, y + 8, "Describe what to generate (optional)", COL_DIM);
        gfx_noclip();
        bar_button(fx + fw + 8, y + 5, 96, "Generate", TRUE);
        bar_button(x + w - 36, y + 5, 28, "x", FALSE);
    } else if (BAR_NOSEL) {
        bar_button(x + 8, y + 5, 132, "Select Subject", FALSE);
        bar_button(x + 148, y + 5, 144, "Remove Background", FALSE);
    } else {
        bar_button(x + 8, y + 5, 140, "Generative Fill", TRUE);
        bar_button(x + 156, y + 5, 88, "Remove", FALSE);
    }
}

/* bar_x/y/w/h: Umriss aller seit dem letzten bar_inval gezeichneten Leisten
 * (ai_overlay traegt ein). Neu zeichnen = diesen Umriss und die neue Lage
 * ungueltig machen; danach zeichnet ai_overlay die Leiste nur an der neuen Lage. */
static void bar_inval(void)
{
    LONG x, y, w, h;
    if (bar_w)
        view_inval(bar_x, bar_y, bar_x + bar_w + 3, bar_y + bar_h + 3);
    if (bar_visible()) {
        bar_rect(&x, &y, &w, &h);
        view_inval(x, y, x + w + 3, y + h + 3);
    }
    bar_w = 0;
}

void ai_bar_refresh(void)
{
    bar_inval();
}

/* zyklisch (INTUITICKS): Lage/Zeit nachfuehren, Ergebnis abholen */
void ai_tick(void)
{
    LONG x = 0, y = 0, w = 0, h = 0;
    BOOL vis;
    if (ui_menu_open())                 /* Menue offen: Leiste unveraendert lassen, danach
                                         * alte und neue Lage gemeinsam neu zeichnen */
        return;
    vis = bar_visible();
    if (bar_recheck) {
        bar_recheck = FALSE;
        bar_inval();
    }
    if (job && !job->done) {
        if (!job->t_up && job->ctl.sent >= job->blen)
            job->t_up = now_us();
        if (!job->t_first && job->ctl.recvd > 0)
            job->t_first = now_us();
    }
    ai_poll();
    if (vis)
        bar_rect(&x, &y, &w, &h);
    if (!vis && !doc->hassel)
        bar_mode = BAR_IDLE;
    /* neue Auswahl: wieder mit Generative Fill / Remove beginnen */
    if (doc->hassel && !job && (doc->selx0 != bar_sel[0] || doc->sely0 != bar_sel[1] ||
                                doc->selx1 != bar_sel[2] || doc->sely1 != bar_sel[3])) {
        bar_sel[0] = doc->selx0;
        bar_sel[1] = doc->sely0;
        bar_sel[2] = doc->selx1;
        bar_sel[3] = doc->sely1;
        if (bar_mode != BAR_IDLE) {
            bar_mode = BAR_IDLE;
            bar_inval();
        }
    }
    if ((vis ? w : 0) != bar_w || (vis && (x != bar_x || y != bar_y || h != bar_h)))
        bar_inval();
    else if (job && (now_us() - job->t0) / 500000 != bar_secs) {
        bar_secs = (now_us() - job->t0) / 500000;
        bar_inval();
    }
}

static void bar_edit(void)
{
    LONG x, y, w, h;
    extern BOOL ui_edit_enter;
    bar_rect(&x, &y, &w, &h);
    if (ui_edit_text(FIELD_X(x), y + 5, FIELD_W(w), bar_prompt, sizeof(bar_prompt)) && ui_edit_enter)
        ai_start(bar_prompt);
    bar_inval();
    view_flush();
}

/* Edit > Generative Fill...: Leiste mit Eingabefeld oeffnen */
void ai_open_prompt(void)
{
    if (job || !doc->hassel) {
        status_message(job ? "An AI request is already running" : "Make a selection first");
        return;
    }
    bar_sel[0] = doc->selx0;
    bar_sel[1] = doc->sely0;
    bar_sel[2] = doc->selx1;
    bar_sel[3] = doc->sely1;
    bar_mode = BAR_PROMPT;
    bar_inval();
    view_flush();
    bar_edit();
}

/* Mausklick: TRUE, wenn er die Leiste getroffen hat */
BOOL ai_bar_click(LONG mx, LONG my)
{
    LONG x, y, w, h;
    if (!bar_visible())
        return FALSE;
    bar_rect(&x, &y, &w, &h);
    if (mx < x || mx >= x + w || my < y || my >= y + h)
        return FALSE;
    if (job) {
        if (mx >= x + w - 78)
            ai_cancel();
    } else if (BAR_NOSEL) {
        if (mx >= x + 8 && mx < x + 140)
            ai_segment(AI_SUBJECT);
        else if (mx >= x + 148 && mx < x + 292)
            ai_segment(AI_RMBG);
    } else if (bar_mode == BAR_PROMPT) {
        LONG fx = FIELD_X(x), fw = FIELD_W(w);
        if (mx >= fx && mx < fx + fw)
            bar_edit();
        else if (mx >= fx + fw + 8 && mx < fx + fw + 104)
            ai_start(bar_prompt);
        else if (mx >= x + w - 36) {
            bar_mode = BAR_IDLE;
            bar_inval();
        }
    } else if (mx >= x + 8 && mx < x + 148) {
        ai_open_prompt();
    } else if (mx >= x + 156 && mx < x + 244) {
        ai_start(NULL);
    }
    return TRUE;
}
