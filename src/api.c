// api.c - LAN HTTP API for pixel2mqtt. Unity-included into pixeld.c (after the MQTT/HTTP code, before main).
//
// Everything runs non-blocking inside pixeld's 30 fps main loop (no threads): up to NCONN connections,
// uploads are streamed straight to disk, long-polls are parked until an event arrives.
//
//   GET    /api/info                      no auth: id, name, model, size, paired/pairing flags
//   POST   /api/pair                      no auth: start pairing, the panel shows a 6-digit code for 120 s
//   POST   /api/pair?code=123456          no auth: returns {"token":"..."} if the code matches (5 tries)
//   DELETE /api/pair                      forget the calling token
//   GET    /api/storage                   free/total bytes of /data and /mnt/UDISK
//   GET    /api/discovery                 every MQTT message pixeld would publish on connect (discovery + state)
//   GET    /api/events?since=N&wait=25&mqtt=1  long-poll for messages pixeld published since sequence N
//                                         (mqtt=1: the caller is connected to the broker and owns MQTT)
//   POST   /api/cmd/<name>                body = payload; same as MQTT govee-pixel/<name>/set
//   GET    /api/frame                     the last rendered frame (4992 bytes RGB)
//   GET    /api/files/<kind>              kind = anims | icons | sounds | screens; JSON list
//   GET    /api/screens                   layout screens: name, file, order, visible, missing values, errors
//   GET|PUT|DELETE /api/files/<kind>/<file>
//   GET    /api/res?path=/usr/res/...     read-only stock resources (file, or JSON listing for a directory);
//                                         also the scene folders the Govee app filled while on stock firmware:
//                                         /mnt/UDISK/data/{gif,gra_file,looplay_A,big_data,dial_mode,dial_effect}
// Other auth: "Authorization: Bearer <token>". Tokens live in /data/custom/api.tokens (one per line).
// UDP 7778: a datagram starting with "pixel2mqtt?" is answered with the /api/info JSON (LAN discovery).
// While a paired client long-polls /api/events with mqtt=1, pixeld's own MQTT client stays off (pixel2mqtt owns MQTT).
// After MANAGED_TIMEOUT s without a poll, pixeld goes back to direct MQTT (if mqtt.conf exists).
#include <sys/statvfs.h>

#define API_TOKENS "/data/custom/api.tokens"
#define API_SOUND_DIR "/mnt/UDISK/sounds"
#define DISCOVERY_PORT 7778
#define MANAGED_TIMEOUT 300.0
#define NCONN 8
#define MAXTOK 8
#define SMALL_BODY 65536

/* ------------------------------------------------------------------ string builder */
typedef struct { char *b; size_t n, cap; } sb_t;
static void sb_need(sb_t *s, size_t more) {
    if (s->n + more + 1 <= s->cap) return;
    size_t c = s->cap ? s->cap : 1024; while (c < s->n + more + 1) c *= 2;
    char *nb = realloc(s->b, c); if (!nb) return;
    s->b = nb; s->cap = c;
}
static void sb_raw(sb_t *s, const void *d, size_t n) { sb_need(s, n); if (s->n + n + 1 > s->cap) return; memcpy(s->b + s->n, d, n); s->n += n; s->b[s->n] = 0; }
static void sb_f(sb_t *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    if (n < 0) return;
    sb_need(s, (size_t)n); if (s->n + n + 1 > s->cap) return;
    va_start(ap, fmt); vsnprintf(s->b + s->n, (size_t)n + 1, fmt, ap); va_end(ap); s->n += n;
}
static void sb_js(sb_t *s, const char *p, size_t n) {  // JSON string literal
    sb_raw(s, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; sb_raw(s, e, 2); }
        else if (c < 0x20) sb_f(s, "\\u%04x", c);
        else sb_raw(s, &p[i], 1);
    }
    sb_raw(s, "\"", 1);
}
static void sb_put(void *ctx, const char *d, size_t n) { sb_raw((sb_t *)ctx, d, n); }
static void sb_free(sb_t *s) { free(s->b); memset(s, 0, sizeof *s); }

/* ------------------------------------------------------------------ tokens, pairing */
static char tokens[MAXTOK][33]; static int ntokens;
static double api_last_poll;
static double api_rescan_at;  // rescan + republish discovery at this time (debounced after file changes)
static char pair_code[7]; static double pair_until; static int pair_tries;

static int rand_bytes(uint8_t *b, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY); if (fd < 0) return -1;
    ssize_t r = read(fd, b, n); close(fd); return r == (ssize_t)n ? 0 : -1;
}
static void tokens_load(void) {
    ntokens = 0; FILE *f = fopen(API_TOKENS, "r"); char l[80];
    if (!f) return;
    while (fgets(l, sizeof l, f) && ntokens < MAXTOK) { l[strcspn(l, "\r\n ")] = 0; if (strlen(l) == 32) snprintf(tokens[ntokens++], 33, "%s", l); }
    fclose(f);
}
static void tokens_save(void) {
    FILE *f = fopen(API_TOKENS ".tmp", "w"); if (!f) return;
    for (int i = 0; i < ntokens; i++) fprintf(f, "%s\n", tokens[i]);
    fclose(f); chmod(API_TOKENS ".tmp", 0600); rename(API_TOKENS ".tmp", API_TOKENS);
}
static int token_find(const char *t) {
    if (!t || strlen(t) != 32) return -1;
    for (int i = 0; i < ntokens; i++) { int d = 0; for (int k = 0; k < 32; k++) d |= tokens[i][k] ^ t[k]; if (!d) return i; }
    return -1;
}
static int api_managed(double t) { return ntokens > 0 && t - api_last_poll < MANAGED_TIMEOUT; }

static void draw_pair(double t) {
    static const uint8_t c1[3] = {80, 200, 255}, c2[3] = {255, 255, 255};
    text_at("PAIR", (W - text_w("PAIR")) / 2, 3, c1);
    text_at(pair_code, (W - text_w(pair_code)) / 2, 14, c2);
    int w = (int)((pair_until - t) / 120.0 * W); for (int i = 0; i < w; i++) px(i, 28, c1);
}

/* ------------------------------------------------------------------ outbox (events for the bridge) */
#define OUTBOX 256
static struct { char *t, *p; size_t pn; int r; } ob[OUTBOX];
static uint32_t ob_seq;          // sequence number of the newest message (0 = none yet)
static sb_t cap; static int cap_n;

static void api_outbox(const char *topic, const void *payload, size_t n, int retain) {
    if (api_capturing) {
        if (cap_n++) sb_raw(&cap, ",", 1);
        sb_raw(&cap, "{\"t\":", 5);
        sb_js(&cap, topic, strlen(topic)); sb_raw(&cap, ",\"p\":", 5); sb_js(&cap, payload, n);
        sb_f(&cap, ",\"r\":%d}", retain ? 1 : 0);
        return;
    }
    if (!ntokens) return;
    int i = (int)(++ob_seq % OUTBOX);
    free(ob[i].t); free(ob[i].p);
    ob[i].t = strdup(topic); ob[i].p = malloc(n + 1);
    if (ob[i].p) { memcpy(ob[i].p, payload, n); ob[i].p[n] = 0; }
    ob[i].pn = ob[i].p ? n : 0; ob[i].r = retain;
}

/* ------------------------------------------------------------------ device info */
static void dev_id(char *out, size_t on) {
    FILE *f = fopen("/sys/class/net/wlan0/address", "r"); char m[32] = "";
    if (f) { if (!fgets(m, sizeof m, f)) m[0] = 0; fclose(f); }
    size_t o = 0; for (char *p = m; *p && o + 1 < on; p++) if (isxdigit((unsigned char)*p)) out[o++] = (char)tolower((unsigned char)*p);
    out[o] = 0; if (!o) snprintf(out, on, "unknown");
}
static void info_json(sb_t *s) {
    char id[32], host[64] = "govee-pixel"; dev_id(id, sizeof id); gethostname(host, sizeof host - 1);
    sb_f(s, "{\"api\":1,\"app\":\"pixeld\",\"id\":\"%s\",\"name\":", id); sb_js(s, host, strlen(host));
    sb_f(s, ",\"model\":\"H6631\",\"w\":%d,\"h\":%d,\"udp\":%d,\"paired\":%s,\"pairing\":%s,\"mqtt_direct\":%s}",
         W, H, UDP_PORT, ntokens ? "true" : "false", now_s() < pair_until ? "true" : "false", mq >= 0 ? "true" : "false");
}

/* ------------------------------------------------------------------ files */
// Paths /api/res may read: the stock resources, and the scene folders the Govee app wrote (read-only, no "..").
static int under(const char *p, const char *dir) {
    size_t n = strlen(dir);
    return !strncmp(p, dir, n) && (p[n] == 0 || p[n] == '/');
}
static int res_allowed(const char *p) {
    static const char *app[] = {"gif", "gra_file", "looplay_A", "big_data", "dial_mode", "dial_effect"};
    char d[64];
    if (strstr(p, "..")) return 0;
    if (under(p, "/usr/res")) return 1;
    if (!strcmp(p, "/mnt/UDISK/data")) return 1;   // listing only shows which folders exist
    for (size_t i = 0; i < sizeof app / sizeof *app; i++) {
        snprintf(d, sizeof d, "/mnt/UDISK/data/%s", app[i]);
        if (under(p, d)) return 1;
    }
    return 0;
}

// stat() is unusable in the zig/glibc-2.29 ARM build (it always fails: the libc stub's struct stat/__xstat
// handling doesn't match the device glibc), so probe paths with opendir/open+lseek instead.
// Returns 0 = missing, 1 = regular file (size set), 2 = directory.
static int path_kind(const char *p, long long *size) {
    DIR *d = opendir(p); if (d) { closedir(d); if (size) *size = 0; return 2; }
    int fd = open(p, O_RDONLY); if (fd < 0) return 0;
    off_t n = lseek(fd, 0, SEEK_END); close(fd);
    if (size) *size = n < 0 ? 0 : (long long)n;
    return 1;
}

typedef struct { const char *kind, *dir; const char *ext[3]; size_t max; } kind_t;
static const kind_t KINDS[] = {
    {"anims", ANIM_DIR, {".anim", ".clock", NULL}, 48u << 20},
    {"icons", ICON_DIR, {".pxi", NULL, NULL}, 2u << 20},
    {"sounds", API_SOUND_DIR, {".mp3", ".wav", ".ogg"}, 16u << 20},
    {"screens", SCR_DIR, {".scr", NULL, NULL}, 16u << 10},
};
static const char *SOUND_DIRS_RW[] = {API_SOUND_DIR, "/data/custom/sounds"};
static const char *SOUND_DIRS_RO[] = {"/usr/res/sound/tips", "/usr/res/sound"};

static const kind_t *kind_get(const char *k) { for (size_t i = 0; i < sizeof KINDS / sizeof KINDS[0]; i++) if (!strcmp(KINDS[i].kind, k)) return &KINDS[i]; return NULL; }
static int name_ok(const kind_t *k, const char *n) {
    size_t l = strlen(n); if (!l || l > 68 || n[0] == '.') return 0;
    for (const char *p = n; *p; p++) if ((unsigned char)*p < 0x20 || *p == '/' || *p == '\\' || *p == '"' || *p == 0x7f) return 0;
    const char *dot = strrchr(n, '.'); if (!dot || dot - n > 63) return 0;
    for (int i = 0; i < 3; i++) if (k->ext[i] && !strcasecmp(dot, k->ext[i])) return 1;
    return 0;
}
static void list_dir(sb_t *s, const char *dir, const kind_t *k, int ro, int *first) {
    DIR *dp = opendir(dir); struct dirent *e; if (!dp) return;
    while ((e = readdir(dp))) {
        if (!name_ok(k, e->d_name)) continue;
        char p[256]; long long fsz; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (path_kind(p, &fsz) != 1) continue;
        sb_raw(s, *first ? "{\"name\":" : ",{\"name\":", *first ? 8 : 9); *first = 0;
        sb_js(s, e->d_name, strlen(e->d_name));
        sb_f(s, ",\"size\":%lld,\"ro\":%s,\"dir\":", fsz, ro ? "true" : "false"); sb_js(s, dir, strlen(dir));
        sb_raw(s, "}", 1);
    }
    closedir(dp);
}
// locate an existing file of a kind; writable dirs first. Returns 1 if found (path filled), *ro set.
static int file_find(const kind_t *k, const char *name, char *path, size_t pn, int *ro) {
    *ro = 0;
    if (strcmp(k->kind, "sounds")) { snprintf(path, pn, "%s/%s", k->dir, name); return path_kind(path, NULL) == 1; }
    for (int i = 0; i < 2; i++) { snprintf(path, pn, "%s/%s", SOUND_DIRS_RW[i], name); if (path_kind(path, NULL) == 1) return 1; }
    for (int i = 0; i < 2; i++) { snprintf(path, pn, "%s/%s", SOUND_DIRS_RO[i], name); if (path_kind(path, NULL) == 1) { *ro = 1; return 1; } }
    return 0;
}
static void storage_json(sb_t *s) {
    const char *names[2] = {"data", "udisk"}, *paths[2] = {"/data", "/mnt/UDISK"};
    sb_raw(s, "{", 1);
    for (int i = 0; i < 2; i++) {
        struct statvfs v; unsigned long long tot = 0, fr = 0;
        if (!statvfs(paths[i], &v)) { tot = (unsigned long long)v.f_blocks * v.f_frsize; fr = (unsigned long long)v.f_bavail * v.f_frsize; }
        sb_f(s, "%s\"%s\":{\"path\":\"%s\",\"total\":%llu,\"free\":%llu}", i ? "," : "", names[i], paths[i], tot, fr);
    }
    sb_f(s, ",\"anims\":%d,\"sounds\":%d}", nanims, nsnd);
}
static unsigned long long free_bytes(const char *dir) { struct statvfs v; return statvfs(dir, &v) ? 0 : (unsigned long long)v.f_bavail * v.f_frsize; }

// after a file changed: drop caches of that name so the next use reloads it
static void files_changed(const kind_t *k, const char *name) {
    if (!strcmp(k->kind, "anims")) {
        char stem[72]; snprintf(stem, sizeof stem, "%s", name); char *d = strrchr(stem, '.'); if (d) *d = 0;
        if (!strcmp(cur_anim.name, stem)) anim_free(&cur_anim);
        if (!strcmp(clock_anim.name, stem)) anim_free(&clock_anim);
        if (!strcmp(notify_anim.name, stem)) anim_free(&notify_anim);
    }
    if (!strcmp(k->kind, "icons")) icons_flush();
    api_rescan_at = now_s() + 2;  // bulk uploads: one rescan after the last file
}

/* ------------------------------------------------------------------ connections */
typedef struct {
    int fd, st;                       // st: 0 free, 1 reading, 2 sending, 3 parked long-poll
    char h[4096]; size_t hn, hend;
    char method[8], path[512]; int authed, tok; size_t clen, got;
    sb_t body;
    FILE *up; char up_tmp[200], up_dst[200]; const kind_t *upk; char up_name[72];
    sb_t out; size_t sent;
    double last, wait_until; uint32_t since; int owns_mqtt;
} conn_t;
static conn_t conns[NCONN];

static void conn_free(conn_t *k) {
    if (k->fd >= 0) close(k->fd);
    if (k->up) { fclose(k->up); unlink(k->up_tmp); }
    sb_free(&k->body); sb_free(&k->out);
    memset(k, 0, sizeof *k); k->fd = -1;
}
static void reply(conn_t *k, const char *status, const char *ctype, const void *data, size_t n) {
    sb_free(&k->out);
    sb_f(&k->out, "HTTP/1.1 %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n", status, ctype, n);
    if (n) sb_raw(&k->out, data, n);
    k->sent = 0; k->st = 2;
}
static void reply_sb(conn_t *k, const char *status, sb_t *s) { reply(k, status, "application/json", s->b ? s->b : "", s->n); sb_free(s); }
static void reply_err(conn_t *k, const char *status, const char *msg) {
    sb_t s = {0}; sb_raw(&s, "{\"error\":", 9); sb_js(&s, msg, strlen(msg)); sb_raw(&s, "}", 1); reply_sb(k, status, &s);
}
static void reply_ok(conn_t *k) { reply(k, "200 OK", "application/json", "{\"ok\":true}", 11); }

static void pct_decode(char *s) {  // %XX only ('+' stays: these are paths)
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) { char hx[3] = {s[1], s[2], 0}; *o++ = (char)strtol(hx, NULL, 16); s += 2; }
        else *o++ = *s;
    }
    *o = 0;
}
static int query_get(const char *path, const char *key, char *out, size_t on) {
    const char *q = strchr(path, '?'); if (!q) return -1;
    if (form_get(q + 1, key, out, on)) return -1;  // form_get url-decodes
    return 0;
}

static void reply_file(conn_t *k, const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { reply_err(k, "404 Not Found", "not found"); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (64L << 20)) { fclose(f); reply_err(k, "413 Payload Too Large", "file too large"); return; }
    sb_free(&k->out);
    sb_f(&k->out, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: %ld\r\n\r\n", n);
    sb_need(&k->out, (size_t)n);
    if (k->out.cap < k->out.n + (size_t)n + 1 || fread(k->out.b + k->out.n, 1, (size_t)n, f) != (size_t)n) { fclose(f); reply_err(k, "500 Internal Server Error", "read failed"); return; }
    k->out.n += (size_t)n; fclose(f); k->sent = 0; k->st = 2;
}

static void events_reply(conn_t *k) {
    sb_t s = {0};
    uint32_t oldest = ob_seq >= OUTBOX ? ob_seq - OUTBOX + 1 : 1;
    int reset = k->since > ob_seq || (k->since + 1 < oldest);
    sb_f(&s, "{\"seq\":%u,\"reset\":%s,\"msgs\":[", ob_seq, reset ? "true" : "false");
    if (!reset) for (uint32_t q = k->since + 1, first = 1; q <= ob_seq; q++) {
        int i = (int)(q % OUTBOX); if (!ob[i].t) continue;
        sb_raw(&s, first ? "{\"t\":" : ",{\"t\":", first ? 5 : 6); first = 0;
        sb_js(&s, ob[i].t, strlen(ob[i].t)); sb_raw(&s, ",\"p\":", 5); sb_js(&s, ob[i].p ? ob[i].p : "", ob[i].pn);
        sb_f(&s, ",\"r\":%d}", ob[i].r);
    }
    sb_raw(&s, "]}", 2);
    reply_sb(k, "200 OK", &s);
}

static void http_page(conn_t *k, const char *req, const char *body);  // setup page (pixeld.c)

// headers complete: decide what to do with the body
static void on_headers(conn_t *k) {
    char *line_end = strstr(k->h, "\r\n"); if (!line_end) { reply_err(k, "400 Bad Request", "bad request"); return; }
    char target[512] = "";
    if (sscanf(k->h, "%7s %511s", k->method, target) != 2) { reply_err(k, "400 Bad Request", "bad request"); return; }
    snprintf(k->path, sizeof k->path, "%s", target);
    char *cl = strcasestr(k->h, "\r\nContent-Length:"); k->clen = cl ? (size_t)strtoull(cl + 17, NULL, 10) : 0;
    if (strcasestr(k->h, "\r\nTransfer-Encoding:")) { reply_err(k, "411 Length Required", "chunked bodies are not supported"); return; }
    char *au = strcasestr(k->h, "\r\nAuthorization: Bearer ");
    k->tok = -1;
    if (au) { char t[40] = ""; sscanf(au + 24, "%39s", t); k->tok = token_find(t); }
    k->authed = k->tok >= 0;
    if (!strcmp(k->method, "PUT") && !strncmp(k->path, "/api/files/", 11)) {
        if (!k->authed) { reply_err(k, "401 Unauthorized", "pair first"); return; }
        char kind[16] = "", name[200] = "";
        const char *p = k->path + 11, *sl = strchr(p, '/');
        if (!sl || sl - p >= (int)sizeof kind) { reply_err(k, "400 Bad Request", "bad path"); return; }
        memcpy(kind, p, sl - p); kind[sl - p] = 0; snprintf(name, sizeof name, "%s", sl + 1); pct_decode(name);
        const kind_t *kd = kind_get(kind);
        if (!kd || !name_ok(kd, name)) { reply_err(k, "400 Bad Request", "bad kind or file name"); return; }
        if (!k->clen || k->clen > kd->max) { reply_err(k, "413 Payload Too Large", "empty or too large"); return; }
        mkdir(kd->dir, 0755);
        if ((unsigned long long)k->clen + (1u << 20) > free_bytes(kd->dir)) { reply_err(k, "507 Insufficient Storage", "not enough free space"); return; }
        snprintf(k->up_dst, sizeof k->up_dst, "%s/%s", kd->dir, name);
        snprintf(k->up_tmp, sizeof k->up_tmp, "%s/.upload-%d.tmp", kd->dir, k->fd);
        k->up = fopen(k->up_tmp, "wb"); k->upk = kd; snprintf(k->up_name, sizeof k->up_name, "%s", name);
        if (!k->up) { reply_err(k, "500 Internal Server Error", "cannot write"); return; }
    } else if (k->clen > SMALL_BODY) { reply_err(k, "413 Payload Too Large", "body too large"); return; }
}

static void dispatch(conn_t *k) {
    double t = now_s();
    char path[512]; snprintf(path, sizeof path, "%s", k->path);
    char *q = strchr(path, '?'); if (q) *q = 0;
    const char *m = k->method;
    if (strncmp(path, "/api/", 5)) {  // setup / status page
        char *hdr_end = strstr(k->h, "\r\n\r\n"); if (hdr_end) hdr_end[2] = 0;
        http_page(k, k->h, k->body.b ? k->body.b : "");
        return;
    }
    if (!strcmp(path, "/api/info")) { sb_t s = {0}; info_json(&s); reply_sb(k, "200 OK", &s); return; }
    if (!strcmp(path, "/api/pair") && !strcmp(m, "POST")) {
        char code[16];
        if (query_get(k->path, "code", code, sizeof code)) {  // start pairing
            if (ntokens >= MAXTOK) { reply_err(k, "409 Conflict", "too many paired clients"); return; }
            if (t >= pair_until) {
                uint8_t r[4]; if (rand_bytes(r, 4)) { reply_err(k, "500 Internal Server Error", "no randomness"); return; }
                snprintf(pair_code, sizeof pair_code, "%06u", (unsigned)((r[0] | r[1] << 8 | r[2] << 16 | (uint32_t)r[3] << 24) % 1000000u));
                pair_tries = 0; logf_("pairing started");
#ifdef PIXELD_STUB_DISPLAY
                logf_("test build: pairing code %s", pair_code);  // no panel in the host test
#endif
            }
            pair_until = t + 120;
            static const char pj[] = "{\"pairing\":true,\"seconds\":120}";
            reply(k, "200 OK", "application/json", pj, sizeof pj - 1); return;
        }
        if (t >= pair_until) { reply_err(k, "409 Conflict", "no pairing in progress"); return; }
        if (strcmp(code, pair_code)) {
            if (++pair_tries >= 5) { pair_until = 0; logf_("pairing cancelled after 5 wrong codes"); }
            reply_err(k, "403 Forbidden", "wrong code"); return;
        }
        uint8_t r[16]; if (rand_bytes(r, 16)) { reply_err(k, "500 Internal Server Error", "no randomness"); return; }
        char tok[33]; for (int i = 0; i < 16; i++) snprintf(tok + i * 2, 3, "%02x", r[i]);
        snprintf(tokens[ntokens++], 33, "%s", tok); tokens_save();
        pair_until = 0; logf_("paired a new client (%d total)", ntokens);
        sb_t s = {0}; sb_f(&s, "{\"token\":\"%s\"}", tok); reply_sb(k, "200 OK", &s); return;
    }
    if (!k->authed) { reply_err(k, "401 Unauthorized", "pair first"); return; }
    if (!strcmp(path, "/api/pair") && !strcmp(m, "DELETE")) {
        for (int i = k->tok; i < ntokens - 1; i++) memcpy(tokens[i], tokens[i + 1], 33);
        ntokens--; tokens_save(); logf_("client unpaired (%d left)", ntokens); reply_ok(k); return;
    }
    if (!strcmp(path, "/api/storage")) { sb_t s = {0}; storage_json(&s); reply_sb(k, "200 OK", &s); return; }
    if (!strcmp(path, "/api/frame")) { reply(k, "200 OK", "application/octet-stream", fb, FRAME); return; }
    char mq_flag[4] = "";  // ?mqtt=1: the client is connected to MQTT and takes over (editor-only clients don't)
    int owns = !query_get(k->path, "mqtt", mq_flag, sizeof mq_flag) && mq_flag[0] == '1';
    if (!strcmp(path, "/api/discovery")) {
        if (owns) api_last_poll = t;
        sb_free(&cap); cap_n = 0; api_capturing = 1;
        publish_discovery(); publish_state();
        api_capturing = 0;
        sb_t s = {0}; sb_f(&s, "{\"seq\":%u,\"base\":\"" BASE "\",\"disc\":\"" DISC "\",\"msgs\":[", ob_seq);
        if (cap.b) sb_raw(&s, cap.b, cap.n);
        sb_raw(&s, "]}", 2); sb_free(&cap);
        reply_sb(k, "200 OK", &s); return;
    }
    if (!strcmp(path, "/api/events")) {
        char v[24]; k->owns_mqtt = owns; if (owns) api_last_poll = t;
        k->since = query_get(k->path, "since", v, sizeof v) ? ob_seq : (uint32_t)strtoul(v, NULL, 10);
        double wait = query_get(k->path, "wait", v, sizeof v) ? 25 : atof(v); if (wait > 25) wait = 25; if (wait < 0) wait = 0;
        if (ob_seq != k->since || wait == 0) { events_reply(k); return; }
        k->st = 3; k->wait_until = t + wait; return;
    }
    if (!strcmp(path, "/api/screens") && !strcmp(m, "GET")) { sb_t s = {0}; scr_json(sb_put, &s); reply_sb(k, "200 OK", &s); return; }
    if (!strncmp(path, "/api/cmd/", 9) && !strcmp(m, "POST")) {
        const char *name = path + 9;
        if (!*name || strlen(name) > 40 || strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789_") != strlen(name)) { reply_err(k, "400 Bad Request", "bad command"); return; }
        char topic[96]; snprintf(topic, sizeof topic, BASE "/%s/set", name);
        on_message(topic, (const uint8_t *)(k->body.b ? k->body.b : ""), k->body.n);
        reply_ok(k); return;
    }
    if (!strcmp(path, "/api/res")) {
        char rp[300];
        if (query_get(k->path, "path", rp, sizeof rp) || !res_allowed(rp)) {
            reply_err(k, "400 Bad Request", "path must be under /usr/res or a Govee app scene folder in /mnt/UDISK/data"); return;
        }
        int pk = path_kind(rp, NULL);
        if (!pk) { reply_err(k, "404 Not Found", "not found"); return; }
        if (pk == 1) { reply_file(k, rp); return; }
        DIR *dp = opendir(rp); struct dirent *e; sb_t s = {0}; int first = 1;
        sb_raw(&s, "[", 1);
        while (dp && (e = readdir(dp))) {
            if (e->d_name[0] == '.') continue;
            char fp[600]; long long esz; snprintf(fp, sizeof fp, "%s/%s", rp, e->d_name);
            int ek = path_kind(fp, &esz); if (!ek) continue;
            sb_raw(&s, first ? "{\"name\":" : ",{\"name\":", first ? 8 : 9); first = 0;
            sb_js(&s, e->d_name, strlen(e->d_name));
            sb_f(&s, ",\"dir\":%s,\"size\":%lld}", ek == 2 ? "true" : "false", esz);
        }
        if (dp) closedir(dp);
        sb_raw(&s, "]", 1); reply_sb(k, "200 OK", &s); return;
    }
    if (!strncmp(path, "/api/files/", 11)) {
        char kind[16] = "", name[200] = ""; const char *p = path + 11, *sl = strchr(p, '/');
        size_t kl = sl ? (size_t)(sl - p) : strlen(p); if (kl >= sizeof kind) kl = sizeof kind - 1;
        memcpy(kind, p, kl); kind[kl] = 0;
        if (sl) { snprintf(name, sizeof name, "%s", sl + 1); pct_decode(name); }
        const kind_t *kd = kind_get(kind); if (!kd) { reply_err(k, "404 Not Found", "unknown kind"); return; }
        if (!name[0] && !strcmp(m, "GET")) {
            sb_t s = {0}; int first = 1; sb_raw(&s, "[", 1);
            if (!strcmp(kd->kind, "sounds")) {
                for (int i = 0; i < 2; i++) list_dir(&s, SOUND_DIRS_RW[i], kd, 0, &first);
                for (int i = 0; i < 2; i++) list_dir(&s, SOUND_DIRS_RO[i], kd, 1, &first);
            } else list_dir(&s, kd->dir, kd, 0, &first);
            sb_raw(&s, "]", 1); reply_sb(k, "200 OK", &s); return;
        }
        if (!name_ok(kd, name)) { reply_err(k, "400 Bad Request", "bad file name"); return; }
        char fp[300]; int ro;
        if (!file_find(kd, name, fp, sizeof fp, &ro)) { reply_err(k, "404 Not Found", "not found"); return; }
        if (!strcmp(m, "GET")) { reply_file(k, fp); return; }
        if (!strcmp(m, "DELETE")) {
            if (ro) { reply_err(k, "403 Forbidden", "built-in file"); return; }
            if (unlink(fp)) { reply_err(k, "500 Internal Server Error", strerror(errno)); return; }
            logf_("api: deleted %s", fp); files_changed(kd, name); reply_ok(k); return;
        }
    }
    reply_err(k, "404 Not Found", "unknown endpoint");
}

static void upload_done(conn_t *k) {
    if (fclose(k->up)) { k->up = NULL; unlink(k->up_tmp); reply_err(k, "500 Internal Server Error", "write failed"); return; }
    k->up = NULL;
    // light sanity check of the container formats
    const char *dot = strrchr(k->up_name, '.'); char magic[4] = {0};
    FILE *f = fopen(k->up_tmp, "rb"); if (f) { if (fread(magic, 1, 4, f) != 4) memset(magic, 0, 4); fclose(f); }
    if ((!strcasecmp(dot, ".anim") && memcmp(magic, "PXA1", 4)) || (!strcasecmp(dot, ".pxi") && memcmp(magic, "PXI1", 4)) ||
        (!strcasecmp(dot, ".scr") && memcmp(magic, "PXS1", 4))) {
        unlink(k->up_tmp); reply_err(k, "400 Bad Request", "not a PXA1/PXI1/PXS1 file"); return;
    }
    if (rename(k->up_tmp, k->up_dst)) { unlink(k->up_tmp); reply_err(k, "500 Internal Server Error", strerror(errno)); return; }
    logf_("api: stored %s (%zu bytes)", k->up_dst, k->got);
    files_changed(k->upk, k->up_name);
    reply_ok(k);
}

static void conn_read(conn_t *k) {
    uint8_t buf[16384];
    for (;;) {
        ssize_t r = recv(k->fd, buf, sizeof buf, 0);
        if (r == 0) { conn_free(k); return; }
        if (r < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) conn_free(k); return; }
        k->last = now_s();
        size_t off = 0;
        if (!k->hend) {
            size_t before = k->hn, take = (size_t)r; if (take > sizeof k->h - 1 - k->hn) take = sizeof k->h - 1 - k->hn;
            memcpy(k->h + k->hn, buf, take); k->hn += take; k->h[k->hn] = 0;
            char *e = strstr(k->h, "\r\n\r\n");
            if (!e) { if (k->hn >= sizeof k->h - 1) { reply_err(k, "431 Request Header Fields Too Large", "headers too large"); return; } continue; }
            k->hend = (size_t)(e + 4 - k->h);
            off = k->hend - before;                     // bytes of buf before the body starts
            k->h[k->hend] = 0;
            on_headers(k); if (k->st != 1) return;
        }
        size_t n = (size_t)r - off;
        if (n > k->clen - k->got) n = k->clen - k->got;
        if (n) {
            if (k->up) { if (fwrite(buf + off, 1, n, k->up) != n) { reply_err(k, "500 Internal Server Error", "write failed"); return; } }
            else sb_raw(&k->body, buf + off, n);
            k->got += n;
        }
        if (k->got >= k->clen) { if (k->up) upload_done(k); else dispatch(k); return; }
    }
}

static void conn_write(conn_t *k) {
    while (k->sent < k->out.n) {
        ssize_t w = send(k->fd, k->out.b + k->sent, k->out.n - k->sent, MSG_NOSIGNAL);
        if (w < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) conn_free(k); return; }
        k->sent += (size_t)w; k->last = now_s();
    }
    conn_free(k);
}

static void api_accept(int ls) {
    for (;;) {
        int c = accept(ls, NULL, NULL); if (c < 0) return;
        conn_t *k = NULL; for (int i = 0; i < NCONN; i++) if (conns[i].st == 0) { k = &conns[i]; break; }
        if (!k) { close(c); continue; }
        memset(k, 0, sizeof *k); k->fd = c; k->st = 1; k->last = now_s();
        fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
        int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
}

static int api_busy_upload(void) { for (int i = 0; i < NCONN; i++) if (conns[i].up) return 1; return 0; }

// per loop: timeouts, parked long-polls, rescans after file changes
static void api_tick(void) {
    double t = now_s();
    for (int i = 0; i < NCONN; i++) {
        conn_t *k = &conns[i];
        if (k->st == 3) { if (k->owns_mqtt) api_last_poll = t; if (ob_seq != k->since || t >= k->wait_until) events_reply(k); }
        else if ((k->st == 1 || k->st == 2) && t - k->last > 30) conn_free(k);
    }
    if (api_rescan_at && t >= api_rescan_at && !api_busy_upload()) {
        api_rescan_at = 0;
        sound_scan(); anim_scan(); scr_scan();
        publish_discovery(); publish_state();  // new dropdown options (MQTT and/or the bridge)
    }
    if (pair_until && t >= pair_until) { pair_until = 0; logf_("pairing timed out"); }
}

static int api_pollfds(struct pollfd *p) {
    int n = 0;
    for (int i = 0; i < NCONN; i++) {
        if (conns[i].st == 1) { p[n].fd = conns[i].fd; p[n].events = POLLIN; n++; }
        else if (conns[i].st == 2) { p[n].fd = conns[i].fd; p[n].events = POLLOUT; n++; }
    }
    return n;
}
static void api_io(int fd, short rev) {
    for (int i = 0; i < NCONN; i++) {
        conn_t *k = &conns[i]; if (!k->st || k->fd != fd) continue;
        if (k->st == 1 && (rev & (POLLIN | POLLERR | POLLHUP))) conn_read(k);
        if (k->st == 2) conn_write(k);  // also right after a request completed (no extra loop round trip)
        return;
    }
}

// UDP discovery: answer "pixel2mqtt?" with the info JSON
static void disco_read(int s) {
    char b[64]; struct sockaddr_in from; socklen_t fl = sizeof from;
    ssize_t r;
    while ((r = recvfrom(s, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl)) > 0) {
        b[r] = 0;
        if (strncmp(b, "pixel2mqtt?", 11)) continue;
        sb_t s2 = {0}; info_json(&s2);
        if (s2.b) sendto(s, s2.b, s2.n, 0, (struct sockaddr *)&from, fl);
        sb_free(&s2); fl = sizeof from;
    }
}

static void api_init(void) {
    for (int i = 0; i < NCONN; i++) conns[i].fd = -1;
    tokens_load();
    if (ntokens) api_last_poll = now_s() - MANAGED_TIMEOUT + 30;  // 30 s for the bridge to reconnect after a pixeld restart
    mkdir(API_SOUND_DIR, 0755);
    mkdir(SCR_DIR, 0755);
}
