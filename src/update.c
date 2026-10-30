/*
 * update.c - Update ueber Dropbox (latest.txt + Paket) oder GitHub-Releases
 *
 * Beim Start (abschaltbar) und ueber Help > Check for Updates fragt ein
 * Hintergrundprozess die neueste Version ab: zuerst latest.txt in der Dropbox,
 * geht das nicht, das neueste GitHub-Release. Ist sie neuer, wird nachgefragt,
 * das Paket AmiPixelShop.upd geladen, die Signatur geprueft (Ed25519), entpackt
 * (CRC), ins Programmverzeichnis geschrieben und das Programm neu gestartet.
 * Ohne Netz passiert nichts. Pakete ohne gueltige Signatur werden abgelehnt.
 *
 * Paketformat (tools/mkupd.py):
 *   "APSUPD1\n", je Datei "F <name> <laenge> <zlib-laenge> <crc32 hex>\n" + zlib-Daten,
 *   dann "END\n", dann "SIG <128 Hex>\n" (Signatur ueber alles davor).
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include "apx.h"
#include "net.h"
#include "version.h"
#include "zlib.h"
#include "tweetnacl.h"

/* Dropbox: feste Freigabelinks (dl=1: Datei direkt statt Vorschauseite) */
#define UPD_DBX_LATEST "https://www.dropbox.com/scl/fi/nqvdkojervz4w3joqnbyw/latest.txt?rlkey=7xwrw9zwvutlph9iogi08u3bu&dl=1"
#define UPD_DBX_PKG    ""
#define UPD_HOST  "api.github.com"
#define UPD_PATH  "/repos/radi7777/AmiPixelShop/releases/latest"
#define UPD_ASSET "AmiPixelShop.upd"
#define UPD_EXE   "AmiPixelShop"
#define UPD_MAXF  64
#define UPD_SIGLEN 133          /* "SIG " + 128 Hex + "\n" */

/* oeffentlicher Schluessel; der geheime liegt nur beim Autor */
static const unsigned char upd_pk[32] = {
    0xe8, 0x6b, 0x34, 0x83, 0x01, 0x67, 0x25, 0x52, 0x50, 0xfd, 0x8a, 0x70, 0xee, 0xf0, 0xfe, 0x29,
    0x35, 0x3b, 0x1d, 0xba, 0x24, 0xb5, 0x48, 0x29, 0x96, 0xf6, 0x1d, 0x98, 0x27, 0x1a, 0xb6, 0x8d
};

/* TweetNaCl will das fuer die Schluesselerzeugung; die nutzen wir nicht */
void randombytes(unsigned char *x, unsigned long long n)
{
    memset(x, 0, (size_t)n);
}

typedef struct UpdJob {
    struct Task *parent;
    NetCtl ctl;
    volatile LONG done;
    LONG stage;                 /* 0 = Release abfragen, 1 = Paket laden */
    BOOL manual;                /* ueber das Menue: auch "aktuell" und Fehler melden */
    BOOL ok;
    char err[128];
    char tag[32];               /* z. B. "v0.6-beta" */
    char url[1024];             /* Paket, leer = Release ohne Paket */
    char latest[512];           /* Dropbox latest.txt (leer: nur GitHub) */
    char pkg[512];              /* Dropbox-Paket */
    UBYTE *data;
    LONG len;
} UpdJob;

BOOL upd_auto = TRUE;           /* beim Start pruefen */
char upd_skip[32];              /* uebersprungene Version */
BOOL upd_restart;               /* nach dem Beenden neu starten */

static UpdJob *ujob;
static LONG upd_sigbit = -1;
static LONG last_kb = -1;

/* ---------- Hintergrundprozess: keine stdio, kein malloc ---------- */

/* "assets.<i>.<feld>" ohne sprintf */
static void asset_path(char *dst, LONG i, const char *field)
{
    strcpy(dst, "assets.");
    dst += 7;
    if (i >= 10)
        *dst++ = '0' + i / 10;
    *dst++ = '0' + i % 10;
    *dst++ = '.';
    strcpy(dst, field);
}

static void upd_worker(void)
{
    UpdJob *j = ujob;
    NetResp r;
    const char *v;
    LONG vl, i;
    char path[40], name[64];

    if (j->stage == 0 && j->latest[0]) {
        /* Dropbox latest.txt: 1. Zeile Versionsnummer, 2. Zeile (optional) Link zum
         * Paket - sonst der feste Paketlink */
        if (net_get_url(j->latest, NULL, &r, &j->ctl)) {
            if (r.status == 200 && r.len > 0 && r.len < 1000 && r.body[0] >= '0' && r.body[0] <= '9') {
                LONG k = 0;
                j->tag[0] = 'v';
                for (i = 0; i < r.len && i < (LONG)sizeof(j->tag) - 2 && r.body[i] > ' '; i++)
                    j->tag[i + 1] = r.body[i];
                j->tag[i + 1] = 0;
                while (i < r.len && r.body[i] != '\n')
                    i++;
                if (i + 9 < r.len && !strncmp((const char *)r.body + i + 1, "https://", 8)) {
                    for (i++; i < r.len && r.body[i] > ' ' && k < (LONG)sizeof(j->url) - 1; i++)
                        j->url[k++] = r.body[i];
                    j->url[k] = 0;
                } else
                    strcpy(j->url, j->pkg);
                j->ok = TRUE;           /* ohne Paketlink: Hinweis zum Selbst-Laden */
            }
            net_free(&r);
        }
    }
    if (j->stage == 0 && !j->ok) {
        if (!net_https(UPD_HOST, "GET", UPD_PATH, "Accept: application/vnd.github+json\r\n", NULL, 0, &r, &j->ctl)) {
            strcpy(j->err, r.err);
        } else {
            const char *js = (const char *)r.body;
            if (r.status != 200) {
                strcpy(j->err, r.status == 404 ? "No release found" : "Server error");
            } else if ((v = json_get(js, "tag_name", &vl)) && *v == '"') {
                json_str(v, vl, j->tag, sizeof(j->tag));
                for (i = 0; i < 20; i++) {
                    asset_path(path, i, "name");
                    if (!(v = json_get(js, path, &vl)))
                        break;
                    json_str(v, vl, name, sizeof(name));
                    if (strcmp(name, UPD_ASSET))
                        continue;
                    asset_path(path, i, "browser_download_url");
                    if ((v = json_get(js, path, &vl)))
                        json_str(v, vl, j->url, sizeof(j->url));
                    break;
                }
                j->ok = TRUE;
            } else
                strcpy(j->err, "Invalid answer from GitHub");
            net_free(&r);
        }
    } else {
        if (!net_get_url(j->url, NULL, &r, &j->ctl)) {
            strcpy(j->err, r.err);
        } else if (r.status != 200) {
            strcpy(j->err, "Download failed");
            net_free(&r);
        } else {
            j->data = r.body;
            j->len = r.len;
            j->ok = TRUE;
        }
    }
    Forbid();                   /* Prozess endet, bevor der Hauptprozess weiterlaeuft */
    j->done = 1;
    Signal(j->parent, 1UL << upd_sigbit);
}

/* ---------- Hauptprozess ---------- */

ULONG upd_sigmask(void)
{
    return upd_sigbit >= 0 ? 1UL << upd_sigbit : 0;
}

static void job_free(UpdJob *j)
{
    if (!j)
        return;
    net_freemem(j->data);
    FreeVec(j);
}

static BOOL launch(UpdJob *j)
{
    ujob = j;
    if (!CreateNewProcTags(NP_Entry, (ULONG)upd_worker, NP_Name, (ULONG)"AmiPixelShop Update",
                           NP_StackSize, 32768, NP_Priority, -1, NP_Output, 0, NP_Input, 0,
                           NP_CloseOutput, FALSE, NP_CloseInput, FALSE, TAG_DONE)) {
        ujob = NULL;
        job_free(j);
        return FALSE;
    }
    return TRUE;
}

void upd_check(BOOL manual)
{
    UpdJob *j;
    if (ujob) {
        if (manual)
            status_message("An update check is already running");
        return;
    }
    if (upd_sigbit < 0 && (upd_sigbit = AllocSignal(-1)) < 0)
        return;
    if (!(j = AllocVec(sizeof(UpdJob), MEMF_ANY | MEMF_CLEAR)))
        return;
    j->parent = FindTask(NULL);
    j->manual = manual;
    j->ctl.timeout = 20;
    /* Quelle zum Testen ueberschreibbar: ENV:AmiPixelShop/updlatest und updpkg */
    if (GetVar("AmiPixelShop/updlatest", j->latest, sizeof(j->latest), GVF_GLOBAL_ONLY) <= 0) {
        strcpy(j->latest, UPD_DBX_LATEST);
        strcpy(j->pkg, UPD_DBX_PKG);
    } else if (GetVar("AmiPixelShop/updpkg", j->pkg, sizeof(j->pkg), GVF_GLOBAL_ONLY) <= 0)
        j->pkg[0] = 0;
    if (launch(j) && manual)
        status_message("Checking for updates...");
}

/* "v0.6-beta" / "0.5" -> 0x00000600 / 0x00000500 (Haupt << 16 | Neben << 8 | Patch) */
static ULONG ver_num(const char *s)
{
    ULONG part[3] = { 0, 0, 0 };
    LONG k = 0;
    while (*s && (*s < '0' || *s > '9'))
        s++;
    while (*s && k < 3) {
        if (*s >= '0' && *s <= '9')
            part[k] = part[k] * 10 + (*s - '0');
        else if (*s == '.')
            k++;
        else
            break;
        s++;
    }
    return part[0] << 16 | part[1] << 8 | part[2];
}

/* Zeile ab *p lesen (bis \n), *p dahinter */
static BOOL get_line(const UBYTE **p, const UBYTE *end, char *dst, LONG max)
{
    LONG i = 0;
    while (*p < end && **p != '\n') {
        if (i < max - 1)
            dst[i++] = **p;
        (*p)++;
    }
    dst[i] = 0;
    if (*p >= end)
        return FALSE;
    (*p)++;
    return TRUE;
}

static BOOL safe_name(const char *n)
{
    if (!n[0] || n[0] == '/' || strchr(n, ':') || strstr(n, "..") || strlen(n) > 100)
        return FALSE;
    return TRUE;
}

static LONG hexval(UBYTE c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Signatur am Paketende pruefen; TRUE = echt. *len wird auf den signierten Teil gekuerzt */
BOOL upd_verify(const UBYTE *data, LONG *len, char *err)
{
    const UBYTE *t;
    unsigned char *sm, *m;
    unsigned long long mlen;
    LONG n, i, ok;
    if (*len <= UPD_SIGLEN || memcmp(data + *len - UPD_SIGLEN, "SIG ", 4) || data[*len - 1] != '\n') {
        strcpy(err, "The update package is not signed");
        return FALSE;
    }
    n = *len - UPD_SIGLEN;
    t = data + n + 4;
    if (!(sm = malloc(n + 64)) || !(m = malloc(n + 64))) {
        free(sm);
        strcpy(err, "Not enough memory");
        return FALSE;
    }
    for (i = 0; i < 64; i++) {
        LONG h = hexval(t[2 * i]), l = hexval(t[2 * i + 1]);
        sm[i] = (unsigned char)((h << 4) | l);
        if (h < 0 || l < 0)
            break;
    }
    memcpy(sm + 64, data, n);
    ok = i == 64 && crypto_sign_open(m, &mlen, sm, n + 64, upd_pk) == 0;
    free(m);
    free(sm);
    if (!ok) {
        strcpy(err, "The update package has no valid signature");
        return FALSE;
    }
    *len = n;
    return TRUE;
}

/* Paket pruefen und einspielen; err bei FALSE */
static BOOL install(const UBYTE *data, LONG len, char *err)
{
    static char names[UPD_MAXF][104];
    char dir[256], line[200], path[400], path2[400];
    const UBYTE *p = data, *end;
    LONG nf = 0, i;
    BPTR lock = GetProgramDir();

    if (!upd_verify(data, &len, err))
        return FALSE;
    end = data + len;
    if (!lock || !NameFromLock(lock, dir, sizeof(dir))) {
        strcpy(err, "Program directory not found");
        return FALSE;
    }
    if (!get_line(&p, end, line, sizeof(line)) || strcmp(line, "APSUPD1")) {
        strcpy(err, "The update package is invalid");
        return FALSE;
    }
    /* 1. alles entpacken, pruefen und als .new schreiben */
    for (;;) {
        char name[104];
        unsigned long usize, csize, crc;
        uLongf outlen;
        UBYTE *out;
        BPTR fh;
        if (!get_line(&p, end, line, sizeof(line))) {
            strcpy(err, "The update package is incomplete");
            goto fail;
        }
        if (!strcmp(line, "END"))
            break;
        if (sscanf(line, "F %100s %lu %lu %lx", name, &usize, &csize, &crc) != 4 || !safe_name(name) ||
            nf >= UPD_MAXF || csize > (ULONG)(end - p) || usize > 64 * 1024 * 1024) {
            strcpy(err, "The update package is invalid");
            goto fail;
        }
        if (!(out = malloc(usize ? usize : 1))) {
            strcpy(err, "Not enough memory");
            goto fail;
        }
        outlen = usize;
        if (uncompress(out, &outlen, p, csize) != Z_OK || outlen != usize ||
            crc32(0L, out, usize) != crc) {
            free(out);
            strcpy(err, "The update package is damaged");
            goto fail;
        }
        p += csize;
        strcpy(path, dir);
        AddPart(path, name, sizeof(path) - 8);
        /* Unterverzeichnis (images/) bei Bedarf anlegen */
        {
            char *slash = strrchr(name, '/');
            if (slash) {
                BPTR l;
                strcpy(path2, dir);
                *slash = 0;
                AddPart(path2, name, sizeof(path2));
                *slash = '/';
                if ((l = CreateDir(path2)))
                    UnLock(l);
            }
        }
        strcat(path, ".new");
        if (!(fh = Open(path, MODE_NEWFILE)) || Write(fh, out, usize) != (LONG)usize) {
            if (fh)
                Close(fh);
            free(out);
            strcpy(err, "Cannot write to the program directory");
            goto fail;
        }
        Close(fh);
        free(out);
        strcpy(names[nf++], name);
    }
    /* 2. austauschen: alt -> .old, .new -> Name */
    for (i = 0; i < nf; i++) {
        strcpy(path, dir);
        AddPart(path, names[i], sizeof(path) - 8);
        strcpy(path2, path);
        strcat(path2, ".old");
        DeleteFile(path2);
        Rename(path, path2);
        strcpy(path2, path);
        strcat(path2, ".new");
        if (!Rename(path2, path)) {
            strcpy(err, "Cannot replace the program files");
            return FALSE;
        }
        SetProtection(path, 0);     /* rwed */
        if (strcmp(names[i], UPD_EXE)) {
            strcat(path, ".old");    /* Sicherung nur vom Programm behalten */
            DeleteFile(path);
        }
    }
    return TRUE;

fail:
    for (i = 0; i < nf; i++) {
        strcpy(path, dir);
        AddPart(path, names[i], sizeof(path) - 8);
        strcat(path, ".new");
        DeleteFile(path);
    }
    return FALSE;
}

static LONG unsaved(void)
{
    LONG i, n = 0;
    for (i = 0; i < ndocs; i++)
        n += docs[i]->modified ? 1 : 0;
    return n;
}

/* "v0.6-beta" -> "0.6-beta" */
static const char *nice(const char *tag)
{
    return tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag;
}

static void found(UpdJob *j)
{
    char msg[300];
    UpdJob *k;
    LONG r;
    if (ver_num(j->tag) <= ver_num(APS_VER_STR)) {
        if (j->manual)
            request("You have the latest version (" APS_VER_STR ").", "OK");
        return;
    }
    if (!j->manual && !strcmp(j->tag, upd_skip))
        return;
    if (!j->url[0]) {
        if (j->manual) {
            sprintf(msg, "AmiPixelShop %s is available.\nPlease download it from\n"
                    "Aminet or github.com/radi7777/AmiPixelShop", nice(j->tag));
            request(msg, "OK");
        }
        return;
    }
    sprintf(msg, "AmiPixelShop %s is available (you have " APS_VER_STR ").\n"
            "Download and install it now?\nAmiPixelShop restarts afterwards.", nice(j->tag));
    r = request(msg, j->manual ? "Install|Later" : "Install|Later|Skip This Version");
    if (r == 0 && !j->manual) {     /* rechter Knopf liefert 0 */
        strncpy(upd_skip, j->tag, sizeof(upd_skip) - 1);
        ui_prefs_save();
        return;
    }
    if (r != 1)
        return;
    if ((r = unsaved())) {
        sprintf(msg, r == 1 ? "One image has unsaved changes.\nThey are lost when AmiPixelShop restarts."
                            : "%ld images have unsaved changes.\nThey are lost when AmiPixelShop restarts.", (long)r);
        if (request(msg, "Continue|Cancel") != 1)
            return;
    }
    if (!(k = AllocVec(sizeof(UpdJob), MEMF_ANY | MEMF_CLEAR)))
        return;
    k->parent = FindTask(NULL);
    k->stage = 1;
    k->manual = TRUE;
    k->ctl.timeout = 60;
    strcpy(k->tag, j->tag);
    strcpy(k->url, j->url);
    last_kb = -1;
    if (launch(k))
        status_message("Downloading update...");
}

void upd_poll(void)
{
    UpdJob *j = ujob;
    char msg[200];
    if (!j || !j->done)
        return;
    ujob = NULL;
    if (!j->ok) {
        status_message("");
        if (j->manual) {
            sprintf(msg, j->stage ? "The update could not be downloaded:\n%s"
                                  : "Could not check for updates:\n%s", j->err);
            request(msg, "OK");
        }
    } else if (j->stage == 0) {
        if (j->manual)
            status_message("");
        found(j);
    } else {
        status_message("Installing update...");
        if (install(j->data, j->len, msg)) {
            sprintf(msg, "AmiPixelShop %s is installed.\nThe program restarts now.", nice(j->tag));
            request(msg, "OK");
            upd_restart = TRUE;
            main_request_quit();
        } else {
            char m2[240];
            sprintf(m2, "The update could not be installed:\n%s", msg);
            request(m2, "OK");
        }
        status_message("");
    }
    job_free(j);
}

/* Fortschritt beim Laden in der Statuszeile */
void upd_tick(void)
{
    LONG kb;
    if (!ujob || ujob->stage != 1 || ujob->done)
        return;
    kb = ujob->ctl.recvd / 1024;
    if (kb != last_kb) {
        char msg[48];
        sprintf(msg, "Downloading update... %ld KB", (long)kb);
        status_message(msg);
        last_kb = kb;
    }
}

void upd_shutdown(void)
{
    if (ujob) {
        ujob->ctl.cancel = 1;
        while (!ujob->done)
            Wait(1UL << upd_sigbit);
        job_free(ujob);
        ujob = NULL;
    }
    if (upd_sigbit >= 0) {
        FreeSignal(upd_sigbit);
        upd_sigbit = -1;
    }
}

/* nach dem Beenden: neue Version starten */
void upd_relaunch(void)
{
    char cmd[300];
    BPTR in, out;
    if (!upd_restart || !GetProgramDir())
        return;
    strcpy(cmd, "\"");
    if (!NameFromLock(GetProgramDir(), cmd + 1, sizeof(cmd) - 20))
        return;
    AddPart(cmd + 1, UPD_EXE, sizeof(cmd) - 4);
    strcat(cmd, "\"");
    in = Open("NIL:", MODE_OLDFILE);
    out = Open("NIL:", MODE_NEWFILE);
    if (!in || !out || SystemTags(cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE, TAG_DONE) == -1) {
        if (in) Close(in);
        if (out) Close(out);
    }
}

/* Kommandozeile: AmiPixelShop UPDVERIFY <datei> - Signatur pruefen, Dauer messen */
void upd_verify_file(const char *path)
{
    BPTR fh = Open(path, MODE_OLDFILE);
    LONG len;
    UBYTE *buf;
    char err[128];
    ULONG t;
    if (!fh) {
        PutStr("Cannot open file\n");
        return;
    }
    Seek(fh, 0, OFFSET_END);
    len = Seek(fh, 0, OFFSET_BEGINNING);
    if (len <= 0 || !(buf = malloc(len)) || Read(fh, buf, len) != len) {
        Close(fh);
        PutStr("Cannot read file\n");
        return;
    }
    Close(fh);
    t = now_us();
    if (upd_verify(buf, &len, err))
        Printf("Signature OK (%ld ms)\n", (now_us() - t) / 1000);
    else
        Printf("FAILED: %s (%ld ms)\n", (ULONG)err, (now_us() - t) / 1000);
    free(buf);
}
