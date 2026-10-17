/* lcdd: tiny DRM-native text console for the GL-BE14000 SPI LCD (panel-mipi-dbi).
 * Reads a subset of Linux-console ANSI text from a FIFO, keeps an 8x8-font cell grid,
 * draws into one dumb buffer and flushes damage with DRM_IOCTL_MODE_DIRTYFB.
 * No libdrm; raw ioctls only. GPL-2.0 (font data from the Linux kernel, lib/fonts/font_8x8.c).
 * usage: lcdd [-d /dev/dri/card0] [-f /tmp/lcd.fifo] [-r 0|90|180|270] [-q quiet_ms] [-m max_ms]
 * Flushes are coalesced: after input, wait up to quiet_ms for more before flushing, never longer than max_ms.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <time.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include "font8x8.h"

static int drmfd, PW, PH, W, H, COLS, ROWS, ROT, BPP, QUIET_MS = 50, MAX_MS = 150;
static uint32_t fb_id, pitch;
static uint8_t *fbmem; static size_t fbsize;
static int dx0, dy0, dx1, dy1, dirty;
static unsigned char *cch, *cat;         /* cell char, cell attr (fg | bg<<4) */
static int cur_r, cur_c, bold, fg = 7, bg = 0;
static const uint32_t pal[16] = { 0x000000,0xaa0000,0x00aa00,0xaa5500,0x0000aa,0xaa00aa,0x00aaaa,0xaaaaaa,
                                  0x555555,0xff5555,0x55ff55,0xffff55,0x5555ff,0xff55ff,0x55ffff,0xffffff };

static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000; }
static int xioctl(unsigned long req, void *arg) { int r; do r = ioctl(drmfd, req, arg); while (r == -1 && errno == EINTR); return r; }
static void die(const char *m) { perror(m); exit(1); }

static inline void put_px(int x, int y, uint32_t rgb) {       /* x,y logical (after rotation) */
    int px, py;
    switch (ROT) { case 90: px = PW - 1 - y; py = x; break; case 180: px = PW - 1 - x; py = PH - 1 - y; break;
                   case 270: px = y; py = PH - 1 - x; break; default: px = x; py = y; }
    if (BPP == 16) *(uint16_t *)(fbmem + py * pitch + px * 2) = ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x1f);
    else *(uint32_t *)(fbmem + py * pitch + px * 4) = rgb;
    if (!dirty) { dx0 = dx1 = px; dy0 = dy1 = py; dirty = 1; }
    else { if (px < dx0) dx0 = px; if (px > dx1) dx1 = px; if (py < dy0) dy0 = py; if (py > dy1) dy1 = py; }
}
static void draw_cell(int r, int c) {
    const uint8_t *g = font8x8[cch[r * COLS + c]]; int a = cat[r * COLS + c];
    uint32_t f = pal[a & 15], b = pal[(a >> 4) & 15];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) put_px(c * 8 + x, r * 8 + y, (g[y] & (0x80 >> x)) ? f : b);
}
static int attr(void) { int f = fg; if (bold && f < 8) f += 8; return f | (bg << 4); }
static void set_cell(int r, int c, int ch, int a) {
    if (cch[r * COLS + c] == ch && cat[r * COLS + c] == a) return;
    cch[r * COLS + c] = ch; cat[r * COLS + c] = a; draw_cell(r, c);
}
static void flush(void) {
    if (!dirty) return;
    struct drm_clip_rect clip = { dx0, dy0, dx1 + 1, dy1 + 1 };
    struct drm_mode_fb_dirty_cmd d = { .fb_id = fb_id, .flags = 0, .num_clips = 1, .clips_ptr = (uintptr_t)&clip };
    xioctl(DRM_IOCTL_MODE_DIRTYFB, &d); dirty = 0;
}
static void erase(int r0, int c0, int r1, int c1) {        /* inclusive range in reading order */
    int a = bg << 4 | 7;
    for (int r = r0; r <= r1; r++) for (int c = (r == r0 ? c0 : 0); c <= (r == r1 ? c1 : COLS - 1); c++) set_cell(r, c, ' ', a);
}
static void scroll(void) {
    memmove(cch, cch + COLS, (ROWS - 1) * COLS); memmove(cat, cat + COLS, (ROWS - 1) * COLS);
    memset(cch + (ROWS - 1) * COLS, ' ', COLS); memset(cat + (ROWS - 1) * COLS, bg << 4 | 7, COLS);
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) draw_cell(r, c);
}
static void newline(void) { cur_c = 0; if (++cur_r >= ROWS) { cur_r = ROWS - 1; scroll(); } }
static void putch(int ch) {
    if (cur_c >= COLS) newline();
    set_cell(cur_r, cur_c, ch, attr()); cur_c++;
}
static void csi(int *p, int n, int priv, int final) {
    switch (final) {
    case 'H': case 'f': { int r = (n > 0 && p[0] > 0 ? p[0] : 1) - 1, c = (n > 1 && p[1] > 0 ? p[1] : 1) - 1;
        cur_r = r < ROWS ? r : ROWS - 1; cur_c = c < COLS ? c : COLS - 1; break; }
    case 'J': { int m = n > 0 ? p[0] : 0;
        if (m == 2 || m == 3) erase(0, 0, ROWS - 1, COLS - 1);
        else if (m == 1) erase(0, 0, cur_r, cur_c < COLS ? cur_c : COLS - 1);
        else erase(cur_r, cur_c < COLS ? cur_c : COLS - 1, ROWS - 1, COLS - 1);
        break; }
    case 'K': { int m = n > 0 ? p[0] : 0;
        if (m == 2) erase(cur_r, 0, cur_r, COLS - 1); else if (m == 1) erase(cur_r, 0, cur_r, cur_c < COLS ? cur_c : COLS - 1);
        else erase(cur_r, cur_c < COLS ? cur_c : COLS - 1, cur_r, COLS - 1);
        break; }
    case 'A': cur_r -= n > 0 && p[0] > 0 ? p[0] : 1; if (cur_r < 0) cur_r = 0; break;
    case 'B': cur_r += n > 0 && p[0] > 0 ? p[0] : 1; if (cur_r >= ROWS) cur_r = ROWS - 1; break;
    case 'C': cur_c += n > 0 && p[0] > 0 ? p[0] : 1; if (cur_c >= COLS) cur_c = COLS - 1; break;
    case 'D': cur_c -= n > 0 && p[0] > 0 ? p[0] : 1; if (cur_c < 0) cur_c = 0; break;
    case 'm': if (n == 0) { bold = 0; fg = 7; bg = 0; }
        for (int i = 0; i < n; i++) { int v = p[i];
            if (v == 0) { bold = 0; fg = 7; bg = 0; } else if (v == 1) bold = 1; else if (v == 22) bold = 0;
            else if (v >= 30 && v <= 37) fg = v - 30; else if (v == 39) fg = 7;
            else if (v >= 40 && v <= 47) bg = v - 40; else if (v == 49) bg = 0;
            else if (v >= 90 && v <= 97) fg = v - 90 + 8; else if (v >= 100 && v <= 107) bg = v - 100 + 8; }
        break;
    default: break;   /* ?25l, 9;0], etc. */
    }
    (void)priv;
}
static void process(const unsigned char *buf, int len) {
    static int st = 0, np, priv, utf_n; static int p[8]; static uint32_t utf_cp;
    for (int i = 0; i < len; i++) { unsigned ch = buf[i];
        if (st == 1) { if (ch == '[') { st = 2; np = 0; priv = 0; memset(p, 0, sizeof p); } else st = 0; continue; }
        if (st == 2) {
            if (ch >= '0' && ch <= '9') { if (np == 0) np = 1; p[np - 1] = p[np - 1] * 10 + (ch - '0'); }
            else if (ch == ';') { if (np < 8) np++; if (np == 1) np = 2; }
            else if (ch == '?') priv = 1;
            else if (ch >= 0x40 && ch <= 0x7e) { csi(p, np, priv, ch); st = 0; }
            else if (ch == 0x1b) st = 1;
            continue;
        }
        if (utf_n) { if ((ch & 0xc0) == 0x80) { utf_cp = utf_cp << 6 | (ch & 0x3f); if (--utf_n == 0) putch('?'); continue; } utf_n = 0; }
        switch (ch) {
        case 0x1b: st = 1; break;
        case '\n': newline(); break;
        case '\r': cur_c = 0; break;
        case '\t': do putch(' '); while (cur_c % 8 && cur_c < COLS); break;
        case '\b': if (cur_c > 0) cur_c--; break;
        case 0x07: case 0x0e: case 0x0f: break;
        default:
            if (ch >= 0xc0) { utf_n = ch >= 0xf0 ? 3 : ch >= 0xe0 ? 2 : 1; utf_cp = ch; }
            else if (ch >= 0x20 && ch < 0x7f) putch(ch);
        }
    }
}

static void setup_drm(const char *card) {
    drmfd = open(card, O_RDWR | O_CLOEXEC); if (drmfd < 0) die(card);
    struct drm_mode_card_res res = {0};
    if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res)) die("GETRESOURCES");
    uint32_t conns[res.count_connectors ? res.count_connectors : 1], crtcs[res.count_crtcs ? res.count_crtcs : 1];
    res.connector_id_ptr = (uintptr_t)conns; res.crtc_id_ptr = (uintptr_t)crtcs; res.count_encoders = 0; res.count_fbs = 0;
    if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res)) die("GETRESOURCES2");
    uint32_t conn_id = 0, crtc_id = 0; struct drm_mode_modeinfo mode;
    for (unsigned i = 0; i < res.count_connectors && !conn_id; i++) {
        struct drm_mode_get_connector c = { .connector_id = conns[i] };
        if (xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
        if (c.connection != 1 || !c.count_modes) continue;
        struct drm_mode_modeinfo modes[c.count_modes];
        c.modes_ptr = (uintptr_t)modes; c.count_props = 0; c.count_encoders = 0;
        if (xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
        conn_id = conns[i]; mode = modes[0];
        if (c.encoder_id) { struct drm_mode_get_encoder e = { .encoder_id = c.encoder_id };
            if (!xioctl(DRM_IOCTL_MODE_GETENCODER, &e)) { crtc_id = e.crtc_id;
                if (!crtc_id) for (unsigned k = 0; k < res.count_crtcs; k++) if (e.possible_crtcs & (1u << k)) { crtc_id = crtcs[k]; break; } } }
        if (!crtc_id && res.count_crtcs) crtc_id = crtcs[0];
    }
    if (!conn_id || !crtc_id) { fprintf(stderr, "no connected connector/crtc\n"); exit(1); }
    PW = mode.hdisplay; PH = mode.vdisplay;
    struct drm_mode_create_dumb cd = { .width = PW, .height = PH, .bpp = 16 };
    uint32_t fmt = DRM_FORMAT_RGB565; BPP = 16;
    if (xioctl(DRM_IOCTL_MODE_CREATE_DUMB, &cd)) die("CREATE_DUMB");
    struct drm_mode_fb_cmd2 f = { .width = PW, .height = PH, .pixel_format = fmt };
    f.handles[0] = cd.handle; f.pitches[0] = cd.pitch;
    if (xioctl(DRM_IOCTL_MODE_ADDFB2, &f)) {               /* fall back to XRGB8888 */
        struct drm_mode_destroy_dumb dd = { .handle = cd.handle }; xioctl(DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        memset(&cd, 0, sizeof cd); cd.width = PW; cd.height = PH; cd.bpp = 32; BPP = 32;
        if (xioctl(DRM_IOCTL_MODE_CREATE_DUMB, &cd)) die("CREATE_DUMB32");
        memset(&f, 0, sizeof f); f.width = PW; f.height = PH; f.pixel_format = DRM_FORMAT_XRGB8888; f.handles[0] = cd.handle; f.pitches[0] = cd.pitch;
        if (xioctl(DRM_IOCTL_MODE_ADDFB2, &f)) die("ADDFB2");
    }
    fb_id = f.fb_id; pitch = cd.pitch; fbsize = cd.size;
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (xioctl(DRM_IOCTL_MODE_MAP_DUMB, &md)) die("MAP_DUMB");
    fbmem = mmap(0, fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, drmfd, md.offset); if (fbmem == MAP_FAILED) die("mmap");
    memset(fbmem, 0, fbsize);
    struct drm_mode_crtc crtc = { .crtc_id = crtc_id, .fb_id = fb_id, .set_connectors_ptr = (uintptr_t)&conn_id, .count_connectors = 1, .mode = mode, .mode_valid = 1 };
    if (xioctl(DRM_IOCTL_MODE_SETCRTC, &crtc)) die("SETCRTC");
    fprintf(stderr, "lcdd: connector %u crtc %u %dx%d %s bpp%d rot%d grid %dx%d\n", conn_id, crtc_id, PW, PH, mode.name, BPP, ROT, COLS, ROWS);
}

int main(int argc, char **argv) {
    const char *card = "/dev/dri/card0", *fifo = "/tmp/lcd.fifo"; int o;
    while ((o = getopt(argc, argv, "d:f:r:q:m:")) != -1) { if (o == 'd') card = optarg; else if (o == 'f') fifo = optarg; else if (o == 'r') ROT = atoi(optarg); else if (o == 'q') QUIET_MS = atoi(optarg); else if (o == 'm') MAX_MS = atoi(optarg); else { fprintf(stderr, "usage: lcdd [-d card] [-f fifo] [-r 0|90|180|270] [-q quiet_ms] [-m max_ms]\n"); return 2; } }
    signal(SIGPIPE, SIG_IGN);
    setup_drm(card);
    if (ROT == 90 || ROT == 270) { W = PH; H = PW; } else { W = PW; H = PH; }
    COLS = W / 8; ROWS = H / 8;
    cch = malloc(COLS * ROWS); cat = malloc(COLS * ROWS); memset(cch, ' ', COLS * ROWS); memset(cat, 7, COLS * ROWS);
    fprintf(stderr, "lcdd: grid %dx%d fifo %s\n", COLS, ROWS, fifo);
    erase(0, 0, ROWS - 1, COLS - 1); dirty = 1; dx0 = dy0 = 0; dx1 = PW - 1; dy1 = PH - 1; flush();
    if (mkfifo(fifo, 0622) && errno != EEXIST) die("mkfifo");
    int fd = open(fifo, O_RDWR); if (fd < 0) die("open fifo");   /* O_RDWR: never sees EOF between writers */
    unsigned char buf[4096]; int n; long first = 0;
    for (;;) {
        /* coalesce: a redraw arrives as several writes; push one frame when the burst goes quiet, or at the cap */
        int timeout = -1;
        if (dirty) { long el = now_ms() - first; if (el >= MAX_MS) { flush(); timeout = -1; }
                     else timeout = QUIET_MS < MAX_MS - el ? QUIET_MS : (int)(MAX_MS - el); }
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        int r = poll(&pf, 1, timeout);
        if (r < 0) { if (errno == EINTR) continue; die("poll"); }
        if (r == 0) { flush(); continue; }
        n = read(fd, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; die("read"); }
        if (n == 0) break;
        int was = dirty; process(buf, n);
        if (dirty && !was) first = now_ms();
    }
    return 0;
}
