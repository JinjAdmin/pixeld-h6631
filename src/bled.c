// bled: passive BLE sensor bridge for the repurposed Govee H6631 (RTL8723DS, hci0).
// Listens to advertisements on a raw HCI socket (no bluetoothd, never connects to anything),
// decodes thermo-hygrometers and publishes them to MQTT with Home Assistant discovery.
// Optionally feeds named sensors to pixeld's info screen (govee-pixel/climate/set).
//
// Decoded formats (all unencrypted broadcasts):
//   * Govee H5072/H5075/H5101/H5102 (mfg 0xEC88, 6 bytes, packed temp/hum)
//   * Govee H5074/H5051/H5052/H5071 (mfg 0xEC88, 7 or 9 bytes, LE int16 /100)
//   * Govee H5100/H5103/H5104/H5105/H5108/H5110/H5174/H5177 (mfg 0x0001, 6 bytes, name "GVH5..." required)
//   * Govee H5179 (mfg 0x0001, 9 bytes, name required)
//   * BTHome v2 (service data 0xFCD2, unencrypted): temperature, humidity, battery
//   * ATC1441 / pvvx custom firmware (service data 0x181A, 13 or 15 bytes)
//
// hci0 must already be up (see /data/custom/apps/autostart/ble). Config: /data/custom/ble.conf.
// Test without radio: bled -t <AA:BB:CC:DD:EE:FF> <hex AD payload> [name]   (decode only, prints result)
// Build: zig cc -target arm-linux-gnueabihf.2.29 -mcpu=cortex_a7 -O2 -fPIE -pie -s -o bled bled.c
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
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
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define BASE "govee-pixel"
#define DISC "homeassistant"
#define MQTT_CONF "/data/custom/mqtt.conf"
#define BLE_CONF "/data/custom/ble.conf"
#define SEEN_FILE "/tmp/ble_sensors"
#define MAXS 32
#define MAXMAP 12

/* ------------------------------------------------------------------ raw HCI (no bluez headers needed) */
#define AF_BT 31
#define BTPROTO_HCI 1
#define SOL_HCI 0
#define HCI_FILTER 2
struct sockaddr_hci { sa_family_t family; unsigned short dev, channel; };
struct hci_filter { uint32_t type_mask; uint32_t event_mask[2]; uint16_t opcode; };

static void logf_(const char *fmt, ...) {
    char ts[32]; time_t t = time(NULL); strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    va_list ap; va_start(ap, fmt); printf("%s ", ts); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout);
}
static double now_s(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }

/* ------------------------------------------------------------------ config */
static struct {
    int interval_ms, window_ms, publish_s, expire_s, listed_only, fahrenheit, verbose;
    int nmap; struct { uint8_t mac[6]; char room[16]; } map[MAXMAP];
    int nign; uint8_t ign[MAXMAP][6];
} cf = {.interval_ms = 1000, .window_ms = 30, .publish_s = 60, .expire_s = 900};

static int parse_mac(const char *s, uint8_t *m) {  // "AA:BB:CC:DD:EE:FF" -> m[0]=AA
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -1;
    for (int i = 0; i < 6; i++) m[i] = (uint8_t)v[i];
    return 0;
}

static void load_ble_conf(void) {
    FILE *f = fopen(BLE_CONF, "r"); char l[160];
    if (!f) { logf_("no %s, using defaults (all decoded sensors are published)", BLE_CONF); return; }
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n#")] = 0;
        char *v = strchr(l, '='); if (!v) continue; *v++ = 0;
        if (!strcmp(l, "interval_ms")) cf.interval_ms = atoi(v);
        else if (!strcmp(l, "window_ms")) cf.window_ms = atoi(v);
        else if (!strcmp(l, "publish_s")) cf.publish_s = atoi(v);
        else if (!strcmp(l, "expire_s")) cf.expire_s = atoi(v);
        else if (!strcmp(l, "discover")) cf.listed_only = !strcmp(v, "listed");
        else if (!strcmp(l, "unit")) cf.fahrenheit = (v[0] == 'F' || v[0] == 'f');
        else if (!strcmp(l, "verbose")) cf.verbose = atoi(v);
        else if (!strcmp(l, "sensor") && cf.nmap < MAXMAP) {  // sensor=AA:BB:CC:DD:EE:FF Room name
            if (parse_mac(v, cf.map[cf.nmap].mac)) continue;
            const char *r = v + 17; while (*r == ' ' || *r == '\t') r++;
            snprintf(cf.map[cf.nmap].room, sizeof cf.map[0].room, "%s", r);
            cf.nmap++;
        } else if (!strcmp(l, "ignore") && cf.nign < MAXMAP) {
            if (!parse_mac(v, cf.ign[cf.nign])) cf.nign++;
        }
    }
    fclose(f);
    // keep the radio duty cycle sane: Wi-Fi shares the antenna with BT on the RTL8723DS
    if (cf.interval_ms < 20) cf.interval_ms = 20;
    if (cf.interval_ms > 10240) cf.interval_ms = 10240;
    if (cf.window_ms < 3) cf.window_ms = 3;
    if (cf.window_ms > cf.interval_ms / 2) cf.window_ms = cf.interval_ms / 2;
    if (cf.publish_s < 10) cf.publish_s = 10;
}

/* ------------------------------------------------------------------ sensor table */
enum { F_T = 1, F_H = 2, F_B = 4 };
typedef struct {
    uint8_t mac[6];        // display order (AA:BB:..)
    char name[32], model[16], mf[16];
    float t, h; int batt, rssi, fields;
    double seen, pub, first;
    int announced, room;   // room: index into cf.map or -1
    unsigned long n;
} sensor_t;
static sensor_t S[MAXS]; static int nS;
static unsigned long n_adv, n_adv_min; static double t_last_adv;

static void mac_str(const uint8_t *m, char *o) { sprintf(o, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]); }
static void mac_id(const uint8_t *m, char *o) { sprintf(o, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]); }

static int room_of(const uint8_t *m) { for (int i = 0; i < cf.nmap; i++) if (!memcmp(cf.map[i].mac, m, 6)) return i; return -1; }
static int ignored(const uint8_t *m) { for (int i = 0; i < cf.nign; i++) if (!memcmp(cf.ign[i], m, 6)) return 1; return 0; }

/* ------------------------------------------------------------------ decoders */
typedef struct { int fields; float t, h; int batt; char model[16], mf[16]; } reading_t;

static int sane(reading_t *r) {
    if ((r->fields & F_T) && (r->t < -50 || r->t > 100)) return 0;
    if ((r->fields & F_H) && (r->h < 0 || r->h > 100)) return 0;
    if ((r->fields & F_B) && (r->batt < 0 || r->batt > 100)) r->fields &= ~F_B;
    return r->fields != 0;
}

static void govee_packed(const uint8_t *p, reading_t *r) {  // 24-bit BE: bit23 = negative, value = t*10000 + h*10
    uint32_t v = ((uint32_t)p[0] << 16) | (p[1] << 8) | p[2];
    int neg = v & 0x800000; v &= 0x7FFFFF;
    r->t = (float)(v / 1000) / 10.0f; if (neg) r->t = -r->t;
    r->h = (float)(v % 1000) / 10.0f;
    r->fields |= F_T | F_H;
}

static void model_from_name(const char *name, const char *dflt, char *out) {
    const char *p = strstr(name, "H5");   // GVH5075_XXXX / Govee_H5179_XXXX / ihoment_H5074_XXXX
    if (p) { int i = 0; while (i < 15 && p[i] && p[i] != '_' && p[i] != ' ') { out[i] = p[i]; i++; } out[i] = 0; }
    else snprintf(out, 16, "%s", dflt);
}

static int govee_named(const char *name) { return !strncmp(name, "GVH5", 4) || !strncmp(name, "GV5", 3) || strstr(name, "Govee_H5") || strstr(name, "ihoment_H5"); }

// d = manufacturer data after the 2-byte company id
static int decode_mfg(uint16_t cid, const uint8_t *d, int n, const char *name, reading_t *r) {
    if (cid == 0xEC88) {
        snprintf(r->mf, sizeof r->mf, "Govee");
        if (n == 6) { govee_packed(d + 1, r); r->batt = d[4] & 0x7F; r->fields |= F_B; model_from_name(name, "H5075", r->model); return sane(r); }
        if (n == 7 || n == 9) {
            int16_t t = (int16_t)(d[1] | (d[2] << 8)); uint16_t h = d[3] | (d[4] << 8);
            r->t = t / 100.0f; r->h = h / 100.0f; r->batt = d[5]; r->fields = F_T | F_H | F_B;
            model_from_name(name, n == 7 ? "H5074" : "H5052", r->model); return sane(r);
        }
    }
    if (cid == 0x0001 && govee_named(name)) {  // 0x0001 is used by other vendors too: require a Govee name
        snprintf(r->mf, sizeof r->mf, "Govee");
        model_from_name(name, "H5xxx", r->model);
        if (n == 6) { govee_packed(d + 2, r); r->batt = d[5] & 0x7F; r->fields |= F_B; return sane(r); }
        if (n == 9 && strstr(name, "5179")) {
            int16_t t = (int16_t)(d[4] | (d[5] << 8)); uint16_t h = d[6] | (d[7] << 8);
            r->t = t / 100.0f; r->h = h / 100.0f; r->batt = d[8]; r->fields = F_T | F_H | F_B; return sane(r);
        }
    }
    return 0;
}

static int bthome_len(uint8_t id) {  // value length of BTHome v2 object ids (common subset); -1 = unknown
    switch (id) {
    case 0x00: case 0x01: case 0x09: case 0x0F: case 0x10: case 0x11: case 0x15: case 0x16: case 0x17: case 0x18:
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22:
    case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C:
    case 0x2D: case 0x2E: case 0x2F: case 0x3A: case 0x46: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x60: return 1;
    case 0x02: case 0x03: case 0x06: case 0x07: case 0x08: case 0x0C: case 0x0D: case 0x0E: case 0x12: case 0x13:
    case 0x14: case 0x3D: case 0x3F: case 0x40: case 0x41: case 0x43: case 0x44: case 0x45: case 0x47: case 0x48:
    case 0x49: case 0x51: case 0x52: case 0x5E: case 0x5F: return 2;
    case 0x04: case 0x05: case 0x0A: case 0x0B: case 0x42: case 0x4B: return 3;
    case 0x3E: case 0x4C: case 0x4D: case 0x4E: case 0x4F: case 0x50: case 0x55: case 0x56: return 4;
    default: return -1;
    }
}

static int decode_svc(uint16_t uuid, const uint8_t *d, int n, reading_t *r) {
    if (uuid == 0xFCD2 && n >= 1) {  // BTHome v2
        if ((d[0] & 1) || ((d[0] >> 5) & 7) != 2) return 0;  // encrypted or not v2
        snprintf(r->mf, sizeof r->mf, "BTHome"); snprintf(r->model, sizeof r->model, "BTHome v2");
        for (int i = 1; i < n;) {
            uint8_t id = d[i++]; int l = bthome_len(id);
            if (l < 0 || i + l > n) break;
            const uint8_t *v = d + i;
            if (id == 0x01) { r->batt = v[0]; r->fields |= F_B; }
            else if (id == 0x02) { r->t = (int16_t)(v[0] | (v[1] << 8)) / 100.0f; r->fields |= F_T; }
            else if (id == 0x45) { r->t = (int16_t)(v[0] | (v[1] << 8)) / 10.0f; r->fields |= F_T; }
            else if (id == 0x03) { r->h = (uint16_t)(v[0] | (v[1] << 8)) / 100.0f; r->fields |= F_H; }
            else if (id == 0x2E) { r->h = v[0]; r->fields |= F_H; }
            i += l;
        }
        return sane(r);
    }
    if (uuid == 0x181A) {  // ATC1441 (13 bytes, BE) or pvvx custom (15 bytes, LE); first 6 bytes = MAC
        snprintf(r->mf, sizeof r->mf, "ATC/pvvx");
        if (n == 13) {
            r->t = (int16_t)((d[6] << 8) | d[7]) / 10.0f; r->h = d[8]; r->batt = d[9];
            r->fields = F_T | F_H | F_B; snprintf(r->model, sizeof r->model, "ATC1441"); return sane(r);
        }
        if (n == 15) {
            r->t = (int16_t)(d[6] | (d[7] << 8)) / 100.0f; r->h = (uint16_t)(d[8] | (d[9] << 8)) / 100.0f; r->batt = d[12];
            r->fields = F_T | F_H | F_B; snprintf(r->model, sizeof r->model, "pvvx"); return sane(r);
        }
    }
    return 0;
}

// Parse one advertisement payload; returns 1 with r filled if it is a sensor reading.
static int decode_adv(const uint8_t *ad, int len, char *name, size_t nn, reading_t *r) {
    memset(r, 0, sizeof *r);
    // pass 1: name (the decoders for 0x0001 need it)
    for (int i = 0; i + 1 < len;) {
        int l = ad[i]; if (!l || i + 1 + l > len) break;
        uint8_t ty = ad[i + 1];
        if ((ty == 0x09 || ty == 0x08) && l > 1) {
            size_t k = (size_t)(l - 1) < nn - 1 ? (size_t)(l - 1) : nn - 1;
            memcpy(name, ad + i + 2, k); name[k] = 0;
            for (size_t j = 0; j < k; j++) if (!isprint((unsigned char)name[j])) name[j] = '?';
        }
        i += 1 + l;
    }
    for (int i = 0; i + 1 < len;) {
        int l = ad[i]; if (!l || i + 1 + l > len) break;
        uint8_t ty = ad[i + 1]; const uint8_t *p = ad + i + 2; int pl = l - 1;
        if (ty == 0xFF && pl >= 2 && decode_mfg(p[0] | (p[1] << 8), p + 2, pl - 2, name, r)) return 1;
        if (ty == 0x16 && pl >= 2 && decode_svc(p[0] | (p[1] << 8), p + 2, pl - 2, r)) return 1;
        memset(r, 0, sizeof *r);
        i += 1 + l;
    }
    return 0;
}

/* ------------------------------------------------------------------ MQTT 3.1.1 (QoS 0, publish only) */
static struct { char host[64], user[64], pass[128]; int port; } mc;
static int mq = -1;
static double mq_last_tx, mq_next_try;

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
static void mq_close(void) {
    if (mq >= 0) close(mq);
    mq = -1; mq_next_try = now_s() + 15;
    for (int i = 0; i < nS; i++) S[i].announced = 0;  // re-announce after reconnecting
}
static int mq_pub(const char *topic, const char *s, int retain) {
    if (mq < 0) return -1;
    size_t tl = strlen(topic), pl = strlen(s), rl = 2 + tl + pl;
    uint8_t *b = malloc(rl + 5); if (!b) return -1;
    size_t i = 0; b[i++] = 0x30 | (retain ? 1 : 0); i += put_len(b + i, rl); i += put_str(b + i, topic, tl);
    memcpy(b + i, s, pl); i += pl;
    int r = send_all(b, i); free(b);
    if (r < 0) { logf_("mqtt send failed"); mq_close(); }
    return r;
}
static void publish_bridge(void);
static int mq_connect(void) {
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(mc.port);
    if (inet_pton(AF_INET, mc.host, &a.sin_addr) != 1) { logf_("mqtt host must be an IP: %s", mc.host); mq_next_try = now_s() + 3600; return -1; }
    mq = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = {5, 0};
    setsockopt(mq, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(mq, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(mq, (struct sockaddr *)&a, sizeof a) < 0) { logf_("mqtt connect: %s", strerror(errno)); mq_close(); return -1; }
    int one = 1; setsockopt(mq, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    uint8_t b[512], vb[480]; size_t i = 0, v = 0;
    const char *cid = "govee-pixel-ble", *wt = BASE "/ble/status", *wm = "offline";
    v += put_str(vb + v, "MQTT", 4); vb[v++] = 4;
    vb[v++] = 0x02 | 0x04 | 0x20 | (mc.user[0] ? 0x80 : 0) | (mc.pass[0] ? 0x40 : 0);
    vb[v++] = 0; vb[v++] = 60;  // keepalive 60 s
    v += put_str(vb + v, cid, strlen(cid)); v += put_str(vb + v, wt, strlen(wt)); v += put_str(vb + v, wm, strlen(wm));
    if (mc.user[0]) v += put_str(vb + v, mc.user, strlen(mc.user));
    if (mc.pass[0]) v += put_str(vb + v, mc.pass, strlen(mc.pass));
    b[i++] = 0x10; i += put_len(b + i, v); memcpy(b + i, vb, v); i += v;
    if (send_all(b, i) < 0) { mq_close(); return -1; }
    uint8_t ack[4];
    if (recv(mq, ack, 4, MSG_WAITALL) != 4 || ack[0] != 0x20) { logf_("mqtt: no CONNACK"); mq_close(); return -1; }
    if (ack[3] != 0) { logf_("mqtt: refused, code %d", ack[3]); mq_close(); mq_next_try = now_s() + 120; return -1; }
    fcntl(mq, F_SETFL, fcntl(mq, F_GETFL) | O_NONBLOCK);
    logf_("mqtt connected to %s:%d", mc.host, mc.port);
    mq_pub(BASE "/ble/status", "online", 1);
    publish_bridge();
    return 0;
}
static void mq_drain(void) {  // we never subscribe; only PINGRESP arrives. Detect a dead connection.
    uint8_t b[256];
    for (;;) {
        ssize_t r = recv(mq, b, sizeof b, 0);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { logf_("mqtt connection lost"); mq_close(); return; }
        if (r < 0) return;
    }
}

/* ------------------------------------------------------------------ HA discovery + state */
static const char *DEV_BRIDGE = "\"dev\":{\"ids\":[\"govee_pixel_h6631\"],\"name\":\"Govee Pixel\",\"mf\":\"Govee (custom firmware)\",\"mdl\":\"H6631\"}";

static void publish_bridge(void) {  // diagnostic sensor on the Govee Pixel device itself
    char b[640];
    snprintf(b, sizeof b, "{\"name\":\"BLE sensors\",\"uniq_id\":\"govee_pixel_ble_count\",\"stat_t\":\"" BASE "/ble/bridge\","
             "\"val_tpl\":\"{{ value_json.sensors }}\",\"json_attr_t\":\"" BASE "/ble/bridge\",\"icon\":\"mdi:bluetooth\","
             "\"ent_cat\":\"diagnostic\",\"avty_t\":\"" BASE "/ble/status\",%s}", DEV_BRIDGE);
    mq_pub(DISC "/sensor/govee_pixel_ble_count/config", b, 1);
}

static void sensor_label(const sensor_t *s, char *o, size_t n) {
    if (s->room >= 0) { snprintf(o, n, "%s sensor", cf.map[s->room].room); return; }
    snprintf(o, n, "%s %02X%02X", s->model[0] ? s->model : "BLE", s->mac[4], s->mac[5]);
}

static void announce(sensor_t *s) {
    char id[16], mac[20], label[48], dev[400], b[900], t[128];
    mac_id(s->mac, id); mac_str(s->mac, mac); sensor_label(s, label, sizeof label);
    snprintf(dev, sizeof dev, "\"dev\":{\"ids\":[\"gpble_%s\"],\"name\":\"%s\",\"mf\":\"%s\",\"mdl\":\"%s\",\"cns\":[[\"bluetooth\",\"%s\"]],"
             "\"via_device\":\"govee_pixel_h6631\"},\"stat_t\":\"" BASE "/ble/%s\",\"exp_aft\":%d",
             id, label, s->mf, s->model, mac, id, cf.expire_s);
    struct { int f; const char *key, *name, *cls, *unit, *extra; } e[] = {
        {F_T, "temperature", "Temperature", "temperature", "\xc2\xb0" "C", "\"stat_cla\":\"measurement\",\"sug_dsp_prc\":1"},
        {F_H, "humidity", "Humidity", "humidity", "%", "\"stat_cla\":\"measurement\",\"sug_dsp_prc\":0"},
        {F_B, "battery", "Battery", "battery", "%", "\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\""},
        {-1, "rssi", "Signal strength", "signal_strength", "dBm", "\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",\"en\":false"},
    };
    for (int k = 0; k < 4; k++) {
        if (e[k].f > 0 && !(s->fields & e[k].f)) continue;
        snprintf(b, sizeof b, "{\"name\":\"%s\",\"uniq_id\":\"gpble_%s_%s\",\"val_tpl\":\"{{ value_json.%s }}\",\"dev_cla\":\"%s\","
                 "\"unit_of_meas\":\"%s\",%s,%s}", e[k].name, id, e[k].key, e[k].key, e[k].cls, e[k].unit, e[k].extra, dev);
        snprintf(t, sizeof t, DISC "/sensor/gpble_%s/%s/config", id, e[k].key);
        if (mq_pub(t, b, 1) < 0) return;
    }
    s->announced = 1;
    logf_("announced %s (%s %s) to Home Assistant", mac, s->mf, s->model);
}

static void publish_sensor(sensor_t *s) {
    char id[16], t[64], b[200]; int n = 0;
    mac_id(s->mac, id);
    n += snprintf(b + n, sizeof b - n, "{");
    if (s->fields & F_T) n += snprintf(b + n, sizeof b - n, "\"temperature\":%.2f,", s->t);
    if (s->fields & F_H) n += snprintf(b + n, sizeof b - n, "\"humidity\":%.1f,", s->h);
    if (s->fields & F_B) n += snprintf(b + n, sizeof b - n, "\"battery\":%d,", s->batt);
    snprintf(b + n, sizeof b - n, "\"rssi\":%d}", s->rssi);
    snprintf(t, sizeof t, BASE "/ble/%s", id);
    if (mq_pub(t, b, 0) < 0) return;
    if (s->room >= 0 && (s->fields & F_T)) {  // feed pixeld's climate info screen
        float tv = cf.fahrenheit ? s->t * 9.0f / 5.0f + 32.0f : s->t;
        snprintf(b, sizeof b, "{\"name\":\"%s\",\"temp\":%.1f,\"unit\":\"%s\",\"humidity\":%d}", cf.map[s->room].room, tv,
                 cf.fahrenheit ? "F" : "C", (s->fields & F_H) ? (int)lroundf(s->h) : -1);
        mq_pub(BASE "/climate/set", b, 0);
    }
    s->pub = now_s();
}

static void write_seen(void) {  // human-readable table for `cat /tmp/ble_sensors`
    FILE *f = fopen(SEEN_FILE ".tmp", "w"); if (!f) return;
    double t = now_s();
    fprintf(f, "# mac               model      mf        temp_C  hum%%  batt  rssi  age_s  name / room\n");
    for (int i = 0; i < nS; i++) {
        sensor_t *s = &S[i]; char m[20]; mac_str(s->mac, m);
        fprintf(f, "%s  %-9s  %-8s  %6.2f  %5.1f  %4d  %4d  %5d  %s%s%s\n", m, s->model, s->mf, s->t, s->h, s->batt, s->rssi,
                (int)(t - s->seen), s->name, s->room >= 0 ? " -> " : "", s->room >= 0 ? cf.map[s->room].room : "");
    }
    fprintf(f, "# adverts in the last minute: %lu\n", n_adv_min);
    fclose(f); rename(SEEN_FILE ".tmp", SEEN_FILE);
}

static void on_reading(const uint8_t *mac, const char *name, const reading_t *r, int rssi) {
    if (ignored(mac)) return;
    int room = room_of(mac);
    if (cf.listed_only && room < 0) return;
    sensor_t *s = NULL;
    for (int i = 0; i < nS; i++) if (!memcmp(S[i].mac, mac, 6)) { s = &S[i]; break; }
    if (!s) {
        if (nS == MAXS) {  // replace the stalest
            int o = 0; for (int i = 1; i < nS; i++) if (S[i].seen < S[o].seen) o = i;
            s = &S[o];
        } else s = &S[nS++];
        memset(s, 0, sizeof *s); memcpy(s->mac, mac, 6); s->first = now_s(); s->room = room;
        char m[20]; mac_str(mac, m);
        logf_("new sensor %s %s %s '%s' t=%.2f h=%.1f b=%d rssi=%d", m, r->mf, r->model, name, r->t, r->h, r->batt, rssi);
    }
    if (name[0]) snprintf(s->name, sizeof s->name, "%s", name);
    snprintf(s->model, sizeof s->model, "%s", r->model); snprintf(s->mf, sizeof s->mf, "%s", r->mf);
    if (r->fields & F_T) s->t = r->t;
    if (r->fields & F_H) s->h = r->h;
    if (r->fields & F_B) s->batt = r->batt;
    if ((s->fields | r->fields) != s->fields) s->announced = 0;  // new entity kinds -> re-announce
    s->fields |= r->fields; s->rssi = rssi; s->seen = now_s(); s->n++;
    if (cf.verbose) { char m[20]; mac_str(mac, m); logf_("%s t=%.2f h=%.1f b=%d rssi=%d", m, s->t, s->h, s->batt, rssi); }
}

/* ------------------------------------------------------------------ HCI */
static int hci_cmd(int fd, uint16_t ocf_ogf, const uint8_t *p, uint8_t n) {
    uint8_t b[64]; b[0] = 0x01; b[1] = ocf_ogf & 0xFF; b[2] = ocf_ogf >> 8; b[3] = n; memcpy(b + 4, p, n);
    if (write(fd, b, 4 + n) != 4 + n) { logf_("hci write 0x%04x: %s", ocf_ogf, strerror(errno)); return -1; }
    // wait for Command Complete / Status for this opcode (other events are ignored here)
    for (double t0 = now_s(); now_s() - t0 < 2;) {
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, 200) <= 0) continue;
        uint8_t r[260]; ssize_t k = read(fd, r, sizeof r);
        if (k < 7 || r[0] != 0x04) continue;
        if (r[1] == 0x0E && (r[4] | (r[5] << 8)) == ocf_ogf) return r[6];
        if (r[1] == 0x0F && (r[5] | (r[6] << 8)) == ocf_ogf) return r[3];
    }
    logf_("hci 0x%04x: no reply", ocf_ogf);
    return -1;
}

static int scan_enable(int fd, int on) {
    uint8_t e[2] = {(uint8_t)on, 0x00};  // no duplicate filtering: we want every new reading
    return hci_cmd(fd, 0x200C, e, 2);
}

static int scan_start(int fd) {
    scan_enable(fd, 0);  // may fail with "command disallowed" if it was off; that's fine
    uint16_t iv = (uint16_t)(cf.interval_ms * 1000 / 625), wn = (uint16_t)(cf.window_ms * 1000 / 625);
    uint8_t p[7] = {0x00 /* passive: never sends scan requests */, iv & 0xFF, iv >> 8, wn & 0xFF, wn >> 8, 0x00, 0x00};
    int r = hci_cmd(fd, 0x200B, p, 7);
    if (r) { logf_("set scan parameters failed (%d)", r); return -1; }
    r = scan_enable(fd, 1);
    if (r) { logf_("scan enable failed (%d)", r); return -1; }
    logf_("passive scan on (window %d ms every %d ms)", cf.window_ms, cf.interval_ms);
    return 0;
}

static int hci_open(void) {
    int fd = socket(AF_BT, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) { logf_("hci socket: %s", strerror(errno)); return -1; }
    struct sockaddr_hci a = {AF_BT, 0, 0};  // hci0, raw channel
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { logf_("bind hci0: %s", strerror(errno)); close(fd); return -1; }
    struct hci_filter f = {0};
    f.type_mask = 1u << 0x04;                      // event packets
    f.event_mask[0] = (1u << 0x0E) | (1u << 0x0F); // command complete / status
    f.event_mask[1] = 1u << (0x3E - 32);           // LE meta
    if (setsockopt(fd, SOL_HCI, HCI_FILTER, &f, sizeof f) < 0) { logf_("hci filter: %s", strerror(errno)); close(fd); return -1; }
    return fd;
}

static void hci_event(const uint8_t *b, ssize_t n) {
    if (n < 5 || b[0] != 0x04 || b[1] != 0x3E || b[3] != 0x02) return;  // LE Advertising Report only
    int nrep = b[4], i = 5;
    for (int k = 0; k < nrep && i + 9 <= n; k++) {
        int dl = b[i + 8];
        if (i + 9 + dl + 1 > n) break;
        uint8_t mac[6]; for (int j = 0; j < 6; j++) mac[j] = b[i + 2 + 5 - j];
        const uint8_t *ad = b + i + 9; int rssi = (int8_t)b[i + 9 + dl];
        n_adv++; t_last_adv = now_s();
        char name[32] = ""; reading_t r;
        if (decode_adv(ad, dl, name, sizeof name, &r)) on_reading(mac, name, &r, rssi);
        i += 9 + dl + 1;
    }
}

/* ------------------------------------------------------------------ main */
static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static int hexval(int c) { return isdigit(c) ? c - '0' : isxdigit(c) ? (tolower(c) - 'a' + 10) : -1; }

static int test_mode(int argc, char **argv) {  // bled -t MAC HEX [name]
    uint8_t mac[6], ad[64]; int n = 0;
    if (argc < 4 || parse_mac(argv[2], mac)) { fprintf(stderr, "usage: bled -t AA:BB:CC:DD:EE:FF <hex AD payload>\n"); return 2; }
    for (const char *p = argv[3]; p[0] && p[1] && n < 64; p += 2) {
        int h = hexval(p[0]), l = hexval(p[1]); if (h < 0 || l < 0) break; ad[n++] = (uint8_t)(h * 16 + l);
    }
    char name[32] = ""; reading_t r;
    if (!decode_adv(ad, n, name, sizeof name, &r)) { printf("not a sensor (%d bytes, name '%s')\n", n, name); return 1; }
    printf("%s %s name='%s' fields=%d t=%.2f h=%.1f batt=%d\n", r.mf, r.model, name, r.fields, r.t, r.h, r.batt);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-t")) return test_mode(argc, argv);
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN);
    load_ble_conf();
    int have_mqtt = load_mqtt_conf() == 0;
    if (!have_mqtt) logf_("no %s - MQTT disabled (sensors still listed in %s)", MQTT_CONF, SEEN_FILE);
    int fd = hci_open();
    if (fd < 0) return 1;
    if (scan_start(fd) < 0) { close(fd); return 1; }
    double t_min = now_s(), t_kick = 0; t_last_adv = now_s();
    unsigned long adv_at_min = 0;
    while (!stop) {
        double t = now_s();
        if (have_mqtt && mq < 0 && t >= mq_next_try) mq_connect();
        struct pollfd pf[2] = {{fd, POLLIN, 0}, {mq, POLLIN, 0}};
        int np = mq >= 0 ? 2 : 1;
        if (poll(pf, np, 1000) < 0 && errno != EINTR) break;
        if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL)) { logf_("hci0 went away"); break; }
        if (pf[0].revents & POLLIN) {
            uint8_t b[300];
            for (int k = 0; k < 64; k++) { ssize_t r = recv(fd, b, sizeof b, MSG_DONTWAIT); if (r <= 0) break; hci_event(b, r); }
        }
        if (mq >= 0 && (pf[1].revents & POLLIN)) mq_drain();
        t = now_s();
        if (mq >= 0 && t - mq_last_tx > 45) { uint8_t ping[2] = {0xC0, 0}; if (send_all(ping, 2) < 0) mq_close(); }
        for (int i = 0; i < nS && mq >= 0; i++) {
            sensor_t *s = &S[i];
            if (!s->announced) announce(s);
            if (s->announced && s->seen > s->pub && t - s->pub >= cf.publish_s) publish_sensor(s);
        }
        if (t - t_min >= 60) {
            n_adv_min = n_adv - adv_at_min; adv_at_min = n_adv; t_min = t;
            write_seen();
            if (mq >= 0) {
                int active = 0; for (int i = 0; i < nS; i++) if (t - S[i].seen < cf.expire_s) active++;
                char b[96]; snprintf(b, sizeof b, "{\"sensors\":%d,\"adverts_per_min\":%lu}", active, n_adv_min);
                mq_pub(BASE "/ble/bridge", b, 1);
            }
        }
        // watchdog: a busy neighbourhood gives adverts every second; silence means the scan stopped
        if (t - t_last_adv > 120 && t - t_kick > 120) { logf_("no adverts for 2 min, restarting scan"); scan_start(fd); t_kick = t; }
        if (t - t_last_adv > 600) { logf_("no adverts for 10 min, exiting so the wrapper re-initialises hci0"); break; }
    }
    scan_enable(fd, 0);
    close(fd);
    if (mq >= 0) { mq_pub(BASE "/ble/status", "offline", 1); uint8_t d[2] = {0xE0, 0}; send_all(d, 2); close(mq); }
    logf_("bled stopped");
    return 0;
}
