/*
 * net.c - HTTP(S)-Client (Roadshow + AmiSSL v5), Base64, Mini-JSON
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "net.h"
#include "version.h"

struct Library *SocketBase;
struct Library *AmiSSLMasterBase;
struct Library *AmiSSLBase;
struct Library *AmiSSLExtBase;

/* Speicher ueber exec statt malloc: die Anfrage laeuft in einem eigenen
 * Prozess, und malloc von libnix ist nicht fuer mehrere Tasks gedacht */
void *net_alloc(LONG n)
{
    ULONG *p = AllocVec(n + 4, MEMF_ANY);
    if (!p)
        return NULL;
    p[0] = n;
    return p + 1;
}

void net_freemem(void *p)
{
    if (p)
        FreeVec((ULONG *)p - 1);
}

static void *net_realloc(void *p, LONG n)
{
    void *q = net_alloc(n);
    if (q && p) {
        LONG o = ((ULONG *)p)[-1];
        CopyMem(p, q, o < n ? o : n);
    }
    if (q)
        net_freemem(p);
    return q;
}

static void seterr(NetResp *r, const char *a, const char *b)
{
    strncpy(r->err, a, sizeof(r->err) - 1);
    r->err[sizeof(r->err) - 1] = 0;
    if (b)
        strncat(r->err, b, sizeof(r->err) - 1 - strlen(r->err));
}

static BOOL ssl_open(NetResp *r, BOOL tls)
{
    if (!(SocketBase = OpenLibrary("bsdsocket.library", 4))) {
        seterr(r, "No TCP/IP stack (bsdsocket.library)", NULL);
        return FALSE;
    }
    if (!tls)                   /* einfaches HTTP (z. B. ComfyUI im LAN): ohne AmiSSL */
        return TRUE;
    if (!(AmiSSLMasterBase = OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION))) {
        seterr(r, "AmiSSL v5 not installed (amisslmaster.library)", NULL);
        return FALSE;
    }
    if (OpenAmiSSLTags(AMISSL_V3xx,
                       AmiSSL_UsesOpenSSLStructs, FALSE,
                       AmiSSL_InitAmiSSL, TRUE,
                       AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                       AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                       AmiSSL_SocketBase, (ULONG)SocketBase,
                       AmiSSL_ErrNoPtr, (ULONG)&errno,
                       TAG_DONE) != 0) {
        AmiSSLBase = NULL;
        seterr(r, "Cannot open AmiSSL", NULL);
        return FALSE;
    }
    return TRUE;
}

static void ssl_close(void)
{
    if (AmiSSLBase) {
        CloseAmiSSL();
        AmiSSLBase = AmiSSLExtBase = NULL;
    }
    if (AmiSSLMasterBase) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
    }
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

/* Wartet bis Socket lesbar/schreibbar; FALSE bei Abbruch oder Zeitueberschreitung */
static BOOL wait_sock(LONG s, BOOL wr, NetCtl *ctl, LONG *idle)
{
    for (;;) {
        fd_set fs;
        struct timeval tv;
        LONG n;
        if (ctl && ctl->cancel)
            return FALSE;
        FD_ZERO(&fs);
        FD_SET(s, &fs);
        tv.tv_sec = 0;
        tv.tv_usec = 250000;
        n = WaitSelect(s + 1, wr ? NULL : &fs, wr ? &fs : NULL, NULL, &tv, NULL);
        if (n > 0) {
            *idle = 0;
            return TRUE;
        }
        if (n < 0)
            return FALSE;
        if (++*idle > 4 * ((ctl && ctl->timeout) ? ctl->timeout : 120))
            return FALSE;
    }
}

/* Chunked-Transfer-Encoding an Ort und Stelle aufloesen */
static LONG dechunk(UBYTE *b, LONG n)
{
    LONG i = 0, o = 0;
    while (i < n) {
        LONG sz = 0;
        while (i < n) {
            UBYTE c = b[i];
            if (c >= '0' && c <= '9') sz = sz * 16 + c - '0';
            else if (c >= 'a' && c <= 'f') sz = sz * 16 + c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') sz = sz * 16 + c - 'A' + 10;
            else break;
            i++;
        }
        while (i < n && b[i] != '\n')
            i++;
        i++;
        if (sz == 0 || i + sz > n)
            break;
        memmove(b + o, b + i, sz);
        o += sz;
        i += sz + 2;
    }
    return o;
}

static BOOL ieq(const char *a, const char *b, LONG n)  /* ohne Gross/klein */
{
    while (n-- > 0) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y)
            return FALSE;
    }
    return TRUE;
}

static const char *find_hdr(const char *h, const char *end, const char *name)
{
    LONG nl = strlen(name);
    while (h < end) {
        const char *e = h;
        while (e < end && *e != '\n')
            e++;
        if (e - h > nl && ieq(h, name, nl) && h[nl] == ':') {
            h += nl + 1;
            while (*h == ' ')
                h++;
            return h;
        }
        h = e + 1;
    }
    return NULL;
}

/* Senden/Empfangen ueber TLS oder direkt ueber den Socket */
static LONG io_write(SSL *ssl, LONG s, const void *b, LONG n)
{
    return ssl ? SSL_write(ssl, b, n) : send(s, (APTR)b, n, 0);
}

static LONG io_read(SSL *ssl, LONG s, void *b, LONG n)
{
    return ssl ? SSL_read(ssl, b, n) : recv(s, b, n, 0);
}

BOOL net_https(const char *host, const char *method, const char *path,
               const char *hdrs, const void *body, LONG blen,
               NetResp *r, NetCtl *ctl)
{
    return net_http(host, 443, TRUE, method, path, hdrs, body, blen, r, ctl);
}

BOOL net_http(const char *host, LONG port, BOOL tls, const char *method, const char *path,
              const char *hdrs, const void *body, LONG blen,
              NetResp *r, NetCtl *ctl)
{
    struct hostent *he;
    struct sockaddr_in sa;
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    LONG s = -1, idle = 0, cap, n, hl;
    char *req = NULL;
    UBYTE *buf = NULL, *p;
    BOOL ok = FALSE;

    memset(r, 0, sizeof(*r));
    if (!ssl_open(r, tls))
        goto out;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if ((sa.sin_addr.s_addr = inet_addr((STRPTR)host)) == INADDR_NONE) {    /* keine IP-Adresse */
        if (!(he = gethostbyname((STRPTR)host))) {
            seterr(r, "Host not found: ", host);
            goto out;
        }
        memcpy(&sa.sin_addr, he->h_addr, he->h_length);
    }
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        seterr(r, "socket() failed", NULL);
        goto out;
    }
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        seterr(r, "Cannot connect to ", host);
        goto out;
    }

    if (!tls)
        goto plain;
    if (!(ctx = SSL_CTX_new(TLS_client_method()))) {
        seterr(r, "SSL_CTX_new failed", NULL);
        goto out;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    if (!(ssl = SSL_new(ctx))) {
        seterr(r, "SSL_new failed", NULL);
        goto out;
    }
    SSL_set_tlsext_host_name(ssl, host);
    SSL_set1_host(ssl, host);
    SSL_set_fd(ssl, s);
    if (SSL_connect(ssl) != 1) {
        long v = SSL_get_verify_result(ssl);
        if (v != X509_V_OK)
            seterr(r, "Certificate error: ", X509_verify_cert_error_string(v));
        else
            seterr(r, "TLS handshake failed", NULL);
        goto out;
    }

plain:
    /* Anfrage */
    if (!(req = net_alloc(512 + strlen(path) + (hdrs ? strlen(hdrs) : 0)))) {
        seterr(r, "Out of memory", NULL);
        goto out;
    }
    /* ohne sprintf: laeuft auch im Hintergrundprozess */
    {
        char num[12];
        LONG i = 11;
        ULONG v = blen;
        num[i] = 0;
        do {
            num[--i] = '0' + v % 10;
            v /= 10;
        } while (v);
        strcpy(req, method);
        strcat(req, " ");
        strcat(req, path);
        strcat(req, " HTTP/1.1\r\nHost: ");
        strcat(req, host);
        if (port != (tls ? 443 : 80)) {
            char pn[8];
            LONG k = 7;
            ULONG pv = port;
            pn[k] = 0;
            do {
                pn[--k] = '0' + pv % 10;
                pv /= 10;
            } while (pv && k > 1);
            pn[--k] = ':';
            strcat(req, pn + k);
        }
        strcat(req, "\r\nUser-Agent: AmiPixelShop/" APS_VER_STR " (AmigaOS)\r\n"
                    "Accept-Encoding: identity\r\nConnection: close\r\n");
        if (hdrs)
            strcat(req, hdrs);
        strcat(req, "Content-Length: ");
        strcat(req, num + i);
        strcat(req, "\r\n\r\n");
    }
    if (io_write(ssl, s, req, strlen(req)) <= 0) {
        seterr(r, "Send failed", NULL);
        goto out;
    }
    for (n = 0; n < blen;) {
        LONG k = blen - n > 16384 ? 16384 : blen - n, w;
        if (ctl && ctl->cancel) {
            seterr(r, "Cancelled", NULL);
            goto out;
        }
        if ((w = io_write(ssl, s, (const UBYTE *)body + n, k)) <= 0) {
            seterr(r, "Send failed", NULL);
            goto out;
        }
        n += w;
        if (ctl)
            ctl->sent = n;
    }

    /* Antwort bis Verbindungsende lesen */
    cap = 65536;
    if (!(buf = net_alloc(cap + 1))) {
        seterr(r, "Out of memory", NULL);
        goto out;
    }
    for (n = 0;;) {
        LONG k;
        if (cap - n < 16384) {
            UBYTE *nb = net_realloc(buf, cap * 2 + 1);
            if (!nb) {
                seterr(r, "Out of memory", NULL);
                goto out;
            }
            buf = nb;
            cap *= 2;
        }
        if (!(ssl && SSL_pending(ssl)) && !wait_sock(s, FALSE, ctl, &idle)) {
            seterr(r, (ctl && ctl->cancel) ? "Cancelled" : "Timeout", NULL);
            goto out;
        }
        k = io_read(ssl, s, buf + n, cap - n);
        if (k <= 0)
            break;
        n += k;
        if (ctl)
            ctl->recvd = n;
    }
    buf[n] = 0;

    /* Kopf auswerten */
    p = (UBYTE *)strstr((char *)buf, "\r\n\r\n");
    if (n < 12 || strncmp((char *)buf, "HTTP/1.", 7) || !p) {
        seterr(r, "Invalid HTTP response", NULL);
        goto out;
    }
    r->status = atol((char *)buf + 9);
    hl = p + 4 - buf;
    {
        const char *te = find_hdr((char *)buf, (char *)p, "Transfer-Encoding");
        const char *loc = r->status >= 300 && r->status < 400 ?
                          find_hdr((char *)buf, (char *)p, "Location") : NULL;
        if (loc) {
            LONG i;
            for (i = 0; i < (LONG)sizeof(r->location) - 1 && loc[i] != '\r' && loc[i] != '\n'; i++)
                r->location[i] = loc[i];
            r->location[i] = 0;
        }
        BOOL chunked = te && ieq(te, "chunked", 7);
        LONG bl = n - hl;
        memmove(buf, buf + hl, bl);
        if (chunked)
            bl = dechunk(buf, bl);
        buf[bl] = 0;
        r->body = buf;
        r->len = bl;
        buf = NULL;
    }
    ok = TRUE;

out:
    net_freemem(buf);
    net_freemem(req);
    if (ssl) {
        if (ok)
            SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    if (ctx)
        SSL_CTX_free(ctx);
    if (s >= 0)
        CloseSocket(s);
    ssl_close();
    return ok;
}

void net_free(NetResp *r)
{
    net_freemem(r->body);
    r->body = NULL;
    r->len = 0;
}

/* https://host/pfad zerlegen; host max. 127 Zeichen */
static BOOL split_url(const char *url, char *host, const char **path)
{
    LONG i;
    if (strncmp(url, "https://", 8))
        return FALSE;
    url += 8;
    for (i = 0; url[i] && url[i] != '/' && i < 127; i++)
        host[i] = url[i];
    host[i] = 0;
    *path = url[i] == '/' ? url + i : "/";
    return i > 0 && (!url[i] || url[i] == '/');
}

BOOL net_get_url(const char *url, const char *hdrs, NetResp *r, NetCtl *ctl)
{
    char host[128], *cur;
    const char *path;
    LONG hop;
    if (!(cur = net_alloc(1024))) {
        memset(r, 0, sizeof(*r));
        seterr(r, "Out of memory", NULL);
        return FALSE;
    }
    strncpy(cur, url, 1023);
    cur[1023] = 0;
    for (hop = 0;; hop++) {
        char *frag = strchr(cur, '#');     /* Fragment geht nicht an den Server (Dropbox haengt "#" an) */
        if (frag)
            *frag = 0;
        if (!split_url(cur, host, &path)) {
            memset(r, 0, sizeof(*r));
            seterr(r, "Invalid URL", NULL);
            break;
        }
        if (!net_https(host, "GET", path, hdrs, NULL, 0, r, ctl))
            break;
        if (r->status < 300 || r->status >= 400 || !r->location[0]) {
            net_freemem(cur);
            return TRUE;
        }
        if (hop == 5) {
            seterr(r, "Too many redirects", NULL);
            net_free(r);
            break;
        }
        strcpy(cur, r->location);
        net_free(r);
        hdrs = NULL;            /* z. B. Accept nur fuer die erste Adresse */
    }
    net_freemem(cur);
    return FALSE;
}

/* ---------- Base64 ---------- */

static const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *b64_encode(const UBYTE *s, LONG n, LONG *outlen)
{
    LONG ol = (n + 2) / 3 * 4, i;
    char *o = net_alloc(ol + 1), *d = o;
    if (!o)
        return NULL;
    for (i = 0; i + 2 < n; i += 3) {
        ULONG v = (s[i] << 16) | (s[i + 1] << 8) | s[i + 2];
        d[0] = b64c[v >> 18];
        d[1] = b64c[(v >> 12) & 63];
        d[2] = b64c[(v >> 6) & 63];
        d[3] = b64c[v & 63];
        d += 4;
    }
    if (i < n) {
        ULONG v = s[i] << 16;
        if (i + 1 < n)
            v |= s[i + 1] << 8;
        d[0] = b64c[v >> 18];
        d[1] = b64c[(v >> 12) & 63];
        d[2] = i + 1 < n ? b64c[(v >> 6) & 63] : '=';
        d[3] = '=';
        d += 4;
    }
    *d = 0;
    if (outlen)
        *outlen = ol;
    return o;
}

LONG b64_decode(const char *s, LONG n, UBYTE *dst)
{
    static BYTE tab[256];
    static BOOL init;
    ULONG v = 0;
    LONG i, bits = 0, o = 0;
    if (!init) {
        memset(tab, -1, sizeof(tab));
        for (i = 0; i < 64; i++)
            tab[(UBYTE)b64c[i]] = i;
        tab['-'] = 62;                  /* base64url */
        tab['_'] = 63;
        init = TRUE;
    }
    for (i = 0; i < n; i++) {
        BYTE c = tab[(UBYTE)s[i]];
        if (c < 0) {
            if (s[i] == '=' || s[i] == '"')
                break;
            continue;                   /* \n, \\ usw. ueberspringen */
        }
        v = (v << 6) | c;
        if ((bits += 6) >= 8) {
            bits -= 8;
            dst[o++] = v >> bits;
        }
    }
    return o;
}

/* ---------- Mini-JSON ---------- */

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

static const char *skip_str(const char *p)     /* p auf ", liefert hinter " */
{
    for (p++; *p && *p != '"'; p++)
        if (*p == '\\' && p[1])
            p++;
    return *p ? p + 1 : p;
}

static const char *skip_val(const char *p)
{
    LONG depth = 0;
    p = ws(p);
    if (*p == '"')
        return skip_str(p);
    if (*p != '{' && *p != '[') {
        while (*p && *p != ',' && *p != '}' && *p != ']' && *p > ' ')
            p++;
        return p;
    }
    for (; *p; p++) {
        if (*p == '"') {
            p = skip_str(p) - 1;
        } else if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (--depth == 0)
                return p + 1;
        }
    }
    return p;
}

const char *json_get(const char *js, const char *path, LONG *len)
{
    const char *p = ws(js);
    while (*path) {
        char key[64];
        LONG kl = 0;
        while (*path && *path != '.' && kl < 63)
            key[kl++] = *path++;
        key[kl] = 0;
        if (*path == '.')
            path++;
        if (*p == '[') {
            LONG idx = atol(key);
            p = ws(p + 1);
            if (*p == ']')
                return NULL;
            while (idx-- > 0) {
                p = ws(skip_val(p));
                if (*p != ',')
                    return NULL;
                p = ws(p + 1);
            }
        } else if (*p == '{') {
            p = ws(p + 1);
            for (;;) {
                const char *k, *ke;
                if (*p != '"')
                    return NULL;
                k = p + 1;
                ke = skip_str(p);
                p = ws(ke);
                if (*p != ':')
                    return NULL;
                p = ws(p + 1);
                if (ke - 1 - k == kl && !memcmp(k, key, kl))
                    break;
                p = ws(skip_val(p));
                if (*p != ',')
                    return NULL;
                p = ws(p + 1);
            }
        } else {
            return NULL;
        }
    }
    if (len)
        *len = skip_val(p) - p;
    return p;
}

LONG json_str(const char *v, LONG vlen, char *dst, LONG dmax)
{
    const char *e = v + vlen;
    LONG o = 0;
    if (v < e && *v == '"')
        v++;
    while (v < e && *v != '"' && o < dmax - 1) {
        char c = *v++;
        if (c == '\\' && v < e) {
            c = *v++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                ULONG u = 0;
                LONG i;
                for (i = 0; i < 4 && v < e; i++, v++) {
                    char h = *v;
                    u = u * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                }
                c = u < 256 ? (char)u : '?';    /* Latin-1 */
                break;
            }
            }
        }
        dst[o++] = c;
    }
    dst[o] = 0;
    return o;
}

LONG json_esc(const char *s, char *dst, LONG dmax)
{
    LONG o = 0;
    for (; *s && o < dmax - 7; s++) {
        UBYTE c = *s;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = c;
        } else if (c == '\n') {
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else if (c < 32) {
            dst[o++] = ' ';
        } else {                        /* auch UTF-8-Bytes unveraendert */
            dst[o++] = c;
        }
    }
    dst[o] = 0;
    return o;
}
