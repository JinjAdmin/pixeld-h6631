// screen_preview: render a layout screen (.scr) on the PC with pixeld's own renderer (no panel, no network).
// Used for design work and as the reference for pixel2mqtt's browser renderer (tests/screens).
//
// Build (from the repo root):
//   zig cc -target x86_64-linux-musl -O1 -DPIXELD_STUB_DISPLAY -DICON_DIR='"<icons dir>"' \
//          -o build/screen_preview tools/screen_preview.c -lm
// Run:
//   screen_preview <out.raw> <screen.scr> <frames> <ms> [options] [values.json ...] [cmd=payload ...]
//     values.json      a values/set payload (flat JSON object); several files are applied in order
//     cmd=payload      any pixeld command, e.g. clock12=ON  fahrenheit=OFF  values={"a":1}
//     --time=<epoch>   fixed clock for the first frame (later frames add <ms>); default: now
//     --tz=<TZ>        time zone (default: the environment's TZ, else UTC)
//     --check          only parse; print errors and exit 0 (1 if there are errors)
// Output: <frames> x 4992-byte RGB frames (52x32, row-major), appended to <out.raw>.
// stderr: the screen's name, problems ("line N: msg"), visibility and missing values.
#define main pixeld_main
#include "../src/pixeld.c"
#undef main

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)sz + 1); if (!b) { fclose(f); return NULL; }
    *n = fread(b, 1, (size_t)sz, f); b[*n] = 0; fclose(f); return b;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s out.raw screen.scr frames ms [--time=epoch] [--tz=TZ] [--check] [values.json ...] [cmd=payload ...]\n", argv[0]);
        return 1;
    }
    double t0 = 0; int check = 0;
    if (!getenv("TZ")) setenv("TZ", "UTC0", 1);
    for (int i = 5; i < argc; i++) {
        if (!strncmp(argv[i], "--time=", 7)) t0 = atof(argv[i] + 7);
        else if (!strncmp(argv[i], "--tz=", 5)) setenv("TZ", argv[i] + 5, 1);
        else if (!strcmp(argv[i], "--check")) check = 1;
    }
    tzset();
    if (t0 > 0) stub_now = t0;

    // the screen: point the screens directory at the file's folder and load just that file
    static char dir[400]; const char *file = argv[2], *sl = strrchr(file, '/');
    if (sl) { snprintf(dir, sizeof dir, "%.*s", (int)(sl - file), file); file = sl + 1; } else snprintf(dir, sizeof dir, ".");
    scr_dir = dir;

    // values and commands first, so the load-time check sees them
    for (int i = 5; i < argc; i++) {
        if (!strncmp(argv[i], "--", 2)) continue;
        char *eq = strchr(argv[i], '=');
        size_t n = strlen(argv[i]);
        if (n > 5 && !strcmp(argv[i] + n - 5, ".json") && !eq) {
            size_t sz; char *b = slurp(argv[i], &sz);
            if (!b) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
            int c = values_message((const uint8_t *)b, sz); free(b);
            fprintf(stderr, "%s: %d value(s)\n", argv[i], c);
        } else if (eq) {
            char topic[96]; snprintf(topic, sizeof topic, BASE "/%.*s/set", (int)(eq - argv[i]), argv[i]);
            on_message(topic, (const uint8_t *)(eq + 1), strlen(eq + 1));
        }
    }

    scr_t *s = scr_load(file);
    if (!s) { fprintf(stderr, "cannot open %s/%s\n", dir, file); return 1; }
    fprintf(stderr, "screen \"%s\": %d element(s), %d problem(s)\n", s->name, s->nel, s->nerr);
    for (int i = 0; i < s->nerr; i++) fprintf(stderr, "  line %d: %s\n", s->err[i].line, s->err[i].msg);
    if (check) return s->nerr ? 1 : 0;

    FILE *o = fopen(argv[1], "ab"); if (!o) return 2;
    int frames = atoi(argv[3]), ms = atoi(argv[4]);
    double t = t0 > 0 ? t0 : now_s();
    for (int i = 0; i < frames; i++) {
        stub_now = t;
        scr_tick_linger(s, t);
        if (i == 0) {
            char miss[200]; scr_needs_ok(s, miss, sizeof miss);
            fprintf(stderr, "visible: %s%s%s\n", scr_visible(s, t) ? "yes" : "no", miss[0] ? ", missing/stale: " : "", miss);
        }
        memset(fb, 0, sizeof fb);
        scr_render(s, t);
        fwrite(fb, 1, FRAME, o);
        t += ms / 1000.0;
        if (t0 <= 0 && i < frames - 1) usleep(ms * 1000);
    }
    fclose(o);
    scr_free(s);
    return 0;
}
