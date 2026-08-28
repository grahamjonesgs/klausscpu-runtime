/*
 * vnc_c2.c — RFB 3.8 server on lwIP's RAW API (NO_SYS=1), for AMP core 2.
 *
 * The framebuffer is core 1's, in shared DDR; its geometry and the dirty
 * rect of each posted frame arrive through the amp_fb_desc_t descriptor
 * (amp/amp_proto.h).  Core 2 reads pixels through its uncached DDR window,
 * so tiles are first pulled into a small local buffer with 64-bit loads
 * (one DDR burst each) and encoded from there.
 *
 * Encoders (LUT conversion, Raw, Hextile with the adaptive Raw fallback)
 * are ported verbatim from zephyr-ws/klausscpu-zephyr/vnc/vnc_server.c —
 * the encoder was fuzzed there (vnc/tests/host).  What changes is the
 * transport: lwIP raw API with a chunked, flow-controlled update pump —
 * encode up to STAGE_SZ bytes, tcp_write(COPY) while tcp_sndbuf allows,
 * resume from the `sent` callback / main-loop poll.  Single client.
 *
 * Implemented subset: ProtocolVersion 3.8, security None, ServerInit
 * (native RGB565 LE), SetPixelFormat, SetEncodings (Hextile detected),
 * FramebufferUpdateRequest (incremental: served when core 1 posts a new
 * seq; non-incremental: served immediately), Key/Pointer/CutText parsed
 * and dropped.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "../amp/amp_proto.h"
#include "vnc_c2.h"

#define VNC_PORT      5900
#define DESKTOP_NAME  "KlaussCPU core2"
#define STAGE_SZ      4096
#define HT_TILE_MAX   (1 + 16 * 16 * 4)
#define HT_RETRY      32

#define HT_RAW      0x01
#define HT_BG       0x02
#define HT_FG       0x04
#define HT_SUBRECTS 0x08

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* ── pixel formats + LUT conversion (ported) ───────────────────────────── */
struct pixfmt {
    uint8_t  bpp, depth, big_endian, true_colour;
    uint16_t red_max, green_max, blue_max;
    uint8_t  red_shift, green_shift, blue_shift;
};
static const struct pixfmt native_fmt = {
    16, 16, 0, 1, 31, 63, 31, 11, 5, 0
};
static uint32_t lut_r[32], lut_g[64], lut_b[32];
static bool     fmt_native;

static void be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void parse_pixfmt(struct pixfmt *f, const uint8_t *p)
{
    f->bpp = p[0]; f->depth = p[1]; f->big_endian = p[2]; f->true_colour = p[3];
    f->red_max = rd16(p + 4); f->green_max = rd16(p + 6); f->blue_max = rd16(p + 8);
    f->red_shift = p[10]; f->green_shift = p[11]; f->blue_shift = p[12];
}

static void build_luts(const struct pixfmt *f)
{
    fmt_native = (f->bpp == 16 && !f->big_endian &&
                  f->red_max == 31 && f->green_max == 63 && f->blue_max == 31 &&
                  f->red_shift == 11 && f->green_shift == 5 && f->blue_shift == 0);
    for (int i = 0; i < 32; i++) {
        uint8_t c8 = (uint8_t)((i << 3) | (i >> 2));
        lut_r[i] = (((uint32_t)c8 * f->red_max + 127) / 255) << f->red_shift;
        lut_b[i] = (((uint32_t)c8 * f->blue_max + 127) / 255) << f->blue_shift;
    }
    for (int i = 0; i < 64; i++) {
        uint8_t c8 = (uint8_t)((i << 2) | (i >> 4));
        lut_g[i] = (((uint32_t)c8 * f->green_max + 127) / 255) << f->green_shift;
    }
}

static inline uint32_t convert_pixel(uint16_t px)
{
    return lut_r[(px >> 11) & 0x1F] | lut_g[(px >> 5) & 0x3F] | lut_b[px & 0x1F];
}

static void put_pixel_bytes(uint8_t *p, uint32_t v, int bytes, bool big_endian)
{
    if (big_endian) {
        for (int i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (8 * (bytes - 1 - i)));
    } else {
        for (int i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (8 * i));
    }
}

static inline uint8_t *emit_px(uint8_t *o, uint16_t px, int bytes,
                               const struct pixfmt *f)
{
    put_pixel_bytes(o, convert_pixel(px), bytes, f->big_endian);
    return o + bytes;
}

/* ── frame source: core 1's framebuffer via the shared descriptor ──────── */
static inline const uint16_t *fb_row(int y)
{
    const amp_fb_desc_t *d = AMP_FB_DESC;
    return (const uint16_t *)(uintptr_t)(d->fb_base + (uint32_t)y * d->stride);
}

/* Pull a run of pixels from the (uncached) framebuffer into local memory
 * with 64-bit loads — one DDR burst per 4 pixels instead of one per pixel. */
static void fb_fetch(uint16_t *dst, int x, int y, int n)
{
    const uint16_t *src = fb_row(y) + x;
    int i = 0;
    if ((((uintptr_t)src) & 7) == 0) {
        const uint64_t *s64 = (const uint64_t *)src;
        uint64_t *d64 = (uint64_t *)dst;
        for (; i + 4 <= n; i += 4) *d64++ = *s64++;
    }
    for (; i < n; i++) dst[i] = src[i];
}

/* ── Hextile tile encoder (ported; reads from a local tile buffer) ─────── */
/* 8-byte aligned: read as uint64_t by the solid fast path + fb_fetch. */
static uint16_t tbuf[16 * 16] __attribute__((aligned(8)));

static size_t hextile_tile(uint8_t *out, int tx, int ty, int tw, int th,
                           int bytes, const struct pixfmt *f)
{
    for (int r = 0; r < th; r++) fb_fetch(tbuf + r * 16, tx, ty + r, tw);

    uint16_t c0 = tbuf[0], c1 = 0;
    int n0 = 0, ncol = 1;
    /* Fast path: full 16-wide tiles that are solid — compare 4 pixels per
     * 64-bit word (32 compares instead of 256 per-pixel iterations). */
    if (tw == 16) {
        uint64_t w0 = 0x0001000100010001ull * c0;
        const uint64_t *t64 = (const uint64_t *)tbuf;
        int solid = 1;
        for (int i = 0; i < th * 4; i++) if (t64[i] != w0) { solid = 0; break; }
        if (solid) {
            out[0] = HT_BG;
            (void)emit_px(out + 1, c0, bytes, f);
            return 1 + (size_t)bytes;
        }
    }
    for (int r = 0; r < th && ncol <= 2; r++) {
        const uint16_t *rp = tbuf + r * 16;
        for (int c = 0; c < tw; c++) {
            uint16_t px = rp[c];
            if (px == c0) n0++;
            else if (ncol == 1) { c1 = px; ncol = 2; }
            else if (px != c1) { ncol = 3; break; }
        }
    }
    if (ncol == 1) {
        out[0] = HT_BG;
        (void)emit_px(out + 1, c0, bytes, f);
        return 1 + (size_t)bytes;
    }
    if (ncol == 2) {
        uint16_t bg = c0, fg = c1;
        if (2 * n0 < tw * th) { bg = c1; fg = c0; }
        uint8_t *o = out + 1;
        o = emit_px(o, bg, bytes, f);
        o = emit_px(o, fg, bytes, f);
        uint8_t *nrun = o++;
        int runs = 0;
        for (int r = 0; r < th; r++) {
            const uint16_t *rp = tbuf + r * 16;
            for (int c = 0; c < tw; ) {
                if (rp[c] != fg) { c++; continue; }
                int c2 = c + 1;
                while (c2 < tw && rp[c2] == fg) c2++;
                *o++ = (uint8_t)((c << 4) | r);
                *o++ = (uint8_t)((c2 - c - 1) << 4);
                runs++;
                c = c2;
            }
        }
        *nrun = (uint8_t)runs;
        out[0] = HT_BG | HT_FG | HT_SUBRECTS;
        return (size_t)(o - out);
    }
    out[0] = HT_RAW;
    uint8_t *o = out + 1;
    for (int r = 0; r < th; r++) {
        const uint16_t *rp = tbuf + r * 16;
        if (fmt_native) {
            memcpy(o, rp, (size_t)tw * 2);
            o += (size_t)tw * 2;
        } else {
            for (int c = 0; c < tw; c++) o = emit_px(o, rp[c], bytes, f);
        }
    }
    return (size_t)(o - out);
}

/* ── connection + update state (single client) ─────────────────────────── */
enum st { S_VER, S_SEC, S_INIT, S_MSG };

static struct {
    struct tcp_pcb *pcb;
    enum st   st;
    uint8_t   rx[32];
    int       rxn;
    int       skip;                 /* bytes still to discard (encodings / cut text) */
    bool      in_enc;               /* the skipped bytes are SetEncodings entries */
    uint8_t   enc[4];
    int       encn;
    bool      hextile;
    uint32_t  ht_skip;
    struct pixfmt fmt;

    bool      pending;              /* an incremental request is waiting     */
    uint16_t  rq_x, rq_y, rq_w, rq_h;
    uint32_t  served_seq;

    bool      active;               /* an update is being pumped             */
    bool      upd_hex;
    int       ux, uy, uw, uh;
    int       cur_y;                /* raw: next row; hex: next tile row     */
    int       cur_tx;               /* hex: next tile column                 */
    uint32_t  upd_bytes;            /* payload bytes so far (adaptive check) */
} C;

static uint8_t stage[STAGE_SZ];
static uint16_t rowbuf[1024] __attribute__((aligned(8)));   /* u64-filled by fb_fetch */

static void conn_reset(void)
{
    memset(&C, 0, sizeof(C));
    C.fmt = native_fmt;
    build_luts(&C.fmt);
}

/* Coarse profile (REG_CLOCK_MS granularity, summed per 5 s heartbeat): where
 * core 2's time goes during an update — encode vs TCP output. */
#include "../mmio.h"
static err_t send_bytes(const void *p, size_t n)
{
    uint64_t t0 = REG_CLOCK_MS;
    err_t e = tcp_write(C.pcb, p, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (e == ERR_OK) tcp_output(C.pcb);
    AMP_FB_DESC->prof_tx_ms += (uint32_t)(REG_CLOCK_MS - t0);
    return e;
}

static void conn_close(void)
{
    if (C.pcb) {
        tcp_arg(C.pcb, NULL); tcp_recv(C.pcb, NULL); tcp_sent(C.pcb, NULL);
        tcp_err(C.pcb, NULL); tcp_poll(C.pcb, NULL, 0);
        tcp_close(C.pcb);
    }
    AMP_FB_DESC->clients = 0;
    conn_reset();
    printf("core2 vnc: client disconnected\n");
}

/* ── update pump ───────────────────────────────────────────────────────── */
static void update_begin(int x, int y, int w, int h)
{
    const amp_fb_desc_t *d = AMP_FB_DESC;
    C.active = true;
    C.ux = x; C.uy = y; C.uw = w; C.uh = h;
    C.cur_y = y; C.cur_tx = x;
    C.upd_bytes = 0;
    C.upd_hex = C.hextile && (C.ht_skip == 0);
    if (C.hextile && C.ht_skip) C.ht_skip--;
    C.served_seq = d->seq;

    uint8_t hdr[16] = { 0, 0, 0, 1 };
    be16(hdr + 4, (uint16_t)x); be16(hdr + 6, (uint16_t)y);
    be16(hdr + 8, (uint16_t)w); be16(hdr + 10, (uint16_t)h);
    be32(hdr + 12, C.upd_hex ? 5u : 0u);
    send_bytes(hdr, sizeof(hdr));
}

static void update_end(void)
{
    amp_fb_desc_t *d = AMP_FB_DESC;
    int bytes = C.fmt.bpp / 8;
    uint64_t raw = (uint64_t)C.uw * C.uh * bytes;
    if (C.upd_hex && (uint64_t)C.upd_bytes * 8 > raw * 7) C.ht_skip = HT_RETRY;
    C.active = false;
    d->ack = C.served_seq;
    d->updates++;
    d->bytes_lo += C.upd_bytes;
}

/* Encode+send ONE chunk if the TCP send buffer allows, then return so the
 * main loop can service RX (the MAC has only two RX slots: filling the whole
 * 46 KB window in one go starved ACKs and stalled TCP).  Called from the
 * recv/sent callbacks and every main-loop iteration (vnc_c2_poll). */
static void pump(void)
{
    int chunks = 1;
    if (!C.pcb) return;
    while (C.active && chunks-- > 0) {
        if (tcp_sndbuf(C.pcb) < STAGE_SZ) return;       /* resume on sent() */
        size_t len = 0;
        int bytes = C.fmt.bpp / 8;
        uint64_t t_enc = REG_CLOCK_MS;
        if (!C.upd_hex) {
            size_t row_bytes = (size_t)C.uw * bytes;
            while (C.cur_y < C.uy + C.uh && len + row_bytes <= STAGE_SZ) {
                fb_fetch(rowbuf, C.ux, C.cur_y, C.uw);
                if (fmt_native) {
                    memcpy(stage + len, rowbuf, row_bytes);
                } else {
                    uint8_t *o = stage + len;
                    for (int c = 0; c < C.uw; c++) o = emit_px(o, rowbuf[c], bytes, &C.fmt);
                }
                len += row_bytes;
                C.cur_y++;
            }
            if (len == 0) { update_end(); break; }
        } else {
            while (C.cur_y < C.uy + C.uh && len + HT_TILE_MAX <= STAGE_SZ) {
                int th = MIN(16, C.uy + C.uh - C.cur_y);
                int tw = MIN(16, C.ux + C.uw - C.cur_tx);
                len += hextile_tile(stage + len, C.cur_tx, C.cur_y, tw, th, bytes, &C.fmt);
                C.cur_tx += 16;
                if (C.cur_tx >= C.ux + C.uw) { C.cur_tx = C.ux; C.cur_y += 16; }
            }
            if (len == 0) { update_end(); break; }
        }
        AMP_FB_DESC->prof_enc_ms += (uint32_t)(REG_CLOCK_MS - t_enc);
        if (send_bytes(stage, len) != ERR_OK) return;   /* retry on sent()   */
        C.upd_bytes += (uint32_t)len;
        if (C.cur_y >= C.uy + C.uh) update_end();
    }
    /* Serve a pending incremental request once core 1 has posted anew. */
    if (!C.active && C.pending) {
        const amp_fb_desc_t *d = AMP_FB_DESC;
        if (d->seq != C.served_seq) {
            int x0 = MAX(C.rq_x, d->dx), y0 = MAX(C.rq_y, d->dy);
            int x1 = MIN(C.rq_x + C.rq_w, d->dx + d->dw);
            int y1 = MIN(C.rq_y + C.rq_h, d->dy + d->dh);
            C.pending = false;
            if (x0 < x1 && y0 < y1) update_begin(x0, y0, x1 - x0, y1 - y0);
            else C.served_seq = d->seq;                 /* nothing in view   */
        }
    }
}

/* ── RFB message handling ───────────────────────────────────────────────── */
static void send_server_init(void)
{
    const amp_fb_desc_t *d = AMP_FB_DESC;
    uint8_t b[24 + sizeof(DESKTOP_NAME)];
    be16(b + 0, d->width); be16(b + 2, d->height);
    b[4] = native_fmt.bpp; b[5] = native_fmt.depth; b[6] = 0; b[7] = 1;
    be16(b + 8, 31); be16(b + 10, 63); be16(b + 12, 31);
    b[14] = 11; b[15] = 5; b[16] = 0; b[17] = b[18] = b[19] = 0;
    be32(b + 20, sizeof(DESKTOP_NAME) - 1);
    memcpy(b + 24, DESKTOP_NAME, sizeof(DESKTOP_NAME) - 1);
    send_bytes(b, 24 + sizeof(DESKTOP_NAME) - 1);
}

/* Bytes needed to complete the message whose first byte(s) are in rx[]. */
static int msg_need(void)
{
    switch (C.st) {
    case S_VER:  return 12;
    case S_SEC:  return 1;
    case S_INIT: return 1;
    default:
        switch (C.rx[0]) {
        case 0:  return 20;                         /* SetPixelFormat        */
        case 2:  return 4;                          /* SetEncodings (+4n)    */
        case 3:  return 10;                         /* FramebufferUpdateReq  */
        case 4:  return 8;                          /* KeyEvent              */
        case 5:  return 6;                          /* PointerEvent          */
        case 6:  return 8;                          /* ClientCutText (+len)  */
        default: return 1;
        }
    }
}

static void handle_msg(void)
{
    const amp_fb_desc_t *d = AMP_FB_DESC;
    switch (C.st) {
    case S_VER: {
        uint8_t sec[2] = { 1, 1 };
        send_bytes(sec, 2);
        C.st = S_SEC;
        break;
    }
    case S_SEC: {
        uint8_t ok[4] = { 0, 0, 0, 0 };
        send_bytes(ok, 4);
        C.st = S_INIT;
        break;
    }
    case S_INIT:
        send_server_init();
        C.st = S_MSG;
        printf("core2 vnc: client connected\n");
        AMP_FB_DESC->clients = 1;
        break;
    default:
        switch (C.rx[0]) {
        case 0:
            parse_pixfmt(&C.fmt, C.rx + 4);
            build_luts(&C.fmt);
            printf("core2 vnc: fmt %u bpp %s\n", C.fmt.bpp, fmt_native ? "native" : "conv");
            break;
        case 2: {
            uint16_t n = rd16(C.rx + 2);
            C.hextile = false;
            C.skip = 4 * n;                          /* entries scanned in feed() */
            C.in_enc = (n != 0);
            C.encn = 0;
            if (n == 0) printf("core2 vnc: encodings raw only\n");
            break;
        }
        case 3: {
            int incr = C.rx[1];
            int x = rd16(C.rx + 2), y = rd16(C.rx + 4);
            int w = rd16(C.rx + 6), h = rd16(C.rx + 8);
            if (x > d->width)  x = d->width;
            if (y > d->height) y = d->height;
            if (x + w > d->width)  w = d->width - x;
            if (y + h > d->height) h = d->height - y;
            if (w <= 0 || h <= 0) break;
            if (!incr) {
                if (!C.active) update_begin(x, y, w, h);
                C.pending = false;
            } else {
                C.rq_x = x; C.rq_y = y; C.rq_w = w; C.rq_h = h;
                C.pending = true;
            }
            break;
        }
        case 6:
            C.skip = (int)rd32(C.rx + 4);
            break;
        default:
            break;
        }
    }
}

/* Feed one received byte through the parser. */
static void feed(uint8_t b)
{
    if (C.skip > 0) {
        C.skip--;
        if (C.in_enc) {                 /* SetEncodings entries: spot Hextile (5) */
            C.enc[C.encn++] = b;
            if (C.encn == 4) {
                if (rd32(C.enc) == 5u) C.hextile = true;
                C.encn = 0;
            }
            if (C.skip == 0) {
                C.in_enc = false;
                printf("core2 vnc: encodings raw%s\n", C.hextile ? " + hextile" : " only");
            }
        }
        return;
    }
    if (C.rxn < (int)sizeof(C.rx)) C.rx[C.rxn++] = b;
    if (C.rxn >= msg_need()) {
        handle_msg();
        C.rxn = 0;
    }
}

/* ── lwIP callbacks ────────────────────────────────────────────────────── */
static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg; (void)err;
    if (p == NULL) { conn_close(); return ERR_OK; }
    for (struct pbuf *q = p; q; q = q->next) {
        const uint8_t *d = (const uint8_t *)q->payload;
        for (u16_t i = 0; i < q->len; i++) feed(d[i]);
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    pump();
    return ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg; (void)pcb; (void)len;
    pump();
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)arg; (void)err;
    C.pcb = NULL;                   /* pcb already freed by lwIP */
    AMP_FB_DESC->clients = 0;
    conn_reset();
    printf("core2 vnc: connection error\n");
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg; (void)err;
    /* Single client — but never brick the server on a half-dead connection:
     * a client that vanished without FIN/err leaves C.pcb set forever, and
     * aborting NEW connects made the listener unreachable.  Evict the old
     * connection instead and take the new one. */
    if (C.pcb) {
        struct tcp_pcb *old = C.pcb;
        tcp_arg(old, NULL); tcp_recv(old, NULL); tcp_sent(old, NULL);
        tcp_err(old, NULL); tcp_poll(old, NULL, 0);
        tcp_abort(old);
        AMP_FB_DESC->clients = 0;
        printf("core2 vnc: evicted stale client\n");
    }
    conn_reset();
    C.pcb = newpcb;
    tcp_arg(newpcb, NULL);
    tcp_recv(newpcb, on_recv);
    tcp_sent(newpcb, on_sent);
    tcp_err(newpcb, on_err);
    tcp_nagle_disable(newpcb);
    send_bytes("RFB 003.008\n", 12);
    C.st = S_VER;
    return ERR_OK;
}

void vnc_c2_init(void)
{
    conn_reset();
    struct tcp_pcb *l = tcp_new();
    tcp_bind(l, IP_ADDR_ANY, VNC_PORT);
    l = tcp_listen(l);
    tcp_accept(l, on_accept);
    printf("core2 vnc: listening :%d (%ux%u)\n", VNC_PORT,
           AMP_FB_DESC->width, AMP_FB_DESC->height);
}

void vnc_c2_poll(void)
{
    if (C.pcb && C.st == S_MSG) pump();
}

int vnc_c2_status(void)
{
    if (!C.pcb) return 0;
    if (C.st != S_MSG) return 1;
    return C.active ? 3 : 2;
}
