// pixeld: display daemon for the repurposed Govee H6631 (52x32 RGB panel).
// Owns the panel (via /usr/lib/libgovee_display.so) and takes commands from:
//   * MQTT (Home Assistant discovery: light on/off+brightness, mode select, text, notify)
//   * UDP port 7777: raw 52x32 RGB frames (4992 bytes, row-major from top-left) for live
//     animations from any program; the panel returns to the normal mode 3 s after the last frame.
//   * HTTP port 80: Wi-Fi setup page + the LAN API used by pixel2mqtt (api.c; UDP 7778 = discovery).
//     While pixel2mqtt is connected it owns MQTT/HA discovery and pixeld's own MQTT client stays off.
// Config: /data/custom/mqtt.conf (host= port= user= pass=), TZ env var for the clock.
// State (power, brightness, mode, text) persists in /data/custom/pixeld.state.
// Build: zig cc -target arm-linux-gnueabihf.2.29 -mcpu=cortex_a7 -O2 -fPIE -pie -s -o pixeld pixeld.c -ldl
#define _GNU_SOURCE  // strcasestr
#include <arpa/inet.h>
#include <ctype.h>
#include <dlfcn.h>
#include <linux/input.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#define W 52
#define H 32
#define FRAME (W * H * 3)
#define BASE "govee-pixel"
#define DISC "homeassistant"
#define STATE_FILE "/data/custom/pixeld.state"
#define MQTT_CONF "/data/custom/mqtt.conf"
#define UDP_PORT 7777

static void logf_(const char *fmt, ...) {
    char ts[32]; time_t n = time(NULL); struct tm t; localtime_r(&n, &t);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &t);
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "%s pixeld: ", ts); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

#ifdef PIXELD_STUB_DISPLAY
static double stub_now;  // host builds: a fixed clock for reproducible renders (tools/screen_preview.c)
static double now_s(void) { if (stub_now > 0) return stub_now; struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }
#else
static double now_s(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }
#endif

/* ------------------------------------------------------------------ display */
static int (*d_init)(void), (*d_deinit)(void), (*d_reset)(void), (*d_lumi)(uint8_t);
static int (*d_data)(int, int, const uint8_t *, int);
static uint8_t fb[FRAME];

#ifdef PIXELD_STUB_DISPLAY  // host test build (no panel): see pixel2mqtt/tests/run_pixeld_host.sh
static int stub_ok(void) { return 0; }
static int stub_lumi(uint8_t v) { (void)v; return 0; }
static int stub_data(int w, int h, const uint8_t *b, int n) { (void)w; (void)h; (void)b; (void)n; return 0; }
static int display_open(void) { d_init = d_deinit = d_reset = stub_ok; d_lumi = stub_lumi; d_data = stub_data; return 0; }
#else
static int display_open(void) {
    void *h = dlopen("/usr/lib/libgovee_display.so", RTLD_NOW);
    if (!h) { logf_("dlopen: %s", dlerror()); return -1; }
    d_init = (int (*)(void))dlsym(h, "LightKal_lcd_display_init");
    d_deinit = (int (*)(void))dlsym(h, "LightKal_lcd_display_deinit");
    d_reset = (int (*)(void))dlsym(h, "LightKal_lcd_display_reset_frame");
    d_lumi = (int (*)(uint8_t))dlsym(h, "LightKal_lcd_display_lumi_set");
    d_data = (int (*)(int, int, const uint8_t *, int))dlsym(h, "LightKal_lcd_display_data_set");
    if (!d_init || !d_deinit || !d_reset || !d_lumi || !d_data) { logf_("missing display symbols"); return -1; }
    return d_init();
}
#endif

static void px(int x, int y, const uint8_t *c) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    uint8_t *p = &fb[(y * W + x) * 3]; p[0] = c[0]; p[1] = c[1]; p[2] = c[2];
}

/* 5x7 ASCII font 0x20..0x7E, column bytes, bit0 = top row */
static const uint8_t F5[95][5] = {
    {0,0,0,0,0},{0,0,0x5F,0,0},{0,7,0,7,0},{0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},
    {0x23,0x13,8,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0,5,3,0,0},{0,0x1C,0x22,0x41,0},{0,0x41,0x22,0x1C,0},
    {0x2A,0x1C,0x7F,0x1C,0x2A},{8,8,0x3E,8,8},{0,0x50,0x30,0,0},{8,8,8,8,8},{0,0x60,0x60,0,0},
    {0x20,0x10,8,4,2},{0x3E,0x51,0x49,0x45,0x3E},{0,0x42,0x7F,0x40,0},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{1,0x71,9,5,3},{0x36,0x49,0x49,0x49,0x36},
    {6,0x49,0x49,0x29,0x1E},{0,0x36,0x36,0,0},{0,0x56,0x36,0,0},{8,0x14,0x22,0x41,0},{0x14,0x14,0x14,0x14,0x14},
    {0,0x41,0x22,0x14,8},{2,1,0x51,9,6},{0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,9,9,9,1},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,8,8,8,0x7F},{0,0x41,0x7F,0x41,0},{0x20,0x40,0x41,0x3F,1},{0x7F,8,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,2,0x0C,2,0x7F},{0x7F,4,8,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},{0x7F,9,9,9,6},{0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,9,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{1,1,0x7F,1,1},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,8,0x14,0x63},{7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},{0,0x7F,0x41,0x41,0},
    {2,4,8,0x10,0x20},{0,0x41,0x41,0x7F,0},{4,2,1,2,4},{0x40,0x40,0x40,0x40,0x40},{0,1,2,4,0},
    {0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},
    {8,0x7E,9,1,2},{0x0C,0x52,0x52,0x52,0x3E},{0x7F,8,4,4,0x78},{0,0x44,0x7D,0x40,0},{0x20,0x40,0x44,0x3D,0},
    {0x7F,0x10,0x28,0x44,0},{0,0x41,0x7F,0x40,0},{0x7C,4,0x18,4,0x78},{0x7C,8,4,4,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,8},{8,0x14,0x14,0x18,0x7C},{0x7C,8,4,4,8},{0x48,0x54,0x54,0x54,0x20},{4,0x3F,0x44,0x40,0x20},
    {0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},{0,8,0x36,0x41,0},{0,0,0x7F,0,0},{0,0x41,0x36,8,0},{0x10,8,8,0x10,8},
};

static void glyph(char ch, int x, int y, int scale, const uint8_t *c) {
    unsigned char u = (unsigned char)ch;
    if (u < 0x20 || u > 0x7E) u = '?';
    const uint8_t *g = F5[u - 0x20];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (g[col] & (1 << row))
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++) px(x + col * scale + dx, y + row * scale + dy, c);
}
static int text_w(const char *s) { int n = (int)strlen(s); return n ? n * 6 - 1 : 0; }
static void text_at(const char *s, int x, int y, const uint8_t *c) { for (; *s; s++, x += 6) if (x > -6 && x < W) glyph(*s, x, y, 1, c); }

/* ------------------------------------------------------------------ state */
enum { M_CLOCK, M_TEXT, M_IMAGE, M_ANIM, M_EFFECT, M_MUSIC, M_INFO, M_COUNTDOWN, M_STOPWATCH, M_PLAYLIST };
static const char *MODES[] = {"clock", "text", "image", "animation", "effect", "music", "screens", "countdown", "stopwatch", "playlist"};
#define NMODES 10
static struct {
    int on, bright, mode;
    char text[256];
    uint8_t image[FRAME];
    char notify[256]; uint8_t ncolor[3]; double nuntil, nstart;
    double stream_until; uint8_t stream[FRAME];
    double text_start;
} st = {1, 30, M_CLOCK, "Hello", {0}, "", {255, 200, 0}, 0, 0, 0, {0}, 0};
static int chime_vol = 60;  // 0-100, chime.sh volume
static char effect_name[32] = "Flow";  // persisted effect (features.c)
static int music_style, info_rotate = 8;   // features.c
static int clock12 = 0, temp_f = 1;          // display prefs: 12-hour clock, Fahrenheit
static char clock_style[32] = "Digital";     // "Digital", "Analog" or a clock-scene label
static char scr_pin[48];                     // HA "Screen" select: "" = Auto (info rotation), else a screen name
static void draw_analog(void);               // features.c
static char cd_sound[48] = "Alarm";      // features.c
static double vol_shown_until;  // show the volume overlay until this time
static char door_sound[64] = "Ding Dong";  // label of the sound used by the Doorbell button
static double pair_until;                  // api.c: pairing code shown on the panel until this time
static void draw_pair(double t);           // api.c

// sound catalogue: label ("Ding Dong") -> file stem ("26-Ding_Dong"), built from the sound dirs
#define MAXSND 160
static struct { char label[48], file[64]; } snd[MAXSND];
static int nsnd;

static void sound_scan(void) {
    static const char *dirs[] = {"/mnt/UDISK/sounds", "/data/custom/sounds", "/usr/res/sound/tips", "/usr/res/sound"};
    nsnd = 0;
    for (int d = 0; d < 4; d++) {
        DIR *dp = opendir(dirs[d]); struct dirent *e;
        if (!dp) continue;
        while ((e = readdir(dp)) && nsnd < MAXSND) {
            const char *n = e->d_name, *dot = strrchr(n, '.');
            if (!dot || (strcasecmp(dot, ".ogg") && strcasecmp(dot, ".wav") && strcasecmp(dot, ".mp3"))) continue;
            char stem[64]; snprintf(stem, sizeof stem, "%.*s", (int)(dot - n), n);
            const char *l = stem; while (*l >= '0' && *l <= '9') l++;
            if (l != stem && *l == '-') l++; else l = stem;   // drop "26-" style prefixes
            char label[48]; snprintf(label, sizeof label, "%s", l);
            for (char *c = label; *c; c++) if (*c == '_') *c = ' ';
            int dup = 0; for (int i = 0; i < nsnd; i++) if (!strcasecmp(snd[i].label, label)) dup = 1;
            if (dup || !label[0]) continue;
            snprintf(snd[nsnd].label, sizeof snd[0].label, "%s", label);
            snprintf(snd[nsnd].file, sizeof snd[0].file, "%s", stem);
            nsnd++;
        }
        closedir(dp);
    }
    for (int i = 1; i < nsnd; i++)  // alphabetical by label
        for (int j = i; j > 0 && strcasecmp(snd[j - 1].label, snd[j].label) > 0; j--) {
            __typeof__(snd[0]) t = snd[j]; snd[j] = snd[j - 1]; snd[j - 1] = t;
        }
}

/* animations: /mnt/UDISK/anims/<name>.anim  ("PXA1", u16 n, n x (u16 delay_ms, 4992 RGB)) */
#define ANIM_DIR "/mnt/UDISK/anims"
#define MAXANIM 256
static char anims[MAXANIM][64]; static int nanims;
typedef struct { int n, total; uint16_t *delay; uint8_t *px; char name[64]; double start; } anim_t;
static anim_t cur_anim, notify_anim;
static char anim_name[64] = "";

/* clock scenes: "<anim>.clock" sidecar = "cx cy Label" -> selectable in Clock style, time drawn at (cx, cy) */
#define MAXCSC 16
static struct { char anim[64], label[32]; int cx, cy; } csc[MAXCSC]; static int ncsc;
static anim_t clock_anim;

static void anim_scan(void) {
    nanims = 0;
    DIR *dp = opendir(ANIM_DIR); struct dirent *e;
    if (!dp) return;
    while ((e = readdir(dp)) && nanims < MAXANIM) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".anim") || dot == e->d_name || dot - e->d_name > 63) continue;
        snprintf(anims[nanims++], 64, "%.*s", (int)(dot - e->d_name), e->d_name);
    }
    closedir(dp);
    ncsc = 0;
    for (int i = 0; i < nanims && ncsc < MAXCSC; i++) {
        char path[160]; snprintf(path, sizeof path, ANIM_DIR "/%s.clock", anims[i]);
        FILE *f = fopen(path, "r"); if (!f) continue;
        char lab[32] = "";
        if (fscanf(f, "%d %d %31[^\n]", &csc[ncsc].cx, &csc[ncsc].cy, lab) >= 2) {
            snprintf(csc[ncsc].anim, 64, "%s", anims[i]);
            snprintf(csc[ncsc].label, 32, "%s", lab[0] ? lab : anims[i]);
            ncsc++;
        }
        fclose(f);
    }
    for (int i = 1; i < nanims; i++)
        for (int j = i; j > 0 && strcasecmp(anims[j - 1], anims[j]) > 0; j--) {
            char t[64]; memcpy(t, anims[j], 64); memcpy(anims[j], anims[j - 1], 64); memcpy(anims[j - 1], t, 64);
        }
}

static void anim_free(anim_t *a) { free(a->delay); free(a->px); memset(a, 0, sizeof *a); }

static int anim_load(const char *name, anim_t *a) {
    const char *real = NULL;
    for (int i = 0; i < nanims; i++) if (!strcasecmp(anims[i], name)) real = anims[i];
    if (!real) { logf_("unknown animation '%s'", name); return -1; }
    if (a->px && !strcmp(a->name, real)) { a->start = now_s(); return 0; }
    char path[160]; snprintf(path, sizeof path, ANIM_DIR "/%s.anim", real);
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    uint8_t h[6]; uint16_t n = 0;
    if (fread(h, 1, 6, f) != 6 || memcmp(h, "PXA1", 4)) { fclose(f); return -1; }
    n = h[4] | (h[5] << 8);
    if (n == 0 || n > 600) { fclose(f); return -1; }
    anim_t t = {0};
    t.n = n; t.delay = calloc(n, 2); t.px = malloc((size_t)n * FRAME);
    if (!t.delay || !t.px) { anim_free(&t); fclose(f); return -1; }
    for (int i = 0; i < n; i++) {
        uint8_t d[2];
        if (fread(d, 1, 2, f) != 2 || fread(t.px + (size_t)i * FRAME, 1, FRAME, f) != FRAME) { anim_free(&t); fclose(f); return -1; }
        t.delay[i] = d[0] | (d[1] << 8); if (t.delay[i] < 20) t.delay[i] = 20;
        t.total += t.delay[i];
    }
    fclose(f);
    snprintf(t.name, sizeof t.name, "%s", real);
    t.start = now_s();
    anim_free(a); *a = t;
    return 0;
}

// HH:MM in the 5x7 font, white with a 1-px black outline, centred on (cx, cy)
static void clock_overlay(int cx, int cy) {
    time_t n = time(NULL); struct tm t; localtime_r(&n, &t);
    if (n < 1700000000) return;
    char s[6];
    if (clock12) snprintf(s, sizeof s, "%d:%02d", (t.tm_hour + 11) % 12 + 1, t.tm_min);
    else snprintf(s, sizeof s, "%02d:%02d", t.tm_hour, t.tm_min);
    char *colon = strchr(s, ':'); if (t.tm_sec % 2 && colon) *colon = ' ';
    int x0 = cx - text_w(s) / 2, y0 = cy - 3;
    static const uint8_t blk[3] = {0, 0, 0}, wht[3] = {255, 255, 255};
    for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) if (ox || oy) text_at(s, x0 + ox, y0 + oy, blk);
    text_at(s, x0, y0, wht);
}

static const uint8_t *anim_frame(const anim_t *a) {
    if (!a->px) return NULL;
    long ms = (long)((now_s() - a->start) * 1000) % a->total;
    for (int i = 0; i < a->n; i++) { if (ms < a->delay[i]) return a->px + (size_t)i * FRAME; ms -= a->delay[i]; }
    return a->px;
}

// accept a label ("Ding Dong") or a file stem ("26-Ding_Dong"); returns the file stem or NULL
static const char *sound_file(const char *name) {
    for (int i = 0; i < nsnd; i++) if (!strcasecmp(snd[i].label, name) || !strcasecmp(snd[i].file, name)) return snd[i].file;
    return NULL;
}

static void state_save(void) {
    FILE *f = fopen(STATE_FILE ".tmp", "w");
    if (!f) return;
    fprintf(f, "on=%d\nbright=%d\nmode=%d\nvol=%d\nsound=%s\nanim=%s\neffect=%s\nmstyle=%d\nrotate=%d\ncdsound=%s\nclock12=%d\ntempf=%d\nclockstyle=%s\nscreen=%s\ntext=%s\n", st.on, st.bright, st.mode, chime_vol, door_sound, anim_name, effect_name, music_style, info_rotate, cd_sound, clock12, temp_f, clock_style, scr_pin, st.text);
    fclose(f); rename(STATE_FILE ".tmp", STATE_FILE);
}
static void state_load(void) {
    FILE *f = fopen(STATE_FILE, "r"); char l[300];
    if (!f) return;
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\n")] = 0;
        if (!strncmp(l, "on=", 3)) st.on = atoi(l + 3);
        else if (!strncmp(l, "bright=", 7)) st.bright = atoi(l + 7);
        else if (!strncmp(l, "sound=", 6)) snprintf(door_sound, sizeof door_sound, "%s", l + 6);
        else if (!strncmp(l, "vol=", 4)) { chime_vol = atoi(l + 4); if (chime_vol < 0 || chime_vol > 100) chime_vol = 60; }
        else if (!strncmp(l, "mode=", 5)) { int m = atoi(l + 5); st.mode = (m >= 0 && m < NMODES && m != M_IMAGE && m != M_COUNTDOWN) ? m : M_CLOCK; }
        else if (!strncmp(l, "effect=", 7)) snprintf(effect_name, sizeof effect_name, "%s", l + 7);
        else if (!strncmp(l, "mstyle=", 7)) { int v = atoi(l + 7); if (v >= 0 && v < 4) music_style = v; }
        else if (!strncmp(l, "rotate=", 7)) { int v = atoi(l + 7); if (v >= 3 && v <= 120) info_rotate = v; }
        else if (!strncmp(l, "cdsound=", 8)) snprintf(cd_sound, sizeof cd_sound, "%s", l + 8);
        else if (!strncmp(l, "clock12=", 8)) clock12 = atoi(l + 8) ? 1 : 0;
        else if (!strncmp(l, "tempf=", 6)) temp_f = atoi(l + 6) ? 1 : 0;
        else if (!strncmp(l, "screen=", 7)) snprintf(scr_pin, sizeof scr_pin, "%s", l + 7);
        else if (!strncmp(l, "clockstyle=", 11)) { if (!strcmp(l + 11, "1")) snprintf(clock_style, sizeof clock_style, "Analog");
                                                     else if (l[11] && strcmp(l + 11, "0")) snprintf(clock_style, sizeof clock_style, "%s", l + 11); }
        else if (!strncmp(l, "anim=", 5)) snprintf(anim_name, sizeof anim_name, "%s", l + 5);
        else if (!strncmp(l, "text=", 5)) snprintf(st.text, sizeof st.text, "%s", l + 5);
    }
    fclose(f);
    if (st.bright < 1 || st.bright > 100) st.bright = 30;
}

/* ------------------------------------------------------------------ rendering */
static const uint8_t C_DIG[3] = {255, 140, 40}, C_COLON[3] = {255, 60, 20}, C_DATE[3] = {80, 160, 255},
                     C_BAR[3] = {40, 200, 120}, C_TEXT[3] = {255, 255, 255};

static void draw_clock(void) {
    static const char *WD[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    time_t n = time(NULL); struct tm t; localtime_r(&n, &t);
    if (n < 1700000000) {  // clock not synced yet (no RTC): show dashes
        for (int i = 0; i < 4; i++) for (int x = 0; x < 10; x++) for (int y = 0; y < 2; y++) px(1 + i * 12 + (i > 1 ? 4 : 0) + x, 9 + y, C_DIG);
        return;
    }
    if (!strcasecmp(clock_style, "Analog")) { draw_analog(); return; }
    for (int i = 0; i < ncsc; i++) if (!strcasecmp(clock_style, csc[i].label)) {
        if (strcmp(clock_anim.name, csc[i].anim) && anim_load(csc[i].anim, &clock_anim)) break;
        const uint8_t *f = anim_frame(&clock_anim); if (!f) break;
        memcpy(fb, f, FRAME); clock_overlay(csc[i].cx, csc[i].cy);
        return;
    }
    int x = 1, y = 2; char d[3];
    if (clock12) snprintf(d, sizeof d, "%2d", (t.tm_hour + 11) % 12 + 1); else snprintf(d, sizeof d, "%02d", t.tm_hour);
    if (d[0] != ' ') glyph(d[0], x, y, 2, C_DIG);
    glyph(d[1], x + 12, y, 2, C_DIG);
    if (clock12 && t.tm_hour >= 12) for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) px(x + dx, y + 12 + dy, C_COLON);  // PM dot
    if (t.tm_sec % 2 == 0) for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) { px(x + 24 + dx, y + 3 + dy, C_COLON); px(x + 24 + dx, y + 9 + dy, C_COLON); }
    snprintf(d, sizeof d, "%02d", t.tm_min); glyph(d[0], x + 28, y, 2, C_DIG); glyph(d[1], x + 40, y, 2, C_DIG);
    char line[16]; snprintf(line, sizeof line, "%s %d", WD[t.tm_wday], t.tm_mday);
    text_at(line, (W - text_w(line)) / 2, 19, C_DATE);
    for (int i = 0; i < (t.tm_sec * W) / 60; i++) px(i, H - 2, C_BAR);
}

// centred if it fits, otherwise scroll right-to-left at ~20 px/s with a gap between repeats
static void draw_scroll(const char *s, int y, const uint8_t *c, double t0) {
    int w = text_w(s);
    if (w <= W) { text_at(s, (W - w) / 2, y, c); return; }
    int period = w + 20, off = (int)fmod((now_s() - t0) * 20, (double)period);  // t0 may be 0 (absolute time)
    text_at(s, W - off, y, c);
    text_at(s, W - off + period, y, c);
}

static void play_sound(const char *name, int vol);
static int mq_pub(const char *topic, const char *s, int retain);
static int json_str(const char *j, const char *key, char *out, size_t on);
#include "features.c"

/* ------------------------------------------------------------------ Wi-Fi setup state (from wifimgr) */
static char wifi_mode[96] = "normal";
static double hold_start;  // >0 while the Mode button is held (setup-mode countdown)
#define HOLD_SETUP 5.0

static void wifi_mode_poll(void) {
    static double next;
    if (now_s() < next) return;
    next = now_s() + 1;
    FILE *f = fopen("/tmp/wifi_mode", "r");
    if (!f) { strcpy(wifi_mode, "normal"); return; }
    if (!fgets(wifi_mode, sizeof wifi_mode, f)) strcpy(wifi_mode, "normal");
    wifi_mode[strcspn(wifi_mode, "\r\n")] = 0;
    fclose(f);
}

static const uint8_t C_SETUP[3] = {255, 200, 0}, C_PW[3] = {80, 255, 120};

static void draw_setup(void) {
    double t = now_s();
    if (!strncmp(wifi_mode, "setup ", 6)) {
        text_at("SETUP", (W - text_w("SETUP")) / 2, 1, C_SETUP);
        draw_scroll("Join Wi-Fi: govee-pixel-setup   then open any web page", 12, C_TEXT, 0);
        const char *pw = wifi_mode + 6;
        text_at(pw, (W - text_w(pw)) / 2, 23, C_PW);
    } else {  // connecting
        text_at("Wi-Fi", (W - text_w("Wi-Fi")) / 2, 4, C_SETUP);
        int dots = phase(t, 2, 4); char s[8] = "...";
        s[dots] = 0;
        text_at("join", 4, 16, C_TEXT); text_at(s, 30, 16, C_TEXT);
    }
}

static void render(void) {
    double t = now_s();
    memset(fb, 0, sizeof fb);
    if (strcmp(wifi_mode, "normal")) { draw_setup(); return; }
    if (t < pair_until) { draw_pair(t); return; }
    if (hold_start > 0 && t - hold_start > 1) {  // Mode button held: progress bar towards setup mode
        text_at("Wi-Fi", (W - text_w("Wi-Fi")) / 2, 4, C_SETUP);
        text_at("setup", (W - text_w("setup")) / 2, 14, C_SETUP);
        int w = (int)((t - hold_start) / HOLD_SETUP * W); if (w > W) w = W;
        for (int i = 0; i < w; i++) for (int y = 26; y < 29; y++) px(i, y, C_PW);
        return;
    }
    if (t < vol_shown_until) {
        char s[16]; snprintf(s, sizeof s, "Vol %d", chime_vol);
        text_at(s, (W - text_w(s)) / 2, 8, C_DATE);
        for (int i = 0; i < chime_vol * W / 100; i++) for (int y = 20; y < 23; y++) px(i, y, C_BAR);
        return;
    }
    if (t < st.stream_until) { memcpy(fb, st.stream, FRAME); return; }
    if (t < st.nuntil && notify_anim.px) {  // notification with an animation: animation + text over a dark band
        memcpy(fb, anim_frame(&notify_anim), FRAME);
        if (st.notify[0]) {
            for (int y = 10; y < 21; y++) for (int x = 0; x < W; x++) { uint8_t *q = &fb[(y * W + x) * 3]; q[0] /= 4; q[1] /= 4; q[2] /= 4; }
            draw_scroll(st.notify, 12, C_TEXT, st.nstart);
        }
        return;
    }
    if (t < st.nuntil) {
        // notification: blinking border for the first 3 s, then steady; text scrolls in the middle
        int blink = (t - st.nstart) < 3 && ((int)((t - st.nstart) * 4) % 2);
        if (!blink) for (int i = 0; i < W; i++) { px(i, 0, st.ncolor); px(i, H - 1, st.ncolor); }
        if (!blink) for (int j = 0; j < H; j++) { px(0, j, st.ncolor); px(W - 1, j, st.ncolor); }
        draw_scroll(st.notify, 12, C_TEXT, st.nstart);
        return;
    }
    if (cd_running || t < cd_done_until) {  // a running countdown finishing shows "TIME!" even in other modes
        if (cd_running && t >= cd_end) { countdown_render(t); return; }
        if (!cd_running && t < cd_done_until) { memset(fb, 0, sizeof fb); countdown_render(t); return; }
    }
    if (!st.on) return;
    int m = st.mode;
    if (m == M_PLAYLIST) {
        if (pl_idx < 0 || t >= pl_next) playlist_advance(t);
        m = !strcasecmp(pl_kind, "text") ? M_TEXT : !strcasecmp(pl_kind, "animation") ? M_ANIM : !strcasecmp(pl_kind, "effect") ? M_EFFECT
          : !strcasecmp(pl_kind, "music") ? M_MUSIC : !strcasecmp(pl_kind, "screens") || !strcasecmp(pl_kind, "info") || !strcasecmp(pl_kind, "screen") ? M_INFO : M_CLOCK;
    }
    scr_pl_pin = (st.mode == M_PLAYLIST && !strcasecmp(pl_kind, "screen") && pl_arg[0]) ? pl_arg : NULL;
    switch (m) {
    case M_CLOCK: draw_clock(); break;
    case M_TEXT: draw_scroll(st.text, 12, C_TEXT, st.text_start); break;
    case M_ANIM: { const uint8_t *f = anim_frame(&cur_anim); if (f) memcpy(fb, f, FRAME); else draw_clock(); } break;
    case M_EFFECT: effect_render(t); break;
    case M_MUSIC: music_render(t); break;
    case M_INFO: info_render(t); break;
    case M_COUNTDOWN: countdown_render(t); break;
    case M_STOPWATCH: stopwatch_render(t); break;
    default: memcpy(fb, st.image, FRAME); break;
    }
}

static int mic_wanted(void) {
    if (!st.on || strcmp(wifi_mode, "normal") || now_s() < st.nuntil) return 0;
    return st.mode == M_MUSIC || (st.mode == M_PLAYLIST && !strcasecmp(pl_kind, "music"));
}

/* ------------------------------------------------------------------ sound (via chime.sh) */
static void play_sound(const char *name, int vol) {
    if (!name[0] || (vol <= 0 && strcmp(name, "stop"))) return;
    if (strcmp(name, "stop")) {
        const char *f = sound_file(name);
        if (!f) { logf_("unknown sound '%s'", name); return; }
        name = f;
    }
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') { logf_("bad sound name"); return; }
    char v[8]; snprintf(v, sizeof v, "%d", vol > 100 ? 100 : vol);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int fd = open("/dev/null", O_RDWR); dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
        execl("/bin/sh", "sh", "/data/custom/apps/chime.sh", name, v, (char *)NULL);
        _exit(127);
    }
    logf_("chime %s at %s%%", name, v);
}

/* ------------------------------------------------------------------ MQTT 3.1.1 (QoS 0) */
static struct { char host[64], user[64], pass[128]; int port; } mc;
static int mq = -1;
static double mq_last_tx, mq_next_try;
static uint8_t rx[16384]; static size_t rxn;

static int load_mqtt_conf(void) {
    FILE *f = fopen(MQTT_CONF, "r"); char l[256];
    if (!f) return -1;
    mc.port = 1883;
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n")] = 0;
        if (!strncmp(l, "host=", 5)) snprintf(mc.host, sizeof mc.host, "%s", l + 5);
        else if (!strncmp(l, "port=", 5)) mc.port = atoi(l + 5);
        else if (!strncmp(l, "user=", 5)) snprintf(mc.user, sizeof mc.user, "%s", l + 5);
        else if (!strncmp(l, "pass=", 5)) snprintf(mc.pass, sizeof mc.pass, "%s", l + 5);
    }
    fclose(f);
    return mc.host[0] ? 0 : -1;
}

static int send_all(const uint8_t *b, size_t n) {
    while (n) { ssize_t w = send(mq, b, n, MSG_NOSIGNAL); if (w <= 0) return -1; b += w; n -= w; }
    mq_last_tx = now_s(); return 0;
}
static size_t put_len(uint8_t *b, size_t len) { size_t i = 0; do { uint8_t d = len % 128; len /= 128; if (len) d |= 128; b[i++] = d; } while (len); return i; }
static size_t put_str(uint8_t *b, const char *s, size_t n) { b[0] = n >> 8; b[1] = n & 255; memcpy(b + 2, s, n); return n + 2; }

static void mq_close(void) { if (mq >= 0) close(mq); mq = -1; rxn = 0; mq_next_try = now_s() + 10; }
static int send_all(const uint8_t *b, size_t n);
// Clean MQTT DISCONNECT that never fires our will ("offline"). Closing a socket that still has unread data makes
// Linux send a reset, the broker may then treat it as a dropped connection and publish the will. So: drain what
// the broker sent, send DISCONNECT, half-close, and wait (up to 1 s) for the broker to close its side first.
static void mq_disconnect(void) {
    if (mq < 0) return;
    uint8_t junk[512], dc[2] = {0xE0, 0};
    while (recv(mq, junk, sizeof junk, MSG_DONTWAIT) > 0) {}
    send_all(dc, 2);
    shutdown(mq, SHUT_WR);
    for (int i = 0; i < 10; i++) {
        struct pollfd p = {mq, POLLIN, 0};
        if (poll(&p, 1, 100) <= 0) continue;
        if (recv(mq, junk, sizeof junk, MSG_DONTWAIT) <= 0) break;   // 0 = broker closed: done
    }
    close(mq); mq = -1; rxn = 0;
}

static int api_capturing;  // api.c: collecting publishes for GET /api/discovery instead of sending them
static void api_outbox(const char *topic, const void *payload, size_t n, int retain);
static int mq_publish_raw(const char *topic, const void *payload, size_t plen, int retain) {
    api_outbox(topic, payload, plen, retain);  // pixel2mqtt bridge (no-op while nothing is paired)
    if (api_capturing) return 0;
    if (mq < 0) return -1;
    size_t tl = strlen(topic), rl = 2 + tl + plen;
    uint8_t *b = malloc(rl + 5); if (!b) return -1;
    size_t i = 0; b[i++] = 0x30 | (retain ? 1 : 0); i += put_len(b + i, rl); i += put_str(b + i, topic, tl);
    memcpy(b + i, payload, plen); i += plen;
    int r = send_all(b, i); free(b);
    if (r < 0) { logf_("mqtt send failed"); mq_close(); }
    return r;
}
static int mq_pub(const char *topic, const char *s, int retain) { return mq_publish_raw(topic, s, strlen(s), retain); }

static void publish_state(void) {
    char b[16];
    mq_pub(BASE "/power/state", st.on ? "ON" : "OFF", 1);
    snprintf(b, sizeof b, "%d", st.bright); mq_pub(BASE "/brightness/state", b, 1);
    mq_pub(BASE "/mode/state", MODES[st.mode], 1);
    mq_pub(BASE "/text/state", st.text, 1);
    snprintf(b, sizeof b, "%d", chime_vol); mq_pub(BASE "/volume/state", b, 1);
    mq_pub(BASE "/sound/state", door_sound, 1);
    mq_pub(BASE "/animation/state", anim_name, 1);
    mq_pub(BASE "/effect/state", effect_name, 1);
    mq_pub(BASE "/music_style/state", MUSIC_STYLES[music_style], 1);
    mq_pub(BASE "/playlist/state", playlist, 1);
    snprintf(b, sizeof b, "%d", info_rotate); mq_pub(BASE "/info_rotate/state", b, 1);
    mq_pub(BASE "/countdown_sound/state", cd_sound, 1);
    mq_pub(BASE "/clock12/state", clock12 ? "ON" : "OFF", 1);
    mq_pub(BASE "/fahrenheit/state", temp_f ? "ON" : "OFF", 1);
    mq_pub(BASE "/clock_style/state", clock_style, 1);
    mq_pub(BASE "/screen/state", scr_pin[0] ? scr_pin : "Auto", 1);
}

static void publish_discovery(void) {
    const char *dev = "\"dev\":{\"ids\":[\"govee_pixel_h6631\"],\"name\":\"Govee Pixel\",\"mf\":\"Govee (custom firmware)\",\"mdl\":\"H6631\"},"
                      "\"avty_t\":\"" BASE "/status\"";
    char b[1024];
    snprintf(b, sizeof b, "{\"name\":null,\"uniq_id\":\"govee_pixel_light\",\"cmd_t\":\"" BASE "/power/set\",\"stat_t\":\"" BASE "/power/state\","
             "\"bri_cmd_t\":\"" BASE "/brightness/set\",\"bri_stat_t\":\"" BASE "/brightness/state\",\"bri_scl\":100,\"icon\":\"mdi:dots-grid\",%s}", dev);
    mq_pub(DISC "/light/govee_pixel/config", b, 1);
    snprintf(b, sizeof b, "{\"name\":\"Mode\",\"uniq_id\":\"govee_pixel_mode\",\"cmd_t\":\"" BASE "/mode/set\",\"stat_t\":\"" BASE "/mode/state\","
             "\"ops\":[\"clock\",\"text\",\"image\",\"animation\",\"effect\",\"music\",\"info\",\"countdown\",\"stopwatch\",\"playlist\"],\"icon\":\"mdi:view-dashboard\",%s}", dev);
    mq_pub(DISC "/select/govee_pixel_mode/config", b, 1);
    snprintf(b, sizeof b, "{\"name\":\"Text\",\"uniq_id\":\"govee_pixel_text\",\"cmd_t\":\"" BASE "/text/set\",\"stat_t\":\"" BASE "/text/state\","
             "\"max\":255,\"icon\":\"mdi:message-text\",%s}", dev);
    mq_pub(DISC "/text/govee_pixel_text/config", b, 1);
    snprintf(b, sizeof b, "{\"name\":\"Chime volume\",\"uniq_id\":\"govee_pixel_volume\",\"cmd_t\":\"" BASE "/volume/set\",\"stat_t\":\"" BASE "/volume/state\","
             "\"min\":0,\"max\":100,\"step\":5,\"unit_of_meas\":\"%%\",\"icon\":\"mdi:volume-high\",%s}", dev);
    mq_pub(DISC "/number/govee_pixel_volume/config", b, 1);
    snprintf(b, sizeof b, "{\"name\":\"Doorbell\",\"uniq_id\":\"govee_pixel_doorbell\",\"cmd_t\":\"" BASE "/chime/set\",\"pl_prs\":\"doorbell\","
             "\"icon\":\"mdi:bell-ring\",%s}", dev);
    mq_pub(DISC "/button/govee_pixel_doorbell/config", b, 1);
    snprintf(b, sizeof b, "{\"name\":\"Stop sound\",\"uniq_id\":\"govee_pixel_stop_sound\",\"cmd_t\":\"" BASE "/chime/set\",\"pl_prs\":\"stop\","
             "\"icon\":\"mdi:volume-off\",%s}", dev);
    mq_pub(DISC "/button/govee_pixel_stop_sound/config", b, 1);
    {
        static char big[16384]; size_t o = 0;
        o += snprintf(big + o, sizeof big - o, "{\"name\":\"Doorbell sound\",\"uniq_id\":\"govee_pixel_sound\",\"cmd_t\":\"" BASE "/sound/set\","
                      "\"stat_t\":\"" BASE "/sound/state\",\"icon\":\"mdi:music-note\",\"ops\":[");
        for (int i = 0; i < nsnd && o < sizeof big - 400; i++) o += snprintf(big + o, sizeof big - o, "%s\"%s\"", i ? "," : "", snd[i].label);
        snprintf(big + o, sizeof big - o, "],%s}", dev);
        mq_pub(DISC "/select/govee_pixel_sound/config", big, 1);
        o = 0;
        o += snprintf(big + o, sizeof big - o, "{\"name\":\"Animation\",\"uniq_id\":\"govee_pixel_animation\",\"cmd_t\":\"" BASE "/animation/set\","
                      "\"stat_t\":\"" BASE "/animation/state\",\"icon\":\"mdi:animation-play\",\"ops\":[");
        for (int i = 0; i < nanims && o < sizeof big - 400; i++) o += snprintf(big + o, sizeof big - o, "%s\"%s\"", i ? "," : "", anims[i]);
        snprintf(big + o, sizeof big - o, "],%s}", dev);
        mq_pub(DISC "/select/govee_pixel_animation/config", big, 1);
    }
    {
        static char big2[2048]; size_t o = 0;
        o += snprintf(big2 + o, sizeof big2 - o, "{\"name\":\"Effect\",\"uniq_id\":\"govee_pixel_effect\",\"cmd_t\":\"" BASE "/effect/set\","
                      "\"stat_t\":\"" BASE "/effect/state\",\"icon\":\"mdi:auto-fix\",\"ops\":[");
        for (int i = 0; i < NEFFECTS; i++) o += snprintf(big2 + o, sizeof big2 - o, "%s\"%s\"", i ? "," : "", EFFECTS[i]);
        snprintf(big2 + o, sizeof big2 - o, "],%s}", dev);
        mq_pub(DISC "/select/govee_pixel_effect/config", big2, 1);
        o = 0;
        o += snprintf(big2 + o, sizeof big2 - o, "{\"name\":\"Music style\",\"uniq_id\":\"govee_pixel_music_style\",\"cmd_t\":\"" BASE "/music_style/set\","
                      "\"stat_t\":\"" BASE "/music_style/state\",\"icon\":\"mdi:music\",\"ops\":[");
        for (int i = 0; i < NMSTYLES; i++) o += snprintf(big2 + o, sizeof big2 - o, "%s\"%s\"", i ? "," : "", MUSIC_STYLES[i]);
        snprintf(big2 + o, sizeof big2 - o, "],%s}", dev);
        mq_pub(DISC "/select/govee_pixel_music_style/config", big2, 1);
    }
    snprintf(b, sizeof b, "{\"name\":\"Playlist\",\"uniq_id\":\"govee_pixel_playlist\",\"cmd_t\":\"" BASE "/playlist/set\",\"stat_t\":\"" BASE "/playlist/state\","
             "\"max\":255,\"icon\":\"mdi:playlist-play\",%s}", dev);
    mq_pub(DISC "/text/govee_pixel_playlist/config", b, 1);
    snprintf(b, sizeof b, "{\"name\":\"Screen rotate seconds\",\"uniq_id\":\"govee_pixel_info_rotate\",\"cmd_t\":\"" BASE "/info_rotate/set\",\"stat_t\":\"" BASE "/info_rotate/state\","
             "\"min\":3,\"max\":120,\"step\":1,\"unit_of_meas\":\"s\",\"icon\":\"mdi:timer-sync\",\"ent_cat\":\"config\",%s}", dev);
    mq_pub(DISC "/number/govee_pixel_info_rotate/config", b, 1);
    snprintf(b, sizeof b, "{\"name\":\"12-hour clock\",\"uniq_id\":\"govee_pixel_clock12\",\"cmd_t\":\"" BASE "/clock12/set\",\"stat_t\":\"" BASE "/clock12/state\","
             "\"icon\":\"mdi:clock-time-four-outline\",%s}", dev);
    mq_pub(DISC "/switch/govee_pixel_clock12/config", b, 1);
    snprintf(b, sizeof b, "{\"name\":\"Fahrenheit\",\"uniq_id\":\"govee_pixel_fahrenheit\",\"cmd_t\":\"" BASE "/fahrenheit/set\",\"stat_t\":\"" BASE "/fahrenheit/state\","
             "\"icon\":\"mdi:temperature-fahrenheit\",%s}", dev);
    mq_pub(DISC "/switch/govee_pixel_fahrenheit/config", b, 1);
    {
        char ops[700] = "\"Digital\",\"Analog\""; size_t o = strlen(ops);
        for (int i = 0; i < ncsc && o < sizeof ops - 40; i++) o += snprintf(ops + o, sizeof ops - o, ",\"%s\"", csc[i].label);
        snprintf(b, sizeof b, "{\"name\":\"Clock style\",\"uniq_id\":\"govee_pixel_clock_style\",\"cmd_t\":\"" BASE "/clock_style/set\",\"stat_t\":\"" BASE "/clock_style/state\","
                 "\"ops\":[%s],\"icon\":\"mdi:clock-outline\",%s}", ops, dev);
    }
    mq_pub(DISC "/select/govee_pixel_clock_style/config", b, 1);
    {
        static char sb[4096]; size_t o = 0;
        o += snprintf(sb + o, sizeof sb - o, "{\"name\":\"Screen\",\"uniq_id\":\"govee_pixel_screen\",\"cmd_t\":\"" BASE "/screen/set\","
                      "\"stat_t\":\"" BASE "/screen/state\",\"icon\":\"mdi:monitor-dashboard\",\"ops\":[\"Auto\"");
        for (int i = 0; i < nscr && o < sizeof sb - 200; i++) {
            if (strchr(scrs[i]->name, '"') || strchr(scrs[i]->name, '\\')) continue;
            o += snprintf(sb + o, sizeof sb - o, ",\"%s\"", scrs[i]->name);
        }
        snprintf(sb + o, sizeof sb - o, "],%s}", dev);
        mq_pub(DISC "/select/govee_pixel_screen/config", sb, 1);
    }
    snprintf(b, sizeof b, "{\"name\":\"Start countdown\",\"uniq_id\":\"govee_pixel_countdown\",\"cmd_t\":\"" BASE "/countdown_min/set\","
             "\"min\":1,\"max\":180,\"step\":1,\"mode\":\"box\",\"unit_of_meas\":\"min\",\"icon\":\"mdi:timer-sand\",%s}", dev);
    mq_pub(DISC "/number/govee_pixel_countdown/config", b, 1);
    snprintf(b, sizeof b, "{\"name\":\"Countdown stop\",\"uniq_id\":\"govee_pixel_countdown_stop\",\"cmd_t\":\"" BASE "/countdown/set\",\"pl_prs\":\"stop\","
             "\"icon\":\"mdi:timer-off\",%s}", dev);
    mq_pub(DISC "/button/govee_pixel_countdown_stop/config", b, 1);
    snprintf(b, sizeof b, "{\"name\":\"Stopwatch start/stop\",\"uniq_id\":\"govee_pixel_sw_toggle\",\"cmd_t\":\"" BASE "/stopwatch/set\",\"pl_prs\":\"toggle\","
             "\"icon\":\"mdi:timer-play\",%s}", dev);
    mq_pub(DISC "/button/govee_pixel_sw_toggle/config", b, 1);
    snprintf(b, sizeof b, "{\"name\":\"Stopwatch reset\",\"uniq_id\":\"govee_pixel_sw_reset\",\"cmd_t\":\"" BASE "/stopwatch/set\",\"pl_prs\":\"reset\","
             "\"icon\":\"mdi:timer-refresh\",%s}", dev);
    mq_pub(DISC "/button/govee_pixel_sw_reset/config", b, 1);
    snprintf(b, sizeof b, "{\"name\":\"Sound level\",\"uniq_id\":\"govee_pixel_sound_level\",\"stat_t\":\"" BASE "/sound_level/state\","
             "\"unit_of_meas\":\"%%\",\"stat_cla\":\"measurement\",\"icon\":\"mdi:waveform\",%s}", dev);
    mq_pub(DISC "/sensor/govee_pixel_sound_level/config", b, 1);
    {
        static char big3[16384]; size_t o = 0;
        o += snprintf(big3 + o, sizeof big3 - o, "{\"name\":\"Countdown sound\",\"uniq_id\":\"govee_pixel_cd_sound\",\"cmd_t\":\"" BASE "/countdown_sound/set\","
                      "\"stat_t\":\"" BASE "/countdown_sound/state\",\"icon\":\"mdi:alarm-note\",\"ent_cat\":\"config\",\"ops\":[");
        for (int i = 0; i < nsnd && o < sizeof big3 - 400; i++) o += snprintf(big3 + o, sizeof big3 - o, "%s\"%s\"", i ? "," : "", snd[i].label);
        snprintf(big3 + o, sizeof big3 - o, "],%s}", dev);
        mq_pub(DISC "/select/govee_pixel_cd_sound/config", big3, 1);
    }
    snprintf(b, sizeof b, "{\"name\":\"Notify\",\"uniq_id\":\"govee_pixel_notify\",\"cmd_t\":\"" BASE "/notify/set\",%s}", dev);
    mq_pub(DISC "/notify/govee_pixel/config", b, 1);
    // physical buttons as device triggers (short / long press)
    static const char *BTN[] = {"power", "mode", "color", "volume"};
    for (int i = 0; i < 4; i++)
        for (int l = 0; l < 2; l++) {
            char topic[128];
            snprintf(b, sizeof b, "{\"atype\":\"trigger\",\"t\":\"" BASE "/button/%s\",\"type\":\"button_%s_press\",\"stype\":\"%s\",\"pl\":\"%s\",%s}",
                     BTN[i], l ? "long" : "short", BTN[i], l ? "long" : "short",
                     "\"dev\":{\"ids\":[\"govee_pixel_h6631\"]}");  // triggers don't accept availability keys
            snprintf(topic, sizeof topic, DISC "/device_automation/govee_pixel_%s_%s/config", BTN[i], l ? "long" : "short");
            mq_pub(topic, b, 1);
        }
}

static int mq_connect(void) {
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(mc.port);
    if (inet_pton(AF_INET, mc.host, &a.sin_addr) != 1) { logf_("mqtt host must be an IP: %s", mc.host); return -1; }
    mq = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = {5, 0};
    setsockopt(mq, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(mq, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(mq, (struct sockaddr *)&a, sizeof a) < 0) { logf_("mqtt connect %s:%d: %s", mc.host, mc.port, strerror(errno)); mq_close(); return -1; }
    int one = 1; setsockopt(mq, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    uint8_t b[512]; size_t i = 0, v = 0; uint8_t vb[480];
    const char *cid = "govee-pixel", *wt = BASE "/status", *wm = "offline";
    v += put_str(vb + v, "MQTT", 4); vb[v++] = 4;
    vb[v++] = 0x02 | 0x04 | 0x20 | (mc.user[0] ? 0x80 : 0) | (mc.pass[0] ? 0x40 : 0);  // clean, will, will-retain, user, pass
    vb[v++] = 0; vb[v++] = 30;  // keepalive 30 s
    v += put_str(vb + v, cid, strlen(cid)); v += put_str(vb + v, wt, strlen(wt)); v += put_str(vb + v, wm, strlen(wm));
    if (mc.user[0]) v += put_str(vb + v, mc.user, strlen(mc.user));
    if (mc.pass[0]) v += put_str(vb + v, mc.pass, strlen(mc.pass));
    b[i++] = 0x10; i += put_len(b + i, v); memcpy(b + i, vb, v); i += v;
    if (send_all(b, i) < 0) { mq_close(); return -1; }
    uint8_t ack[4];
    if (recv(mq, ack, 4, MSG_WAITALL) != 4 || ack[0] != 0x20) { logf_("mqtt: no CONNACK"); mq_close(); return -1; }
    if (ack[3] != 0) { logf_("mqtt: connection refused, code %d (4/5 = bad username/password or not authorised)", ack[3]); mq_close(); mq_next_try = now_s() + 60; return -1; }
    // subscribe
    const char *subs[] = {BASE "/+/set", DISC "/status"};
    i = 0; v = 0; vb[v++] = 0; vb[v++] = 1;
    for (int k = 0; k < 2; k++) { v += put_str(vb + v, subs[k], strlen(subs[k])); vb[v++] = 0; }
    b[i++] = 0x82; i += put_len(b + i, v); memcpy(b + i, vb, v); i += v;
    if (send_all(b, i) < 0) { mq_close(); return -1; }
    fcntl(mq, F_SETFL, fcntl(mq, F_GETFL) | O_NONBLOCK);
    logf_("mqtt connected to %s:%d", mc.host, mc.port);
    mq_pub(BASE "/status", "online", 1);
    publish_discovery();
    publish_state();
    return 0;
}

static int parse_hex_color(const char *s, uint8_t *c) {
    if (*s == '#') s++;
    unsigned v; if (strlen(s) < 6 || sscanf(s, "%6x", &v) != 1) return -1;
    c[0] = v >> 16; c[1] = (v >> 8) & 255; c[2] = v & 255; return 0;
}

// minimal JSON string/number field extraction (flat objects only)
static int json_str(const char *j, const char *key, char *out, size_t on) {
    char k[64]; snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(j, k); if (!p) return -1;
    p = strchr(p + strlen(k), ':'); if (!p) return -1;
    while (*++p == ' ');
    if (*p == '"') {
        size_t i = 0; p++;
        while (*p && *p != '"' && i + 1 < on) { if (*p == '\\' && p[1]) p++; out[i++] = *p++; }
        out[i] = 0; return 0;
    }
    size_t i = 0; while (*p && *p != ',' && *p != '}' && i + 1 < on) out[i++] = *p++;
    out[i] = 0; return 0;
}

static void on_message(const char *topic, const uint8_t *pl, size_t n) {
    if (!strcmp(topic, BASE "/values/set")) { values_message(pl, n); return; }  // up to VALUES_MAX, RAM only
    char s[1024]; size_t m = n < sizeof s - 1 ? n : sizeof s - 1; memcpy(s, pl, m); s[m] = 0;
    if (!strcmp(topic, DISC "/status")) { if (!strcmp(s, "online")) { sound_scan(); anim_scan(); scr_scan(); publish_discovery(); publish_state(); } return; }
    if (!strcmp(topic, BASE "/power/set")) { st.on = !strcasecmp(s, "ON"); }
    else if (!strcmp(topic, BASE "/chime/set")) {  // "<sound>" or "<sound> <volume>"
        // "<sound>" | "<sound> <volume>" | "doorbell" | "stop"; sound = label or file stem
        char name[96]; int v = chime_vol;
        snprintf(name, sizeof name, "%s", s);
        char *sp = strrchr(name, ' ');
        if (sp && sp[1] && strspn(sp + 1, "0123456789") == strlen(sp + 1)) { v = atoi(sp + 1); *sp = 0; }
        play_sound(!strcmp(name, "doorbell") ? door_sound : name, v); return;
    }
    else if (!strcmp(topic, BASE "/sound/set")) {  // choose the doorbell sound (and preview it)
        if (!sound_file(s)) { logf_("unknown sound '%s'", s); return; }
        snprintf(door_sound, sizeof door_sound, "%s", s);
        play_sound(door_sound, chime_vol);
    }
    else if (!strcmp(topic, BASE "/volume/set")) { int v = atoi(s); if (v >= 0 && v <= 100) chime_vol = v; }
    else if (!strcmp(topic, BASE "/brightness/set")) { int b = atoi(s); if (b >= 1 && b <= 100) { st.bright = b; d_lumi((uint8_t)b); } st.on = 1; }
    else if (!strcmp(topic, BASE "/mode/set")) {
        for (int i = 0; i < NMODES; i++) if (!strcmp(s, MODES[i])) st.mode = i;
        if (!strcmp(s, "info")) st.mode = M_INFO;   // the mode's old name (playlists and automations)
        if (st.mode == M_ANIM && !cur_anim.px && (!anim_name[0] || anim_load(anim_name, &cur_anim))) st.mode = M_CLOCK;
        st.text_start = now_s();
    }
    else if (!strcmp(topic, BASE "/animation/set")) {
        if (anim_load(s, &cur_anim)) return;
        snprintf(anim_name, sizeof anim_name, "%s", cur_anim.name); st.mode = M_ANIM; st.on = 1;
    }
    else if (!strcmp(topic, BASE "/effect/set")) {
        int e = effect_find(s); if (e < 0) { logf_("unknown effect '%s'", s); return; }
        effect_start(e); st.mode = M_EFFECT; st.on = 1;
    }
    else if (!strcmp(topic, BASE "/music_style/set")) {
        for (int i = 0; i < NMSTYLES; i++) if (!strcasecmp(s, MUSIC_STYLES[i])) music_style = i;
        memset(efb, 0, sizeof efb); st.mode = M_MUSIC; st.on = 1;
    }
    else if (!strcmp(topic, BASE "/playlist/set")) {
        if (!s[0]) return;
        snprintf(playlist, sizeof playlist, "%s", s); playlist_save(); pl_idx = -1; st.mode = M_PLAYLIST; st.on = 1;
    }
    else if (!strcmp(topic, BASE "/countdown_min/set")) { double v = atof(s); if (v > 0) { countdown_start(v * 60); st.mode = M_COUNTDOWN; st.on = 1; } }
    else if (!strcmp(topic, BASE "/countdown/set")) {  // seconds, "m:ss", "h:mm:ss" or "stop"
        if (!strcasecmp(s, "stop")) { cd_running = 0; cd_done_until = 0; if (st.mode == M_COUNTDOWN) st.mode = M_CLOCK; }
        else {
            int a = 0, b2 = 0, c = 0, n2 = sscanf(s, "%d:%d:%d", &a, &b2, &c);
            double secs = n2 == 3 ? a * 3600 + b2 * 60 + c : n2 == 2 ? a * 60 + b2 : atof(s);
            if (secs <= 0) return;
            countdown_start(secs); st.mode = M_COUNTDOWN; st.on = 1;
        }
    }
    else if (!strcmp(topic, BASE "/countdown_sound/set")) { if (!sound_file(s)) return; snprintf(cd_sound, sizeof cd_sound, "%s", s); }
    else if (!strcmp(topic, BASE "/stopwatch/set")) {  // start | stop | toggle | reset
        double t = now_s();
        if (!strcasecmp(s, "reset")) { sw_acc = 0; sw_start = t; }
        else if (!strcasecmp(s, "start") || (!strcasecmp(s, "toggle") && !sw_running)) { if (!sw_running) { sw_running = 1; sw_start = t; } }
        else if (!strcasecmp(s, "stop") || !strcasecmp(s, "toggle")) { if (sw_running) { sw_acc += t - sw_start; sw_running = 0; } }
        st.mode = M_STOPWATCH; st.on = 1;
    }
    else if (!strcmp(topic, BASE "/clock12/set")) clock12 = !strcasecmp(s, "ON") || !strcmp(s, "1");
    else if (!strcmp(topic, BASE "/fahrenheit/set")) temp_f = !strcasecmp(s, "ON") || !strcmp(s, "1");
    else if (!strcmp(topic, BASE "/screen/set")) {  // "Auto" or a screen name: pins it in info mode
        if (!strcasecmp(s, "Auto") || !s[0]) scr_pin[0] = 0;
        else { scr_t *sc = scr_find(s); if (!sc) { logf_("unknown screen '%s'", s); return; } snprintf(scr_pin, sizeof scr_pin, "%s", sc->name); }
        st.mode = M_INFO; st.on = 1; info_cur = -1;
    }
    else if (!strcmp(topic, BASE "/clock_style/set")) {
        int ok = !strcasecmp(s, "Digital") || !strcasecmp(s, "Analog");
        for (int i = 0; i < ncsc; i++) if (!strcasecmp(s, csc[i].label)) ok = 1;
        if (!ok) { logf_("unknown clock style '%s'", s); return; }
        snprintf(clock_style, sizeof clock_style, "%s", s); st.mode = M_CLOCK; st.on = 1;
    }
    else if (info_message(topic, s)) { if (strcmp(topic, BASE "/info_rotate/set")) return; }
    else if (!strcmp(topic, BASE "/text/set")) { snprintf(st.text, sizeof st.text, "%s", s); st.mode = M_TEXT; st.text_start = now_s(); }
    else if (!strcmp(topic, BASE "/image/set")) {
        if (n != FRAME) { logf_("image/set: expected %d bytes, got %zu", FRAME, n); return; }
        memcpy(st.image, pl, FRAME); st.mode = M_IMAGE;
    } else if (!strcmp(topic, BASE "/notify/set")) {
        // plain text, or JSON {"message"|"text": "...", "duration": 15, "color": "#ff8800"}
        double dur = 15; uint8_t col[3] = {255, 200, 0}; char v[256];
        if (s[0] == '{') {
            if (json_str(s, "message", st.notify, sizeof st.notify) && json_str(s, "text", st.notify, sizeof st.notify)) st.notify[0] = 0;
            if (!json_str(s, "duration", v, sizeof v) && atof(v) > 0) dur = atof(v);
            if (!json_str(s, "color", v, sizeof v)) parse_hex_color(v, col);
            if (!json_str(s, "sound", v, sizeof v)) play_sound(v, chime_vol);
            if (!json_str(s, "animation", v, sizeof v) && !anim_load(v, &notify_anim)) {} else anim_free(&notify_anim);
        } else { snprintf(st.notify, sizeof st.notify, "%s", s); anim_free(&notify_anim); }
        memcpy(st.ncolor, col, 3); st.nstart = now_s(); st.nuntil = st.nstart + dur;
        logf_("notify %.0fs: %s", dur, st.notify);
        return;  // notifications are transient: no state change
    } else return;
    state_save();
    publish_state();
}

static void mq_poll_in(void) {
    for (;;) {
        ssize_t r = recv(mq, rx + rxn, sizeof rx - rxn, 0);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { logf_("mqtt connection lost"); mq_close(); return; }
        if (r < 0) break;
        rxn += r;
        if (rxn == sizeof rx) { logf_("mqtt: oversized packet"); mq_close(); return; }
    }
    for (;;) {  // parse complete packets
        if (rxn < 2) return;
        size_t len = 0, mul = 1, i = 1;
        for (;;) { if (i >= rxn) return; uint8_t d = rx[i++]; len += (d & 127) * mul; mul *= 128; if (!(d & 128)) break; if (i > 4) { mq_close(); return; } }
        if (rxn < i + len) return;
        uint8_t type = rx[0] >> 4;
        if (type == 3 && len >= 2) {
            const uint8_t *p = rx + i; size_t tl = (p[0] << 8) | p[1];
            if (2 + tl <= len) {
                char topic[128]; size_t cl = tl < sizeof topic - 1 ? tl : sizeof topic - 1;
                memcpy(topic, p + 2, cl); topic[cl] = 0;
                size_t hdr = 2 + tl + (((rx[0] >> 1) & 3) ? 2 : 0);
                if (hdr <= len) on_message(topic, p + hdr, len - hdr);
            }
        }
        memmove(rx, rx + i + len, rxn - i - len); rxn -= i + len;
    }
}

/* ------------------------------------------------------------------ buttons (gpio-keys, /dev/input/event0) */
// codes measured on this unit; the keys report 0 when pressed and 1 when released
enum { K_POWER = 101, K_COLOR = 102, K_MODE = 104, K_VOLUME = 108 };
static double kdown[4];
static int kidx(int code) { return code == K_POWER ? 0 : code == K_MODE ? 1 : code == K_COLOR ? 2 : code == K_VOLUME ? 3 : -1; }
static const char *KNAME[] = {"power", "mode", "color", "volume"};

static void request_setup(void) {
    FILE *f = fopen("/tmp/wifi_setup_request", "w"); if (f) fclose(f);
    logf_("Wi-Fi setup requested from the Mode button");
}

static void on_button(int k, double held) {
    int lng = held >= 1.0;
    char topic[64]; snprintf(topic, sizeof topic, BASE "/button/%s", KNAME[k]);
    mq_pub(topic, lng ? "long" : "short", 0);
    if (strcmp(wifi_mode, "normal")) return;
    if (k == 0 && !lng) { st.on = !st.on; state_save(); publish_state(); }
    else if (k == 1 && !lng) {
        static const int cyc[] = {M_CLOCK, M_TEXT, M_ANIM, M_EFFECT, M_INFO, M_MUSIC};
        int ci = 0; for (int i = 0; i < 6; i++) if (cyc[i] == st.mode) ci = i;
        st.mode = cyc[(ci + 1) % 6];
        if (st.mode == M_ANIM && !cur_anim.px) st.mode = M_EFFECT;
        if (st.mode == M_EFFECT) effect_start(effect_idx);
        st.on = 1; st.text_start = now_s(); info_cur = -1; state_save(); publish_state();
    }
    else if (k == 2 && !lng && (st.mode == M_EFFECT || st.mode == M_MUSIC || st.mode == M_ANIM)) {  // Color: next effect / style / animation
        if (st.mode == M_EFFECT) effect_start((effect_idx + 1) % NEFFECTS);
        else if (st.mode == M_MUSIC) { music_style = (music_style + 1) % NMSTYLES; memset(efb, 0, sizeof efb); }
        else if (nanims) { int i = 0; for (; i < nanims; i++) if (!strcmp(anims[i], anim_name)) break; i = (i + 1) % nanims; if (!anim_load(anims[i], &cur_anim)) snprintf(anim_name, sizeof anim_name, "%s", anims[i]); }
        state_save(); publish_state();
    }
    else if (k == 1 && held >= HOLD_SETUP) request_setup();
    else if (k == 3 && !lng) {  // Volume: cycle 0 -> 30 -> 60 -> 100, preview with a click
        chime_vol = chime_vol >= 100 ? 0 : chime_vol >= 60 ? 100 : chime_vol >= 30 ? 60 : 30;
        vol_shown_until = now_s() + 2; state_save(); publish_state();
        if (chime_vol) play_sound("5-click", chime_vol);
    }
}

static void keys_read(int fd) {
    struct input_event ev;
    while (read(fd, &ev, sizeof ev) == sizeof ev) {
        if (ev.type != EV_KEY) continue;
        int k = kidx(ev.code); if (k < 0) continue;
        double t = now_s();
        if (ev.value == 0) { kdown[k] = t; if (k == 1) hold_start = t; }
        else if (ev.value == 1 && kdown[k] > 0) {
            double held = t - kdown[k]; kdown[k] = 0; if (k == 1) hold_start = 0;
            if (!(k == 1 && held >= HOLD_SETUP)) on_button(k, held);  // setup already fired while held
        }
    }
}

static void keys_tick(void) {  // fire setup as soon as the hold time is reached (feedback without release)
    if (hold_start > 0 && now_s() - hold_start >= HOLD_SETUP) {
        hold_start = 0; kdown[1] = 0;
        on_button(1, HOLD_SETUP);
    }
}

/* ------------------------------------------------------------------ HTTP (port 80): setup page / status */
static int hs = -1;

static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) { char h[3] = {s[1], s[2], 0}; *o++ = (char)strtol(h, NULL, 16); s += 2; }
        else *o++ = *s;
    }
    *o = 0;
}
static int form_get(const char *body, const char *key, char *out, size_t on) {
    size_t kl = strlen(key); const char *p = body;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1; size_t i = 0;
            while (*p && *p != '&' && i + 1 < on) out[i++] = *p++;
            out[i] = 0; url_decode(out); return 0;
        }
        p = strchr(p, '&'); if (p) p++;
    }
    return -1;
}
static void html_esc(const char *s, char *o, size_t on) {
    size_t i = 0;
    for (; *s && i + 7 < on; s++) {
        const char *r = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : NULL;
        if (r) { size_t l = strlen(r); memcpy(o + i, r, l); i += l; } else o[i++] = *s;
    }
    o[i] = 0;
}

static const char *PAGE_HEAD = "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Govee Pixel</title><style>body{font-family:system-ui,sans-serif;max-width:420px;margin:24px auto;padding:0 16px;background:#111;color:#eee}"
    "input,select,button{width:100%;box-sizing:border-box;font-size:17px;padding:10px;margin:6px 0 14px;border-radius:8px;border:1px solid #555;background:#222;color:#eee}"
    "button{background:#e8a33a;color:#111;border:0;font-weight:600}label{font-size:14px;color:#aaa}</style></head><body><h2>Govee Pixel</h2>";

static int save_wifi(const char *ssid, const char *psk) {
    // ssid as hex (any characters allowed); psk quoted (wpa_supplicant takes everything up to the last quote)
    if (!*ssid || strlen(ssid) > 32 || strchr(psk, '\n') || strchr(psk, '\r')) return -1;
    size_t pl = strlen(psk); if (pl && (pl < 8 || pl > 63)) return -1;
    FILE *f = fopen("/data/custom/wpa_supplicant.conf.new", "w"); if (!f) return -1;
    fprintf(f, "ctrl_interface=/var/run/wpa_supplicant\nupdate_config=0\n\nnetwork={\n\t#ssid_text=%s\n\tssid=", ssid);
    for (const unsigned char *p = (const unsigned char *)ssid; *p; p++) fprintf(f, "%02x", *p);
    if (pl) fprintf(f, "\n\tpsk=\"%s\"\n\tkey_mgmt=WPA-PSK\n", psk); else fprintf(f, "\n\tkey_mgmt=NONE\n");
    fprintf(f, "\tscan_ssid=1\n}\n");
    if (fclose(f)) return -1;
    rename("/data/custom/wpa_supplicant.conf", "/data/custom/wpa_supplicant.conf.old");
    if (rename("/data/custom/wpa_supplicant.conf.new", "/data/custom/wpa_supplicant.conf")) return -1;
    chmod("/data/custom/wpa_supplicant.conf", 0600);
    sync();
    return 0;
}

#include "api.c"

static void http_reply(conn_t *c, const char *status, const char *body) { reply(c, status, "text/html; charset=utf-8", body, strlen(body)); }

// setup / status page (requests outside /api/), served through api.c's non-blocking connections
static void http_page(conn_t *c, const char *req, const char *body) {
    int setup = !strncmp(wifi_mode, "setup ", 6);
    static char page[8192], esc[200];
    if (!strncmp(req, "POST /save", 10) && setup) {
        char ssid[128] = "", other[128] = "", psk[128] = "";
        form_get(body, "ssid", ssid, sizeof ssid); form_get(body, "other", other, sizeof other); form_get(body, "psk", psk, sizeof psk);
        if (other[0]) snprintf(ssid, sizeof ssid, "%s", other);
        html_esc(ssid, esc, sizeof esc);
        if (save_wifi(ssid, psk) == 0) {
            snprintf(page, sizeof page, "%s<p>Saved. The light is now joining <b>%s</b>.</p><p>This hotspot will disappear. If the password was wrong, "
                     "the setup hotspot comes back in about 2 minutes with a new password on the panel.</p></body></html>", PAGE_HEAD, esc);
            http_reply(c, "200 OK", page);
            FILE *f = fopen("/tmp/wifi_apply", "w"); if (f) fclose(f);
            logf_("new Wi-Fi config saved for '%s'", ssid);
        } else {
            snprintf(page, sizeof page, "%s<p>That didn't look right: the network name must be 1-32 characters and the password 8-63 characters "
                     "(or empty for an open network).</p><p><a href='/' style='color:#e8a33a'>Back</a></p></body></html>", PAGE_HEAD);
            http_reply(c, "200 OK", page);
        }
    } else if (setup) {
        size_t o = (size_t)snprintf(page, sizeof page, "%s<form method=post action=/save><label>Wi-Fi network</label><select name=ssid>", PAGE_HEAD);
        FILE *f = fopen("/tmp/wifi_scan.txt", "r"); char l[128];
        while (f && fgets(l, sizeof l, f) && o < sizeof page - 600) {
            l[strcspn(l, "\r\n")] = 0; if (!l[0]) continue;
            html_esc(l, esc, sizeof esc);
            o += snprintf(page + o, sizeof page - o, "<option value=\"%s\">%s</option>", esc, esc);
        }
        if (f) fclose(f);
        snprintf(page + o, sizeof page - o, "</select><label>...or type a network name</label><input name=other autocomplete=off autocapitalize=none>"
                 "<label>Password</label><input name=psk type=password autocomplete=off><button>Save and connect</button></form></body></html>");
        http_reply(c, "200 OK", page);
    } else {
        snprintf(page, sizeof page, "%s<p>Running. Wi-Fi setup is available when the light can't reach its network, "
                 "or hold the <b>Mode</b> button for 5 seconds.</p><p>Managed by pixel2mqtt: %s</p></body></html>", PAGE_HEAD,
                 api_managed(now_s()) ? "yes" : ntokens ? "paired, not connected" : "no");
        http_reply(c, "200 OK", page);
    }
}

/* ------------------------------------------------------------------ main */
static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

int main(void) {
    tzset();
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_IGN);
    state_load();
    sound_scan();
    anim_scan();
    scr_scan();
    if (anim_name[0]) anim_load(anim_name, &cur_anim);
    if (st.mode == M_ANIM && !cur_anim.px) st.mode = M_CLOCK;
    playlist_load();
    { int e = effect_find(effect_name); effect_start(e >= 0 ? e : 4); }
    rnd_state ^= (uint32_t)time(NULL);
    if (display_open() < 0) { logf_("display init failed"); return 3; }
    d_lumi((uint8_t)st.bright);
    int have_mqtt = load_mqtt_conf() == 0;
    if (!have_mqtt) logf_("no %s - MQTT disabled", MQTT_CONF);

    int us = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua = {0}; ua.sin_family = AF_INET; ua.sin_port = htons(UDP_PORT); ua.sin_addr.s_addr = INADDR_ANY;
    if (bind(us, (struct sockaddr *)&ua, sizeof ua) < 0) { logf_("udp bind: %s", strerror(errno)); us = -1; }
    else fcntl(us, F_SETFL, O_NONBLOCK);
    int kfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (kfd < 0) logf_("buttons: %s", strerror(errno));
    hs = socket(AF_INET, SOCK_STREAM, 0);
    { int one = 1; setsockopt(hs, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }
    int http_port = 80;
#ifdef PIXELD_STUB_DISPLAY
    if (getenv("PIXELD_HTTP_PORT")) http_port = atoi(getenv("PIXELD_HTTP_PORT"));  // host test without a net namespace
#endif
    struct sockaddr_in ha = {0}; ha.sin_family = AF_INET; ha.sin_port = htons(http_port); ha.sin_addr.s_addr = INADDR_ANY;
    if (bind(hs, (struct sockaddr *)&ha, sizeof ha) < 0 || listen(hs, 4) < 0) { logf_("http bind: %s", strerror(errno)); close(hs); hs = -1; }
    else fcntl(hs, F_SETFL, O_NONBLOCK);
    api_init();
    int ds = socket(AF_INET, SOCK_DGRAM, 0);  // pixel2mqtt LAN discovery
    struct sockaddr_in da = {0}; da.sin_family = AF_INET; da.sin_port = htons(DISCOVERY_PORT); da.sin_addr.s_addr = INADDR_ANY;
    if (bind(ds, (struct sockaddr *)&da, sizeof da) < 0) { logf_("discovery bind: %s", strerror(errno)); close(ds); ds = -1; }
    else fcntl(ds, F_SETFL, O_NONBLOCK);
    logf_("started (udp %d, http 80, brightness %d, mode %s)", UDP_PORT, st.bright, MODES[st.mode]);

    static uint8_t ubuf[FRAME + 16];
    double next_frame = 0;
    while (!stop) {
        double t = now_s();
        wifi_mode_poll();
        keys_tick();
        int managed = api_managed(t);  // pixel2mqtt is polling us: it owns MQTT, ours stays off
        if (have_mqtt && mq < 0 && t >= mq_next_try && !strcmp(wifi_mode, "normal") && !managed) {
            if (ntokens) logf_("no pixel2mqtt bridge with MQTT is connected: using direct MQTT");
            mq_connect();
        }
        if (mq >= 0 && managed) {  // hand over without publishing "offline" (the bridge reports availability)
            logf_("pixel2mqtt connected: closing direct MQTT");
            mq_disconnect();
        }
        api_tick();
        if (mq >= 0 && t - mq_last_tx > 20) { uint8_t ping[2] = {0xC0, 0}; if (send_all(ping, 2) < 0) mq_close(); }

        if (mic_wanted()) mic_start(); else mic_stop();
        if (mic && t - level_pub > 5) { char lb[8]; snprintf(lb, sizeof lb, "%d", (int)(level * 100)); mq_pub(BASE "/sound_level/state", lb, 0); level_pub = t; }
        struct pollfd p[6 + NCONN]; int np = 0;
        if (ds >= 0) { p[np].fd = ds; p[np].events = POLLIN; np++; }
        if (us >= 0) { p[np].fd = us; p[np].events = POLLIN; np++; }
        if (mq >= 0) { p[np].fd = mq; p[np].events = POLLIN; np++; }
        if (kfd >= 0) { p[np].fd = kfd; p[np].events = POLLIN; np++; }
        if (hs >= 0) { p[np].fd = hs; p[np].events = POLLIN; np++; }
        if (mic_fd >= 0) { p[np].fd = mic_fd; p[np].events = POLLIN; np++; }
        int napi = api_pollfds(p + np), api0 = np; np += napi;
        int wait_ms = (int)((next_frame - now_s()) * 1000); if (wait_ms < 0) wait_ms = 0;
        poll(p, np, wait_ms);
        for (int k = 0; k < np; k++) {
            if (k >= api0) { if (p[k].revents) api_io(p[k].fd, p[k].revents); continue; }
            if (!(p[k].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            if (p[k].fd == ds) disco_read(ds);
            else if (p[k].fd == us) {
                ssize_t r;
                while ((r = recv(us, ubuf, sizeof ubuf, 0)) > 0)
                    if (r == FRAME) { memcpy(st.stream, ubuf, FRAME); st.stream_until = now_s() + 3; }
            } else if (p[k].fd == mq) mq_poll_in();
            else if (p[k].fd == kfd) keys_read(kfd);
            else if (p[k].fd == mic_fd) mic_read();
            else if (p[k].fd == hs) api_accept(hs);
        }
        if (now_s() >= next_frame) {
            render();
            d_data(32, 52, fb, FRAME);
            next_frame = now_s() + 1.0 / 30;  // up to 30 fps
        }
    }
    if (mq >= 0) { mq_pub(BASE "/status", "offline", 1); mq_disconnect(); }
    mic_stop();
    d_reset(); d_deinit();
    logf_("stopped");
    return 0;
}
