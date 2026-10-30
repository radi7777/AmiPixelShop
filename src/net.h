/*
 * net.h - HTTP(S) ueber Roadshow (bsdsocket) + AmiSSL v5, Base64, Mini-JSON
 *
 * Alle Netzfunktionen oeffnen bsdsocket/AmiSSL im aufrufenden Task selbst
 * (Bibliotheksbasen sind dort pro Task) - so laeuft eine Anfrage auch in
 * einem eigenen Hintergrundprozess.
 */
#ifndef APX_NET_H
#define APX_NET_H

#include <exec/types.h>

typedef struct NetResp {
    LONG  status;           /* HTTP-Status, 0 = keine Antwort */
    UBYTE *body;            /* nullterminiert, mit net_free() freigeben */
    LONG  len;
    char  err[128];         /* Fehlertext bei FALSE */
    char  location[1024];   /* Ziel einer Weiterleitung (3xx), sonst leer */
} NetResp;

typedef struct NetCtl {
    volatile LONG cancel;   /* != 0: abbrechen */
    volatile LONG sent;     /* gesendete Bytes */
    volatile LONG recvd;    /* empfangene Bytes */
    LONG timeout;           /* Sekunden ohne Daten bis Abbruch, 0 = 120 */
} NetCtl;

/* hdrs: zusaetzliche Kopfzeilen, jede mit \r\n abgeschlossen (oder NULL) */
BOOL net_https(const char *host, const char *method, const char *path,
               const char *hdrs, const void *body, LONG blen,
               NetResp *r, NetCtl *ctl);
/* wie net_https, mit Port; tls FALSE: einfaches HTTP ohne AmiSSL */
BOOL net_http(const char *host, LONG port, BOOL tls, const char *method, const char *path,
              const char *hdrs, const void *body, LONG blen,
              NetResp *r, NetCtl *ctl);
void net_free(NetResp *r);
/* GET einer https-URL, folgt bis zu 5 Weiterleitungen */
BOOL net_get_url(const char *url, const char *hdrs, NetResp *r, NetCtl *ctl);
void *net_alloc(LONG n);         /* AllocVec mit Laenge; in jedem Task nutzbar */
void net_freemem(void *p);

char *b64_encode(const UBYTE *src, LONG n, LONG *outlen);   /* net_alloc, nullterminiert */
LONG  b64_decode(const char *src, LONG n, UBYTE *dst);      /* dst >= n*3/4, liefert Laenge */

/* Wert zu einem Pfad wie "choices.0.message.images.0.image_url.url";
 * liefert Zeiger auf den Rohwert (bei Strings ab dem ") und dessen Laenge */
const char *json_get(const char *js, const char *path, LONG *len);
/* String-Wert (inkl. ") entschluesseln, dst nullterminiert; liefert Laenge */
LONG json_str(const char *v, LONG vlen, char *dst, LONG dmax);
/* JSON-String-Escape (UTF-8) von src nach dst, ohne Anfuehrungszeichen, nullterminiert */
LONG json_esc(const char *src, char *dst, LONG dmax);

#endif
