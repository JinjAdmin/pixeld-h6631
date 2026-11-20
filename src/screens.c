/* screens.c - generic info screens (.scr layout files + named values). Spec: docs/SCREENS.md.
 * Unity-included into features.c (needs px/glyph/T3 font/icons/phase/draw_moon from there and pixeld.c).
 *
 * The display knows nothing about printers, weather or markets: a screen file says what to draw and which named
 * value each part shows; values arrive on <base>/values/set (from pixel2mqtt or any MQTT client). */
#ifndef SCR_DIR
#define SCR_DIR "/mnt/UDISK/screens"
#endif
#define NVAL 128
#define NSER 64
#define NSCR 24
#define NEL 32
#define NERR 8
#define VALUES_MAX 16384
static const char *scr_dir = SCR_DIR;  // host preview builds point this elsewhere

/* ------------------------------------------------------------------ values */
enum { V_NONE, V_NUM, V_STR, V_SER };
typedef struct { char name[32]; int type; double num; char str[64]; char unit[8]; int nser; float ser[NSER]; double upd; } val_t;
static val_t vals[NVAL]; static int nvals;
static val_t vbuiltin;  // scratch for built-in and derived values (valid until the next val_get)

static int vname_ok(const char *s) {
    size_t n = strlen(s); if (!n || n > 31) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '.' || *s == '-')) return 0;
    return 1;
}
static val_t *val_slot(const char *name, int create) {
    for (int i = 0; i < nvals; i++) if (!strcmp(vals[i].name, name)) return &vals[i];
    if (!create) return NULL;
    if (nvals >= NVAL) {  // full: reuse the oldest
        int o = 0; for (int i = 1; i < nvals; i++) if (vals[i].upd < vals[o].upd) o = i;
        memset(&vals[o], 0, sizeof vals[o]); snprintf(vals[o].name, sizeof vals[o].name, "%s", name); return &vals[o];
    }
    val_t *v = &vals[nvals++]; memset(v, 0, sizeof *v); snprintf(v->name, sizeof v->name, "%s", name); return v;
}
static void val_del(const char *name) {
    for (int i = 0; i < nvals; i++) if (!strcmp(vals[i].name, name)) { vals[i] = vals[--nvals]; return; }
}

static time_t scr_now(void) { return (time_t)now_s(); }
static const char *WDAY[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *MON3[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

static void fmt_clock(char *b, size_t n, int h, int m) {
    if (clock12) snprintf(b, n, "%d:%02d", (h + 11) % 12 + 1, m); else snprintf(b, n, "%02d:%02d", h, m);
}

// value by name: stored values, built-ins (time, date, ...) and "<series>.trend"
static const val_t *val_get(const char *name) {
    val_t *v = val_slot(name, 0); if (v) return v;
    val_t *b = &vbuiltin; memset(b, 0, sizeof *b); b->upd = now_s();
    time_t n = scr_now(); struct tm t; localtime_r(&n, &t);
    if (!strcmp(name, "time")) { b->type = V_STR; fmt_clock(b->str, sizeof b->str, t.tm_hour, t.tm_min); return b; }
    if (!strcmp(name, "date")) { b->type = V_STR; snprintf(b->str, sizeof b->str, "%s %s %d", WDAY[t.tm_wday], MON3[t.tm_mon], t.tm_mday); return b; }
    if (!strcmp(name, "weekday")) { b->type = V_STR; snprintf(b->str, sizeof b->str, "%s", WDAY[t.tm_wday]); return b; }
    if (!strcmp(name, "month")) { b->type = V_STR; snprintf(b->str, sizeof b->str, "%s", MON3[t.tm_mon]); return b; }
    b->type = V_NUM;
    if (!strcmp(name, "day")) { b->num = t.tm_mday; return b; }
    if (!strcmp(name, "hour")) { b->num = t.tm_hour; return b; }
    if (!strcmp(name, "minute")) { b->num = t.tm_min; return b; }
    if (!strcmp(name, "second")) { b->num = t.tm_sec; return b; }
    if (!strcmp(name, "year")) { b->num = t.tm_year + 1900; return b; }
    size_t l = strlen(name);
    if (l > 6 && !strcmp(name + l - 6, ".trend")) {
        char base[32]; snprintf(base, sizeof base, "%.*s", (int)(l - 6), name);
        val_t *s = val_slot(base, 0);
        if (s && s->type == V_SER && s->nser >= 2) { b->num = s->ser[s->nser - 1] - s->ser[0]; b->upd = s->upd; return b; }
    }
    return NULL;
}
static int val_is_builtin(const char *name) {
    static const char *B[] = {"time", "date", "weekday", "month", "day", "hour", "minute", "second", "year"};
    for (size_t i = 0; i < sizeof B / sizeof B[0]; i++) if (!strcmp(name, B[i])) return 1;
    return 0;
}

// a number in its plain form: integers without decimals, else up to 2 decimals (4 below 1), zeros trimmed
static void num_str(char *b, size_t n, double v) {
    if (fabs(v - llround(v)) < 1e-9 && fabs(v) < 1e15) { snprintf(b, n, "%lld", (long long)llround(v)); return; }
    snprintf(b, n, "%.*f", fabs(v) < 1 ? 4 : 2, v);
    char *d = strchr(b, '.'); if (!d) return;
    char *e = b + strlen(b) - 1; while (e > d && *e == '0') *e-- = 0;
    if (e == d) *e = 0;
}
static void val_text(const val_t *v, char *b, size_t n) {
    if (!v) { b[0] = 0; return; }
    if (v->type == V_STR) snprintf(b, n, "%s", v->str);
    else if (v->type == V_NUM) num_str(b, n, v->num);
    else if (v->type == V_SER && v->nser) num_str(b, n, v->ser[v->nser - 1]);
    else b[0] = 0;
}
static int val_num(const val_t *v, double *out) {
    if (!v) return 0;
    if (v->type == V_NUM) { *out = v->num; return 1; }
    if (v->type == V_SER && v->nser) { *out = v->ser[v->nser - 1]; return 1; }
    if (v->type == V_STR) { char *e; double d = strtod(v->str, &e); if (e != v->str && !*e) { *out = d; return 1; } }
    return 0;
}

/* --- values/set: a flat JSON object --- */
static const char *jws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }
static void utf8_put(char **o, char *end, unsigned cp) {
    char t[4]; int n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | cp >> 6); t[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else { t[0] = (char)(0xE0 | cp >> 12); t[1] = (char)(0x80 | ((cp >> 6) & 63)); t[2] = (char)(0x80 | (cp & 63)); n = 3; }
    if (*o + n < end) { memcpy(*o, t, n); *o += n; }
}
static const char *jstring(const char *p, char *out, size_t on) {  // p at '"'; NULL on error
    char *o = out, *end = out + on - 1; p++;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': if (o < end) *o++ = ' '; p++; break;
            case 't': if (o < end) *o++ = ' '; p++; break;
            case 'u': { unsigned cp = 0; for (int i = 1; i <= 4; i++) { char c = p[i]; cp = cp * 16 + (unsigned)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'); if (!c) return NULL; }
                        utf8_put(&o, end, cp); p += 5; break; }
            case 0: return NULL;
            default: if (o < end) *o++ = *p; p++; break;
            }
        } else { if (o < end) *o++ = *p; p++; }
    }
    *o = 0;
    return *p == '"' ? p + 1 : NULL;
}
static const char *jskip(const char *p) {  // skip any JSON value (for unsupported nested objects)
    p = jws(p);
    if (*p == '"') { char tmp[2]; return jstring(p, tmp, sizeof tmp); }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') { char tmp[2]; p = jstring(p, tmp, sizeof tmp); if (!p) return NULL; p--; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

static int values_message(const uint8_t *pl, size_t n) {
    if (n > VALUES_MAX) { logf_("values: message too large (%zu bytes)", n); return 0; }
    char *buf = malloc(n + 1); if (!buf) return 0;
    memcpy(buf, pl, n); buf[n] = 0;
    const char *p = jws(buf); int count = 0;
    if (*p != '{') { logf_("values: expected a JSON object"); free(buf); return 0; }
    p = jws(p + 1);
    while (*p && *p != '}') {
        char key[48];
        if (*p != '"' || !(p = jstring(p, key, sizeof key))) break;
        for (char *k = key; *k; k++) *k = (char)tolower((unsigned char)*k);
        p = jws(p); if (*p != ':') break; p = jws(p + 1);
        int ok = vname_ok(key);
        if (!ok) logf_("values: bad name '%s'", key);
        if (*p == '"') {
            char sv[64]; if (!(p = jstring(p, sv, sizeof sv))) break;
            if (ok) { val_t *v = val_slot(key, 1); v->type = V_STR; snprintf(v->str, sizeof v->str, "%s", sv); v->unit[0] = 0; v->upd = now_s(); count++; }
        } else if (*p == '[') {
            float ser[NSER]; int ns = 0; char unit[8] = ""; int bad = 0;
            p = jws(p + 1);
            while (*p && *p != ']') {
                if (*p == '"') { if (ns == 1 && !unit[0]) { if (!(p = jstring(p, unit, sizeof unit))) { bad = 1; break; } } else { p = jskip(p); if (!p) { bad = 1; break; } } }
                else if (*p == 'n') { p = jskip(p); if (!p) { bad = 1; break; } }  // null points are skipped
                else { char *e; double d = strtod(p, &e); if (e == p) { bad = 1; break; } if (ns < NSER) ser[ns++] = (float)d; else memmove(ser, ser + 1, sizeof ser[0] * (NSER - 1)), ser[NSER - 1] = (float)d; p = e; }
                p = jws(p); if (*p == ',') p = jws(p + 1);
            }
            if (bad || *p != ']') break;
            p++;
            if (ok) {
                val_t *v = val_slot(key, 1);
                if (unit[0] && ns == 1) { v->type = V_NUM; v->num = ser[0]; snprintf(v->unit, sizeof v->unit, "%s", unit); }
                else { v->type = V_SER; v->nser = ns; memcpy(v->ser, ser, sizeof ser[0] * ns); v->unit[0] = 0; }
                v->upd = now_s(); count++;
            }
        } else if (!strncmp(p, "null", 4)) { p += 4; if (ok) { val_del(key); count++; } }
        else if (!strncmp(p, "true", 4) || !strncmp(p, "false", 5)) {
            int tv = *p == 't'; p += tv ? 4 : 5;
            if (ok) { val_t *v = val_slot(key, 1); v->type = V_STR; snprintf(v->str, sizeof v->str, "%s", tv ? "true" : "false"); v->unit[0] = 0; v->upd = now_s(); count++; }
        } else if (*p == '{') { p = jskip(p); if (!p) break; if (ok) logf_("values: '%s' is an object (not supported)", key); }
        else {
            char *e; double d = strtod(p, &e); if (e == p) break; p = e;
            if (ok) { val_t *v = val_slot(key, 1); v->type = V_NUM; v->num = d; v->unit[0] = 0; v->upd = now_s(); count++; }
        }
        p = jws(p); if (*p == ',') p = jws(p + 1);
    }
    if (*p != '}') logf_("values: JSON error near byte %d", (int)(p - buf));
    free(buf);
    return count;
}

/* ------------------------------------------------------------------ screens */
enum { E_TEXT, E_ICON, E_BAR, E_SPARK, E_RECT, E_LINE, E_PIXEL };
typedef struct {
    int type, line, x, y, w, h, x2, y2;
    int font, align, valign, ovf, spacing, fillrect, area, dot, pulse, dir, blink;
    float speed, vmin, vmax;
    char *text, *cond, *color, *icon, *value, *map, *frame, *fill, *bg, *series;
} el_t;
typedef struct { int line; char msg[56]; } scr_err_t;
typedef struct {
    char file[72], name[48];
    char *needs, *show, *linger; double linger_s, stale_s; int time_s, order, has_bg; uint8_t bg[3];
    int nel; el_t el[NEL];
    int nerr; scr_err_t err[NERR];
    int lin_prev; double lin_start;
} scr_t;
static scr_t *scrs[NSCR]; static int nscr;
static const char *scr_pl_pin;          // playlist item "screen:<name>" (set by render)
static scr_t *scr_check;                // while loading: collect expression errors here
static int scr_check_line;

static void scr_error(scr_t *s, int line, const char *fmt, ...) {
    if (!s || s->nerr >= NERR) return;
    scr_err_t *e = &s->err[s->nerr++]; e->line = line;
    va_list ap; va_start(ap, fmt); vsnprintf(e->msg, sizeof e->msg, fmt, ap); va_end(ap);
}
#define CHECK_ERR(...) do { if (scr_check) scr_error(scr_check, scr_check_line, __VA_ARGS__); } while (0)

static void el_free(el_t *e) {
    free(e->text); free(e->cond); free(e->color); free(e->icon); free(e->value); free(e->map); free(e->frame); free(e->fill); free(e->bg); free(e->series);
}
static void scr_free(scr_t *s) { if (!s) return; for (int i = 0; i < s->nel; i++) el_free(&s->el[i]); free(s->needs); free(s->show); free(s->linger); free(s); }

static double cur_stale = 6 * 3600;   // stale limit of the screen being evaluated (fresh())

/* --- conditions: a==b, a!=b, a<b ..., a in x,y,z, has(a), fresh(a), !<term>; joined by && and || --- */
static void trim(char *s) {
    char *a = s; while (*a == ' ' || *a == '\t') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s); while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}
static void unquote(char *s) { size_t n = strlen(s); if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) { memmove(s, s + 1, n - 2); s[n - 2] = 0; } }
static int lit_cmp(const val_t *v, const char *lit, int *cmp) {  // 1 if comparable; *cmp = sign(v - lit)
    double a, b; char *e;
    b = strtod(lit, &e);
    if (e != lit && !*e && val_num(v, &a)) { *cmp = a < b ? -1 : a > b ? 1 : 0; return 1; }
    char t[64]; val_text(v, t, sizeof t); *cmp = strcasecmp(t, lit); *cmp = *cmp < 0 ? -1 : *cmp > 0; return 1;
}
static int term_eval(char *term, int *missing) {
    trim(term);
    if (term[0] == '!') return !term_eval(term + 1, missing);
    if (!term[0]) { CHECK_ERR("empty condition"); return 0; }
    if (!strncmp(term, "has(", 4) || !strncmp(term, "fresh(", 6)) {
        int fr = term[0] == 'f'; char *a = strchr(term, '(') + 1, *z = strchr(a, ')');
        if (!z) { CHECK_ERR("missing ')'"); return 0; }
        *z = 0; trim(a);
        const val_t *v = val_get(a);
        if (!v) return 0;
        return fr ? (now_s() - v->upd <= cur_stale) : 1;
    }
    char *in = strstr(term, " in ");
    if (in) {
        *in = 0; char name[32]; snprintf(name, sizeof name, "%s", term); trim(name);
        const val_t *v = val_get(name); if (!v) { if (!val_is_builtin(name)) *missing = 1; return 0; }
        char list[160]; snprintf(list, sizeof list, "%s", in + 4);
        for (char *sv, *it = strtok_r(list, ",", &sv); it; it = strtok_r(NULL, ",", &sv)) {
            trim(it); unquote(it); int c; if (lit_cmp(v, it, &c) && c == 0) return 1;
        }
        return 0;
    }
    static const char *OPS[] = {"==", "!=", "<=", ">=", "<", ">"};
    char *best = NULL; int op = -1;
    for (int i = 0; i < 6; i++) { char *f = strstr(term, OPS[i]); if (f && (!best || f < best)) { best = f; op = i; } }
    if (!best) {  // a bare name: true if it exists and is not false/off/0/empty
        const val_t *v = val_get(term); if (!v) { if (!vname_ok(term)) CHECK_ERR("bad condition '%.30s'", term); else *missing = 1; return 0; }
        char t[64]; val_text(v, t, sizeof t);
        return t[0] && strcmp(t, "0") && strcasecmp(t, "false") && strcasecmp(t, "off");
    }
    char name[32], lit[64];
    snprintf(name, sizeof name, "%.*s", (int)(best - term), term); trim(name);
    snprintf(lit, sizeof lit, "%s", best + strlen(OPS[op])); trim(lit); unquote(lit);
    if (scr_check && !vname_ok(name)) CHECK_ERR("bad value name '%.24s'", name);
    const val_t *v = val_get(name);
    if (!v) { *missing = 1; return op == 1; }
    int c; lit_cmp(v, lit, &c);
    switch (op) { case 0: return c == 0; case 1: return c != 0; case 2: return c <= 0; case 3: return c >= 0; case 4: return c < 0; default: return c > 0; }
}
static int cond_eval(const char *cond, int *missing) {
    int miss = 0;
    if (!cond || !*cond) return 1;
    char buf[400]; snprintf(buf, sizeof buf, "%s", cond);
    int any = 0;
    char *p = buf;
    for (;;) {  // OR of AND-groups
        char *o = strstr(p, "||"); if (o) *o = 0;
        int all = 1; char *q = p;
        for (;;) { char *a = strstr(q, "&&"); if (a) *a = 0; if (!term_eval(q, &miss)) all = 0; if (!a) break; q = a + 2; }
        if (all) any = 1;
        if (!o) break; p = o + 2;
    }
    if (missing) *missing = miss;
    return any;
}

/* --- colours: "#rrggbb", rules "cond:#rrggbb;cond:#rrggbb;#default" --- */
static int hexcol(const char *s, uint8_t c[3]) {
    if (s[0] != '#') return 0;
    for (int i = 1; i <= 6; i++) if (!isxdigit((unsigned char)s[i])) return 0;
    unsigned v = (unsigned)strtoul(s + 1, NULL, 16); c[0] = (uint8_t)(v >> 16); c[1] = (uint8_t)(v >> 8); c[2] = (uint8_t)v;
    return 1;
}
static void color_eval(const char *spec, uint8_t c[3], const uint8_t def[3]) {
    memcpy(c, def, 3);
    if (!spec || !*spec) return;
    char buf[400]; snprintf(buf, sizeof buf, "%s", spec);
    for (char *sv, *r = strtok_r(buf, ";", &sv); r; r = strtok_r(NULL, ";", &sv)) {
        trim(r);
        char *k = strstr(r, ":#");
        if (!k) { if (!hexcol(r, c)) CHECK_ERR("bad colour '%.20s'", r); return; }
        *k = 0;
        int dummy; if (cond_eval(r, &dummy)) { if (!hexcol(k + 1, c)) CHECK_ERR("bad colour '%.20s'", k + 1); return; }
        if (scr_check) { uint8_t t[3]; if (!hexcol(k + 1, t)) CHECK_ERR("bad colour '%.20s'", k + 1); }
    }
}

/* --- time parsing for the time/ago formatters: epoch seconds, ISO 8601, or "HH:MM" --- */
static long long days_civil(int y, int m, int d) {
    y -= m <= 2; long long era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}
static int parse_ts(const char *s, double *out) {  // 1 = absolute time parsed
    int Y, M, D, h, mi; double sec = 0; char *e;
    double v = strtod(s, &e);
    if (e != s && !*e && v > 1e8) { *out = v; return 1; }
    if (sscanf(s, "%d-%d-%d%*[T ]%d:%d", &Y, &M, &D, &h, &mi) == 5) {
        const char *p = strchr(s, ':') + 3; if (*p == ':') { sec = strtod(p + 1, &e); p = e; }
        long long ts = days_civil(Y, M, D) * 86400 + h * 3600 + mi * 60;
        int off = 0;
        if (*p == 'Z') off = 0;
        else if (*p == '+' || *p == '-') { int oh = 0, om = 0; sscanf(p + 1, "%d:%d", &oh, &om); off = (oh * 60 + om) * 60 * (*p == '-' ? -1 : 1); }
        else { struct tm t = {0}; t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D; t.tm_hour = h; t.tm_min = mi; t.tm_isdst = -1; *out = (double)mktime(&t) + sec; return 1; }
        *out = (double)(ts - off) + sec; return 1;
    }
    return 0;
}

/* --- templates: "text {name|fmt|fmt:arg} text" --- */
typedef struct { char s[128]; int isnum, fixed; double num; char unit[8]; } fv_t;
static void fv_setnum(fv_t *f, double v) { f->isnum = 1; f->num = v; num_str(f->s, sizeof f->s, v); }
static void apply_fmt(fv_t *f, const char *fmt) {
    char name[16], arg[96] = ""; const char *c = strchr(fmt, ':');
    if (c) { snprintf(name, sizeof name, "%.*s", (int)(c - fmt), fmt); snprintf(arg, sizeof arg, "%s", c + 1); } else snprintf(name, sizeof name, "%s", fmt);
    if (!f->isnum) { char *e; double d = strtod(f->s, &e); if (e != f->s && !*e) { f->isnum = 1; f->num = d; } }
    if (!strcmp(name, "int")) { if (f->isnum) { fv_setnum(f, (double)llround(f->num)); f->fixed = 1; } }
    else if (!strcmp(name, "dec")) { if (f->isnum) { int n = atoi(arg); if (n < 0) n = 0; if (n > 6) n = 6; snprintf(f->s, sizeof f->s, "%.*f", n, f->num); f->fixed = 1; } }
    else if (!strcmp(name, "thousands")) {
        if (f->isnum && !f->fixed && fabs(f->num) >= 1000) snprintf(f->s, sizeof f->s, "%lld", (long long)llround(f->num));
        char o[128]; const char *p = f->s; size_t j = 0;
        if (*p == '-' || *p == '+') o[j++] = *p++;
        size_t nd = strspn(p, "0123456789");
        for (size_t i = 0; i < nd && j < sizeof o - 2; i++) { o[j++] = p[i]; if (i < nd - 1 && (nd - i - 1) % 3 == 0) o[j++] = ','; }
        snprintf(o + j, sizeof o - j, "%s", p + nd); snprintf(f->s, sizeof f->s, "%s", o);
    }
    else if (!strcmp(name, "pct")) { if (f->isnum) snprintf(f->s, sizeof f->s, "%.1f%%", f->num); }
    else if (!strcmp(name, "sign")) { if (f->isnum && f->num > 0 && f->s[0] != '+') { char o[128]; snprintf(o, sizeof o, "+%s", f->s); snprintf(f->s, sizeof f->s, "%s", o); } }
    else if (!strcmp(name, "dur")) {
        if (f->isnum) {
            double mul = !strcmp(arg, "h") ? 3600 : !strcmp(arg, "s") ? 1 : 60;
            long long s = llround(f->num * mul); if (s < 0) s = 0;
            if (s < 60) snprintf(f->s, sizeof f->s, "%lldS", s);
            else if (s < 3600) snprintf(f->s, sizeof f->s, "%lldM", s / 60);
            else if (s < 86400) snprintf(f->s, sizeof f->s, "%lldH%02lldM", s / 3600, s / 60 % 60);
            else snprintf(f->s, sizeof f->s, "%lldD%lldH", s / 86400, s / 3600 % 24);
        }
    }
    else if (!strcmp(name, "temp")) {
        if (f->isnum) {
            int inf = f->unit[0] ? (strchr(f->unit, 'F') || strchr(f->unit, 'f')) : temp_f;
            double c = inf ? (f->num - 32) * 5 / 9 : f->num, d = temp_f ? c * 9 / 5 + 32 : c;
            snprintf(f->s, sizeof f->s, "%lld", (long long)llround(d));
        }
    }
    else if (!strcmp(name, "time") || !strcmp(name, "ago")) {
        double ts; int h, m;
        if (!strcmp(name, "time") && strlen(f->s) <= 8 && sscanf(f->s, "%d:%d", &h, &m) == 2) fmt_clock(f->s, sizeof f->s, h, m);
        else if (parse_ts(f->s, &ts) || (f->isnum && f->num > 1e8 && (ts = f->num, 1))) {
            if (!strcmp(name, "time")) { time_t tt = (time_t)ts; struct tm t; localtime_r(&tt, &t); fmt_clock(f->s, sizeof f->s, t.tm_hour, t.tm_min); }
            else {
                double a = now_s() - ts; if (a < 0) a = 0;
                if (a < 60) snprintf(f->s, sizeof f->s, "JUST NOW");
                else if (a < 3600) snprintf(f->s, sizeof f->s, "%dM AGO", (int)(a / 60));
                else if (a < 86400) snprintf(f->s, sizeof f->s, "%dH AGO", (int)(a / 3600));
                else snprintf(f->s, sizeof f->s, "%dD AGO", (int)(a / 86400));
            }
        }
        f->isnum = 0;
    }
    else if (!strcmp(name, "upper")) { for (char *p = f->s; *p; p++) *p = (char)toupper((unsigned char)*p); f->isnum = 0; }
    else if (!strcmp(name, "lower")) { for (char *p = f->s; *p; p++) *p = (char)tolower((unsigned char)*p); f->isnum = 0; }
    else if (!strcmp(name, "trunc")) { int n = atoi(arg); if (n >= 0 && n < (int)sizeof f->s) f->s[n] = 0; f->isnum = 0; }
    else if (!strcmp(name, "replace")) {  // replace:<from>=<to>, every occurrence
        char *eq = strchr(arg, '='); if (!eq || eq == arg) { CHECK_ERR("replace needs from=to"); return; }
        *eq = 0; const char *from = arg, *to = eq + 1; size_t fl = strlen(from); char o[128]; size_t j = 0;
        for (const char *p = f->s; *p && j < sizeof o - 1;) {
            if (!strncmp(p, from, fl)) { for (const char *q = to; *q && j < sizeof o - 1; q++) o[j++] = *q; p += fl; }
            else o[j++] = *p++;
        }
        o[j] = 0; snprintf(f->s, sizeof f->s, "%s", o); f->isnum = 0;
    }
    else if (!strcmp(name, "or")) { if (!f->s[0]) { snprintf(f->s, sizeof f->s, "%s", arg); f->isnum = 0; } }
    else if (!strcmp(name, "map")) {
        char list[96]; snprintf(list, sizeof list, "%s", arg); const char *def = NULL; char defbuf[48] = "";
        for (char *sv, *it = strtok_r(list, ",", &sv); it; it = strtok_r(NULL, ",", &sv)) {
            char *eq = strchr(it, '='); if (!eq) { CHECK_ERR("map item without '='"); continue; }
            *eq = 0;
            if (!strcmp(it, "*")) { snprintf(defbuf, sizeof defbuf, "%s", eq + 1); def = defbuf; }
            else if (!strcasecmp(it, f->s)) { snprintf(f->s, sizeof f->s, "%s", eq + 1); f->isnum = 0; return; }
        }
        if (def) { snprintf(f->s, sizeof f->s, "%s", def); f->isnum = 0; }
    }
    else CHECK_ERR("unknown formatter '%.15s'", name);
}
static void tmpl_eval(const char *t, char *out, size_t on) {
    size_t o = 0;
    while (*t && o + 1 < on) {
        if (t[0] == '{' && t[1] == '{') { out[o++] = '{'; t += 2; continue; }
        if (t[0] == '}' && t[1] == '}') { out[o++] = '}'; t += 2; continue; }
        if (*t != '{') { out[o++] = *t++; continue; }
        const char *z = strchr(t, '}'); if (!z) { CHECK_ERR("missing '}'"); break; }
        char ex[160]; snprintf(ex, sizeof ex, "%.*s", (int)(z - t - 1), t + 1); t = z + 1;
        char *sv, *nm = strtok_r(ex, "|", &sv); fv_t f; memset(&f, 0, sizeof f);
        if (nm) {
            trim(nm);
            if (scr_check && !vname_ok(nm)) CHECK_ERR("bad value name '%.24s'", nm);
            const val_t *v = val_get(nm);
            if (v) { val_text(v, f.s, sizeof f.s); double d; if (val_num(v, &d)) { f.isnum = 1; f.num = d; } snprintf(f.unit, sizeof f.unit, "%s", v->unit); }
            for (char *fm = strtok_r(NULL, "|", &sv); fm; fm = strtok_r(NULL, "|", &sv)) {
                while (*fm == ' ') fm++;
                if (!strchr(fm, ':')) trim(fm);  // keep spaces inside arguments ("replace:_= ")
                apply_fmt(&f, fm);
            }
        }
        for (const char *p = f.s; *p && o + 1 < on; p++) out[o++] = *p;
    }
    out[o] = 0;
}

/* --- fonts: 0 tiny 3x5 (T3), 1 small 5x7, 2 big 10x14, 3 huge 15x21 --- */
static const int FADV[4] = {4, 6, 12, 18}, FGW[4] = {3, 5, 10, 15}, FH[4] = {5, 7, 14, 21};
static int clip_x0, clip_y0, clip_x1 = W, clip_y1 = H;
static void cpx(int x, int y, const uint8_t *c) { if (x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1) px(x, y, c); }

static unsigned utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s; unsigned cp = *p++;
    if (cp >= 0xC0 && cp < 0xE0 && (*p & 0xC0) == 0x80) { cp = ((cp & 31) << 6) | (*p++ & 63); }
    else if (cp >= 0xE0 && cp < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) { cp = ((cp & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
    else if (cp >= 0x80) { while ((*p & 0xC0) == 0x80) p++; cp = '?'; }
    *s = (const char *)p; return cp;
}
static int str_w(const char *s, int font, int spacing) {
    int n = 0; while (*s) { utf8_next(&s); n++; }
    return n ? n * FADV[font] - (FADV[font] - FGW[font]) + spacing * (n - 1) : 0;
}
static void draw_cp(unsigned cp, int x, int y, int font, const uint8_t *c) {
    int sc = font == 0 ? 1 : font;
    if (cp == 0xB0) {  // degree sign: a small ring in the top-left of the cell
        int r = font == 0 ? 1 : font;
        for (int dy = 0; dy < 3 * r; dy++) for (int dx = 0; dx < 3 * r; dx++) if (!(dx / r == 1 && dy / r == 1)) cpx(x + dx, y + dy, c);
        return;
    }
    if (font == 0) {
        char ch = (char)toupper((int)(cp < 128 ? cp : '?'));
        for (size_t i = 0; i < sizeof T3 / sizeof T3[0]; i++) {
            if (T3[i].c != ch) continue;
            for (int r = 0; r < 5; r++) for (int b = 0; b < 3; b++) if (T3[i].r[r] & (4 >> b)) cpx(x + b, y + r, c);
            return;
        }
        return;  // unsupported in tiny: blank
    }
    unsigned u = cp; if (u < 0x20 || u > 0x7E) u = '?';
    const uint8_t *g = F5[u - 0x20];
    for (int col = 0; col < 5; col++) for (int row = 0; row < 7; row++) if (g[col] & (1 << row))
        for (int dy = 0; dy < sc; dy++) for (int dx = 0; dx < sc; dx++) cpx(x + col * sc + dx, y + row * sc + dy, c);
}
static void draw_str(const char *s, int x, int y, int font, int spacing, const uint8_t *c) {
    while (*s) { unsigned cp = utf8_next(&s); if (x > clip_x0 - 20 && x < clip_x1) draw_cp(cp, x, y, font, c); x += FADV[font] + spacing; }
}
static int tiny_supported(unsigned cp) {
    if (cp == 0xB0) return 1;
    char ch = (char)toupper((int)(cp < 128 ? cp : 1));
    for (size_t i = 0; i < sizeof T3 / sizeof T3[0]; i++) if (T3[i].c == ch) return 1;
    return 0;
}

/* --- drawn built-in icons --- */
static void coin_icon(const char *sym, int x, int y) {  // 9x9 badges: BTC, ETH, otherwise a gold coin with the first letter
    if (!strcasecmp(sym, "ETH")) {
        static const char *E[9] = {"...A...", "..AAB..", ".AAABB.", "AAAABBB", ".AAABB.", "A.AAB.B", ".AAABB.", "..AAB..", "...A..."};
        const uint8_t a[3] = {190, 200, 255}, b[3] = {110, 125, 230};
        for (int j = 0; j < 9; j++) for (int i = 0; i < 7; i++) if (E[j][i] != '.') cpx(x + 1 + i, y + j, E[j][i] == 'A' ? a : b);
        return;
    }
    int btc = !strcasecmp(sym, "BTC");
    const uint8_t o[3] = {247, 147, 26}, g[3] = {230, 190, 50}, w[3] = {255, 255, 255}, d[3] = {90, 60, 0};
    for (int j = 0; j < 9; j++) for (int i = 0; i < 9; i++) { float dx = i - 4.0f, dy = j - 4.0f; if (dx * dx + dy * dy <= 20.5f) cpx(x + i, y + j, btc ? o : g); }
    draw_cp((unsigned)(btc ? 'B' : toupper((unsigned char)(sym[0] ? sym[0] : '?'))), x + 2, y + 1, 1, btc ? w : d);
    if (btc) { cpx(x + 3, y, w); cpx(x + 5, y, w); cpx(x + 3, y + 8, w); cpx(x + 5, y + 8, w); }
}
static int builtin_icon(const char *n, int x, int y, const uint8_t *c) {  // 1 if known
    if (!strcmp(n, "@moon")) { draw_moon(x, y); return 1; }
    if (!strcmp(n, "@degree")) { draw_cp(0xB0, x, y, 1, c); return 1; }
    if (!strcmp(n, "@up") || !strcmp(n, "@down")) {
        int up = n[1] == 'u';
        for (int r = 0; r < 3; r++) { int yy = up ? y + r : y + 2 - r; for (int i = 2 - r; i <= 2 + r; i++) cpx(x + i, yy, c); }
        return 1;
    }
    if (!strncmp(n, "@coin:", 6)) { coin_icon(n + 6, x, y); return 1; }
    return 0;
}
static int icon_draw_frame(const char *name, int x, int y, int fi) {  // fi < 0: animate by time
    icon_t *ic = icon_get(name); if (!ic) return 0;
    if (fi < 0) {
        fi = 0;
        if (ic->n > 1 && ic->total) {
            long ms = (long)fmod(now_s() * 1000.0, (double)ic->total);
            for (fi = 0; fi < ic->n; fi++) { if (ms < ic->delay[fi]) break; ms -= ic->delay[fi]; }
            if (fi >= ic->n) fi = 0;
        }
    }
    if (fi >= ic->n) fi = ic->n - 1;
    const uint8_t *s = ic->rgba + (size_t)ic->w * ic->h * 4 * fi;
    for (int j = 0; j < ic->h; j++) for (int i = 0; i < ic->w; i++) { const uint8_t *p = s + (j * ic->w + i) * 4; if (p[3] >= 128) cpx(x + i, y + j, p); }
    return 1;
}

/* --- element drawing --- */
static const uint8_t C_WHITE[3] = {255, 255, 255};
static void fill_spec(const char *spec, uint8_t a[3], uint8_t b[3], int *grad) {  // "#a" or "#a..#b"
    *grad = 0;
    if (!spec) return;
    const char *d = strstr(spec, "..");
    if (!hexcol(spec, a)) CHECK_ERR("bad fill '%.20s'", spec);
    if (d) { if (hexcol(d + 2, b)) *grad = 1; else CHECK_ERR("bad fill '%.20s'", spec); } else memcpy(b, a, 3);
}
static double el_num(const char *name, double def) {
    if (!name) return def;
    char *e; double d = strtod(name, &e); if (e != name && !*e) return d;
    double v; return val_num(val_get(name), &v) ? v : def;
}
static void draw_line(int x0, int y0, int x1, int y1, const uint8_t *c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int guard = 0; guard < 400; guard++) { cpx(x0, y0, c); if (x0 == x1 && y0 == y1) break; int e2 = 2 * err; if (e2 >= dy) { err += dy; x0 += sx; } if (e2 <= dx) { err += dx; y0 += sy; } }
}

static void el_draw(const el_t *e, double t) {
    uint8_t c[3]; color_eval(e->color, c, C_WHITE);
    switch (e->type) {
    case E_TEXT: {
        char s[160]; tmpl_eval(e->text ? e->text : "", s, sizeof s);
        if (scr_check && e->font == 0) { const char *p = s; while (*p) { unsigned cp = utf8_next(&p); if (!tiny_supported(cp) && cp != ' ') { CHECK_ERR("tiny font can't draw '%c'", cp < 128 ? (char)cp : '?'); break; } } }
        int font = e->font, tw = str_w(s, font, e->spacing);
        if (e->ovf == 2 && e->w > 0) while (font > 0 && tw > e->w) { font--; tw = str_w(s, font, e->spacing); }
        int bw = e->w > 0 ? e->w : tw, bh = e->h > 0 ? e->h : FH[font];
        int x = e->x + (e->align == 1 ? (bw - tw) / 2 : e->align == 2 ? bw - tw : 0);
        int y = e->y + (e->valign == 1 ? (bh - FH[font]) / 2 : e->valign == 2 ? bh - FH[font] : 0);
        if (e->w > 0 || e->h > 0) { clip_x0 = e->x; clip_y0 = e->y; clip_x1 = e->x + bw; clip_y1 = e->y + bh; }
        if (e->ovf == 1 && e->w > 0 && tw > e->w) {
            int gap = 12, period = tw + gap; int off = (int)fmod(t * (e->speed > 0 ? e->speed : 15), (double)period);
            draw_str(s, e->x - off, y, font, e->spacing, c); draw_str(s, e->x - off + period, y, font, e->spacing, c);
        } else draw_str(s, x, y, font, e->spacing, c);
        clip_x0 = clip_y0 = 0; clip_x1 = W; clip_y1 = H;
        break;
    }
    case E_ICON: {
        char name[64] = "";
        if (e->value) {
            char vs[64]; val_text(val_get(e->value), vs, sizeof vs);
            if (e->map) {
                char list[300]; snprintf(list, sizeof list, "%s", e->map); const char *def = NULL;
                for (char *sv, *it = strtok_r(list, ",", &sv); it; it = strtok_r(NULL, ",", &sv)) {
                    char *k = strchr(it, ':'); if (!k) { CHECK_ERR("map item without ':'"); continue; }
                    *k = 0;
                    if (!strcmp(it, "*")) def = k + 1;
                    else if (!strcasecmp(it, vs)) { snprintf(name, sizeof name, "%s", k + 1); break; }
                }
                if (!name[0] && def) snprintf(name, sizeof name, "%s", def);
            } else snprintf(name, sizeof name, "%s", vs);
        } else if (e->icon) { if (strchr(e->icon, '{')) tmpl_eval(e->icon, name, sizeof name); else snprintf(name, sizeof name, "%s", e->icon); }
        if (!name[0]) break;
        if (name[0] == '@') { if (!builtin_icon(name, e->x, e->y, c)) CHECK_ERR("unknown built-in '%.20s'", name); break; }
        int fi = -1;
        if (e->frame) {
            icon_t *ic = icon_get(name);
            if (ic && ic->n > 1) {
                double v = el_num(e->frame, e->vmin), f = e->vmax > e->vmin ? (v - e->vmin) / (e->vmax - e->vmin) : 0;
                if (f < 0) f = 0; if (f > 1) f = 1;
                fi = (int)lround(f * (ic->n - 1));
            } else fi = 0;
        }
        if (!icon_draw_frame(name, e->x, e->y, fi) && scr_check && !e->value) CHECK_ERR("icon '%.24s' not found", name);
        break;
    }
    case E_BAR: {
        double v = el_num(e->value, e->vmin), f = e->vmax > e->vmin ? (v - e->vmin) / (e->vmax - e->vmin) : 0;
        if (f < 0) f = 0; if (f > 1) f = 1;
        uint8_t a[3], b[3], bg[3]; int grad = 0;
        if (e->fill) fill_spec(e->fill, a, b, &grad); else { memcpy(a, c, 3); memcpy(b, c, 3); }
        int hasbg = e->bg && hexcol(e->bg, bg);
        if (e->bg && !hasbg) CHECK_ERR("bad bg '%.20s'", e->bg);
        int horiz = e->dir == 0 || e->dir == 1, len = horiz ? e->w : e->h, fillen = (int)lround(f * len);
        for (int i = 0; i < len; i++) {
            int pos = (e->dir == 1 || e->dir == 2) ? len - 1 - i : i;  // i = distance from the start of the bar
            uint8_t col[3];
            if (i < fillen) { float g = len > 1 ? (float)i / (len - 1) : 0; for (int k = 0; k < 3; k++) col[k] = grad ? (uint8_t)(a[k] + (b[k] - a[k]) * g) : a[k]; }
            else if (hasbg) memcpy(col, bg, 3); else continue;
            if (e->pulse && i == fillen && fillen > 0 && fillen < len && phase(t, 2, 2)) memcpy(col, C_WHITE, 3);
            if (horiz) for (int y = 0; y < e->h; y++) cpx(e->x + pos, e->y + y, col);
            else for (int x = 0; x < e->w; x++) cpx(e->x + x, e->y + pos, col);
        }
        break;
    }
    case E_SPARK: {
        const val_t *v = e->series ? val_get(e->series) : NULL;
        if (!v || v->type != V_SER || v->nser < 2 || e->w < 2 || e->h < 1) break;
        int n = v->nser; float lo = v->ser[0], hi = v->ser[0];
        for (int i = 1; i < n; i++) { if (v->ser[i] < lo) lo = v->ser[i]; if (v->ser[i] > hi) hi = v->ser[i]; }
        float rng = hi - lo; if (rng < 1e-9f) rng = 1;
        uint8_t dim[3] = {(uint8_t)(c[0] / 5), (uint8_t)(c[1] / 5), (uint8_t)(c[2] / 5)};
        int prev = -1, y = 0;
        for (int xi = 0; xi < e->w; xi++) {
            float fx = (float)xi * (n - 1) / (e->w - 1); int k = (int)fx; if (k >= n - 1) k = n - 2;
            float val = v->ser[k] + (v->ser[k + 1] - v->ser[k]) * (fx - k);
            y = e->y + e->h - 1 - (int)lroundf((val - lo) / rng * (e->h - 1));
            if (e->area) for (int yy = y + 1; yy < e->y + e->h; yy++) cpx(e->x + xi, yy, dim);
            if (prev >= 0) for (int yy = (prev < y ? prev : y); yy <= (prev > y ? prev : y); yy++) cpx(e->x + xi, yy, c);
            else cpx(e->x + xi, y, c);
            prev = y;
        }
        if (e->dot && phase(t, 1, 2)) cpx(e->x + e->w - 1, y, C_WHITE);
        break;
    }
    case E_RECT:
        for (int j = 0; j < e->h; j++) for (int i = 0; i < e->w; i++)
            if (e->fillrect || i == 0 || j == 0 || i == e->w - 1 || j == e->h - 1) cpx(e->x + i, e->y + j, c);
        break;
    case E_LINE: draw_line(e->x, e->y, e->x2, e->y2, c); break;
    case E_PIXEL: cpx(e->x, e->y, c); break;
    }
}

/* --- parsing --- */
// next token; quotes may appear anywhere in a token (key="a b"); returns 0 at end of line
static int next_tok(const char **pp, char *out, size_t on, int *quoted) {
    const char *p = *pp; while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *pp = p; return 0; }
    size_t o = 0; *quoted = *p == '"' ? 2 : 0;  // 2: the token starts with a quote
    while (*p && *p != ' ' && *p != '\t') {
        if (*p == '"') {
            if (!*quoted) *quoted = 1;
            p++;
            while (*p && *p != '"') { if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) p++; if (o + 1 < on) out[o++] = *p; p++; }
            if (*p == '"') p++;
        } else { if (o + 1 < on) out[o++] = *p; p++; }
    }
    out[o] = 0; *pp = p; return 1;
}
static void strip_comment(char *l) {  // '#' at the start or after whitespace, followed by whitespace/end; never inside quotes
    int q = 0;
    for (char *p = l; *p; p++) {
        if (*p == '"' && (p == l || p[-1] != '\\')) q = !q;
        if (!q && *p == '#' && (p == l || p[-1] == ' ' || p[-1] == '\t') && (!p[1] || p[1] == ' ' || p[1] == '\t')) { *p = 0; break; }
    }
}
static double parse_dur(const char *s) {
    char *e; double v = strtod(s, &e); if (e == s) return -1;
    switch (*e) { case 0: case 's': return v; case 'm': return v * 60; case 'h': return v * 3600; case 'd': return v * 86400; default: return -1; }
}
static int font_id(const char *s) { return !strcmp(s, "tiny") ? 0 : !strcmp(s, "small") ? 1 : !strcmp(s, "big") ? 2 : !strcmp(s, "huge") ? 3 : -1; }

static void parse_element(scr_t *s, int ln, const char *kw, const char *rest) {
    static const char *KW[] = {"text", "icon", "bar", "spark", "rect", "line", "pixel"};
    static const int NPOS[] = {2, 2, 2, 2, 2, 4, 2}, NMAX[] = {2, 2, 4, 4, 4, 4, 2};  // bar/spark/rect: w h may be options
    int type = -1; for (int i = 0; i < 7; i++) if (!strcmp(kw, KW[i])) type = i;
    if (type < 0) { scr_error(s, ln, "unknown keyword '%.20s'", kw); return; }
    if (s->nel >= NEL) { scr_error(s, ln, "too many elements (max %d)", NEL); return; }
    el_t e; memset(&e, 0, sizeof e); e.type = type; e.line = ln; e.font = 1; e.vmax = 100;
    int pos[4] = {0}, np = 0; char tok[400]; int quoted; const char *p = rest;
    while (np < NMAX[type]) {
        const char *save = p;
        if (!next_tok(&p, tok, sizeof tok, &quoted)) break;
        char *end; long v = strtol(tok, &end, 10);
        if (end == tok || *end) {
            if (np < NPOS[type]) { scr_error(s, ln, "%s: expected a number, got '%.16s'", kw, tok); el_free(&e); return; }
            p = save; break;
        }
        pos[np++] = (int)v;
    }
    if (np < NPOS[type]) { scr_error(s, ln, "%s needs %d numbers", kw, NPOS[type]); el_free(&e); return; }
    e.x = pos[0]; e.y = pos[1];
    if (type == E_LINE) { e.x2 = pos[2]; e.y2 = pos[3]; } else if (np == 4) { e.w = pos[2]; e.h = pos[3]; }
    while (next_tok(&p, tok, sizeof tok, &quoted)) {
        char *eq = quoted == 2 ? NULL : strchr(tok, '=');  // a token that starts with a quote is never an option
        if (!eq) {  // bare token: the text template, or an icon name
            if (type == E_TEXT && !e.text) e.text = strdup(tok);
            else if (type == E_ICON && !e.icon) e.icon = strdup(tok);
            else scr_error(s, ln, "unexpected '%.20s'", tok);
            continue;
        }
        *eq = 0; const char *k = tok, *v = eq + 1;
        if (!strcmp(k, "font")) { int f = font_id(v); if (f < 0) scr_error(s, ln, "unknown font '%.10s'", v); else e.font = f; }
        else if (!strcmp(k, "w")) e.w = atoi(v);
        else if (!strcmp(k, "h")) e.h = atoi(v);
        else if (!strcmp(k, "align")) e.align = !strcmp(v, "center") ? 1 : !strcmp(v, "right") ? 2 : 0;
        else if (!strcmp(k, "valign")) e.valign = !strcmp(v, "middle") ? 1 : !strcmp(v, "bottom") ? 2 : 0;
        else if (!strcmp(k, "overflow")) { e.ovf = !strcmp(v, "scroll") ? 1 : !strcmp(v, "shrink") ? 2 : 0; if (strcmp(v, "scroll") && strcmp(v, "shrink") && strcmp(v, "clip")) scr_error(s, ln, "unknown overflow '%.10s'", v); }
        else if (!strcmp(k, "spacing")) e.spacing = atoi(v);
        else if (!strcmp(k, "speed")) e.speed = (float)atof(v);
        else if (!strcmp(k, "color")) { free(e.color); e.color = strdup(v); }
        else if (!strcmp(k, "if")) { free(e.cond); e.cond = strdup(v); }
        else if (!strcmp(k, "value")) { free(e.value); e.value = strdup(v); }
        else if (!strcmp(k, "series")) { free(e.series); e.series = strdup(v); }
        else if (!strcmp(k, "map")) { free(e.map); e.map = strdup(v); }
        else if (!strcmp(k, "frame")) { free(e.frame); e.frame = strdup(v); }
        else if (!strcmp(k, "range")) { if (sscanf(v, "%f,%f", &e.vmin, &e.vmax) != 2) scr_error(s, ln, "range needs min,max"); }
        else if (!strcmp(k, "min")) e.vmin = (float)atof(v);
        else if (!strcmp(k, "max")) e.vmax = (float)atof(v);
        else if (!strcmp(k, "fill")) { if (type == E_RECT) e.fillrect = atoi(v) != 0; else { free(e.fill); e.fill = strdup(v); } }
        else if (!strcmp(k, "bg")) { free(e.bg); e.bg = strdup(v); }
        else if (!strcmp(k, "dir")) e.dir = !strcmp(v, "left") ? 1 : !strcmp(v, "up") ? 2 : !strcmp(v, "down") ? 3 : 0;
        else if (!strcmp(k, "pulse")) e.pulse = atoi(v) != 0;
        else if (!strcmp(k, "blink")) { e.blink = atoi(v); if (e.blink < 50 || e.blink > 10000) { scr_error(s, ln, "blink must be 50..10000 ms"); e.blink = 0; } }
        else if (!strcmp(k, "area")) e.area = atoi(v) != 0;
        else if (!strcmp(k, "dot")) e.dot = atoi(v) != 0;
        else scr_error(s, ln, "%s: unknown option '%.16s'", kw, k);
    }
    if (type == E_TEXT && !e.text) e.text = strdup("");
    if (type == E_ICON && !e.icon && !e.value) scr_error(s, ln, "icon needs a name or value=");
    if (type == E_BAR && !e.value) scr_error(s, ln, "bar needs value=");
    if (type == E_SPARK && !e.series) scr_error(s, ln, "spark needs series=");
    s->el[s->nel++] = e;
}

static scr_t *scr_load(const char *file) {
    char path[400]; snprintf(path, sizeof path, "%s/%s", scr_dir, file);
    FILE *f = fopen(path, "r"); if (!f) return NULL;
    scr_t *s = calloc(1, sizeof *s); if (!s) { fclose(f); return NULL; }
    snprintf(s->file, sizeof s->file, "%s", file);
    snprintf(s->name, sizeof s->name, "%.*s", (int)(strrchr(file, '.') ? strrchr(file, '.') - file : (long)strlen(file)), file);
    s->stale_s = 6 * 3600; s->order = 100; s->lin_prev = -1;
    char l[520]; int ln = 0, magic = 0;
    while (fgets(l, sizeof l, f)) {
        ln++;
        if (!strchr(l, '\n') && !feof(f)) { scr_error(s, ln, "line too long"); int ch; while ((ch = fgetc(f)) != EOF && ch != '\n') {} continue; }
        l[strcspn(l, "\r\n")] = 0;
        if (!magic) { if (strncmp(l, "PXS1", 4)) { scr_error(s, ln, "not a PXS1 file"); break; } magic = 1; continue; }
        strip_comment(l); trim(l);
        if (!l[0]) continue;
        char kw[16]; size_t kl = strcspn(l, " \t"); snprintf(kw, sizeof kw, "%.*s", (int)(kl < 15 ? kl : 15), l);
        char *rest = l + kl; while (*rest == ' ' || *rest == '\t') rest++;
        if (!strcmp(kw, "name")) { char t[64]; snprintf(t, sizeof t, "%s", rest); unquote(t); if (t[0]) snprintf(s->name, sizeof s->name, "%s", t); }
        else if (!strcmp(kw, "needs")) { free(s->needs); s->needs = strdup(rest); }
        else if (!strcmp(kw, "show")) { free(s->show); s->show = strdup(rest); }
        else if (!strcmp(kw, "linger")) {
            char *sp = strrchr(rest, ' '); double d = sp ? parse_dur(sp + 1) : -1;
            if (d < 0) scr_error(s, ln, "linger needs a condition and a duration");
            else { *sp = 0; free(s->linger); s->linger = strdup(rest); s->linger_s = d; }
        }
        else if (!strcmp(kw, "stale")) { double d = parse_dur(rest); if (d <= 0) scr_error(s, ln, "bad duration '%.10s'", rest); else s->stale_s = d; }
        else if (!strcmp(kw, "time")) { int v = atoi(rest); if (v < 2 || v > 600) scr_error(s, ln, "time must be 2..600 s"); else s->time_s = v; }
        else if (!strcmp(kw, "order")) s->order = atoi(rest);
        else if (!strcmp(kw, "bg")) { if (hexcol(rest, s->bg)) s->has_bg = 1; else scr_error(s, ln, "bad colour '%.10s'", rest); }
        else parse_element(s, ln, kw, rest);
    }
    fclose(f);
    if (!magic && !s->nerr) scr_error(s, 1, "empty file");
    // check expressions once: syntax errors in conditions, colours and templates
    scr_check = s; cur_stale = s->stale_s;
    uint8_t save[FRAME]; memcpy(save, fb, FRAME);
    scr_check_line = 0;
    if (s->show) { scr_check_line = -1; int m; cond_eval(s->show, &m); }
    if (s->linger) { int m; cond_eval(s->linger, &m); }
    for (int i = 0; i < s->nel; i++) {
        scr_check_line = s->el[i].line;
        int m; if (s->el[i].cond) cond_eval(s->el[i].cond, &m);
        el_draw(&s->el[i], now_s());
    }
    memcpy(fb, save, FRAME);
    scr_check = NULL;
    for (int i = 0; i < s->nerr; i++) if (s->err[i].line < 0) s->err[i].line = 0;
    if (s->nerr) logf_("screen %s: %d problem(s), first: line %d: %s", file, s->nerr, s->err[0].line, s->err[0].msg);
    return s;
}

static int scr_cmp(const void *a, const void *b) {
    const scr_t *x = *(scr_t *const *)a, *y = *(scr_t *const *)b;
    return x->order != y->order ? (x->order < y->order ? -1 : 1) : strcasecmp(x->name, y->name);
}
static void scr_scan(void) {
    // keep linger state across reloads
    struct { char file[72]; int prev; double start; } keep[NSCR]; int nk = 0;
    for (int i = 0; i < nscr; i++) { snprintf(keep[nk].file, sizeof keep[nk].file, "%s", scrs[i]->file); keep[nk].prev = scrs[i]->lin_prev; keep[nk].start = scrs[i]->lin_start; nk++; scr_free(scrs[i]); }
    nscr = 0;
    DIR *dp = opendir(scr_dir); struct dirent *e;
    while (dp && (e = readdir(dp)) && nscr < NSCR) {
        const char *dot = strrchr(e->d_name, '.');
        if (e->d_name[0] == '.' || !dot || strcasecmp(dot, ".scr")) continue;
        scr_t *s = scr_load(e->d_name); if (!s) continue;
        for (int k = 0; k < nk; k++) if (!strcmp(keep[k].file, s->file)) { s->lin_prev = keep[k].prev; s->lin_start = keep[k].start; }
        scrs[nscr++] = s;
    }
    if (dp) closedir(dp);
    qsort(scrs, nscr, sizeof scrs[0], scr_cmp);
}
static scr_t *scr_find(const char *name) {
    for (int i = 0; i < nscr; i++) if (!strcasecmp(scrs[i]->name, name)) return scrs[i];
    return NULL;
}

// needs: all present and fresh. *miss gets the names that are missing or stale
static int scr_needs_ok(const scr_t *s, char *miss, size_t mn) {
    int ok = 1; if (miss && mn) miss[0] = 0;
    if (!s->needs) return 1;
    char buf[300]; snprintf(buf, sizeof buf, "%s", s->needs);
    for (char *sv, *n = strtok_r(buf, " ,", &sv); n; n = strtok_r(NULL, " ,", &sv)) {
        const val_t *v = val_get(n);
        if (!v || (!val_is_builtin(n) && now_s() - v->upd > s->stale_s)) {
            ok = 0;
            if (miss && strlen(miss) + strlen(n) + 2 < mn) { if (miss[0]) strcat(miss, " "); strcat(miss, n); }
        }
    }
    return ok;
}
static void scr_tick_linger(scr_t *s, double t) {
    if (!s->linger) return;
    cur_stale = s->stale_s;
    int miss = 0, r = cond_eval(s->linger, &miss);
    if (miss) return;  // only transitions between known values count
    if (r && s->lin_prev == 0) s->lin_start = t;
    if (!r) s->lin_start = 0;
    s->lin_prev = r;
}
static int scr_visible(scr_t *s, double t) {
    if (!scr_needs_ok(s, NULL, 0)) return 0;
    cur_stale = s->stale_s;
    int m;
    if (!s->show || cond_eval(s->show, &m)) return 1;
    return s->linger && s->lin_start > 0 && t - s->lin_start < s->linger_s;
}
static void scr_render(scr_t *s, double t) {
    if (s->has_bg) for (int i = 0; i < W * H; i++) memcpy(&fb[i * 3], s->bg, 3);
    cur_stale = s->stale_s;
    for (int i = 0; i < s->nel; i++) {
        int m; if (s->el[i].cond && !cond_eval(s->el[i].cond, &m)) continue;
        if (s->el[i].blink && phase(t, 500.0 / s->el[i].blink, 2)) continue;  // blink=<ms>: shown for ms, hidden for ms
        el_draw(&s->el[i], t);
    }
}
static void scr_tick(double t) { static double next; if (t < next) return; next = t + 0.25; for (int i = 0; i < nscr; i++) scr_tick_linger(scrs[i], t); }

/* --- GET /api/screens --- */
static void scr_json(void (*put)(void *, const char *, size_t), void *ctx) {
    char b[700]; double t = now_s();
    put(ctx, "[", 1);
    for (int i = 0; i < nscr; i++) {
        scr_t *s = scrs[i]; char miss[200]; scr_needs_ok(s, miss, sizeof miss);
        char nm[100]; size_t j = 0;
        for (const char *p = s->name; *p && j < sizeof nm - 2; p++) { if (*p == '"' || *p == '\\') nm[j++] = '\\'; nm[j++] = *p; }
        nm[j] = 0;
        int n = snprintf(b, sizeof b, "%s{\"name\":\"%s\",\"file\":\"%s\",\"order\":%d,\"visible\":%s,\"missing\":\"%s\",\"errors\":[",
                         i ? "," : "", nm, s->file, s->order, scr_visible(s, t) ? "true" : "false", miss);
        put(ctx, b, (size_t)n);
        for (int k = 0; k < s->nerr; k++) {
            char m[64]; size_t j = 0; for (const char *p = s->err[k].msg; *p && j < sizeof m - 2; p++) { if (*p == '"' || *p == '\\') m[j++] = '\\'; m[j++] = *p; } m[j] = 0;
            n = snprintf(b, sizeof b, "%s{\"line\":%d,\"msg\":\"%s\"}", k ? "," : "", s->err[k].line, m);
            put(ctx, b, (size_t)n);
        }
        put(ctx, "]}", 2);
    }
    put(ctx, "]", 1);
}
