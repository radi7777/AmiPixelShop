/*
 * maglasso.c - Magnetisches Lasso
 *
 * Kantenkarte (Sobel je Farbkanal, Maximum) aus dem sichtbaren Bild. Vom letzten
 * Ankerpunkt zum Mauszeiger wird der guenstigste Weg gesucht (Dijkstra, Kanten
 * sind billig), nur in einem Streifen der Breite mag_width um die Strecke
 * Anker-Maus. Wird der Weg lang, setzt das Werkzeug selbst Anker (mag_freq).
 * Der Weg steht in path_x/path_y (die Ansicht zeichnet ihn): bis path_fix fest,
 * dahinter der bewegliche Teil zum Mauszeiger.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>
#include "apx.h"

LONG mag_width = 10;            /* px: Suchbreite um die Mausspur */
LONG mag_contrast = 10;         /* %: schwaechere Kanten zaehlen nicht */
LONG mag_freq = 57;             /* 0..100: Abstand der automatischen Anker */

#define MAX_ANCH 1024
#define WIN_MAX  600            /* groesstes Suchfenster je Seite; weiter: gerade Linie */

static BOOL active;
static UBYTE *edge;             /* doc->w * doc->h, 0..255 */
static LONG ew, eh;
static LONG anch[MAX_ANCH];     /* Index in path_x/y je Anker */
static LONG nanch, path_fix;    /* path_fix: Anzahl fester Punkte */
static LONG lastx, lasty;       /* letzte Mausposition (Dokument, ganzzahlig) */
static BOOL pending;            /* Mausposition noch nicht ausgewertet */
#define MAX_TRAIL 2048
static WORD trx[MAX_TRAIL], try_[MAX_TRAIL];   /* Mausspur seit dem letzten Anker */
static LONG ntrail;
static UBYTE *allow;            /* Suchfenster: 1 = nahe der Mausspur */

/* Suchpuffer, nach Bedarf vergroessert */
static ULONG *dist;
static UBYTE *from;             /* Richtung 0..7, 8 = Start, 255 = unbesucht */
static LONG cap;
typedef struct { ULONG d; LONG n; } HeapEnt;
static HeapEnt *heap;
static LONG heapcap, heapn;
static LONG *tmpx, *tmpy;
static LONG tmpcap;

static const BYTE ddx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, ddy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

BOOL mag_active(void)
{
    return active;
}

static void free_bufs(void)
{
    if (edge) FreeVec(edge);
    if (dist) FreeVec(dist);
    if (from) FreeVec(from);
    if (heap) FreeVec(heap);
    if (allow) FreeVec(allow);
    allow = NULL;
    if (tmpx) FreeVec(tmpx);
    if (tmpy) FreeVec(tmpy);
    edge = NULL;
    dist = NULL;
    from = NULL;
    heap = NULL;
    tmpx = NULL;
    tmpy = NULL;
    cap = heapcap = tmpcap = 0;
}

void mag_end(void)
{
    active = FALSE;
    nanch = path_fix = 0;
    free_bufs();
}

/* Kantenstaerke aus dem Komposit; Rand = 0 */
static BOOL build_edges(void)
{
    LONG x, y, c;
    const ULONG *f;
    doc_process_dirty();
    f = doc->flat;
    ew = doc->w;
    eh = doc->h;
    if (!f || !(edge = AllocVec(ew * eh, MEMF_ANY | MEMF_CLEAR)))
        return FALSE;
    for (y = 1; y < eh - 1; y++) {
        const ULONG *r0 = f + (y - 1) * ew, *r1 = f + y * ew, *r2 = f + (y + 1) * ew;
        UBYTE *o = edge + y * ew;
        for (x = 1; x < ew - 1; x++) {
            LONG best = 0;
            for (c = 0; c < 24; c += 8) {
#define CH(p) ((LONG)(((p) >> c) & 0xFF))
                LONG gx = CH(r0[x + 1]) + 2 * CH(r1[x + 1]) + CH(r2[x + 1]) -
                          CH(r0[x - 1]) - 2 * CH(r1[x - 1]) - CH(r2[x - 1]);
                LONG gy = CH(r2[x - 1]) + 2 * CH(r2[x]) + CH(r2[x + 1]) -
                          CH(r0[x - 1]) - 2 * CH(r0[x]) - CH(r0[x + 1]);
#undef CH
                LONG m = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
                if (m > best)
                    best = m;
            }
            best >>= 2;         /* Sobel |gx|+|gy| bis 2040 -> 0..255 (Kontrast 50 % = 255) */
            o[x] = best > 255 ? 255 : (UBYTE)best;
        }
    }
    return TRUE;
}

static BOOL grow(LONG n)
{
    if (n <= cap)
        return TRUE;
    if (dist) FreeVec(dist);
    if (from) FreeVec(from);
    if (allow) FreeVec(allow);
    dist = AllocVec(n * 4, MEMF_ANY);
    from = AllocVec(n, MEMF_ANY);
    allow = AllocVec(n, MEMF_ANY);
    cap = dist && from && allow ? n : 0;
    return cap != 0;
}

static BOOL heap_push(ULONG d, LONG n)
{
    LONG i;
    if (heapn >= heapcap) {
        LONG nc = heapcap ? heapcap * 2 : 16384;
        HeapEnt *nh = AllocVec(nc * sizeof(HeapEnt), MEMF_ANY);
        if (!nh)
            return FALSE;
        if (heap) {
            CopyMem(heap, nh, heapn * sizeof(HeapEnt));
            FreeVec(heap);
        }
        heap = nh;
        heapcap = nc;
    }
    for (i = heapn++; i > 0 && heap[(i - 1) >> 1].d > d; i = (i - 1) >> 1)
        heap[i] = heap[(i - 1) >> 1];
    heap[i].d = d;
    heap[i].n = n;
    return TRUE;
}

static HeapEnt heap_pop(void)
{
    HeapEnt top = heap[0], last = heap[--heapn];
    LONG i = 0, k;
    while ((k = 2 * i + 1) < heapn) {
        if (k + 1 < heapn && heap[k + 1].d < heap[k].d)
            k++;
        if (heap[k].d >= last.d)
            break;
        heap[i] = heap[k];
        i = k;
    }
    heap[i] = last;
    return top;
}

static BOOL tmp_grow(LONG n)
{
    if (n <= tmpcap)
        return TRUE;
    if (tmpx) FreeVec(tmpx);
    if (tmpy) FreeVec(tmpy);
    tmpx = AllocVec(n * 4, MEMF_ANY);
    tmpy = AllocVec(n * 4, MEMF_ANY);
    tmpcap = tmpx && tmpy ? n : 0;
    return tmpcap != 0;
}

/* Scheibe mit Radius r um (cx,cy) im Fenster als erlaubt markieren */
static void stamp(LONG cx, LONG cy, LONG r, LONG x0, LONG y0, LONG ww, LONG wh)
{
    LONG x, y;
    for (y = cy - r; y <= cy + r; y++) {
        LONG dy = y - cy, h;
        if (y < y0 || y >= y0 + wh)
            continue;
        for (h = 0; h * h + dy * dy <= r * r; h++)
            ;
        for (x = cx - h + 1; x < cx + h; x++)
            if (x >= x0 && x < x0 + ww)
                allow[(y - y0) * ww + (x - x0)] = 1;
    }
}

/* Weg (ax,ay) -> (bx,by) in tmpx/tmpy (ohne Startpunkt, mit Ziel); Anzahl, 0 = gerade Linie noetig.
 * Gesucht wird nur nahe der Mausspur (Breite mag_width), wie in Photoshop. */
static LONG find_path(LONG ax, LONG ay, LONG bx, LONG by)
{
    LONG w = mag_width < 2 ? 2 : mag_width;
    LONG x0 = ax, y0 = ay, x1 = ax, y1 = ay;
    LONG ww, wh, n, i, start, goal, thr = mag_contrast * 255 / 100;
    for (i = 0; i < ntrail; i++) {
        if (trx[i] < x0) x0 = trx[i];
        if (trx[i] > x1) x1 = trx[i];
        if (try_[i] < y0) y0 = try_[i];
        if (try_[i] > y1) y1 = try_[i];
    }
    if (bx < x0) x0 = bx;
    if (bx > x1) x1 = bx;
    if (by < y0) y0 = by;
    if (by > y1) y1 = by;
    x0 -= w; y0 -= w; x1 += w + 1; y1 += w + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ew) x1 = ew;
    if (y1 > eh) y1 = eh;
    ww = x1 - x0;
    wh = y1 - y0;
    if (ww > WIN_MAX || wh > WIN_MAX || !grow(ww * wh))
        return 0;
    n = ww * wh;
    for (i = 0; i < n; i++) {
        dist[i] = 0xFFFFFFFF;
        from[i] = 255;
        allow[i] = 0;
    }
    /* Band um die Spur: Anker -> Spurpunkte -> Maus, alle w/2 px eine Scheibe */
    {
        LONG px = ax, py = ay, k;
        for (k = 0; k <= ntrail; k++) {
            LONG qx = k < ntrail ? trx[k] : bx, qy = k < ntrail ? try_[k] : by;
            LONG dx = qx - px, dy = qy - py, len = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy), st, t;
            st = len / (w / 2 + 1) + 1;
            for (t = 0; t <= st; t++)
                stamp(px + dx * t / st, py + dy * t / st, w, x0, y0, ww, wh);
            px = qx;
            py = qy;
        }
    }
    start = (ay - y0) * ww + (ax - x0);
    goal = (by - y0) * ww + (bx - x0);
    dist[start] = 0;
    from[start] = 8;
    heapn = 0;
    if (!heap_push(0, start))
        return 0;
    while (heapn) {
        HeapEnt e = heap_pop();
        LONG cx, cy, k;
        if (e.d != dist[e.n])
            continue;               /* veraltet */
        if (e.n == goal)
            break;
        cx = e.n % ww;
        cy = e.n / ww;
        for (k = 0; k < 8; k++) {
            LONG nx = cx + ddx[k], ny = cy + ddy[k], ni, ed, c;
            ULONG nd;
            if (nx < 0 || ny < 0 || nx >= ww || ny >= wh)
                continue;
            ni = ny * ww + nx;
            if (!allow[ni])
                continue;
            ed = edge[(ny + y0) * ew + nx + x0];
            if (ed < thr)
                ed = 0;
            c = 1 + (((255 - ed) * 40) >> 8);   /* Kante 1, Flaeche bis 40 */
            nd = e.d + (k < 4 ? 2 * c : 3 * c);  /* diagonal ~ 1,5-fach */
            if (nd < dist[ni]) {
                dist[ni] = nd;
                from[ni] = k;
                if (!heap_push(nd, ni))
                    return 0;
            }
        }
    }
    if (from[goal] == 255)
        return 0;
    /* rueckwaerts sammeln */
    for (i = goal, n = 0; from[i] != 8; n++)
        i = (i % ww - ddx[from[i]]) + (i / ww - ddy[from[i]]) * ww;
    if (!tmp_grow(n + 1))
        return 0;
    {
        LONG k = n;
        i = goal;
        while (from[i] != 8) {
            k--;
            tmpx[k] = i % ww + x0;
            tmpy[k] = i / ww + y0;
            i = (i % ww - ddx[from[i]]) + (i / ww - ddy[from[i]]) * ww;
        }
    }
    return n;
}

/* Punkt anhaengen; gerade Laeufe werden zusammengefasst (Mitte faellt weg) */
static void put(LONG x, LONG y)
{
    float fx = x + 0.5f, fy = y + 0.5f;
    if (path_n >= 2 && path_n - 1 >= path_fix) {
        float ax = path_x[path_n - 2], ay = path_y[path_n - 2];
        float bx = path_x[path_n - 1], by = path_y[path_n - 1];
        float ux = bx - ax, uy = by - ay, vx = fx - bx, vy = fy - by;
        /* gleiche Richtung: Kreuzprodukt 0, Skalarprodukt > 0 */
        if (ux * vy == uy * vx && ux * vx + uy * vy > 0.0f) {
            path_x[path_n - 1] = fx;
            path_y[path_n - 1] = fy;
            return;
        }
    }
    if (path_n < MAX_PATH) {
        path_x[path_n] = fx;
        path_y[path_n] = fy;
        path_n++;
    }
}

/* beweglichen Teil vom letzten Anker zu (x,y) neu berechnen */
static void live_to(LONG x, LONG y)
{
    LONG ax, ay, n, i;
    path_n = path_fix;
    ax = (LONG)path_x[path_fix - 1];
    ay = (LONG)path_y[path_fix - 1];
    if (ax == x && ay == y)
        return;
    n = find_path(ax, ay, x, y);
    if (!n) {                   /* zu weit / kein Speicher: gerade Linie */
        put(x, y);
        return;
    }
    for (i = 0; i < n; i++)
        put(tmpx[i], tmpy[i]);
}

/* bis zum Index k (im beweglichen Teil) festschreiben, dort neuer Anker */
static void fix_to(LONG k)
{
    LONG ax = (LONG)path_x[k], ay = (LONG)path_y[k], i, best = 0, bd = 0x7FFFFFFF;
    path_fix = k + 1;
    if (nanch < MAX_ANCH)
        anch[nanch++] = k;
    /* Spur ab dem Punkt, der dem neuen Anker am naechsten liegt, behalten */
    for (i = 0; i < ntrail; i++) {
        LONG d = (trx[i] - ax) * (trx[i] - ax) + (try_[i] - ay) * (try_[i] - ay);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (ntrail) {
        ntrail -= best + 1;
        memmove(trx, trx + best + 1, ntrail * sizeof(WORD));
        memmove(try_, try_ + best + 1, ntrail * sizeof(WORD));
    }
}

static void clampxy(float fx, float fy, LONG *x, LONG *y)
{
    *x = (LONG)fx;
    *y = (LONG)fy;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x >= ew) *x = ew - 1;
    if (*y >= eh) *y = eh - 1;
}

BOOL mag_begin(float fx, float fy)
{
    LONG x, y;
    mag_end();
    if (!build_edges()) {
        free_bufs();
        return FALSE;
    }
    clampxy(fx, fy, &x, &y);
    path_n = path_fix = 0;
    put(x, y);
    path_fix = 1;
    anch[0] = 0;
    nanch = 1;
    lastx = x;
    lasty = y;
    ntrail = 0;
    active = TRUE;
    view_path_changed();
    return TRUE;
}

/* Abstand der automatischen Anker in px (Frequenz 0..100 wie Photoshop, 57 = ~40 px) */
static LONG spacing(void)
{
    return 12 + (100 - mag_freq) * 65 / 100;
}

/* Maus: nur merken; gerechnet wird in mag_update, wenn alle Ereignisse abgearbeitet sind */
void mag_move(float fx, float fy)
{
    if (!active)
        return;
    clampxy(fx, fy, &lastx, &lasty);
    if (ntrail < MAX_TRAIL && (!ntrail || trx[ntrail - 1] != lastx || try_[ntrail - 1] != lasty)) {
        trx[ntrail] = lastx;
        try_[ntrail] = lasty;
        ntrail++;
    }
    pending = TRUE;
}

void mag_update(void)
{
    LONG x = lastx, y = lasty, sp = spacing();
    if (!active || !pending)
        return;
    pending = FALSE;
    live_to(x, y);
    /* langer beweglicher Teil: vordere Haelfte festschreiben, Rest neu suchen */
    while (path_n - path_fix > 2 * sp / 3 + 4) {
        LONG k = path_fix - 1, len = 0;
        /* nach Laenge (nicht Punkten) teilen: zusammengefasste Laeufe sind lang */
        while (k + 1 < path_n) {
            float dx = path_x[k + 1] - path_x[k], dy = path_y[k + 1] - path_y[k];
            len += (LONG)(dx < 0 ? -dx : dx) + (LONG)(dy < 0 ? -dy : dy);
            k++;
            if (len >= sp)
                break;
        }
        if (k + 1 >= path_n)
            break;
        fix_to(k);
        live_to(x, y);
    }
    view_path_changed();
}

/* Klick: Anker an der Maus */
void mag_click(float fx, float fy)
{
    if (!active)
        return;
    mag_move(fx, fy);
    mag_update();
    fix_to(path_n - 1);
    view_path_changed();
}

/* letzten Anker zuruecknehmen; FALSE: keiner mehr (Werkzeug beendet) */
BOOL mag_back(void)
{
    if (!active)
        return FALSE;
    if (nanch <= 1) {
        mag_end();
        return FALSE;
    }
    nanch--;
    path_fix = anch[nanch - 1] + 1;
    ntrail = 0;
    live_to(lastx, lasty);
    view_path_changed();
    return TRUE;
}

/* zum Startpunkt schliessen; Punkte bleiben in path_x/y (n >= 3: Auswahl moeglich) */
LONG mag_close(void)
{
    LONG n;
    if (!active)
        return 0;
    /* bewegliche Linie zur Maus festhalten, dann zum Start: sonst ginge sie verloren,
     * solange noch kein Zwischenanker gesetzt war (Weg Start -> Start) */
    mag_update();               /* noch nicht ausgewertete Mausposition */
    if (path_n > path_fix)
        fix_to(path_n - 1);
    live_to((LONG)path_x[0], (LONG)path_y[0]);
    n = path_n;
    if (n > 1 && path_x[n - 1] == path_x[0] && path_y[n - 1] == path_y[0])
        n--;                    /* Startpunkt nicht doppelt */
    path_n = n;
    active = FALSE;
    free_bufs();
    return n;
}

/* Bildschirm: liegt (sx,sy) am Startpunkt? */
BOOL mag_near_start(LONG sx, LONG sy)
{
    LONG fx, fy;
    if (!active || nanch < 2)
        return FALSE;
    fx = doc2scrx(path_x[0]);
    fy = doc2scry(path_y[0]);
    return sx - fx <= 6 && fx - sx <= 6 && sy - fy <= 6 && fy - sy <= 6;
}
