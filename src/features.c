// features.c - included into pixeld.c (unity build): icons, tiny font, generative effects,
// music mode (microphone), info screens, countdown, stopwatch, playlist.
// Needs from pixeld.c: W, H, FRAME, fb, px, glyph, text_at, text_w, now_s, logf_, draw_clock,
// draw_scroll, anim_*, st, play_sound, mq_pub, json_str (forward-declared there).
#include <math.h>

// Blink/frame phase from the absolute time. Never cast t * hz to int/long directly (or to float: a float
// near 1.7e8 has a resolution of 16, so hues built from (float)(t * k) never change): t is ~1.7e9 s, so
// t * 2 already overflows a 32-bit int (and t * 1000 a 32-bit long) on the ARM build; the conversion then
// saturates to a constant and the animation freezes.
static int phase(double t, double hz, int n) { return (int)fmod(t * hz, (double)n); }

static uint32_t rnd_state = 0x12345678;
static uint32_t rnd(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5; return rnd_state; }
static int rndi(int n) { return n > 0 ? (int)(rnd() % (uint32_t)n) : 0; }

static void hsv(float h, float s, float v, uint8_t *c) {
    h = fmodf(h, 1.0f); if (h < 0) h += 1;
    int i = (int)(h * 6); float f = h * 6 - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    float r, g, b;
    switch (i % 6) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
                     case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; }
    c[0] = (uint8_t)(r * 255); c[1] = (uint8_t)(g * 255); c[2] = (uint8_t)(b * 255);
}
static void fill_rect(int x, int y, int w, int h, const uint8_t *c) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x + i, y + j, c); }
static void fade_fb(uint8_t *buf, int num, int den) { for (int i = 0; i < FRAME; i++) buf[i] = (uint8_t)(buf[i] * num / den); }
static void addpx(uint8_t *buf, int x, int y, const uint8_t *c) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    uint8_t *p = &buf[(y * W + x) * 3];
    for (int k = 0; k < 3; k++) { int v = p[k] + c[k]; p[k] = v > 255 ? 255 : v; }
}

/* ------------------------------------------------------------------ icons (.pxi in /mnt/UDISK/icons) */
#define ICON_DIR "/mnt/UDISK/icons"
typedef struct { char name[32]; int w, h, n, total; uint16_t *delay; uint8_t *rgba; } icon_t;
static icon_t icons[48]; static int nicons;

static void icons_flush(void) {  // after icon files changed (api.c)
    for (int i = 0; i < nicons; i++) { free(icons[i].delay); free(icons[i].rgba); }
    nicons = 0;
}

static icon_t *icon_get(const char *name) {
    for (int i = 0; i < nicons; i++) if (!strcmp(icons[i].name, name)) return icons[i].rgba ? &icons[i] : NULL;
    if (nicons >= (int)(sizeof icons / sizeof icons[0])) {  // simple cache reset when full
        for (int i = 0; i < nicons; i++) { free(icons[i].delay); free(icons[i].rgba); }
        nicons = 0;
    }
    icon_t *ic = &icons[nicons++]; memset(ic, 0, sizeof *ic);
    snprintf(ic->name, sizeof ic->name, "%s", name);
    char path[96]; snprintf(path, sizeof path, ICON_DIR "/%s.pxi", name);
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    uint8_t h[8];
    if (fread(h, 1, 8, f) != 8 || memcmp(h, "PXI1", 4)) { fclose(f); return NULL; }
    ic->w = h[4]; ic->h = h[5]; ic->n = h[6] | (h[7] << 8);
    size_t fsz = (size_t)ic->w * ic->h * 4;
    if (!ic->n || ic->n > 64) { fclose(f); return NULL; }
    ic->delay = calloc(ic->n, 2); ic->rgba = malloc(fsz * ic->n);
    for (int i = 0; i < ic->n; i++) {
        uint8_t d[2];
        if (fread(d, 1, 2, f) != 2 || fread(ic->rgba + fsz * i, 1, fsz, f) != fsz) { free(ic->delay); free(ic->rgba); ic->rgba = NULL; fclose(f); return NULL; }
        ic->delay[i] = d[0] | (d[1] << 8); ic->total += ic->delay[i];
    }
    fclose(f);
    return ic;
}

static int icon_draw(const char *name, int x, int y) {
    icon_t *ic = icon_get(name); if (!ic) return 0;
    int fi = 0;
    if (ic->n > 1 && ic->total) {
        long ms = (long)fmod(now_s() * 1000.0, (double)ic->total);
        for (fi = 0; fi < ic->n; fi++) { if (ms < ic->delay[fi]) break; ms -= ic->delay[fi]; }
        if (fi >= ic->n) fi = 0;
    }
    const uint8_t *s = ic->rgba + (size_t)ic->w * ic->h * 4 * fi;
    for (int j = 0; j < ic->h; j++)
        for (int i = 0; i < ic->w; i++) {
            const uint8_t *p = s + (j * ic->w + i) * 4;
            if (p[3] < 128) continue;
            px(x + i, y + j, p);
        }
    return ic->w;
}

/* ------------------------------------------------------------------ tiny 3x5 font */
static const struct { char c; uint8_t r[5]; } T3[] = {
    {'0',{7,5,5,5,7}},{'1',{2,6,2,2,7}},{'2',{7,1,7,4,7}},{'3',{7,1,3,1,7}},{'4',{5,5,7,1,1}},{'5',{7,4,7,1,7}},
    {'6',{7,4,7,5,7}},{'7',{7,1,1,2,2}},{'8',{7,5,7,5,7}},{'9',{7,5,7,1,7}},{'-',{0,0,7,0,0}},{'+',{0,2,7,2,0}},
    {'.',{0,0,0,0,2}},{':',{0,2,0,2,0}},{'%',{5,1,2,4,5}},{'/',{1,1,2,4,4}},{'*',{2,5,2,0,0}},{' ',{0,0,0,0,0}},
    {'A',{2,5,7,5,5}},{'B',{6,5,6,5,6}},{'C',{7,4,4,4,7}},{'D',{6,5,5,5,6}},{'E',{7,4,6,4,7}},{'F',{7,4,6,4,4}},
    {'G',{7,4,5,5,7}},{'H',{5,5,7,5,5}},{'I',{7,2,2,2,7}},{'J',{1,1,1,5,7}},{'K',{5,5,6,5,5}},{'L',{4,4,4,4,7}},
    {'M',{5,7,7,5,5}},{'N',{6,5,5,5,5}},{'O',{7,5,5,5,7}},{'P',{7,5,7,4,4}},{'Q',{7,5,5,7,1}},{'R',{6,5,6,5,5}},
    {'S',{7,4,7,1,7}},{'T',{7,2,2,2,2}},{'U',{5,5,5,5,7}},{'V',{5,5,5,5,2}},{'W',{5,5,7,7,5}},{'X',{5,5,2,5,5}},
    {'Y',{5,5,2,2,2}},{'Z',{7,1,2,4,7}},
};
static int tiny_w(const char *s) { int n = (int)strlen(s); return n ? n * 4 - 1 : 0; }
static int tiny_text(const char *s, int x, int y, const uint8_t *c) {
    for (; *s; s++, x += 4) {
        char ch = (char)toupper((unsigned char)*s);
        for (size_t i = 0; i < sizeof T3 / sizeof T3[0]; i++) {
            if (T3[i].c != ch) continue;
            for (int r = 0; r < 5; r++) for (int b = 0; b < 3; b++) if (T3[i].r[r] & (4 >> b)) px(x + b, y + r, c);
            break;
        }
    }
    return x;
}
static void big_text(const char *s, int x, int y, const uint8_t *c) { for (; *s; s++, x += 12) glyph(*s, x, y, 2, c); }
static void degree(int x, int y, const uint8_t *c) { px(x + 1, y, c); px(x, y + 1, c); px(x + 2, y + 1, c); px(x + 1, y + 2, c); }

/* ------------------------------------------------------------------ generative effects */
static const char *EFFECTS[] = {"Snake", "Bouncing Balls", "Match 3", "Color Wall", "Flow", "Line Spray", "Line Fill",
                                "Pixel Heap", "Fireworks", "Starfield", "Rain"};
#define NEFFECTS ((int)(sizeof EFFECTS / sizeof EFFECTS[0]))
static int effect_idx;
static uint8_t efb[FRAME];       // persistent effect buffer (trails etc.)
static double eff_t0, eff_last;
static int eff_init;

static int effect_find(const char *n) { for (int i = 0; i < NEFFECTS; i++) if (!strcasecmp(EFFECTS[i], n)) return i; return -1; }
static void effect_start(int i) { effect_idx = i; snprintf(effect_name, sizeof effect_name, "%s", EFFECTS[i]); eff_init = 0; eff_t0 = now_s(); memset(efb, 0, sizeof efb); }

// --- Snake (self-playing on a 26x16 grid of 2x2 cells)
#define SGW 26
#define SGH 16
static struct { int x[SGW * SGH], y[SGW * SGH], len, dir, fx, fy, dead; double next; float hue; } sn;
static int snake_hits(int x, int y, int skip_tail) {
    if (x < 0 || y < 0 || x >= SGW || y >= SGH) return 1;
    for (int i = 0; i < sn.len - skip_tail; i++) if (sn.x[i] == x && sn.y[i] == y) return 1;
    return 0;
}
static void snake_food(void) { do { sn.fx = rndi(SGW); sn.fy = rndi(SGH); } while (snake_hits(sn.fx, sn.fy, 0)); }
static int snake_space(int x, int y) {  // flood-fill free area from (x,y), capped
    static uint8_t seen[SGW * SGH]; static int qx[SGW * SGH], qy[SGW * SGH];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < sn.len; i++) seen[sn.y[i] * SGW + sn.x[i]] = 1;
    if (x < 0 || y < 0 || x >= SGW || y >= SGH || seen[y * SGW + x]) return 0;
    int h = 0, t = 0, n = 0; qx[t] = x; qy[t++] = y; seen[y * SGW + x] = 1;
    while (h < t && n < 200) {
        int cx = qx[h], cy = qy[h++]; n++;
        static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; d++) {
            int nx = cx + dx[d], ny = cy + dy[d];
            if (nx < 0 || ny < 0 || nx >= SGW || ny >= SGH || seen[ny * SGW + nx]) continue;
            seen[ny * SGW + nx] = 1; qx[t] = nx; qy[t++] = ny;
        }
    }
    return n;
}
static void fx_snake(double t) {
    static const int DX[4] = {1, 0, -1, 0}, DY[4] = {0, 1, 0, -1};
    if (!eff_init || (sn.dead && t > sn.next)) {
        memset(&sn, 0, sizeof sn); sn.len = 4;
        for (int i = 0; i < 4; i++) { sn.x[i] = 10 - i; sn.y[i] = 8; }
        snake_food(); sn.next = t; eff_init = 1; sn.hue = (float)rndi(100) / 100;
    }
    if (!sn.dead && t >= sn.next) {
        sn.next = t + 0.08;
        int best = -1, bestscore = -100000;
        for (int d = 0; d < 4; d++) {
            if (d == (sn.dir + 2) % 4) continue;
            int nx = sn.x[0] + DX[d], ny = sn.y[0] + DY[d];
            if (snake_hits(nx, ny, 1)) continue;
            int space = snake_space(nx, ny);
            int score = -(abs(nx - sn.fx) + abs(ny - sn.fy)) * 4 + (space < sn.len + 2 ? -1000 + space : 0) + rndi(2);
            if (score > bestscore) { bestscore = score; best = d; }
        }
        if (best < 0) { sn.dead = 1; sn.next = t + 1.5; }
        else {
            sn.dir = best;
            int nx = sn.x[0] + DX[best], ny = sn.y[0] + DY[best], grow = (nx == sn.fx && ny == sn.fy);
            if (grow && sn.len < SGW * SGH - 1) sn.len++;
            for (int i = sn.len - 1; i > 0; i--) { sn.x[i] = sn.x[i - 1]; sn.y[i] = sn.y[i - 1]; }
            sn.x[0] = nx; sn.y[0] = ny;
            if (grow) { if (sn.len >= SGW * SGH - 2) { sn.dead = 1; sn.next = t + 1.5; } else snake_food(); }
        }
    }
    uint8_t c[3];
    for (int i = 0; i < sn.len; i++) {
        hsv(sn.hue + i * 0.01f, 1, i == 0 ? 1 : 0.8f, c);
        if (sn.dead && phase(t, 6, 2)) c[0] = c[1] = c[2] = 60;
        fill_rect(sn.x[i] * 2, sn.y[i] * 2, 2, 2, c);
    }
    const uint8_t food[3] = {255, 40, 40}; fill_rect(sn.fx * 2, sn.fy * 2, 2, 2, food);
}

// --- Bouncing balls with fading trails
static struct { float x, y, vx, vy; uint8_t c[3]; } balls[6];
static void fx_balls(double t, double dt) {
    if (!eff_init) {
        for (int i = 0; i < 6; i++) { balls[i].x = rndi(W); balls[i].y = rndi(H); balls[i].vx = (rndi(30) + 10) * (rndi(2) ? 1 : -1); balls[i].vy = (rndi(30) + 10) * (rndi(2) ? 1 : -1); hsv(i / 6.0f, 1, 1, balls[i].c); }
        eff_init = 1;
    }
    fade_fb(efb, 7, 10);
    for (int i = 0; i < 6; i++) {
        balls[i].x += balls[i].vx * dt; balls[i].y += balls[i].vy * dt;
        if (balls[i].x < 0) { balls[i].x = 0; balls[i].vx = -balls[i].vx; }
        if (balls[i].x > W - 2) { balls[i].x = W - 2; balls[i].vx = -balls[i].vx; }
        if (balls[i].y < 0) { balls[i].y = 0; balls[i].vy = -balls[i].vy; }
        if (balls[i].y > H - 2) { balls[i].y = H - 2; balls[i].vy = -balls[i].vy; }
        for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
            int x = (int)balls[i].x + dx, y = (int)balls[i].y + dy;
            if (x >= 0 && x < W && y >= 0 && y < H) memcpy(&efb[(y * W + x) * 3], balls[i].c, 3);
        }
    }
    (void)t; memcpy(fb, efb, FRAME);
}

// --- Match 3 ("happy eliminate"), self-playing on 8x5 tiles of 6x6
#define MG_W 8
#define MG_H 5
static struct { int8_t g[MG_H][MG_W]; uint8_t clear[MG_H][MG_W]; int phase; double next; int sx1, sy1, sx2, sy2; } m3;
static const uint8_t M3C[6][3] = {{255, 50, 50}, {50, 220, 60}, {60, 120, 255}, {255, 210, 40}, {200, 60, 255}, {40, 220, 220}};
static int m3_find_matches(void) {
    int any = 0; memset(m3.clear, 0, sizeof m3.clear);
    for (int y = 0; y < MG_H; y++) for (int x = 0; x < MG_W - 2; x++) {
        int c = m3.g[y][x]; if (c < 0) continue;
        if (m3.g[y][x + 1] == c && m3.g[y][x + 2] == c) { m3.clear[y][x] = m3.clear[y][x + 1] = m3.clear[y][x + 2] = 1; any = 1; }
    }
    for (int x = 0; x < MG_W; x++) for (int y = 0; y < MG_H - 2; y++) {
        int c = m3.g[y][x]; if (c < 0) continue;
        if (m3.g[y + 1][x] == c && m3.g[y + 2][x] == c) { m3.clear[y][x] = m3.clear[y + 1][x] = m3.clear[y + 2][x] = 1; any = 1; }
    }
    return any;
}
static void m3_swap(int x1, int y1, int x2, int y2) { int8_t t = m3.g[y1][x1]; m3.g[y1][x1] = m3.g[y2][x2]; m3.g[y2][x2] = t; }
static int m3_find_move(void) {
    int cand[MG_W * MG_H * 2][4], n = 0;
    for (int y = 0; y < MG_H; y++) for (int x = 0; x < MG_W; x++) for (int d = 0; d < 2; d++) {
        int x2 = x + (d == 0), y2 = y + (d == 1);
        if (x2 >= MG_W || y2 >= MG_H) continue;
        m3_swap(x, y, x2, y2);
        if (m3_find_matches()) { cand[n][0] = x; cand[n][1] = y; cand[n][2] = x2; cand[n][3] = y2; n++; }
        m3_swap(x, y, x2, y2);
    }
    memset(m3.clear, 0, sizeof m3.clear);
    if (!n) return 0;
    int k = rndi(n); m3.sx1 = cand[k][0]; m3.sy1 = cand[k][1]; m3.sx2 = cand[k][2]; m3.sy2 = cand[k][3];
    return 1;
}
static void m3_fill(void) { for (int y = 0; y < MG_H; y++) for (int x = 0; x < MG_W; x++) m3.g[y][x] = (int8_t)rndi(6); while (m3_find_matches()) for (int y = 0; y < MG_H; y++) for (int x = 0; x < MG_W; x++) if (m3.clear[y][x]) m3.g[y][x] = (int8_t)rndi(6); memset(m3.clear, 0, sizeof m3.clear); }
static void fx_match3(double t) {
    if (!eff_init) { m3_fill(); m3.phase = 0; m3.next = t + 0.6; eff_init = 1; }
    if (t >= m3.next) {
        switch (m3.phase) {
        case 0: if (m3_find_move()) { m3.phase = 1; m3.next = t + 0.35; } else { m3_fill(); m3.next = t + 0.6; } break;   // show selection
        case 1: m3_swap(m3.sx1, m3.sy1, m3.sx2, m3.sy2); m3_find_matches(); m3.phase = 2; m3.next = t + 0.45; break;   // swap, flash matches
        case 2:  // remove + gravity + refill
            for (int x = 0; x < MG_W; x++) {
                int w = MG_H - 1;
                for (int y = MG_H - 1; y >= 0; y--) if (!m3.clear[y][x]) m3.g[w--][x] = m3.g[y][x];
                while (w >= 0) m3.g[w--][x] = (int8_t)rndi(6);
            }
            memset(m3.clear, 0, sizeof m3.clear);
            m3.phase = m3_find_matches() ? 2 : 0; m3.next = t + (m3.phase ? 0.45 : 0.5); break;
        }
    }
    for (int y = 0; y < MG_H; y++) for (int x = 0; x < MG_W; x++) {
        int c = m3.g[y][x]; if (c < 0) continue;
        uint8_t col[3]; memcpy(col, M3C[c], 3);
        if (m3.clear[y][x] && phase(t, 10, 2)) col[0] = col[1] = col[2] = 255;
        fill_rect(2 + x * 6, 1 + y * 6, 5, 5, col);
        if (m3.phase == 1 && ((x == m3.sx1 && y == m3.sy1) || (x == m3.sx2 && y == m3.sy2))) {
            const uint8_t wh[3] = {255, 255, 255};
            for (int i = 0; i < 5; i++) { px(2 + x * 6 + i, 1 + y * 6, wh); px(2 + x * 6 + i, 5 + y * 6, wh); px(2 + x * 6, 1 + y * 6 + i, wh); px(6 + x * 6, 1 + y * 6 + i, wh); }
        }
    }
}

// --- Color wall: 13x8 blocks drifting between random hues
static float cw_h[8][13], cw_t[8][13];
static void fx_colorwall(double t, double dt) {
    if (!eff_init) { for (int y = 0; y < 8; y++) for (int x = 0; x < 13; x++) cw_h[y][x] = cw_t[y][x] = rndi(1000) / 1000.0f; eff_init = 1; }
    for (int y = 0; y < 8; y++) for (int x = 0; x < 13; x++) {
        if (fabsf(cw_h[y][x] - cw_t[y][x]) < 0.01f && !rndi(40)) cw_t[y][x] = rndi(1000) / 1000.0f;
        cw_h[y][x] += (cw_t[y][x] - cw_h[y][x]) * (float)(dt * 1.5);
        uint8_t c[3]; hsv(cw_h[y][x], 0.9f, 0.9f, c);
        fill_rect(x * 4, y * 4, 4, 4, c);
    }
    (void)t;
}

// --- Flow: plasma field
static void fx_flow(double t) {
    float tt = (float)(t - eff_t0);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float v = sinf(x * 0.16f + tt) + sinf(y * 0.21f - tt * 1.3f) + sinf((x + y) * 0.11f + tt * 0.7f) + sinf(sqrtf((x - 26.0f) * (x - 26.0f) + (y - 16.0f) * (y - 16.0f)) * 0.25f - tt * 1.7f);
        uint8_t c[3]; hsv(v * 0.125f + tt * 0.05f, 1, 0.85f, c); px(x, y, c);
    }
}

// --- Line spray: coloured lines shooting across with trails
static struct { float x; int y, dir; uint8_t c[3]; float v; } ls[10];
static void fx_linespray(double t, double dt) {
    if (!eff_init) { for (int i = 0; i < 10; i++) ls[i].x = -1000; eff_init = 1; }
    fade_fb(efb, 8, 10);
    for (int i = 0; i < 10; i++) {
        if (ls[i].x < -5 || ls[i].x > W + 5) {
            if (rndi(20)) continue;
            ls[i].dir = rndi(2) ? 1 : -1; ls[i].x = ls[i].dir > 0 ? 0 : W - 1; ls[i].y = rndi(H); ls[i].v = 30 + rndi(50);
            hsv(rndi(100) / 100.0f, 1, 1, ls[i].c);
        }
        ls[i].x += ls[i].dir * ls[i].v * dt;
        int x = (int)ls[i].x;
        if (x >= 0 && x < W) memcpy(&efb[(ls[i].y * W + x) * 3], ls[i].c, 3);
    }
    (void)t; memcpy(fb, efb, FRAME);
}

// --- Line fill: rows fill one after another in a gradient, then wipe
static void fx_linefill(double t) {
    float tt = (float)(t - eff_t0);
    int cycle = H * 2 + 8; int step = (int)(tt * 12) % cycle; float base = (int)(tt * 12 / cycle) * 0.17f;
    for (int y = 0; y < H; y++) {
        int filled = step < H ? (y < step) : (step < H + 4 ? 1 : (y >= step - H - 4));
        if (!filled) continue;
        uint8_t c[3]; hsv(base + y / (float)H * 0.3f, 1, 0.9f, c);
        int len = (step < H && y == step - 1) ? (int)(fmodf(tt * 12, 1.0f) * W) : W;
        for (int x = 0; x < len; x++) px(x, y, c);
    }
}

// --- Pixel heap: falling sand that piles up, then dissolves
static uint8_t heap[H][W]; static float heap_hue;
static void fx_heap(double t) {
    static int dissolving;
    if (!eff_init) { memset(heap, 0, sizeof heap); dissolving = 0; heap_hue = rndi(100) / 100.0f; eff_init = 1; }
    for (int k = 0; k < 2; k++) {
        for (int y = H - 2; y >= 0; y--) for (int x = 0; x < W; x++) {
            if (!heap[y][x]) continue;
            int dir = rndi(2) ? 1 : -1;
            if (!heap[y + 1][x]) { heap[y + 1][x] = heap[y][x]; heap[y][x] = 0; }
            else if (x + dir >= 0 && x + dir < W && !heap[y + 1][x + dir]) { heap[y + 1][x + dir] = heap[y][x]; heap[y][x] = 0; }
        }
        if (!dissolving) {
            for (int i = 0; i < 3; i++) { int x = rndi(W); if (!heap[0][x]) heap[0][x] = (uint8_t)(1 + rndi(254)); }
            int full = 0; for (int x = 0; x < W; x++) full += heap[3][x] != 0;
            if (full > W / 2) dissolving = 1;
        } else {
            int left = 0;
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (heap[y][x]) { left++; if (!rndi(25)) heap[y][x] = 0; }
            if (!left) { dissolving = 0; heap_hue += 0.23f; }
        }
    }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (heap[y][x]) { uint8_t c[3]; hsv(heap_hue + heap[y][x] / 255.0f * 0.15f, 0.9f, 1, c); px(x, y, c); }
    (void)t;
}

// --- Fireworks
static struct { float x, y, vx, vy, life; uint8_t c[3]; int rocket; } pt[160];
static void fx_fireworks(double t, double dt) {
    if (!eff_init) { memset(pt, 0, sizeof pt); eff_init = 1; }
    fade_fb(efb, 6, 10);
    if (!rndi(18)) for (int i = 0; i < 160; i++) if (pt[i].life <= 0) { pt[i].x = 6 + rndi(W - 12); pt[i].y = H; pt[i].vx = (rndi(20) - 10) / 2.0f; pt[i].vy = -(22 + rndi(10)); pt[i].life = 1; pt[i].rocket = 1; pt[i].c[0] = pt[i].c[1] = pt[i].c[2] = 200; break; }
    for (int i = 0; i < 160; i++) {
        if (pt[i].life <= 0) continue;
        pt[i].x += pt[i].vx * dt; pt[i].y += pt[i].vy * dt; pt[i].vy += 18 * dt;
        if (pt[i].rocket) {
            if (pt[i].vy > -3) {  // burst
                pt[i].life = 0; uint8_t c[3]; hsv(rndi(100) / 100.0f, 0.8f, 1, c); int made = 0;
                for (int j = 0; j < 160 && made < 30; j++) if (pt[j].life <= 0) {
                    float a = made * 0.2094f, sp = 8 + rndi(10);
                    pt[j].x = pt[i].x; pt[j].y = pt[i].y; pt[j].vx = cosf(a) * sp; pt[j].vy = sinf(a) * sp; pt[j].life = 1.2f; pt[j].rocket = 0; memcpy(pt[j].c, c, 3); made++;
                }
                continue;
            }
        } else pt[i].life -= dt;
        uint8_t c[3]; float k = pt[i].rocket ? 1 : (pt[i].life > 1 ? 1 : pt[i].life);
        for (int q = 0; q < 3; q++) c[q] = (uint8_t)(pt[i].c[q] * k);
        int x = (int)pt[i].x, y = (int)pt[i].y;
        if (x >= 0 && x < W && y >= 0 && y < H) memcpy(&efb[(y * W + x) * 3], c, 3);
    }
    (void)t; memcpy(fb, efb, FRAME);
}

// --- Starfield
static struct { float x, y, z; } stars[90];
static void fx_starfield(double t, double dt) {
    if (!eff_init) { for (int i = 0; i < 90; i++) { stars[i].x = rndi(200) - 100; stars[i].y = rndi(200) - 100; stars[i].z = 1 + rndi(100); } eff_init = 1; }
    for (int i = 0; i < 90; i++) {
        stars[i].z -= (float)(dt * 40);
        if (stars[i].z <= 1) { stars[i].x = rndi(200) - 100; stars[i].y = rndi(200) - 100; stars[i].z = 100; }
        int sx = 26 + (int)(stars[i].x / stars[i].z * 16), sy = 16 + (int)(stars[i].y / stars[i].z * 16);
        int v = (int)(255 * (1 - stars[i].z / 100)); uint8_t c[3] = {(uint8_t)v, (uint8_t)v, (uint8_t)(v < 200 ? v + 55 : 255)};
        px(sx, sy, c);
    }
    (void)t;
}

// --- Rain (digital rain)
static struct { float y, v; int len; } drops[W];
static void fx_rain(double t, double dt) {
    if (!eff_init) { for (int x = 0; x < W; x++) { drops[x].y = -rndi(40); drops[x].v = 8 + rndi(20); drops[x].len = 4 + rndi(8); } eff_init = 1; }
    for (int x = 0; x < W; x++) {
        drops[x].y += drops[x].v * dt;
        if (drops[x].y - drops[x].len > H) { drops[x].y = -rndi(20); drops[x].v = 8 + rndi(20); drops[x].len = 4 + rndi(8); }
        for (int k = 0; k < drops[x].len; k++) {
            int y = (int)drops[x].y - k; if (y < 0 || y >= H) continue;
            uint8_t c[3] = {0, (uint8_t)(k ? 220 - k * 200 / drops[x].len : 255), (uint8_t)(k ? 40 : 200)}; if (!k) c[0] = 180;
            px(x, y, c);
        }
    }
    (void)t;
}

static void effect_render(double t) {
    double dt = eff_last ? t - eff_last : 0.05; if (dt > 0.2) dt = 0.05; eff_last = t;
    switch (effect_idx) {
    case 0: fx_snake(t); break;           case 1: fx_balls(t, dt); break;     case 2: fx_match3(t); break;
    case 3: fx_colorwall(t, dt); break;   case 4: fx_flow(t); break;          case 5: fx_linespray(t, dt); break;
    case 6: fx_linefill(t); break;        case 7: fx_heap(t); break;          case 8: fx_fireworks(t, dt); break;
    case 9: fx_starfield(t, dt); break;   default: fx_rain(t, dt); break;
    }
}

/* ------------------------------------------------------------------ music mode (microphone) */
// Audio is captured with arecord only while music mode is shown, analysed in memory and discarded.
static const char *MUSIC_STYLES[] = {"Spectrum", "Fire", "Explosion", "Wave"};
#define NMSTYLES 4
static FILE *mic; static int mic_fd = -1;
#define NFFT 512
static int16_t abuf[NFFT]; static int an;
static float bands[W], peaks[W], level;
static int beats;  // counts detected beats (onsets); music styles compare it with their last seen value
static float beat_strength;  // 0..1, how strong the last beat was
#define MIC_GATE 12.0f      // dB above a band's noise floor before anything shows
#define MIC_MINRANGE 40.0f  // dB: smallest range mapped to the full bar height (quiet sounds stay small)
#define MIC_BEAT_K 1.5f     // a beat needs this many times the average spectral flux
static double level_pub;

static void mic_start(void) {
    if (mic) return;
    mic = popen("exec arecord -q -D hw:0,0 -f S16_LE -r 16000 -c 1 -t raw 2>/dev/null", "r");
    if (!mic) { logf_("mic: popen failed"); return; }
    mic_fd = fileno(mic); fcntl(mic_fd, F_SETFL, O_NONBLOCK);
    an = 0; logf_("mic on (music mode)");
}
static void mic_stop(void) {
    if (!mic) return;
    system("killall arecord 2>/dev/null"); pclose(mic); mic = NULL; mic_fd = -1;
    memset(bands, 0, sizeof bands); level = 0; logf_("mic off");
}

static void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) { int bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2 * (float)M_PI / len, wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                float ur = re[i + k], ui = im[i + k], vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci, vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi; re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                float nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

static void mic_read(void) {
    if (!mic) return;
    for (;;) {
        ssize_t r = read(mic_fd, (uint8_t *)abuf + an * 2, (NFFT - an) * 2);
        if (r <= 0) break;
        an += (int)(r / 2);
        if (an < NFFT) continue;
        an = 0;
        // Tuned on recordings from this mic (silence / typing / music, 2026-11-15): each band is measured in dB
        // above its own learned noise floor (room hum sits ~30 dB at 60-90 Hz), a gate hides room noise and typing
        // clicks, and an auto ceiling (fast attack, slow release, >= MIC_MINRANGE dB) scales music to the height.
        static float re[NFFT], im[NFFT], floor_db[W], ceil_db = MIC_MINRANGE; static int edges[W + 1], init;
        if (!init) {  // contiguous log-spaced bands 120 Hz .. 6.5 kHz (bin = 31.25 Hz), at least one bin each
            init = 1; int prev = (int)(120.0f / (16000.0f / NFFT));
            for (int b = 0; b <= W; b++) {
                int e = (int)lroundf(120.0f * powf(6500.0f / 120.0f, b / (float)W) / (16000.0f / NFFT));
                if (b && e <= prev) e = prev + 1;
                edges[b] = prev = e;
            }
            for (int b = 0; b < W; b++) floor_db[b] = 25;
        }
        float mean = 0;
        for (int i = 0; i < NFFT; i++) mean += abuf[i];
        mean /= NFFT;
        for (int i = 0; i < NFFT; i++) {
            float w = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (NFFT - 1));
            re[i] = (abuf[i] - mean) * w; im[i] = 0;
        }
        fft(re, im, NFFT);
        float ex[W], top = 0, sum = 0;
        for (int b = 0; b < W; b++) {
            float m = 0;
            for (int k = edges[b]; k < edges[b + 1] && k < NFFT / 2; k++) { float v = sqrtf(re[k] * re[k] + im[k] * im[k]); if (v > m) m = v; }
            float db = 20 * log10f(m / (NFFT / 4) + 1e-3f);                         // ~dB of the sine amplitude
            if (db < floor_db[b]) floor_db[b] += (db - floor_db[b]) * 0.1f;        // floor falls fast (~0.3 s)
            else floor_db[b] += 0.01f;                                              // and rises ~0.3 dB/s
            if (floor_db[b] > 35) floor_db[b] = 35;                                 // continuous music never becomes "floor"
            ex[b] = fmaxf(0, db - floor_db[b] - MIC_GATE);
            if (ex[b] > top) top = ex[b];
        }
        ceil_db = top > ceil_db ? top : fmaxf(MIC_MINRANGE, ceil_db - 0.05f);      // release ~1.5 dB/s
        for (int b = 0; b < W; b++) {
            float v = fminf(1.0f, ex[b] / (ceil_db * 0.9f));
            bands[b] = v > bands[b] ? bands[b] + (v - bands[b]) * 0.5f : bands[b] * 0.85f;  // soft attack damps clicks
            peaks[b] = bands[b] > peaks[b] ? bands[b] : peaks[b] - 0.01f;
            sum += bands[b];
        }
        level = fminf(1.0f, sum / W * 2.2f);  // ~0 silence, ~0.3 typing, ~0.8 music
        // beat = onset across the whole spectrum ("spectral flux": how much louder the bands just got, from the
        // raw values before smoothing), above an adaptive threshold. Bass-only detection missed most music on
        // this small mic. While music plays, a beat is forced after 1.2 s without one so the effects never stall.
        {
            static float prev_ex[W], flux_avg; static double last_beat, stat_t; static int stat_beats;
            float flux = 0;
            for (int b = 0; b < W; b++) { float d = ex[b] - prev_ex[b]; if (d > 0) flux += d; prev_ex[b] = ex[b]; }
            flux /= ceil_db * W;  // 0 .. ~1
            double tn = now_s();
            int onset = flux > flux_avg * MIC_BEAT_K + 0.01f && level > 0.12f && tn - last_beat > 0.15;
            int forced = !onset && level > 0.25f && tn - last_beat > 1.2;
            if (onset || forced) {
                beats++; stat_beats++; last_beat = tn;
                beat_strength = forced ? 0.5f : fminf(1.0f, flux / (flux_avg * MIC_BEAT_K + 0.01f) * 0.6f);
            }
            flux_avg += (flux - flux_avg) * 0.05f;  // ~0.6 s memory
            if (tn - stat_t >= 10) {  // tuning aid: beat rate in the log while music mode runs
                if (stat_t) logf_("music: %d beats in 10 s, level %.2f, flux avg %.3f", stat_beats, level, flux_avg);
                stat_t = tn; stat_beats = 0;
            }
        }
    }
}

static void music_render(double t) {
    uint8_t c[3];
    if (!mic) { text_at("no mic", 8, 12, C_TEXT); return; }
    switch (music_style) {
    case 0:  // spectrum bars with peaks
        for (int x = 0; x < W; x++) {
            int h = (int)(bands[x] * H);
            for (int y = 0; y < h; y++) { hsv(0.33f - y / (float)H * 0.33f, 1, 1, c); px(x, H - 1 - y, c); }
            int py = H - 1 - (int)(peaks[x] * H); const uint8_t wh[3] = {255, 255, 255}; if (peaks[x] > 0.03f) px(x, py, wh);
        }
        break;
    case 1:  // fire: spectrum feeds a heat field that rises
        {
            static uint8_t heat[H + 2][W];
            for (int x = 0; x < W; x++) heat[H + 1][x] = heat[H][x] = (uint8_t)fminf(255, bands[x] * 300 + rndi(30));
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                int s = heat[y + 1][x] + heat[y + 1][(x + W - 1) % W] + heat[y + 1][(x + 1) % W] + heat[y + 2][x];
                int v = s / 4 - 6; heat[y][x] = (uint8_t)(v < 0 ? 0 : v);
            }
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                int v = heat[y][x]; c[0] = (uint8_t)(v > 85 ? 255 : v * 3); c[1] = (uint8_t)(v > 170 ? 255 : v > 85 ? (v - 85) * 3 : 0); c[2] = (uint8_t)(v > 200 ? (v - 200) * 4 : 0);
                px(x, y, c);
            }
        }
        break;
    case 2:  // explosion: colour flood like Govee's music "explosion" scene
        {
            // Every beat starts a wavefront (diamond, square or circle) from an origin. The front is a solid band
            // that stays at full brightness until it leaves the screen and paints over whatever it crosses; behind
            // it the colour fades out, so the screen shows bands travelling outward. The origin stays for 2-6 beats
            // (nested bands), then jumps (often to an edge/corner). Each beat picks a new shape, never the same as the last.
            // Stronger beats travel faster. Every pixel remembers the newest wave that painted it, and an older front
            // can't repaint a newer wave's band or trail: when a fast wave overtakes an older one, the older one is
            // absorbed instead of reappearing behind it, so the most recent wave always wins.
            #define NWAVE 12
            static struct { float x, y, r, v, band; int shape, on; uint32_t id; uint8_t c[3]; } wv[NWAVE];
            static uint32_t owner[H][W], wave_id;
            static int seen, next, left, shape; static float ox, oy, hue;
            if (beats != seen) {
                seen = beats;
                if (left <= 0) {  // new origin
                    left = 2 + rndi(5);
                    int edge = rndi(3);  // 2/3: on an edge or corner, 1/3: anywhere
                    ox = rndi(W); oy = rndi(H);
                    if (edge) { if (rndi(2)) ox = rndi(2) ? 0 : W - 1; else oy = rndi(2) ? 0 : H - 1; }
                    hue = rndi(1000) / 1000.0f;
                }
                left--;
                shape = (shape + 1 + rndi(2)) % 3;  // random shape per beat, different from the previous one
                hue = fmodf(hue + 0.09f + rndi(40) / 1000.0f, 1);
                __typeof__(wv[0]) *w = &wv[next]; next = (next + 1) % NWAVE;  // reuse the oldest
                w->on = 1; w->x = ox; w->y = oy; w->r = 0; w->shape = shape; w->id = ++wave_id;
                w->v = 0.9f + beat_strength * 0.9f;    // px per frame: stronger beats spread faster
                w->band = 3 + beat_strength * 3;        // front thickness, px
                hsv(hue, 1, 1, w->c);
            }
            fade_fb(efb, 9, 10);  // the trail behind each front fades in about a second
            for (int n = 0; n < NWAVE; n++) {
                int i = (next + n) % NWAVE;  // oldest first, newer waves paint on top
                if (!wv[i].on) continue;
                wv[i].r += wv[i].v;
                float r = wv[i].r, cov = 0;  // cov: distance to the farthest corner in this wave's metric
                for (int k = 0; k < 4; k++) {
                    float dx = fabsf((k & 1 ? W - 1 : 0) - wv[i].x), dy = fabsf((k & 2 ? H - 1 : 0) - wv[i].y);
                    float d = wv[i].shape == 0 ? dx + dy : wv[i].shape == 1 ? fmaxf(dx, dy) : sqrtf(dx * dx + dy * dy);
                    if (d > cov) cov = d;
                }
                int x0 = (int)fmaxf(0, wv[i].x - r - 2), x1 = (int)fminf(W - 1, wv[i].x + r + 2);
                int y0 = (int)fmaxf(0, wv[i].y - r - 2), y1 = (int)fminf(H - 1, wv[i].y + r + 2);
                for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
                    float dx = fabsf(x - wv[i].x), dy = fabsf(y - wv[i].y);
                    float d = wv[i].shape == 0 ? dx + dy : wv[i].shape == 1 ? fmaxf(dx, dy) : sqrtf(dx * dx + dy * dy);
                    if (wv[i].id < owner[y][x]) continue;  // a newer wave owns this pixel
                    uint8_t *q = &efb[(y * W + x) * 3];
                    if (d <= r && d > r - wv[i].band) { memcpy(q, wv[i].c, 3); owner[y][x] = wv[i].id; }
                    else if (d > r && d <= r + 1) {   // one-pixel soft edge ahead of the band (not owned yet)
                        float a = 0.2f + 0.6f * (1 - (d - r));
                        for (int k = 0; k < 3; k++) q[k] = (uint8_t)(wv[i].c[k] * a + q[k] * (1 - a));
                    }
                }
                if (r - wv[i].band > cov + 1) wv[i].on = 0;  // the front has left the screen
            }
            memcpy(fb, efb, FRAME);
        }
        break;
    default:  // wave: mirrored spectrum around the middle line
        for (int x = 0; x < W; x++) {
            int h = (int)(bands[x] * 15);
            hsv(x / (float)W + (float)fmod(t * 0.05, 1.0), 1, 1, c);
            for (int y = -h; y <= h; y++) px(x, 16 + y, c);
        }
        break;
    }
}

/* ------------------------------------------------------------------ info screens (data pushed by Home Assistant) */
static struct { char cond[24], unit[4]; float temp, hi, lo; int hum, has_hilo; double upd; } wx;
#define NCLIM 6
static struct { char name[16]; float temp; int hum; char unit[4]; double upd; } clim[NCLIM];
#define NSTOCK 6
static struct { char sym[10]; char price[12]; float chg; double upd; } stock[NSTOCK];
#define NSCORE 4
static struct { char league[8], home[8], away[8], status[16]; int hs, as; double upd; } score[NSCORE];
static int info_cur = -1; static double info_next;

static const char *wx_icon(const char *c) {
    if (strstr(c, "lightning")) return "wx_thunder";
    if (strstr(c, "snow")) return "wx_snow";
    if (strstr(c, "hail")) return "wx_hail";
    if (strstr(c, "rain") || strstr(c, "pouring")) return "wx_rain";
    if (strstr(c, "fog")) return "wx_fog";
    if (!strcmp(c, "sunny") || !strcmp(c, "clear")) return "wx_sunny";
    return NULL;  // cloudy, partlycloudy, windy, clear-night, exceptional: handled below
}

/* ------------------------------------------------------------------ analog clock (Govee face, hands drawn here) */
static void hand(float cx, float cy, float deg, float len, int thick, const uint8_t *c) {
    float a = deg * (float)M_PI / 180.0f, sx = sinf(a), sy = -cosf(a);
    for (float r = 0; r <= len; r += 0.25f) {
        int x = (int)floorf(cx + r * sx), y = (int)floorf(cy + r * sy);
        px(x, y, c);
        if (thick) { px(x + (fabsf(sy) > fabsf(sx) ? 1 : 0), y + (fabsf(sy) > fabsf(sx) ? 0 : 1), c); }
    }
}

static void draw_analog(void) {
    time_t n = time(NULL); struct tm t; localtime_r(&n, &t);
    const int fx = 11, fy = 1;
    icon_draw("clk_left", 3, 4); icon_draw("clk_face", fx, fy); icon_draw("clk_right", 42, 4);
    float cx = fx + 15.0f, cy = fy + 15.0f;   // centre of the 30x30 face (between pixels 14 and 15)
    static const uint8_t hc[3] = {232, 189, 32}, mc[3] = {255, 255, 255}, cc[3] = {212, 0, 71}, sc[3] = {212, 0, 71};
    hand(cx, cy, (t.tm_min + t.tm_sec / 60.0f) * 6.0f, 11.5f, 0, mc);
    hand(cx - 0.5f, cy - 0.5f, ((t.tm_hour % 12) + t.tm_min / 60.0f) * 30.0f, 7.0f, 1, hc);
    // seconds: a red dot running round the rim of the face. The ring is the face's outermost pixels (opaque
    // pixels touching the outside, from the icon's alpha); the numerals and hour marks sit 2+ px further in, so
    // the dot never covers them. Each second picks the ring pixel closest to its angle (60 distinct positions).
    static int ring[160][2], nring; static float ringa[160];
    icon_t *face = icon_get("clk_face");
    if (!nring && face) {
        const float ccx = 14.5f, ccy = 14.5f;  // the 30x30 face is centred between pixels 14 and 15
        for (int y = 0; y < face->h && nring < 160; y++)
            for (int x = 0; x < face->w && nring < 160; x++) {
                if (face->rgba[(y * face->w + x) * 4 + 3] < 128) continue;
                int edge = 0;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                    int X = x + dx, Y = y + dy;
                    if (X < 0 || Y < 0 || X >= face->w || Y >= face->h || face->rgba[(Y * face->w + X) * 4 + 3] < 128) edge = 1;
                }
                if (!edge) continue;
                ring[nring][0] = x; ring[nring][1] = y;
                ringa[nring++] = atan2f(x - ccx, -(y - ccy));  // 0 at 12 o'clock, clockwise
            }
    }
    if (nring) {
        float want = t.tm_sec * 6.0f * (float)M_PI / 180.0f, best = 1e9f; int bi = 0;
        for (int i = 0; i < nring; i++) {
            float d = fabsf(remainderf(ringa[i] - want, 2 * (float)M_PI));
            if (d < best) { best = d; bi = i; }
        }
        px(fx + ring[bi][0], fy + ring[bi][1], sc);
    }
    for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) px(fx + 14 + i, fy + 14 + j, cc);
}

static void draw_moon(int x, int y) {
    const uint8_t m[3] = {255, 230, 140};
    for (int j = 0; j < 12; j++) for (int i = 0; i < 12; i++) {
        float d1 = (i - 5.5f) * (i - 5.5f) + (j - 5.5f) * (j - 5.5f), d2 = (i - 8.5f) * (i - 8.5f) + (j - 3.5f) * (j - 3.5f);
        if (d1 < 30 && d2 > 22) px(x + i, y + j, m);
    }
}

// incoming temps: "unit" field ("F"/"°F"/"C"); without one, assume the unit currently displayed
static float to_c(const char *s, float v) {
    char u[8]; int f = json_str(s, "unit", u, sizeof u) ? temp_f : (strchr(u, 'F') || strchr(u, 'f')) ? 1 : 0;
    return f ? (v - 32.0f) * 5.0f / 9.0f : v;
}
static float disp_t(float c) { return temp_f ? c * 9.0f / 5.0f + 32.0f : c; }
static void fmt_temp(char *b, size_t n, float v) { if (fabsf(v - roundf(v)) < 0.05f || fabsf(v) >= 100) snprintf(b, n, "%d", (int)roundf(v)); else snprintf(b, n, "%.1f", v); }

static int screen_weather(void) {
    if (!wx.upd || now_s() - wx.upd > 6 * 3600) return 0;
    const char *ic = wx_icon(wx.cond);
    if (ic) icon_draw(ic, 1, 3);
    else if (!strcmp(wx.cond, "clear-night")) draw_moon(2, 5);
    else icon_draw("wx_cloudy", 1, 3);
    char t[8]; snprintf(t, sizeof t, "%d", (int)roundf(disp_t(wx.temp)));
    const uint8_t tc[3] = {255, 255, 255}, hl[3] = {150, 150, 150}, hb[3] = {80, 170, 255};
    int tw = (int)strlen(t) * 12 - 2, tx = 19 + (33 - tw - 4) / 2;
    big_text(t, tx, 2, tc); degree(tx + tw + 1, 2, tc);
    char l[24];
    if (wx.has_hilo) { snprintf(l, sizeof l, "%d/%d", (int)roundf(disp_t(wx.hi)), (int)roundf(disp_t(wx.lo))); tiny_text(l, 19 + (33 - tiny_w(l)) / 2, 17, hl); }
    if (wx.hum > 0) { snprintf(l, sizeof l, "%d%%", wx.hum); icon_draw("env_hum", 19, 23); tiny_text(l, 28, 25, hb); }
    // condition name along the bottom-left under the icon
    char cn[12]; snprintf(cn, sizeof cn, "%s", wx.cond); for (char *p = cn; *p; p++) if (*p == '-') { *p = 0; break; }
    if (strlen(cn) > 4) cn[4] = 0;
    tiny_text(cn, 1, 25, hl);
    return 1;
}

static int screen_climate(int i) {
    if (!clim[i].upd || now_s() - clim[i].upd > 6 * 3600) return 0;
    const uint8_t nc[3] = {120, 200, 255}, tc[3] = {255, 200, 120}, hc[3] = {120, 180, 255};
    text_at(clim[i].name, (W - text_w(clim[i].name)) / 2, 1, nc);
    char b[16]; fmt_temp(b, sizeof b, disp_t(clim[i].temp));
    icon_draw("env_temp", 3, 12); text_at(b, 13, 13, tc); degree(13 + text_w(b) + 1, 13, tc);
    if (clim[i].hum >= 0) { snprintf(b, sizeof b, "%d%%", clim[i].hum); icon_draw("env_hum", 3, 22); text_at(b, 13, 23, hc); }
    return 1;
}

static int screen_stock(int i) {
    if (!stock[i].upd || now_s() - stock[i].upd > 24 * 3600) return 0;
    const uint8_t sc[3] = {255, 255, 255}, up[3] = {40, 220, 80}, dn[3] = {255, 60, 60}, fl[3] = {180, 180, 180};
    const uint8_t *cc = stock[i].chg > 0.005f ? up : stock[i].chg < -0.005f ? dn : fl;
    text_at(stock[i].sym, 2, 1, sc);
    icon_draw(stock[i].chg > 0.005f ? "fin_rise" : stock[i].chg < -0.005f ? "fin_fall" : "fin_flat", W - 11, 0);
    text_at(stock[i].price, (W - text_w(stock[i].price)) / 2, 12, sc);
    char b[16]; snprintf(b, sizeof b, "%+.2f%%", stock[i].chg);
    text_at(b, (W - text_w(b)) / 2, 23, cc);
    return 1;
}

static int screen_score(int i) {
    if (!score[i].upd || now_s() - score[i].upd > 8 * 3600) return 0;
    const uint8_t tc[3] = {255, 255, 255}, ac[3] = {255, 120, 120}, hc2[3] = {120, 170, 255}, stc[3] = {200, 200, 120};
    char ia[24], ih[24], lg[8];
    snprintf(lg, sizeof lg, "%s", score[i].league); for (char *p = lg; *p; p++) *p = (char)tolower((unsigned char)*p);
    snprintf(ia, sizeof ia, "%s_%s", lg, score[i].away); snprintf(ih, sizeof ih, "%s_%s", lg, score[i].home);
    for (char *p = ia; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (char *p = ih; *p; p++) *p = (char)tolower((unsigned char)*p);
    // away on the left, home on the right; logo if we have it, otherwise the abbreviation
    if (!icon_draw(ia, 2, 2)) tiny_text(score[i].away, 2, 3, ac);
    icon_get(ih);
    icon_t *hic = icon_get(ih);
    if (hic) icon_draw(ih, W - 2 - hic->w, 2); else tiny_text(score[i].home, W - 2 - tiny_w(score[i].home), 3, hc2);
    char a[8], h[8]; snprintf(a, sizeof a, "%d", score[i].as); snprintf(h, sizeof h, "%d", score[i].hs);
    text_at(a, 13 - text_w(a) / 2, 13, ac); text_at("-", 24, 13, tc); text_at(h, 39 - text_w(h) / 2, 13, hc2);
    tiny_text(score[i].status, (W - tiny_w(score[i].status)) / 2, 25, stc);
    return 1;
}

// screen ids: 0 weather, 10+i climate, 20+i stock, 30+i score
static int info_draw_id(int id) {
    if (id == 0) return screen_weather();
    if (id >= 10 && id < 10 + NCLIM) return screen_climate(id - 10);
    if (id >= 20 && id < 20 + NSTOCK) return screen_stock(id - 20);
    if (id >= 30 && id < 30 + NSCORE) return screen_score(id - 30);
    return 0;
}
static void info_render(double t) {
    static const int order[] = {0, 10, 11, 12, 13, 14, 15, 30, 31, 32, 33, 20, 21, 22, 23, 24, 25};
    int n = (int)(sizeof order / sizeof order[0]);
    if (info_cur < 0 || t >= info_next) {  // advance to the next screen that has data
        int start = info_cur < 0 ? -1 : info_cur;
        for (int k = 1; k <= n; k++) {
            int idx = (start + k) % n;
            memset(fb, 0, sizeof fb);
            if (info_draw_id(order[idx])) { info_cur = idx; break; }
            if (k == n) info_cur = -1;
        }
        info_next = t + info_rotate;
    }
    memset(fb, 0, sizeof fb);
    if (info_cur < 0 || !info_draw_id(order[info_cur])) {
        const uint8_t g[3] = {120, 120, 120};
        tiny_text("NO INFO", (W - tiny_w("NO INFO")) / 2, 8, g);
        tiny_text("SEND DATA", (W - tiny_w("SEND DATA")) / 2, 16, g);
        tiny_text("FROM HA", (W - tiny_w("FROM HA")) / 2, 22, g);
    }
}

static float json_num(const char *s, const char *k, float def) { char v[32]; return json_str(s, k, v, sizeof v) ? def : (float)atof(v); }

static int info_message(const char *topic, const char *s) {
    char v[64];
    if (!strcmp(topic, BASE "/weather/set")) {
        if (json_str(s, "condition", wx.cond, sizeof wx.cond)) snprintf(wx.cond, sizeof wx.cond, "cloudy");
        for (char *p = wx.cond; *p; p++) *p = (char)tolower((unsigned char)*p);
        wx.temp = to_c(s, json_num(s, "temp", 0)); wx.hum = (int)json_num(s, "humidity", -1);
        wx.has_hilo = !json_str(s, "high", v, sizeof v) && strcmp(v, "null") && v[0]; wx.hi = to_c(s, json_num(s, "high", 0)); wx.lo = to_c(s, json_num(s, "low", 0));
        wx.upd = now_s(); return 1;
    }
    if (!strcmp(topic, BASE "/climate/set")) {  // {"name":"Living","temp":21.5,"humidity":44} ; {"name":"Living","remove":1}
        char name[16]; if (json_str(s, "name", name, sizeof name)) return 1;
        int slot = -1, free_slot = -1;
        for (int i = 0; i < NCLIM; i++) { if (clim[i].upd && !strcmp(clim[i].name, name)) slot = i; if (!clim[i].upd && free_slot < 0) free_slot = i; }
        if (!json_str(s, "remove", v, sizeof v)) { if (slot >= 0) clim[slot].upd = 0; return 1; }
        if (slot < 0) slot = free_slot; if (slot < 0) slot = 0;
        snprintf(clim[slot].name, sizeof clim[0].name, "%s", name);
        clim[slot].temp = to_c(s, json_num(s, "temp", 0)); clim[slot].hum = (int)json_num(s, "humidity", -1); clim[slot].upd = now_s();
        return 1;
    }
    if (!strcmp(topic, BASE "/stock/set")) {  // {"symbol":"AAPL","price":"189.20","change":-1.25}
        char sym[10]; if (json_str(s, "symbol", sym, sizeof sym)) return 1;
        int slot = -1, free_slot = -1;
        for (int i = 0; i < NSTOCK; i++) { if (stock[i].upd && !strcasecmp(stock[i].sym, sym)) slot = i; if (!stock[i].upd && free_slot < 0) free_slot = i; }
        if (!json_str(s, "remove", v, sizeof v)) { if (slot >= 0) stock[slot].upd = 0; return 1; }
        if (slot < 0) slot = free_slot; if (slot < 0) slot = 0;
        snprintf(stock[slot].sym, sizeof stock[0].sym, "%s", sym);
        if (json_str(s, "price", stock[slot].price, sizeof stock[0].price)) stock[slot].price[0] = 0;
        stock[slot].chg = json_num(s, "change", 0); stock[slot].upd = now_s();
        return 1;
    }
    if (!strcmp(topic, BASE "/score/set")) {  // {"league":"NFL","away":"BUF","home":"KC","away_score":17,"home_score":21,"status":"Q3 5:12"}
        char home[8], away[8];
        if (json_str(s, "home", home, sizeof home) || json_str(s, "away", away, sizeof away)) {
            if (!strcmp(s, "clear")) for (int i = 0; i < NSCORE; i++) score[i].upd = 0;
            return 1;
        }
        int slot = -1, free_slot = -1;
        for (int i = 0; i < NSCORE; i++) { if (score[i].upd && !strcasecmp(score[i].home, home) && !strcasecmp(score[i].away, away)) slot = i; if (!score[i].upd && free_slot < 0) free_slot = i; }
        if (!json_str(s, "remove", v, sizeof v)) { if (slot >= 0) score[slot].upd = 0; return 1; }
        if (slot < 0) slot = free_slot; if (slot < 0) slot = 0;
        snprintf(score[slot].home, 8, "%s", home); snprintf(score[slot].away, 8, "%s", away);
        if (json_str(s, "league", score[slot].league, sizeof score[0].league)) snprintf(score[slot].league, 8, "NFL");
        score[slot].hs = (int)json_num(s, "home_score", 0); score[slot].as = (int)json_num(s, "away_score", 0);
        if (json_str(s, "status", score[slot].status, sizeof score[0].status)) score[slot].status[0] = 0;
        score[slot].upd = now_s();
        return 1;
    }
    if (!strcmp(topic, BASE "/info_rotate/set")) { int r = atoi(s); if (r >= 3 && r <= 120) info_rotate = r; return 1; }
    return 0;
}

/* ------------------------------------------------------------------ countdown / stopwatch */
static double cd_end, cd_done_until; static int cd_running;
static double sw_start, sw_acc; static int sw_running;

static void countdown_start(double secs) { cd_end = now_s() + secs; cd_running = 1; cd_done_until = 0; logf_("countdown %.0fs", secs); }

static int countdown_render(double t) {  // returns 1 while the "time's up" screen should stay
    const uint8_t dc[3] = {255, 255, 255}, rc[3] = {255, 60, 60};
    if (cd_running && t >= cd_end) {
        cd_running = 0; cd_done_until = t + 10;
        play_sound(cd_sound, chime_vol > 0 ? chime_vol : 60);
        mq_pub(BASE "/countdown/event", "done", 0);
    }
    if (!cd_running && t < cd_done_until) {
        if (phase(t, 3, 2)) fill_rect(0, 0, W, H, rc);
        text_at("TIME!", (W - text_w("TIME!")) / 2, 12, dc);
        return 1;
    }
    int rem = cd_running ? (int)ceil(cd_end - t) : 0; if (rem < 0) rem = 0;
    char b[12];
    if (rem >= 3600) snprintf(b, sizeof b, "%d:%02d", rem / 3600, rem / 60 % 60); else snprintf(b, sizeof b, "%d:%02d", rem / 60, rem % 60);
    icon_draw("hourglass", 1, 4);
    int tw = text_w(b); text_at(b, 16 + (36 - tw) / 2, 8, dc);
    if (rem >= 3600) tiny_text("H:M", 16 + (36 - tiny_w("H:M")) / 2, 18, rc);
    else if (!cd_running) tiny_text("STOPPED", 16 + (36 - tiny_w("STOPPED")) / 2, 18, rc);
    return 0;
}

static void stopwatch_render(double t) {
    double el = sw_acc + (sw_running ? t - sw_start : 0);
    int cs = (int)(el * 10) % 10, s = (int)el % 60, m = (int)el / 60 % 100;
    icon_draw("stopwatch", 1, 1);
    char b[12]; snprintf(b, sizeof b, "%02d:%02d", m, s);
    const uint8_t wc[3] = {255, 255, 255}, tc[3] = {255, 180, 60};
    text_at(b, (W - text_w(b)) / 2, 11, wc);
    snprintf(b, sizeof b, ".%d", cs); tiny_text(b, 36, 20, tc);
}

/* ------------------------------------------------------------------ playlist */
#define PLAYLIST_FILE "/data/custom/playlist.txt"
static char playlist[256] = "clock:30,info:40,animation:Clock Night:20,effect:Flow:20";
static int pl_idx = -1; static double pl_next;
static char pl_kind[16], pl_arg[64];

// item "kind[:arg]:seconds"
static int pl_item(int i, char *kind, char *arg, int *secs) {
    const char *p = playlist; int k = 0;
    while (*p && k < i) { if (*p == ',') k++; p++; }
    if (!*p) return 0;
    char item[96]; size_t n = strcspn(p, ","); if (n >= sizeof item) n = sizeof item - 1;
    memcpy(item, p, n); item[n] = 0;
    char *a = strchr(item, ':'), *b = strrchr(item, ':');
    *secs = 20; arg[0] = 0;
    if (!a) { snprintf(kind, 16, "%s", item); return 1; }
    *a = 0; snprintf(kind, 16, "%s", item);
    if (b != a) { *b = 0; snprintf(arg, 64, "%s", a + 1); *secs = atoi(b + 1); }
    else { int v = atoi(a + 1); if (v > 0) *secs = v; else snprintf(arg, 64, "%s", a + 1); }
    if (*secs < 3) *secs = 3;
    return 1;
}
static int pl_count(void) { int n = playlist[0] ? 1 : 0; for (const char *p = playlist; *p; p++) n += *p == ','; return n; }

static void playlist_advance(double t) {
    int n = pl_count(); if (!n) return;
    int secs = 20;
    pl_idx = (pl_idx + 1) % n;
    if (!pl_item(pl_idx, pl_kind, pl_arg, &secs)) return;
    if (!strcasecmp(pl_kind, "animation") && pl_arg[0]) anim_load(pl_arg, &cur_anim);
    if (!strcasecmp(pl_kind, "effect") && pl_arg[0]) { int e = effect_find(pl_arg); if (e >= 0) effect_start(e); }
    if (!strcasecmp(pl_kind, "music") && pl_arg[0]) for (int i = 0; i < NMSTYLES; i++) if (!strcasecmp(MUSIC_STYLES[i], pl_arg)) music_style = i;
    if (!strcasecmp(pl_kind, "text")) st.text_start = t;
    if (!strcasecmp(pl_kind, "info")) info_cur = -1;
    pl_next = t + secs;
}

static void playlist_load(void) { FILE *f = fopen(PLAYLIST_FILE, "r"); if (!f) return; if (fgets(playlist, sizeof playlist, f)) playlist[strcspn(playlist, "\r\n")] = 0; fclose(f); }
static void playlist_save(void) { FILE *f = fopen(PLAYLIST_FILE, "w"); if (!f) return; fprintf(f, "%s\n", playlist); fclose(f); }
