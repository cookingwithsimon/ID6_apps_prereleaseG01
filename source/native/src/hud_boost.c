/* hud_boost.c - boost on the G01 head-up display, sent from the head unit itself.
 *
 * The G01 HUD is drawn by the instrument cluster (KOMBI). The head unit feeds it turn-by-turn
 * guidance over SOME/IP, service 0xB006 at 160.48.199.96:30502 (docs/RIC-G01.md). This worker
 * uses that link with its own client id (0x7E01) to show one text line ("BOOST 12.3 psi") and a
 * 52x33 bar picture to the left of it. It uses only the frames tested on Simon's car in steps 4-12:
 * a guidance turn with no map sketch, no arrow and no distance box, and the road-name text plus
 * one road icon. It never sends a route, a place or a road-number picture.
 *
 * Boost comes from the DME over the diagnostic gateway (HSFZ TCP 6801, UDS 0x22 reads of DIDs 4201
 * ambient pressure and 4205 boost pressure only, BoostDeck's scaling). It shares the gateway with
 * BoostDeck and the other apps through the loopback lease in diagnostic_lease.h.
 *
 * First version: started by hand over SSH, parked, with NO factory nav route running. It does not
 * yet notice factory guidance starting, so it must not be left running while driving with a route.
 * Ctrl+C, SIGTERM, SIGHUP (SSH closed) and the time limit all end it with the factory clear.
 *
 *   hud-boost-qnx [--demo] [--seconds N] [--gateway IP] [--start 1|2] [--max PSI]
 *
 * No floating point and no run-time integer division (only shifts, products and constant
 * divisors), so it builds with the bmw-tools QNX SDK and runs on the NBT Evo's QNX 6.5. */
#define _POSIX_C_SOURCE 200809L
#include "diagnostic_lease.h"
#include "hud_boost_frames.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define KOMBI_IP "160.48.199.96"
#define KOMBI_PORT 30502
#define GATEWAY_PORT 6801
#define CLIENT_ID 0x7E01
#define PLUGINS_DIR "/var/opt/hmi/data/ID5/Plugins"

#define ICON_W 52            /* the road-icon slot: bigger pictures are accepted but not drawn (step 7) */
#define ICON_H 33
#define ICON_EDGE 4          /* outline thickness, ICON_H / 8 as in ric-pic.py */
#define BAR_COLS (ICON_W - 2 * ICON_EDGE)

enum { TICK_MS = 200, REFRESH_MS = 5000, KOMBI_TIMEOUT_MS = 1000, GW_TIMEOUT_MS = 500,
       GW_RETRY_MS = 3000, KOMBI_RETRY_MS = 3000, AMBIENT_EVERY_MS = 1000, AMBIENT_FRESH_MS = 2500,
       HOLD_MS = 1000 /* keep showing the last reading while another app has the gateway */ };

static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint32_t)t.tv_nsec / 1000000u;
}

static uint64_t stop_at;            /* the --seconds limit, 0 = none */

static void pause_ms(unsigned ms) {
    uint64_t end = now_ms() + ms;
    if (stop_at && end > stop_at) end = stop_at;
    while (running) {
        uint64_t now = now_ms();
        if (now >= end) break;
        poll(0, 0, (int)(end - now));
    }
}

static void put_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}
static void put_u16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static uint32_t get_u32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* "-12.3" from tenths, by subtraction so no division helper is needed. */
static void tenths_text(int v, char *out) {
    static const int steps[] = {10000, 1000, 100, 10};
    int i, started = 0;
    if (v < 0) { *out++ = '-'; v = -v; }
    if (v > 99999) v = 99999;
    for (i = 0; i < 4; ++i) {
        int d = 0;
        while (v >= steps[i]) { v -= steps[i]; ++d; }
        if (d || started || steps[i] == 10) { *out++ = (char)('0' + d); started = 1; }
        if (steps[i] == 10) *out++ = '.';
    }
    *out++ = (char)('0' + v);
    *out = 0;
}

/* ---------- sockets ---------- */

static int wait_fd(int fd, short events, uint64_t deadline) {
    struct pollfd p;
    for (;;) {
        uint64_t now = now_ms();
        int rc;
        if (now >= deadline) return 0;
        p.fd = fd; p.events = events; p.revents = 0;
        rc = poll(&p, 1, (int)(deadline - now));
        if (rc < 0 && errno == EINTR) { if (!running) return 0; continue; }
        return rc > 0 && (p.revents & events);
    }
}

static int transfer(int fd, unsigned char *buf, size_t n, int sending, uint64_t deadline) {
    while (n) {
        ssize_t k;
        if (!wait_fd(fd, sending ? POLLOUT : POLLIN, deadline)) return 0;
        k = sending ? send(fd, buf, n, 0) : recv(fd, buf, n, 0);
        if (k < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (k <= 0) return 0;
        buf += k; n -= (size_t)k;
    }
    return 1;
}

static int connect_tcp(const char *ip, unsigned port) {
    struct sockaddr_in a;
    int fd, error = 0, one = 1;
    socklen_t len = sizeof error;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (!wait_fd(fd, POLLOUT, now_ms() + 1500) ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) || error) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---------- KOMBI (SOME/IP, service 0xB006) ---------- */

static int kombi = -1;
static unsigned session;
static unsigned char frame[12288];

/* Sends one request and waits for its answer. 0 = accepted (E_OK), -1 = link lost,
 * otherwise (message type << 8 | return code) of the refusal. */
static int kombi_send(unsigned method, const unsigned char *body, unsigned len) {
    unsigned char hdr[16], skip[256];
    uint64_t deadline = now_ms() + KOMBI_TIMEOUT_MS;
    if (kombi < 0) return -1;
    session = session >= 65534 ? 1 : session + 1;
    put_u16(hdr, 0xB006); put_u16(hdr + 2, method); put_u32(hdr + 4, 8 + len);
    put_u16(hdr + 8, CLIENT_ID); put_u16(hdr + 10, session);
    hdr[12] = 1; hdr[13] = 1; hdr[14] = 0; hdr[15] = 0;
    if (!transfer(kombi, hdr, 16, 1, deadline) || !transfer(kombi, (unsigned char *)body, len, 1, deadline))
        return -1;
    for (;;) {
        uint32_t rest;
        unsigned svc, ses, type, rc;
        if (!transfer(kombi, hdr, 16, 0, deadline)) return -1;
        svc = ((unsigned)hdr[0] << 8) | hdr[1];
        rest = get_u32(hdr + 4);
        if (rest < 8) return -1;
        rest -= 8;
        while (rest) {                               /* answers carry no payload we need */
            unsigned n = rest > sizeof skip ? (unsigned)sizeof skip : (unsigned)rest;
            if (!transfer(kombi, skip, n, 0, deadline)) return -1;
            rest -= n;
        }
        ses = ((unsigned)hdr[10] << 8) | hdr[11];
        type = hdr[14]; rc = hdr[15];
        if (svc == 0xFFFF || svc != 0xB006 || ses != session) continue;   /* magic cookie, events */
        if (type != 0x80 && type != 0x81) continue;
        return type == 0x80 && rc == 0 ? 0 : (int)((type << 8) | rc);
    }
}

static const unsigned char COMMIT[4] = {0x00, 0x00, 0x00, 0xc8};
static const unsigned char CHECK_ICON[9] = {0x00, 0x00, 0x00, 0xc8, 0x01, 0x00, 0x00, 0x00, 0x01};

/* text resource 03000000 (the road-name line), UTF-16BE behind a FEFF mark */
static unsigned text_frame(const char *text) {
    unsigned n = 0, len = sizeof TEXT_PRE;
    memcpy(frame, TEXT_PRE, sizeof TEXT_PRE);
    while (text[n] && n < 40) ++n;
    put_u32(frame + len, 2 + 2 * n + 2); len += 4;
    frame[len++] = 0xFE; frame[len++] = 0xFF;
    for (n = 0; text[n] && n < 40; ++n) { frame[len++] = 0; frame[len++] = (unsigned char)text[n]; }
    frame[len++] = 0; frame[len++] = 0;
    put_u32(frame + 9, len - 17);
    return len;
}

/* distance model 1d000000 with an empty text: hides the distance box (step 5) */
static unsigned empty_distance_frame(void) {
    unsigned len = sizeof DIST_PRE;
    memcpy(frame, DIST_PRE, sizeof DIST_PRE);
    put_u32(frame + len, 4); len += 4;
    frame[len++] = 0xFE; frame[len++] = 0xFF; frame[len++] = 0; frame[len++] = 0;
    memcpy(frame + len, DIST_SUF, sizeof DIST_SUF); len += sizeof DIST_SUF;
    put_u32(frame + 9, len - 17);
    return len;
}

/* The bar picture. Pixels are A,B,G,R (step 7): white outline and ticks, orange fill, clear inside. */
#define PX_CLEAR  0x00000000u
#define PX_WHITE  0xFFFFFFFFu
#define PX_ORANGE 0xFF0066FFu

static uint32_t icon_px[ICON_W * ICON_H];

static void draw_bar(unsigned cols) {
    unsigned x, y, i = 0;
    for (y = 0; y < ICON_H; ++y)
        for (x = 0; x < ICON_W; ++x) {
            int edge = x < ICON_EDGE || x >= ICON_W - ICON_EDGE || y < ICON_EDGE || y >= ICON_H - ICON_EDGE;
            int tick = y >= ICON_H - ICON_EDGE - ICON_H / 4 &&
                       ((x >= 12 && x <= 14) || (x >= 25 && x <= 27) || (x >= 38 && x <= 40));
            icon_px[i++] = edge || tick ? PX_WHITE : x - ICON_EDGE < cols ? PX_ORANGE : PX_CLEAR;
        }
}

/* RLE32 as the factory nav sends it: ctrl & 0x80 -> the next pixel repeated ctrl & 0x7F times,
 * otherwise ctrl literal pixels follow. */
static unsigned rle32(unsigned char *out) {
    unsigned i = 0, n = ICON_W * ICON_H, len = 0;
    while (i < n) {
        unsigned j = i;
        while (j < n && j - i < 127 && icon_px[j] == icon_px[i]) ++j;
        if (j - i >= 2) {
            out[len++] = (unsigned char)(0x80 | (j - i));
            put_u32(out + len, icon_px[i]); len += 4;
            i = j;
            continue;
        }
        j = i;
        while (j < n && j - i < 127 && !(j + 1 < n && icon_px[j] == icon_px[j + 1])) ++j;
        if (j == i) j = i + 1;
        out[len++] = (unsigned char)(j - i);
        for (; i < j; ++i) { put_u32(out + len, icon_px[i]); len += 4; }
    }
    return len;
}

/* bitmap resource 01000000: id|01|u16 w|u16 h|u32 len|RLE32 */
static unsigned icon_frame(unsigned cols) {
    static const unsigned char head[] = {0x00, 0x00, 0x00, 0xc8, 0x01, 0x00, 0x00, 0x00, 0x01};
    unsigned data, len;
    draw_bar(cols);
    memcpy(frame, head, sizeof head);
    memcpy(frame + 13, "\x00\x00\x00\x02", 4);
    memcpy(frame + 17, "\x01\x00\x00\x00\x01", 5);
    put_u16(frame + 22, ICON_W); put_u16(frame + 24, ICON_H);
    data = rle32(frame + 30);
    put_u32(frame + 26, data);
    len = 30 + data;
    put_u32(frame + 9, len - 17);
    return len;
}

struct step { unsigned method; const unsigned char *body; unsigned len; };

static int send_steps(const struct step *s, unsigned count) {
    unsigned i;
    for (i = 0; i < count; ++i) {
        int rc = kombi_send(s[i].method, s[i].body, s[i].len);
        if (rc) {
            if (rc > 0) printf("The cluster refused frame %u (method 0x%04X): type 0x%02X code 0x%02X\n",
                               i + 1, s[i].method, (unsigned)rc >> 8, (unsigned)rc & 0xFF);
            return rc;
        }
    }
    return 0;
}

static void factory_clear(void) {
    unsigned i;
    for (i = 0; i < sizeof CANCEL_LEN / sizeof CANCEL_LEN[0]; ++i)
        if (kombi_send(CANCEL_METHOD[i], CANCEL_BODY[i], CANCEL_LEN[i]) < 0) break;
}

/* Our own start (step 12, ric-min.py): text, plain road, empty distance, one bare turn, one-entry
 * list, commit, guidance state. Variant 2 first sends the idle state and destination info the
 * factory nav sends before a route. */
static int hud_start(int variant) {
    unsigned char text[128], dist[96];
    unsigned text_len = text_frame("BOOST"), dist_len;
    struct step s[9];
    unsigned n = 0;
    memcpy(text, frame, text_len);
    dist_len = empty_distance_frame();
    memcpy(dist, frame, dist_len);
    if (variant == 2) {
        s[n].method = 0x0009; s[n].body = STATE_IDLE; s[n++].len = sizeof STATE_IDLE;
        s[n].method = 0x0009; s[n].body = DEST_INFO; s[n++].len = sizeof DEST_INFO;
    }
    s[n].method = 0x0003; s[n].body = text; s[n++].len = text_len;
    s[n].method = 0x000E; s[n].body = ROAD_PLAIN; s[n++].len = sizeof ROAD_PLAIN;
    s[n].method = 0x000A; s[n].body = dist; s[n++].len = dist_len;
    s[n].method = 0x000A; s[n].body = TURN0; s[n++].len = sizeof TURN0;
    s[n].method = 0x000C; s[n].body = LIST1; s[n++].len = sizeof LIST1;
    s[n].method = 0x0004; s[n].body = COMMIT; s[n++].len = sizeof COMMIT;
    s[n].method = 0x0009; s[n].body = STATE_ON; s[n++].len = sizeof STATE_ON;
    return send_steps(s, n);
}

/* Recipe A (step 4) plus the icon (step 7): new picture and text in the same resource ids, then
 * re-send the road, turn and list that use them, commit and guidance state. */
static int hud_show(const char *line, unsigned cols) {
    static unsigned char icon[8192];
    unsigned char text[128];
    unsigned icon_len = icon_frame(cols), text_len;
    struct step s[8];
    memcpy(icon, frame, icon_len);
    text_len = text_frame(line);
    memcpy(text, frame, text_len);
    s[0].method = 0x0002; s[0].body = CHECK_ICON; s[0].len = sizeof CHECK_ICON;
    s[1].method = 0x0003; s[1].body = icon;       s[1].len = icon_len;
    s[2].method = 0x0003; s[2].body = text;       s[2].len = text_len;
    s[3].method = 0x000E; s[3].body = ROAD_ICON;  s[3].len = sizeof ROAD_ICON;
    s[4].method = 0x000A; s[4].body = TURN0;      s[4].len = sizeof TURN0;
    s[5].method = 0x000C; s[5].body = LIST1;      s[5].len = sizeof LIST1;
    s[6].method = 0x0004; s[6].body = COMMIT;     s[6].len = sizeof COMMIT;
    s[7].method = 0x0009; s[7].body = STATE_ON;   s[7].len = sizeof STATE_ON;
    return send_steps(s, 8);
}

/* ---------- gateway (HSFZ, read-only UDS 0x22) ---------- */

static const char *gw_ip;
static unsigned gw_port = GATEWAY_PORT;
static int gw = -1, gw_lock = -1;
static uint64_t gw_retry_at, gw_since;
static int gw_said_down;

static void gw_release(unsigned wait) {
    release_gateway(&gw, &gw_lock);
    gw_retry_at = now_ms() + wait;
}

/* UDS 0x22 ReadDataByIdentifier to one ECU. Returns the number of data bytes (up to max) copied to out,
 * 0 for a refusal or odd answer (*nrc holds the negative response code, or 0), -1 if the link failed. */
static int read_did_raw(unsigned ecu, unsigned did, unsigned char *out, unsigned max, unsigned *nrc) {
    unsigned char req[11] = {0, 0, 0, 5, 0, 1, 0xF4, 0, 0x22, 0, 0};
    unsigned char hdr[6], body[512];
    uint64_t deadline = now_ms() + GW_TIMEOUT_MS;
    req[7] = (unsigned char)ecu;
    req[9] = (unsigned char)(did >> 8); req[10] = (unsigned char)did;
    *nrc = 0;
    if (!transfer(gw, req, sizeof req, 1, deadline)) return -1;
    for (;;) {
        uint32_t len;
        unsigned ctrl, n;
        if (!transfer(gw, hdr, 6, 0, deadline)) return -1;
        len = get_u32(hdr);
        ctrl = ((unsigned)hdr[4] << 8) | hdr[5];
        if (len > sizeof body || !transfer(gw, body, len, 0, deadline)) return -1;
        if (ctrl == 0x12) {                                  /* alive check */
            unsigned char alive[8] = {0, 0, 0, 2, 0, 0x13, 0, 0xF4};
            if (!transfer(gw, alive, 8, 1, deadline)) return -1;
            continue;
        }
        if (ctrl != 1 || len < 3 || body[0] != ecu || body[1] != 0xF4) continue;   /* ack echo */
        if (body[2] == 0x7F && len >= 5) {
            if (body[4] == 0x78) continue;                   /* response pending */
            *nrc = body[4];
            return 0;
        }
        if (len > 5 && body[2] == 0x62 && body[3] == req[9] && body[4] == req[10]) {
            n = len - 5;
            if (n > max) n = max;
            memcpy(out, body + 5, n);
            return (int)n;
        }
    }
}

/* DME (0x12) 16-bit value: 1 and the value, 0 for a refusal or odd answer, -1 if the link failed. */
static int read_did(unsigned did, unsigned *value) {
    unsigned char b[2];
    unsigned nrc;
    int n = read_did_raw(0x12, did, b, 2, &nrc);
    if (n < 0) return -1;
    if (n != 2) return 0;
    *value = ((unsigned)b[0] << 8) | b[1];
    return 1;
}

static int gw_ready(void) {
    uint64_t now = now_ms();
    if (gw >= 0) return 1;
    if (!gw_ip || now < gw_retry_at) return 0;
    gw_lock = reserve_gateway();
    if (gw_lock < 0) { gw_retry_at = now + DIAGNOSTIC_RETRY_MS; return 0; }   /* another app has it */
    gw = connect_tcp(gw_ip, gw_port);
    if (gw < 0) {
        if (!gw_said_down) printf("Gateway %s:%u not reachable yet, retrying every 3 s\n", gw_ip, gw_port);
        gw_said_down = 1;
        gw_release(GW_RETRY_MS);
        return 0;
    }
    if (gw_said_down) printf("Gateway connected\n");
    gw_said_down = 0;
    gw_since = now_ms();
    return 1;
}

/* Hand the gateway over when another app is waiting for it, after holding it for a short while. */
static void gw_maybe_yield(void) {
    char waiting;
    if (gw < 0 || now_ms() - gw_since < DIAGNOSTIC_MIN_LEASE_MS) return;
    if (recv(gw_lock, &waiting, 1, 0) > 0) gw_release(DIAGNOSTIC_YIELD_MS);
}

static unsigned ambient_raw;        /* DID 4201, kPa * 256 */
static uint64_t ambient_at, ambient_due;

/* Boost in tenths of a psi (gauge pressure), or 0 with *ok = 0. */
static int live_boost(int *ok) {
    unsigned raw;
    int r;
    *ok = 0;
    if (!gw_ready()) return 0;
    if (now_ms() >= ambient_due) {
        ambient_due = now_ms() + AMBIENT_EVERY_MS;
        r = read_did(0x4201, &raw);
        if (r < 0) { printf("Gateway read failed, reconnecting in 3 s\n"); gw_release(GW_RETRY_MS); return 0; }
        if (r && raw >= 40u * 256 && raw <= 115u * 256) { ambient_raw = raw; ambient_at = now_ms(); }
    }
    r = read_did(0x4205, &raw);                               /* kPa * 128, absolute */
    if (r < 0) { printf("Gateway read failed, reconnecting in 3 s\n"); gw_release(GW_RETRY_MS); return 0; }
    gw_maybe_yield();
    if (!r || raw < 20u * 128 || raw > 450u * 128 || !ambient_at || now_ms() - ambient_at > AMBIENT_FRESH_MS)
        return 0;
    {
        /* (raw/128 - ambient/256) kPa * 0.145037738 psi/kPa * 10, as d * 95052 >> 24 with d in kPa/256 */
        int64_t d = (int64_t)raw * 2 - (int64_t)ambient_raw;
        int64_t p = d < 0 ? -d : d;
        int tenths = (int)((p * 95052 + (1 << 23)) >> 24);
        *ok = 1;
        return d < 0 ? -tenths : tenths;
    }
}

/* Made-up boost for --demo: a ramp from -5.0 to 18.0 psi and back every 4.6 s. */
static int demo_boost(uint64_t t) {
    unsigned phase = (unsigned)(t & 0xFFFFFF);
    int v;
    while (phase >= 4600) phase -= 4600;
    v = phase < 2300 ? (int)phase : (int)(4600 - phase);      /* 0..2300 */
    return v / 10 - 50;                                       /* constant divisor */
}

/* --watch-mode: no HUD output. Reads the drive mode from the body controller (BDC, 0x40), DID D611,
 * which strulock/m550-analyzer documents for the G30 (1 SPORT, 2 SPORT+ ("Sport Expert"), 3 SPORT Individual,
 * 4 COMFORT, ...). Prints each change, so the values can be checked on this car before anything depends on them. */
/* BDC DID D611 values. 1, 2, 3, 4, 7, 9 and 10 were seen on Simon's G01 on 2026-11-18 while he switched
 * modes; 5, 6 and 8 are from the G30 notes only. */
static const char *mode_name(unsigned v) {
    switch (v) {
    case 0: return "starting up";
    case 1: return "SPORT";
    case 2: return "SPORT+";
    case 3: return "SPORT Individual";
    case 4: return "COMFORT";
    case 5: return "COMFORT+?";
    case 6: return "COMFORT Individual?";
    case 7: return "ECO PRO";
    case 8: return "ECO PRO+?";
    case 9: return "ECO PRO Individual";
    case 10: return "ADAPTIVE";
    case 63: return "no value";
    default: return "?";
    }
}

static int is_sport(unsigned v) { return v == 1 || v == 2 || v == 3; }

static int drive_mode = -1;             /* last good D611 value, -1 = not known yet */
static uint64_t mode_due;

/* --auto: read the drive mode every 500 ms. Keeps the last good value through refusals and the
 * start-up / no-value codes. Returns -1 if the gateway link failed. */
static int poll_drive_mode(void) {
    unsigned char b[4];
    unsigned nrc;
    int n;
    if (now_ms() < mode_due || !gw_ready()) return 0;
    mode_due = now_ms() + 500;
    n = read_did_raw(0x40, 0xD611, b, sizeof b, &nrc);
    if (n < 0) { printf("Gateway read failed, reconnecting in 3 s\n"); gw_release(GW_RETRY_MS); return -1; }
    if (n >= 1 && b[0] != 0 && b[0] != 63 && (int)b[0] != drive_mode) {
        drive_mode = b[0];
        printf("Drive mode: %s (%u)\n", mode_name(b[0]), (unsigned)b[0]);
    }
    return 0;
}

static int watch_mode(void) {
    unsigned char b[8];
    char shown[64] = "", now[64];
    unsigned nrc, i;
    printf("Watching the drive mode (BDC 0x40, DID D611), read-only. Switch modes now; Ctrl+C to stop.\n");
    while (running && (!stop_at || now_ms() < stop_at)) {
        uint64_t t0 = now_ms();
        int n;
        if (!gw_ready()) { pause_ms(300); continue; }
        n = read_did_raw(0x40, 0xD611, b, sizeof b, &nrc);
        if (n < 0) { printf("Gateway read failed, reconnecting in 3 s\n"); gw_release(GW_RETRY_MS); continue; }
        gw_maybe_yield();
        if (n == 0 && nrc) snprintf(now, sizeof now, "refused, code 0x%02X", nrc);
        else if (n == 0) snprintf(now, sizeof now, "no answer");
        else {
            int k = snprintf(now, sizeof now, "raw");
            for (i = 0; i < (unsigned)n && k < (int)sizeof now - 4; ++i) k += snprintf(now + k, sizeof now - k, " %02X", b[i]);
            snprintf(now + k, sizeof now - k, "  %s", mode_name(b[0]));
        }
        if (strcmp(now, shown)) { printf("drive mode: %s\n", now); strcpy(shown, now); }
        {
            uint64_t spent = now_ms() - t0;
            if (spent < 500) pause_ms((unsigned)(500 - spent));
        }
    }
    release_gateway(&gw, &gw_lock);
    printf("Done.\n");
    return 0;
}

/* ---------- setup ---------- */

static int valid_ip(const char *s) {
    struct in_addr a;
    return inet_pton(AF_INET, s, &a) == 1;
}

static char gw_ip_buf[32];

static int read_ip_file(const char *path) {
    char line[64];
    size_t n;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(line, sizeof line, f)) { fclose(f); return 0; }
    fclose(f);
    n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
    if (n >= sizeof gw_ip_buf || !valid_ip(line)) return 0;
    memcpy(gw_ip_buf, line, n + 1);
    return 1;
}

/* gateway-ip.txt next to this program, else the first one in an installed app's folder */
static const char *find_gateway_ip(const char *argv0) {
    char path[256];
    const char *slash = strrchr(argv0, '/');
    DIR *dir;
    struct dirent *e;
    if (slash && (size_t)(slash - argv0) + sizeof "/gateway-ip.txt" < sizeof path) {
        memcpy(path, argv0, (size_t)(slash - argv0));
        strcpy(path + (slash - argv0), "/gateway-ip.txt");
        if (read_ip_file(path)) return gw_ip_buf;
    }
    dir = opendir(PLUGINS_DIR);
    if (!dir) return 0;
    while ((e = readdir(dir)) != 0) {
        if (e->d_name[0] == '.' || strlen(e->d_name) > 100) continue;
        snprintf(path, sizeof path, "%s/%s/gateway-ip.txt", PLUGINS_DIR, e->d_name);
        if (read_ip_file(path)) { closedir(dir); return gw_ip_buf; }
    }
    closedir(dir);
    return 0;
}

static unsigned number(const char *s, unsigned lo, unsigned hi) {
    char *end;
    unsigned long n = strtoul(s, &end, 10);
    if (!*s || *end || n < lo || n > hi) { fprintf(stderr, "Invalid number: %s\n", s); exit(2); }
    return (unsigned)n;
}

static void usage(void) {
    fputs("usage: hud-boost-qnx [--demo | --watch-mode | --auto] [--seconds N] [--gateway IP] [--start 1|2] [--max PSI]\n"
          "  --demo        made-up boost, no gateway reads\n"
          "  --seconds N   stop after N seconds (default 120, 0 = until Ctrl+C)\n"
          "  --gateway IP  diagnostic gateway (default: gateway-ip.txt next to this program, or in\n"
          "                an app folder under " PLUGINS_DIR ")\n"
          "  --start 1|2   which of the two step-12 start sequences to use (default 2)\n"
          "  --max PSI     boost that fills the bar (default 20)\n"
          "  --watch-mode  no HUD: print the drive mode from the body controller (BDC DID D611) as it changes\n"
          "  --auto        show boost only in SPORT, SPORT+ and SPORT Individual; clear the HUD in other\n"
          "                modes (runs until stopped unless --seconds is given)\n"
          "Run only parked with no factory nav route. Ctrl+C sends the factory clear and stops.\n", stderr);
    exit(2);
}

int main(int argc, char **argv) {
    const char *kombi_ip = KOMBI_IP;
    unsigned kombi_port = KOMBI_PORT, seconds = 120, max_tenths = 200;
    int demo = 0, variant = 2, i, shown_cols = -1, watch = 0, auto_mode = 0, seconds_set = 0;
    char shown[48] = "";
    int last_tenths = 0;
    uint64_t start, end, last_sent = 0, last_print = 0, last_ok_at = 0;
    struct sigaction sa;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--demo")) demo = 1;
        else if (!strcmp(argv[i], "--watch-mode")) watch = 1;
        else if (!strcmp(argv[i], "--auto")) auto_mode = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) { seconds = number(argv[++i], 0, 86400); seconds_set = 1; }
        else if (!strcmp(argv[i], "--gateway") && i + 1 < argc) gw_ip = argv[++i];
        else if (!strcmp(argv[i], "--gateway-port") && i + 1 < argc) gw_port = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--kombi") && i + 1 < argc) kombi_ip = argv[++i];
        else if (!strcmp(argv[i], "--kombi-port") && i + 1 < argc) kombi_port = number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--start") && i + 1 < argc) variant = (int)number(argv[++i], 1, 2);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) max_tenths = number(argv[++i], 5, 60) * 10;
        else usage();
    }
    if (auto_mode && watch) usage();
    if (auto_mode && !seconds_set) seconds = 0;          /* --auto runs until stopped */
    if (gw_ip && !valid_ip(gw_ip)) { fprintf(stderr, "Not an IPv4 address: %s\n", gw_ip); return 2; }
    if (!valid_ip(kombi_ip)) { fprintf(stderr, "Not an IPv4 address: %s\n", kombi_ip); return 2; }
    if ((!demo || auto_mode) && !gw_ip) gw_ip = find_gateway_ip(argv[0]);
    if ((!demo || auto_mode) && !gw_ip) {
        fprintf(stderr, "No gateway address: pass --gateway IP, or put gateway-ip.txt next to this program.\n");
        return 2;
    }
    setvbuf(stdout, 0, _IOLBF, 0);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = stop;                  /* no SA_RESTART: poll() wakes up on Ctrl+C */
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGHUP, &sa, 0);
    signal(SIGPIPE, SIG_IGN);

    if (watch) {
        if (demo) usage();
        stop_at = seconds ? now_ms() + (uint64_t)seconds * 1000u : 0;
        printf("Gateway %s:%u\n", gw_ip, gw_port);
        return watch_mode();
    }
    printf("HUD boost: %s, cluster %s:%u, start %d, bar full at %u psi, %s\n",
           demo ? "demo values" : "live", kombi_ip, kombi_port, variant, max_tenths / 10,
           seconds ? "time limit set" : "until Ctrl+C");
    if (gw_ip) printf("Gateway %s:%u\n", gw_ip, gw_port);
    if (auto_mode) printf("Automatic: boost on the HUD only in SPORT, SPORT+ and SPORT Individual.\n");
    start = now_ms();
    end = stop_at = seconds ? start + (uint64_t)seconds * 1000u : 0;

    while (running && (!end || now_ms() < end)) {
        uint64_t t0 = now_ms();
        int ok, tenths, rc;
        unsigned cols = 0;
        char line[48], num[12];

        if (auto_mode) {
            poll_drive_mode();
            if (drive_mode < 0 || !is_sport((unsigned)drive_mode)) {
                if (kombi >= 0) {
                    factory_clear();             /* left the sport modes: hand the HUD back */
                    close(kombi);
                    kombi = -1;
                    printf("HUD cleared (not a sport mode).\n");
                }
                pause_ms(TICK_MS);
                continue;
            }
        }
        if (kombi < 0) {
            kombi = connect_tcp(kombi_ip, kombi_port);
            if (kombi < 0) {
                printf("Cluster %s:%u not reachable, retrying in 3 s\n", kombi_ip, kombi_port);
                pause_ms(KOMBI_RETRY_MS);
                continue;
            }
            factory_clear();                  /* start from a clean HUD, as ric-min.py does */
            pause_ms(1000);
            rc = hud_start(variant);
            if (rc) {
                factory_clear();
                close(kombi);
                kombi = -1;
                if (rc > 0) { printf("Stopping: the start was refused.\n"); return 1; }
                printf("Lost the cluster during the start, retrying in 3 s\n");
                pause_ms(KOMBI_RETRY_MS);
                continue;
            }
            printf(auto_mode ? "HUD started (sport mode).\n" : "HUD started. Ctrl+C to stop.\n");
            shown_cols = -1;
            shown[0] = 0;
        }

        tenths = demo ? (ok = 1, demo_boost(t0 - start)) : live_boost(&ok);
        if (ok) { last_tenths = tenths; last_ok_at = t0; }
        else if (last_ok_at && t0 - last_ok_at <= HOLD_MS) { tenths = last_tenths; ok = 1; }  /* gateway lent out */
        if (ok) {
            int v = tenths < 0 ? 0 : tenths;
            while (cols < BAR_COLS && (int)((cols + 1) * max_tenths) <= v * BAR_COLS) ++cols;
            tenths_text(tenths, num);
            snprintf(line, sizeof line, "BOOST %s psi", num);
        } else {
            snprintf(line, sizeof line, "BOOST -- psi");
        }

        if ((int)cols != shown_cols || strcmp(line, shown) || t0 - last_sent >= REFRESH_MS) {
            rc = hud_show(line, cols);
            if (rc > 0) { printf("Stopping: an update was refused.\n"); break; }
            if (rc < 0) {
                printf("Lost the cluster connection, reconnecting in 3 s\n");
                close(kombi);
                kombi = -1;
                pause_ms(KOMBI_RETRY_MS);
                continue;
            }
            shown_cols = (int)cols;
            strcpy(shown, line);
            last_sent = t0;
        }
        if (t0 - last_print >= 1000) {
            printf("  %-18s bar %2u/%u\n", line, cols, (unsigned)BAR_COLS);
            last_print = t0;
        }
        {
            uint64_t spent = now_ms() - t0;
            if (spent < TICK_MS) pause_ms((unsigned)(TICK_MS - spent));
        }
    }

    if (kombi >= 0) {
        printf("Sending the factory clear...\n");
        factory_clear();
        close(kombi);
    }
    release_gateway(&gw, &gw_lock);
    printf("Done.\n");
    return 0;
}
