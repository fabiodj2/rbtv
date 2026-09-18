/* ddj400-bridge.c - Pioneer DDJ-400 (USB MIDI) -> XDJ-RX3 rekordbox engine (rbp)
 *
 * `rbp` (the standalone rekordbox player) has no MIDI input: it reads its front
 * panel from Pioneer microcontrollers and exposes an internal key manager.
 * `keyshim.so` (LD_PRELOADed into rbp) already turns 12-byte key records from
 * /tmp/rb-keys.fifo into IKeyManager::sendKey() calls; this bridge adds the
 * value/relative half of the API through /tmp/rb-ctrl.fifo (24-byte records).
 *
 *   DDJ-400 --USB MIDI--> this bridge --24B records--> /tmp/rb-ctrl.fifo
 *                          (host, hard-float musl)        |
 *                                                         v
 *                       keyshim.so -> IKeyManager::sendKey(key, op, ch, param, f, l)
 *
 * Runs on the Chromebit *host* rootfs (postmarketOS armv7, musl), NOT in the
 * soft-float chroot: it only needs /dev/snd/midiC*D* and the shared /tmp.
 * No libasound dependency - the ALSA rawmidi device is read directly, so it
 * builds natively with the device's own gcc.
 *
 * MIDI map source: Pioneer/AlphaTheta "DDJ-400 List of MIDI messages v1.00"
 * (DDJ-400_MIDI_Message_List_E1.pdf), cross-checked against the Mixxx
 * Pioneer-DDJ-400 mapping.  Keycodes and op semantics come from the
 * live-verified PrimeBox knobshim2.c (docs/09-input.md):
 *
 *   op 0 PRESS / op 2 RELEASE : param/f/l unused
 *   op 4 ROTATE: param = 10-bit absolute (faders, EQ, crossfader, cue knobs)
 *                or relative delta (browse knob); f = normalised / speed
 *   op 5 VALUE : param = 10-bit, f = normalised (colour FX, Beat FX depth,
 *                tempo slider [-1..+1])
 *
 * Usage:
 *   ddj400-bridge [-v] [-s] [-d /dev/snd/midiC1D0] [-f /tmp/rb-ctrl.fifo]
 *                 [-J pulses_per_rev] [-F]
 *     -v  verbose (log every mapped event)
 *     -s  sniff mode: log every MIDI message with its decoded name and send
 *         nothing (use this to verify/repair a mapping against new hardware)
 *     -l  list the mapping table and exit
 *     -J  jog pulses per revolution (default 1800)
 *     -F  at startup, select FILTER as the Sound Color FX type on both chans
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <dirent.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* rbp / XDJ-RX3 keys (docs/09-input.md, PrimeBox MAPPING.md)          */
/* ------------------------------------------------------------------ */
#define OP_PRESS      0
#define OP_RELEASE    2
#define OP_ROTATE     4
#define OP_VALUE      5

#define K_SELECTOR    0x420c
#define K_SOURCE      0x0201
#define K_MENU        0x0206
#define K_BACK        0x420d
#define K_LOAD        0x4311   /* + deck channel */
#define K_PLAY        0x4101
#define K_CUE         0x4102
#define K_SYNC        0x4112
#define K_JOG_TOUCH   0x4306
#define K_JOG_ROT     0x4305
#define K_TEMPO_RANGE 0x4107
#define K_TEMPO_SLIDER 0x4109
#define K_ALOOP       0x4114
#define K_HOTCUE      0x4113
#define K_SLIPLOOP    0x4115
#define K_BEATJUMP    0x4116
#define K_PAD1        0x4117   /* .. K_PAD1+7 */
#define K_LOOPIN      0x410c
#define K_LOOPOUT     0x410d
#define K_RELOOP      0x410e
#define K_REV         0x410f
#define K_MASTER      0x4111
#define K_TRIM        0x5019
#define K_EQH         0x501a
#define K_EQM         0x501b
#define K_EQL         0x501c
#define K_FADER       0x501e
#define K_XFADER      0x6017
#define K_HPMIX       0x4405
#define K_HPLEVEL     0x4406
#define K_COLOR       0x509d
#define K_FILTER      0x50a6
#define K_BFXTYPE     0x448b
#define K_BFXCH       0x448c
#define K_BFX         0x448d
#define K_DEPTH       0x448f
#define K_BEATPREV    0x4490
#define K_BEATNEXT    0x4491
#define K_TAP         0x4492

#define CH_GLOBAL     1

/* DDJ-400 MIDI channels (0-based nibble of the status byte) */
#define MC_DECK1      0        /* "DECK 1"  */
#define MC_DECK2      1        /* "DECK 2"  */
#define MC_FX         4        /* "EFFECT" ch 5 */
#define MC_MIXER      6        /* ch 7: browser, crossfader, master, phones, filter */
#define MC_PAD1       7        /* pads deck 1, no SHIFT   */
#define MC_PAD1_SH    8        /* pads deck 1, with SHIFT */
#define MC_PAD2       9        /* pads deck 2, no SHIFT   */
#define MC_PAD2_SH   10        /* pads deck 2, with SHIFT */

/* ------------------------------------------------------------------ */
static int   opt_verbose = 0;
static int   opt_sniff   = 0;
static int   opt_filter_init = 0;
static const char *fifo_path = "/tmp/rb-ctrl.fifo";
static const char *midi_dev  = NULL;
static float jog_ppr = 1800.0f;    /* DDJ-400 jog pulses per revolution */
static int   jog_idle_ms = 60;     /* emit speed 0 after this idle time  */

static int fifo_fd = -1;

struct ctrl_ev {
    int32_t key;
    int32_t ch;
    int32_t op;
    int32_t param;
    float   f;
    int32_t l;
};

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* ---------------- FIFO output ---------------- */
static void send_ctrl(int key, int op, int ch, int param, float f, int l)
{
    struct ctrl_ev ev;
    size_t off = 0;

    if (opt_sniff)
        return;

    ev.key = key; ev.op = op; ev.ch = ch; ev.param = param; ev.f = f; ev.l = l;

    if (fifo_fd < 0)
        fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
    if (fifo_fd < 0)
        return;

    while (off < sizeof(ev)) {
        ssize_t n = write(fifo_fd, (char *)&ev + off, sizeof(ev) - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            static int warned = 0;
            if (!warned) {
                logmsg("ddj400: no reader on %s (rbp/keyshim not running?)\n", fifo_path);
                warned = 1;
            }
            return;
        } else {
            close(fifo_fd);
            fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
            return;
        }
    }
    if (opt_verbose)
        logmsg("  -> key=0x%04x op=%d ch=%d param=%d f=%.3f l=%d\n",
               key, op, ch, param, (double)f, l);
}

static void send_tap(int key, int ch)
{
    send_ctrl(key, OP_PRESS, ch, 0, 0.0f, 0);
    send_ctrl(key, OP_RELEASE, ch, 0, 0.0f, 0);
}

/* 7-bit CC (0..127) -> RX3 10-bit value (0..1023), as in knobshim2 */
static int cc_to_10bit(int v)
{
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    return (v << 3) | (v >> 4);
}

/* ---------------- 14-bit CC pairs ---------------- */
#define N14 16
struct cc14 {
    int ch;
    int msb_cc;      /* LSB is always msb_cc + 0x20 on the DDJ-400 */
    int key;
    int op;          /* OP_ROTATE for faders/knobs, OP_VALUE for colour/depth */
    int send_ch;
    int signed_norm; /* tempo slider: normalise to -1..+1 instead of 0..1 */
    int msb;
    int have_msb;
    int last10;      /* dedupe */
};
static struct cc14 cc14[N14];
static int cc14_n = 0;

static void add_cc14(int ch, int msb_cc, int key, int op, int send_ch, int signed_norm)
{
    if (cc14_n >= N14)
        return;
    cc14[cc14_n].ch = ch;
    cc14[cc14_n].msb_cc = msb_cc;
    cc14[cc14_n].key = key;
    cc14[cc14_n].op = op;
    cc14[cc14_n].send_ch = send_ch;
    cc14[cc14_n].signed_norm = signed_norm;
    cc14[cc14_n].last10 = -1;
    cc14_n++;
}

static int handle_cc14(int ch, int cc, int val)
{
    for (int i = 0; i < cc14_n; i++) {
        struct cc14 *s = &cc14[i];
        if (s->ch != ch)
            continue;
        if (cc == s->msb_cc) {
            s->msb = val;
            s->have_msb = 1;
            return 1;
        }
        if (cc == s->msb_cc + 0x20) {
            if (!s->have_msb)
                return 1;                    /* MSB not seen yet */
            int pos = (s->msb << 7) | val;   /* 0..16383 */
            float norm;
            int v10;
            if (s->signed_norm) {
                norm = ((float)pos / 16383.0f) * 2.0f - 1.0f;
                v10 = (int)((norm + 1.0f) * 511.5f + 0.5f);
            } else {
                norm = (float)pos / 16383.0f;
                v10 = (int)(norm * 1023.0f + 0.5f);
            }
            if (v10 < 0) v10 = 0;
            if (v10 > 1023) v10 = 1023;
            if (v10 != s->last10) {
                s->last10 = v10;
                send_ctrl(s->key, s->op, s->send_ch, v10, norm, pos);
                if (opt_verbose)
                    logmsg("  key=0x%04x ch%d pos=%d norm=%.3f v10=%d\n",
                           s->key, s->send_ch, pos, (double)norm, v10);
            }
            return 1;
        }
    }
    return 0;
}

static void build_cc14(void)
{
    /* mixer channel strips (deck channel == send channel).
     * Live-verified on this engine: faders/EQ/trim/crossfader/cue knobs are
     * driven with OP_ROTATE + 10-bit param (+ normalised float). */
    add_cc14(MC_DECK1, 0x13, K_FADER,  OP_ROTATE, 1, 0);
    add_cc14(MC_DECK2, 0x13, K_FADER,  OP_ROTATE, 2, 0);
    add_cc14(MC_DECK1, 0x04, K_TRIM,   OP_ROTATE, 1, 0);
    add_cc14(MC_DECK2, 0x04, K_TRIM,   OP_ROTATE, 2, 0);
    add_cc14(MC_DECK1, 0x07, K_EQH,    OP_ROTATE, 1, 0);
    add_cc14(MC_DECK2, 0x07, K_EQH,    OP_ROTATE, 2, 0);
    add_cc14(MC_DECK1, 0x0B, K_EQM,    OP_ROTATE, 1, 0);
    add_cc14(MC_DECK2, 0x0B, K_EQM,    OP_ROTATE, 2, 0);
    add_cc14(MC_DECK1, 0x0F, K_EQL,    OP_ROTATE, 1, 0);
    add_cc14(MC_DECK2, 0x0F, K_EQL,    OP_ROTATE, 2, 0);
    /* tempo slider: OP_VALUE, signed normalised [-1 = slow .. +1 = fast] */
    add_cc14(MC_DECK1, 0x00, K_TEMPO_SLIDER, OP_VALUE, 1, 1);
    add_cc14(MC_DECK2, 0x00, K_TEMPO_SLIDER, OP_VALUE, 2, 1);
    /* master section (ch 7) */
    add_cc14(MC_MIXER, 0x1F, K_XFADER,  OP_ROTATE, CH_GLOBAL, 0);
    add_cc14(MC_MIXER, 0x0C, K_HPMIX,   OP_ROTATE, CH_GLOBAL, 0);
    add_cc14(MC_MIXER, 0x0D, K_HPLEVEL, OP_ROTATE, CH_GLOBAL, 0);
}

/* ---------------- jog wheels ---------------- */
struct jog {
    int midi_ch;
    int send_ch;
    unsigned int vpos;
    int moving;
    long long last_ms;
    long long last_emit_ms;
};
static struct jog jogs[2];

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void jog_delta(int midi_ch, int delta)
{
    struct jog *s = NULL;
    for (int i = 0; i < 2; i++)
        if (jogs[i].midi_ch == midi_ch)
            s = &jogs[i];
    if (!s || delta == 0)
        return;

    long long t = now_ms();
    float dt = (float)(t - s->last_ms) / 1000.0f;
    s->last_ms = t;
    if (dt < 0.0005f)
        dt = 0.0005f;

    s->vpos = (s->vpos + (unsigned int)delta) & 0xFFFFu;
    float speed = (float)delta / jog_ppr / dt;
    if (speed > 8.0f) speed = 8.0f;
    if (speed < -8.0f) speed = -8.0f;
    s->moving = 1;
    s->last_emit_ms = t;

    send_ctrl(K_JOG_ROT, OP_ROTATE, s->send_ch, 0, speed, (int)s->vpos);
    if (opt_verbose)
        logmsg("  jog ch%d delta=%d speed=%.2f rev/s pos=%u\n",
               midi_ch, delta, (double)speed, s->vpos);
}

static void jog_tick(void)
{
    long long t = now_ms();
    for (int i = 0; i < 2; i++) {
        struct jog *s = &jogs[i];
        if (!s->moving)
            continue;
        if (t - s->last_emit_ms < jog_idle_ms)
            continue;
        s->moving = 0;
        send_ctrl(K_JOG_ROT, OP_ROTATE, s->send_ch, 0, 0.0f, (int)s->vpos);
    }
}

/* ---------------- pads ---------------- */
/* note = mode*16 + (pad-1);  modes: 0 hot cue, 1 beat loop, 2 beat jump,
 * 3 sampler, 4 keyboard, 5 pad fx1, 6 pad fx2, 7 key shift.
 * The RX3 engine has 4 pad banks: HOT CUE / AUTO LOOP / SLIP LOOP / BEAT JUMP. */
static int pad_bank[2] = { -1, -1 };

static void pad_select_bank(int deck, int mode)
{
    int key;
    switch (mode) {
    case 0: key = K_HOTCUE;   break;
    case 1: key = K_ALOOP;    break;
    case 2: key = K_BEATJUMP; break;
    case 3: key = K_SLIPLOOP; break;   /* no sampler in this engine */
    default: return;
    }
    if (pad_bank[deck] == mode)
        return;
    pad_bank[deck] = mode;
    send_tap(key, deck + 1);
}

static void handle_pad(int deck, int note, int on)
{
    int mode = (note >> 4) & 0x0F;
    int idx  = note & 0x0F;
    if (opt_verbose)
        logmsg("  pad deck%d note 0x%02x (mode %d pad %d) %s%s\n",
               deck + 1, note, mode, idx + 1, on ? "press" : "release",
               mode > 3 ? "  [no RX3 bank: ignored]" : "");
    if (idx > 7 || mode > 3)
        return;
    pad_select_bank(deck, mode);
    send_ctrl(K_PAD1 + idx, on ? OP_PRESS : OP_RELEASE, deck + 1, 0, 0.0f, 0);
}

/* ---------------- note mapping table ---------------- */
struct notemap {
    int ch;
    int note;
    int key;
    int send_ch;   /* 0 = derive from the MIDI channel (deck 1/2) */
    const char *name;
};
static const struct notemap notemap[] = {
    /* BROWSER (ch 7) */
    { MC_MIXER, 0x41, K_SELECTOR, CH_GLOBAL, "BROWSE push (select/enter)" },
    { MC_MIXER, 0x42, K_BACK,     CH_GLOBAL, "SHIFT+BROWSE push (back)" },
    { MC_MIXER, 0x46, K_LOAD,     1,         "LOAD deck 1" },
    { MC_MIXER, 0x47, K_LOAD,     2,         "LOAD deck 2" },
    { MC_MIXER, 0x68, K_SOURCE,   CH_GLOBAL, "SHIFT+LOAD 1 (source/usb)" },
    { MC_MIXER, 0x7A, K_MENU,     CH_GLOBAL, "SHIFT+LOAD 2 (menu)" },
    /* DECK transport / loops (deck channel) */
    { MC_DECK1, 0x0B, K_PLAY, 0, "PLAY/PAUSE" },
    { MC_DECK2, 0x0B, K_PLAY, 0, "PLAY/PAUSE" },
    { MC_DECK1, 0x47, K_REV,  0, "SHIFT+PLAY (censor)" },
    { MC_DECK2, 0x47, K_REV,  0, "SHIFT+PLAY (censor)" },
    { MC_DECK1, 0x0C, K_CUE,  0, "CUE" },
    { MC_DECK2, 0x0C, K_CUE,  0, "CUE" },
    { MC_DECK1, 0x58, K_SYNC, 0, "BEAT SYNC" },
    { MC_DECK2, 0x58, K_SYNC, 0, "BEAT SYNC" },
    { MC_DECK1, 0x5C, K_MASTER, 0, "BEAT SYNC long (master)" },
    { MC_DECK2, 0x5C, K_MASTER, 0, "BEAT SYNC long (master)" },
    { MC_DECK1, 0x60, K_TEMPO_RANGE, 0, "SHIFT+SYNC (tempo range)" },
    { MC_DECK2, 0x60, K_TEMPO_RANGE, 0, "SHIFT+SYNC (tempo range)" },
    { MC_DECK1, 0x10, K_LOOPIN,  0, "LOOP IN / 4 BEAT" },
    { MC_DECK2, 0x10, K_LOOPIN,  0, "LOOP IN / 4 BEAT" },
    { MC_DECK1, 0x11, K_LOOPOUT, 0, "LOOP OUT" },
    { MC_DECK2, 0x11, K_LOOPOUT, 0, "LOOP OUT" },
    { MC_DECK1, 0x4D, K_RELOOP,  0, "RELOOP/EXIT" },
    { MC_DECK2, 0x4D, K_RELOOP,  0, "RELOOP/EXIT" },
    /* jog plate touch */
    { MC_DECK1, 0x36, K_JOG_TOUCH, 0, "jog plate touch" },
    { MC_DECK2, 0x36, K_JOG_TOUCH, 0, "jog plate touch" },
    { MC_DECK1, 0x67, K_JOG_TOUCH, 0, "SHIFT+jog plate touch" },
    { MC_DECK2, 0x67, K_JOG_TOUCH, 0, "SHIFT+jog plate touch" },
    /* pad mode buttons */
    { MC_DECK1, 0x1B, K_HOTCUE,   0, "PAD MODE hot cue" },
    { MC_DECK2, 0x1B, K_HOTCUE,   0, "PAD MODE hot cue" },
    { MC_DECK1, 0x1E, K_ALOOP,    0, "PAD MODE beat loop" },
    { MC_DECK2, 0x1E, K_ALOOP,    0, "PAD MODE beat loop" },
    { MC_DECK1, 0x20, K_BEATJUMP, 0, "PAD MODE beat jump" },
    { MC_DECK2, 0x20, K_BEATJUMP, 0, "PAD MODE beat jump" },
    { MC_DECK1, 0x22, K_SLIPLOOP, 0, "PAD MODE sampler" },
    { MC_DECK2, 0x22, K_SLIPLOOP, 0, "PAD MODE sampler" },
    /* BEAT FX (ch 5) */
    { MC_FX, 0x4A, K_BFXTYPE,  CH_GLOBAL, "BEAT FX select" },
    { MC_FX, 0x4B, K_BFX,      CH_GLOBAL, "BEAT FX on/off" },
    { MC_FX, 0x47, K_TAP,      CH_GLOBAL, "BEAT FX tap" },
    { MC_FX, 0x63, K_BEATPREV, CH_GLOBAL, "BEAT FX beat <" },
    { MC_FX, 0x64, K_BEATNEXT, CH_GLOBAL, "BEAT FX beat >" },
};
#define NNMAP ((int)(sizeof(notemap) / sizeof(notemap[0])))

/* BEAT FX channel-select switch: notes 16/17/20 on ch 5 -> ch 1 / 2 / MASTER */
static int handle_fxch(int note)
{
    int v;
    if (note == 0x10)      v = 1;
    else if (note == 0x11) v = 2;
    else if (note == 0x14) v = 5;
    else return 0;
    if (opt_verbose)
        logmsg("  beat fx channel select -> %d\n", v);
    send_ctrl(K_BFXCH, OP_VALUE, CH_GLOBAL, v, 0.0f, 0);
    return 1;
}

static const char *note_name(int ch, int note)
{
    static char buf[64];
    for (int i = 0; i < NNMAP; i++)
        if (notemap[i].ch == ch && notemap[i].note == note)
            return notemap[i].name;
    snprintf(buf, sizeof(buf), "%s", "");
    return buf;
}

/* ---------------- MIDI dispatch ---------------- */
static void handle_note(int ch, int note, int on)
{
    if (opt_sniff) {
        logmsg("MIDI ch%-2d NOTE %3d (0x%02x) %-3s %s\n",
               ch + 1, note, note, on ? "on" : "off", note_name(ch, note));
        return;
    }
    if (ch == MC_FX && handle_fxch(note))
        return;
    if (ch == MC_PAD1 || ch == MC_PAD1_SH) { handle_pad(0, note, on); return; }
    if (ch == MC_PAD2 || ch == MC_PAD2_SH) { handle_pad(1, note, on); return; }

    for (int i = 0; i < NNMAP; i++) {
        int sch;
        if (notemap[i].ch != ch || notemap[i].note != note)
            continue;
        sch = notemap[i].send_ch;
        if (sch == 0)
            sch = (ch == MC_DECK1) ? 1 : 2;
        send_ctrl(notemap[i].key, on ? OP_PRESS : OP_RELEASE, sch, 0, 0.0f, 0);
        if (opt_verbose)
            logmsg("  %s -> 0x%04x %s ch%d\n", notemap[i].name,
                   notemap[i].key, on ? "press" : "release", sch);
        return;
    }
    if (opt_verbose)
        logmsg("  unmapped ch%d note 0x%02x %s\n", ch, note, on ? "on" : "off");
}

static void handle_cc(int ch, int cc, int val)
{
    if (opt_sniff) {
        logmsg("MIDI ch%-2d CC  %3d (0x%02x) val=%3d\n", ch + 1, cc, cc, val);
        return;
    }

    /* browse encoder: relative two's complement */
    if (ch == MC_MIXER && cc == 0x40) {
        int delta = (val >= 64) ? val - 128 : val;
        if (delta > 16) delta = 16;
        if (delta < -16) delta = -16;
        for (int i = 0; i < (delta < 0 ? -delta : delta); i++)
            send_ctrl(K_SELECTOR, OP_ROTATE, CH_GLOBAL, (delta < 0) ? -1 : 1, 0.0f, 0);
        if (opt_verbose)
            logmsg("  browse delta=%d\n", delta);
        return;
    }

    /* jog wheels: CC 0x21 side, 0x22 platter vinyl, 0x23 platter non-vinyl,
     * 0x29 SHIFT+platter (search).  All relative. */
    if ((ch == MC_DECK1 || ch == MC_DECK2) &&
        (cc == 0x21 || cc == 0x22 || cc == 0x23 || cc == 0x29)) {
        jog_delta(ch, (val >= 64) ? val - 128 : val);
        return;
    }

    if (handle_cc14(ch, cc, val))
        return;

    /* 7-bit absolute controls */
    if (ch == MC_MIXER && (cc == 0x17 || cc == 0x18)) {   /* FILTER ch1 / ch2 */
        int sch = (cc == 0x17) ? 1 : 2;
        send_ctrl(K_COLOR, OP_VALUE, sch, cc_to_10bit(val), (float)val / 127.0f, val);
        if (opt_verbose)
            logmsg("  filter ch%d val=%d\n", sch, val);
        return;
    }
    if (ch == MC_FX && cc == 0x02) {                      /* BEAT FX depth */
        send_ctrl(K_DEPTH, OP_VALUE, CH_GLOBAL, cc_to_10bit(val), (float)val / 127.0f, val);
        if (opt_verbose)
            logmsg("  beat fx depth val=%d\n", val);
        return;
    }

    if (opt_verbose)
        logmsg("  unmapped ch%d cc 0x%02x val=%d\n", ch, cc, val);
}

/* ---------------- device discovery ---------------- */
static int find_ddj_card(void)
{
    FILE *f = fopen("/proc/asound/cards", "r");
    char line[256];
    int card = -1;
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        int idx;
        if (sscanf(line, " %d [", &idx) == 1)
            card = idx;
        else if (card >= 0 && strstr(line, "DDJ")) {
            fclose(f);
            return card;
        }
    }
    fclose(f);
    return -1;
}

static char *find_midi_node(int card)
{
    static char path[128];
    DIR *d = opendir("/dev/snd");
    struct dirent *e;
    if (!d)
        return NULL;
    while ((e = readdir(d))) {
        int c, dev;
        if (sscanf(e->d_name, "midiC%dD%d", &c, &dev) == 2 && c == card) {
            snprintf(path, sizeof(path), "/dev/snd/%.32s", e->d_name);
            closedir(d);
            return path;
        }
    }
    closedir(d);
    return NULL;
}

static int open_midi(void)
{
    if (midi_dev)
        return open(midi_dev, O_RDONLY | O_NONBLOCK);

    int card = find_ddj_card();
    if (card < 0) {
        logmsg("ddj400: no DDJ-400 card in /proc/asound/cards\n");
        return -1;
    }
    char *node = find_midi_node(card);
    if (!node) {
        logmsg("ddj400: card %d has no rawmidi node in /dev/snd\n", card);
        return -1;
    }
    logmsg("ddj400: using %s (ALSA card %d)\n", node, card);
    return open(node, O_RDONLY | O_NONBLOCK);
}

static void list_map(void)
{
    printf("14-bit controls:\n");
    for (int i = 0; i < cc14_n; i++)
        printf("  ch%-2d CC 0x%02x + 0x%02x -> key 0x%04x op %s send-ch %d%s\n",
               cc14[i].ch + 1, cc14[i].msb_cc, cc14[i].msb_cc + 0x20,
               cc14[i].key, cc14[i].op == OP_VALUE ? "VALUE " : "ROTATE",
               cc14[i].send_ch, cc14[i].signed_norm ? " (signed)" : "");
    printf("notes:\n");
    for (int i = 0; i < NNMAP; i++)
        printf("  ch%-2d note 0x%02x -> key 0x%04x send-ch %d  %s\n",
               notemap[i].ch + 1, notemap[i].note, notemap[i].key,
               notemap[i].send_ch, notemap[i].name);
    printf("pads: note = mode*16 + (pad-1) -> keys 0x%04x..0x%04x "
           "(modes 0-3 = hot cue/beat loop/beat jump/sampler)\n", K_PAD1, K_PAD1 + 7);
}

static void run_device(int fd);

int main(int argc, char **argv)
{
    int opt, fd;

    while ((opt = getopt(argc, argv, "vsld:f:J:F")) != -1) {
        switch (opt) {
        case 'v': opt_verbose = 1; break;
        case 's': opt_sniff = 1; opt_verbose = 1; break;
        case 'd': midi_dev = optarg; break;
        case 'f': fifo_path = optarg; break;
        case 'J': jog_ppr = (float)atof(optarg); break;
        case 'F': opt_filter_init = 1; break;
        case 'l': build_cc14(); list_map(); return 0;
        default:
            fprintf(stderr, "usage: %s [-v] [-s] [-l] [-d dev] [-f fifo] "
                            "[-J jog_ppr] [-F]\n", argv[0]);
            return 2;
        }
    }

    build_cc14();
    jogs[0].midi_ch = MC_DECK1; jogs[0].send_ch = 1;
    jogs[1].midi_ch = MC_DECK2; jogs[1].send_ch = 2;

    logmsg("ddj400-bridge: %s -> %s (jog %g pulses/rev)\n",
           opt_sniff ? "SNIFF (no output)" : "bridge", fifo_path, (double)jog_ppr);

    if (!opt_sniff) {
        fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
        if (fifo_fd < 0)
            logmsg("ddj400: cannot open %s: %s (will retry per event)\n",
                   fifo_path, strerror(errno));
        if (opt_filter_init)
            for (int ch = 1; ch <= 2; ch++)
                send_tap(K_FILTER, ch);
    }

    /* Hot-plug friendly: keep waiting for the controller instead of dying,
     * so the bridge can be started before the DDJ-400 is plugged in. */
    for (;;) {
        fd = open_midi();
        if (fd < 0) {
            if (midi_dev)
                return 1;              /* explicit -d device: fail fast */
            logmsg("ddj400: waiting for a DDJ-400 to be plugged in...\n");
            sleep(2);
            continue;
        }
        run_device(fd);
        close(fd);
        logmsg("ddj400: device closed, waiting for reconnect...\n");
        sleep(2);
    }
}

static void run_device(int fd)
{
    unsigned char buf[512];
    int status = 0, d1 = 0, need = 0;

    for (;;) {
        struct pollfd pfd;
        int pr;
        ssize_t n;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pr = poll(&pfd, 1, 20);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            logmsg("ddj400: poll: %s\n", strerror(errno));
            break;
        }
        if (pr == 0) {
            jog_tick();
            continue;
        }
        n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            logmsg("ddj400: read: %s (unplugged?)\n", strerror(errno));
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            unsigned char b = buf[i];
            if (b >= 0xF8)
                continue;
            if (b & 0x80) {
                status = b;
                need = ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) ? 1 : 2;
                d1 = 0;
                continue;
            }
            if (!status)
                continue;
            if (need == 2) {
                d1 = b;
                need = 1;
                continue;
            }
            need = 2;
            int type = status & 0xF0;
            int ch = status & 0x0F;
            if (type == 0x90 || type == 0x80)
                handle_note(ch, d1, (type == 0x90 && b > 0));
            else if (type == 0xB0)
                handle_cc(ch, d1, b);
            else if (opt_sniff)
                logmsg("MIDI ch%d status=0x%02x d1=%d d2=%d\n", ch + 1, status, d1, b);
        }
        jog_tick();
    }
    return;
}
