// pixeltest: drive the H6631 panel through Govee's /usr/lib/libgovee_display.so.
// Signatures were inferred from disassembly (see NOTES.md "Display API").
//   pixeltest fill R G B [pct] [secs]  - whole panel one colour
//   pixeltest orient [pct] [secs]      - orientation markers:
//        x=0,y=0 red; x=31,y=0 green; x=0,y=51 blue; column x=0 dim white; row y=0 dim yellow
// Build: zig cc -target arm-linux-gnueabihf.2.29 -mcpu=cortex_a7 -O2 -fPIE -pie -o pixeltest pixeltest.c -ldl
//   (-fPIE -pie is required: zig 0.13 non-PIE builds copy-relocate stderr with the wrong size and printf segfaults)
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 32
#define H 52

typedef int (*fn_void)(void);
typedef int (*fn_lumi)(uint8_t);
typedef int (*fn_area)(int *, int *);
typedef int (*fn_data)(int, int, const uint8_t *, int);

static uint8_t frame[W * H * 3];

static void px(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *p = &frame[(x * H + y) * 3];
    p[0] = r; p[1] = g; p[2] = b;
}

static void *sym(void *h, const char *n) {
    void *f = dlsym(h, n);
    if (!f) { fprintf(stderr, "missing symbol %s\n", n); exit(2); }
    return f;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s fill R G B [pct] [secs] | orient [pct] [secs]\n", argv[0]);
        return 1;
    }
    int pct = 20, secs = 10, a = 2;
    if (!strcmp(argv[1], "fill")) {
        if (argc < 5) { fprintf(stderr, "fill needs R G B\n"); return 1; }
        int r = atoi(argv[2]), g = atoi(argv[3]), b = atoi(argv[4]);
        for (int x = 0; x < W; x++) for (int y = 0; y < H; y++) px(x, y, r, g, b);
        a = 5;
    } else if (!strcmp(argv[1], "orient")) {
        for (int y = 0; y < H; y++) px(0, y, 40, 40, 40);
        for (int x = 0; x < W; x++) px(x, 0, 40, 40, 0);
        px(0, 0, 255, 0, 0);
        px(W - 1, 0, 0, 255, 0);
        px(0, H - 1, 0, 0, 255);
    } else {
        fprintf(stderr, "unknown mode %s\n", argv[1]); return 1;
    }
    if (argc > a) pct = atoi(argv[a]);
    if (argc > a + 1) secs = atoi(argv[a + 1]);
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;

    void *h = dlopen("/usr/lib/libgovee_display.so", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    fn_void init = (fn_void)sym(h, "LightKal_lcd_display_init");
    fn_void deinit = (fn_void)sym(h, "LightKal_lcd_display_deinit");
    fn_void reset = (fn_void)sym(h, "LightKal_lcd_display_reset_frame");
    fn_lumi lumi = (fn_lumi)sym(h, "LightKal_lcd_display_lumi_set");
    fn_area area = (fn_area)sym(h, "LightKal_lcd_display_area_get");
    fn_data data = (fn_data)sym(h, "LightKal_lcd_display_data_set");

    int rc = init();
    printf("init=%d\n", rc);
    if (rc < 0) return 3;
    int w = 0, hh = 0;
    printf("area=%d w=%d h=%d\n", area(&w, &hh), w, hh);
    printf("lumi=%d (pct %d)\n", lumi((uint8_t)pct), pct);
    printf("data=%d\n", data(W, H, frame, sizeof frame));
    fflush(stdout);
    sleep(secs);
    printf("reset=%d\n", reset());
    printf("deinit=%d\n", deinit());
    return 0;
}
