/* rbkeyd.c - physical USB keyboard control for the XDJ-RX3 rekordbox engine
 * (rbp) on the Chromebit.
 *
 * There is no X/Wayland on this box, so this replaces rb2go's SDL2 viewer
 * (device/rbviewer.c) with a headless evdev reader.  It normalises key events
 * and injects them through the two FIFOs keyshim.so already consumes inside
 * rbp - exactly the same path the DDJ-400 bridge uses:
 *
 *   /tmp/rb-keys.fifo   12-byte { int32 key, ch, down }            (buttons)
 *   /tmp/rb-ctrl.fifo   24-byte { key, ch, op, param, float, l }   (values)
 *
 *        keyboard --evdev--> rbkeyd --> FIFO --> keyshim.so --> IKeyManager::sendKey
 *
 * Hot-plug aware: it waits for a keyboard to appear, and re-scans when one is
 * unplugged (the Chromebit has a single USB-A port, so swapping devices is
 * normal).  Devices that are not keyboards are ignored - notably the DDJ-400,
 * which exposes HID without the input subsystem.
 *
 * Usage:
 *   rbkeyd                  run (default; waits for / re-attaches a keyboard)
 *   rbkeyd -v               verbose: log every action
 *   rbkeyd -d /dev/input/event3
 *   rbkeyd list             print the key map
 *   rbkeyd send <ctl> [args]   one-shot injection for scripting, e.g.
 *                              rbkeyd send play 1
 *                              rbkeyd send fader 1 900
 *                              rbkeyd send eqlow 2 700
 *                              rbkeyd send pad 3 2
 *                              rbkeyd send jog 1 8
 *                              rbkeyd send list
 *
 * ---------------------------------------------------------------------------
 * KEY MAP  (active deck = 1; TAB switches it)
 *
 *   BROWSE / GLOBAL
 *     m menu        s source      b browse     u usb1      r rekordbox
 *     l link        i info        t tag list
 *     ENTER select (browse push)  BACKSPACE back
 *     UP / DOWN   browse knob +/-1        PAGEUP / PAGEDOWN  +/-10
 *     1 / 2       load deck 1 / 2
 *
 *   TRANSPORT  (rb2go's letters, per deck)
 *     p play d1     o play d2
 *     c cue  d1     v cue  d2
 *     y sync d1     h sync d2
 *
 *   DECK (active deck)
 *     k master      g tempo range
 *     [ loop in     ] loop out     \ reloop/exit
 *
 *   PADS (active deck)
 *     F1..F8  pads 1..8
 *     F9 hot cue bank   F10 auto loop   F11 slip loop   F12 beat jump
 *
 *   JOG (active deck)
 *     LEFT / RIGHT  nudge;  SHIFT+LEFT / SHIFT+RIGHT  coarse
 *
 *   MIXER LAYER - press INSERT to toggle.  While it is on, the letters drive
 *   the mixer strip of the active deck (continuous values, all through the
 *   control FIFO, starting from the engine's defaults):
 *     w/s channel fader up/down     e/d TRIM up/down
 *     r/f EQ HI up/down             t/g EQ MID up/down
 *     y/h EQ LOW up/down            u/j Sound Color FX (filter) up/down
 *     o/l CROSSFADER right/left (global)
 *     -/= TEMPO slider down/up
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
#include <sys/ioctl.h>
#include <linux/input.h>

/* ---------------- rbp keys / ops (docs/09, docs/12) ---------------- */
#define OP_PRESS      0
#define OP_RELEASE    2
#define OP_ROTATE     4
#define OP_VALUE      5

#define K_SELECTOR    0x420c
#define K_BROWSE      0x0202
#define K_SOURCE      0x0201
#define K_USB1        0x0209
#define K_MENU        0x0206
#define K_INFO        0x020b
#define K_BACK        0x420d
#define K_TAGLIST     0x0203
#define K_REKORDBOX   0x0208
#define K_LINK        0x0207
#define K_LOAD        0x4311
#define K_PLAY        0x4101
#define K_CUE         0x4102
#define K_SYNC        0x4112
#define K_MASTER      0x4111
#define K_TEMPO_RANGE 0x4107
#define K_TEMPO_SLIDER 0x4109
#define K_LOOPIN      0x410c
#define K_LOOPOUT     0x410d
#define K_RELOOP      0x410e
#define K_JOG_ROT     0x4305
#define K_JOG_TOUCH   0x4306
#define K_PAD1        0x4117      /* .. +7 */
#define K_HOTCUE      0x4113
#define K_ALOOP       0x4114
#define K_SLIPLOOP    0x4115
#define K_BEATJUMP    0x4116
#define K_TRIM        0x5019
#define K_EQH         0x501a
#define K_EQM         0x501b
#define K_EQL         0x501c
#define K_FADER       0x501e
#define K_XFADER      0x6017
#define K_COLOR       0x509d
#define K_DEPTH       0x448f
#define K_BFX         0x448d

/* ---------------- key ids ---------------- */
#define KK_SP     0x1000
enum {
    KK_NONE = 0,
    KK_ENTER = KK_SP, KK_BACKSPACE, KK_TAB, KK_INSERT,
    KK_UP, KK_DOWN, KK_LEFT, KK_RIGHT, KK_PGUP, KK_PGDN,
    KK_S_UP, KK_S_DOWN, KK_S_LEFT, KK_S_RIGHT,
    KK_F1,
    KK_F12 = KK_F1 + 11
};

static const char *fifo_key  = "/tmp/rb-keys.fifo";
static const char *fifo_ctrl = "/tmp/rb-ctrl.fifo";
static int key_fd = -1, ctrl_fd = -1;
static int opt_verbose = 0;
static int active_deck = 1;

struct key_ev  { int32_t key, ch, down; };
struct ctrl_ev { int32_t key, ch, op, param; float f; int32_t l; };

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void write_all(int *fd, const char *path, const void *buf, size_t len)
{
    size_t off = 0;
    if (*fd < 0)
        *fd = open(path, O_RDWR | O_NONBLOCK);
    if (*fd < 0)
        return;
    while (off < len) {
        ssize_t n = write(*fd, (const char *)buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            static int warned = 0;
            if (!warned) {
                logmsg("rbkeyd: no reader on %s (is rbp/keyshim running?)\n", path);
                warned = 1;
            }
            return;
        } else {
            close(*fd);
            *fd = -1;
            return;
        }
    }
}

static void send_btn(int key, int ch, int down)
{
    struct key_ev ev;
    ev.key = key; ev.ch = ch; ev.down = down;
    write_all(&key_fd, fifo_key, &ev, sizeof(ev));
}

static void tap(int key, int ch)
{
    send_btn(key, ch, 1);
    send_btn(key, ch, 0);
}

static void send_ctrl(int key, int op, int ch, int param, float f, int l)
{
    struct ctrl_ev ev;
    ev.key = key; ev.ch = ch; ev.op = op; ev.param = param; ev.f = f; ev.l = l;
    write_all(&ctrl_fd, fifo_ctrl, &ev, sizeof(ev));
    if (opt_verbose)
        logmsg("  ctrl key=0x%04x op=%d ch=%d param=%d f=%.3f\n",
               key, op, ch, param, (double)f);
}

/* ---------------- button map ---------------- */
struct btnmap { int kid; int key; int ch; const char *name; };  /* ch 0 = active deck */
static const struct btnmap btnmap[] = {
    /* browse / global (rb2go's letters, unchanged) */
    { 'm',    K_MENU,      1, "menu" },
    { 's',    K_SOURCE,    1, "source" },
    { 'b',    K_BROWSE,    1, "browse" },
    { 'u',    K_USB1,      1, "usb1" },
    { 'r',    K_REKORDBOX, 1, "rekordbox" },
    { 'l',    K_LINK,      1, "link" },
    { 'i',    K_INFO,      1, "info" },
    { 't',    K_TAGLIST,   1, "tag list" },
    { KK_ENTER,     K_SELECTOR, 1, "select (browse push)" },
    { KK_BACKSPACE, K_BACK,     1, "back" },
    { '1',    K_LOAD,      1, "load deck 1" },
    { '2',    K_LOAD,      2, "load deck 2" },
    /* transport: rb2go's per-deck letters */
    { 'p',    K_PLAY,      1, "play deck 1" },
    { 'o',    K_PLAY,      2, "play deck 2" },
    { 'c',    K_CUE,       1, "cue deck 1" },
    { 'v',    K_CUE,       2, "cue deck 2" },
    { 'y',    K_SYNC,      1, "sync deck 1" },
    { 'h',    K_SYNC,      2, "sync deck 2" },
    /* deck buttons (active deck) */
    { 'k',    K_MASTER,      0, "master" },
    { 'g',    K_TEMPO_RANGE, 0, "tempo range" },
    { '[',    K_LOOPIN,      0, "loop in" },
    { ']',    K_LOOPOUT,     0, "loop out" },
    { '\\',   K_RELOOP,      0, "reloop/exit" },
    /* pads */
    { KK_F1 + 0, K_PAD1 + 0, 0, "pad 1" },
    { KK_F1 + 1, K_PAD1 + 1, 0, "pad 2" },
    { KK_F1 + 2, K_PAD1 + 2, 0, "pad 3" },
    { KK_F1 + 3, K_PAD1 + 3, 0, "pad 4" },
    { KK_F1 + 4, K_PAD1 + 4, 0, "pad 5" },
    { KK_F1 + 5, K_PAD1 + 5, 0, "pad 6" },
    { KK_F1 + 6, K_PAD1 + 6, 0, "pad 7" },
    { KK_F1 + 7, K_PAD1 + 7, 0, "pad 8" },
    { KK_F1 + 8,  K_HOTCUE,   0, "pad bank hot cue" },
    { KK_F1 + 9,  K_ALOOP,    0, "pad bank auto loop" },
    { KK_F1 + 10, K_SLIPLOOP, 0, "pad bank slip loop" },
    { KK_F1 + 11, K_BEATJUMP, 0, "pad bank beat jump" },
};
#define NBTN ((int)(sizeof(btnmap) / sizeof(btnmap[0])))

/* ---------------- mixer layer (continuous values) ---------------- */
struct valmap { int kid; int key; int op; int ch; int step; int init; const char *name; };
static const struct valmap valmap[] = {
    { 'w', K_FADER,        OP_ROTATE, 0, +32, 1023, "channel fader up" },
    { 's', K_FADER,        OP_ROTATE, 0, -32, 1023, "channel fader down" },
    { 'e', K_TRIM,         OP_ROTATE, 0, +16,  512, "trim up" },
    { 'd', K_TRIM,         OP_ROTATE, 0, -16,  512, "trim down" },
    { 'r', K_EQH,          OP_ROTATE, 0, +16,  512, "eq hi up" },
    { 'f', K_EQH,          OP_ROTATE, 0, -16,  512, "eq hi down" },
    { 't', K_EQM,          OP_ROTATE, 0, +16,  512, "eq mid up" },
    { 'g', K_EQM,          OP_ROTATE, 0, -16,  512, "eq mid down" },
    { 'y', K_EQL,          OP_ROTATE, 0, +16,  512, "eq low up" },
    { 'h', K_EQL,          OP_ROTATE, 0, -16,  512, "eq low down" },
    { 'u', K_COLOR,        OP_VALUE,  0, +16,  512, "color/filter fx up" },
    { 'j', K_COLOR,        OP_VALUE,  0, -16,  512, "color/filter fx down" },
    { 'o', K_XFADER,       OP_ROTATE, 1, +32,  512, "crossfader right" },
    { 'l', K_XFADER,       OP_ROTATE, 1, -32,  512, "crossfader left" },
    { '-', K_TEMPO_SLIDER, OP_VALUE,  0, -20,    0, "tempo down" },
    { '=', K_TEMPO_SLIDER, OP_VALUE,  0, +20,    0, "tempo up" },
};
#define NVAL ((int)(sizeof(valmap) / sizeof(valmap[0])))

#define NSTATE 32
static struct { int key, ch, val; } vstate[NSTATE];
static int vstate_n = 0;

static int *vstate_get(int key, int ch, int init)
{
    for (int i = 0; i < vstate_n; i++)
        if (vstate[i].key == key && vstate[i].ch == ch)
            return &vstate[i].val;
    if (vstate_n >= NSTATE)
        return NULL;
    vstate[vstate_n].key = key;
    vstate[vstate_n].ch = ch;
    vstate[vstate_n].val = init;
    return &vstate[vstate_n++].val;
}

/* ---------------- jog ---------------- */
struct jog { int deck; unsigned int vpos; int moving; long long last_ms; };
static struct jog jogs[2] = { { 1, 0, 0, 0 }, { 2, 0, 0, 0 } };

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void do_jog(int deck, int delta)
{
    struct jog *s = &jogs[deck - 1];
    s->vpos = (s->vpos + (unsigned int)delta) & 0xFFFFu;
    s->moving = 1;
    s->last_ms = now_ms();
    float speed = (delta > 0) ? 0.5f : -0.5f;
    send_ctrl(K_JOG_ROT, OP_ROTATE, deck, 0, speed, (int)s->vpos);
    if (opt_verbose)
        logmsg("  jog deck %d delta=%d pos=%u\n", deck, delta, s->vpos);
}

static void jog_tick(void)
{
    long long t = now_ms();
    for (int i = 0; i < 2; i++) {
        if (!jogs[i].moving)
            continue;
        if (t - jogs[i].last_ms < 120)
            continue;
        jogs[i].moving = 0;
        send_ctrl(K_JOG_ROT, OP_ROTATE, jogs[i].deck, 0, 0.0f, (int)jogs[i].vpos);
    }
}

/* ---------------- mixer layer values ---------------- */
static void do_valmap(const struct valmap *v)
{
    int ch = v->ch ? v->ch : active_deck;
    if (v->key == K_TEMPO_SLIDER) {
        int *cur = vstate_get(v->key, ch, 0);
        if (!cur)
            return;
        *cur += v->step;
        if (*cur > 1000) *cur = 1000;
        if (*cur < -1000) *cur = -1000;
        float norm = (float)*cur / 1000.0f;
        int param = (int)((norm + 1.0f) * 511.5f + 0.5f);
        send_ctrl(K_TEMPO_SLIDER, OP_VALUE, ch, param, norm, param);
        if (opt_verbose)
            logmsg("  tempo deck %d norm=%+.2f\n", ch, (double)norm);
        return;
    }
    int *cur = vstate_get(v->key, ch, v->init);
    if (!cur)
        return;
    int nv = *cur + v->step;
    if (nv < 0) nv = 0;
    if (nv > 1023) nv = 1023;
    if (nv == *cur)
        return;
    *cur = nv;
    send_ctrl(v->key, v->op, ch, nv, (float)nv / 1023.0f, nv);
    if (opt_verbose)
        logmsg("  key=0x%04x ch%d val=%d\n", v->key, ch, nv);
}

/* ---------------- key dispatch ---------------- */
static int mixer_layer = 0;

static void handle_key(int kid, int down)
{
    if (kid == KK_NONE)
        return;

    if (kid == KK_INSERT) {
        if (down) {
            mixer_layer = !mixer_layer;
            logmsg("rbkeyd: MIXER layer %s (active deck %d)\n",
                   mixer_layer ? "ON  - letters = mixer strip" : "OFF - letters = buttons",
                   active_deck);
        }
        return;
    }
    if (kid == KK_TAB) {
        if (down) {
            active_deck = (active_deck == 1) ? 2 : 1;
            logmsg("rbkeyd: active deck %d\n", active_deck);
        }
        return;
    }

    /* browse knob / list + menu navigation.
     * A rotary selector turn is CW = +1, and CW moves the highlight DOWN a
     * list - so UP must send -1 (counter-clockwise).  Getting this backwards
     * makes the arrows scroll the track list and every menu upside down. */
    if (kid == KK_UP || kid == KK_DOWN || kid == KK_PGUP || kid == KK_PGDN) {
        if (!down)
            return;
        int up = (kid == KK_UP || kid == KK_PGUP);
        int d = up ? -1 : 1;                  /* up = counter-clockwise */
        int n = (kid == KK_PGUP || kid == KK_PGDN) ? 10 : 1;
        for (int i = 0; i < n; i++)
            send_ctrl(K_SELECTOR, OP_ROTATE, 1, d, 0.0f, 0);
        if (opt_verbose)
            logmsg("  browse %s %d\n", up ? "up" : "down", n);
        return;
    }

    /* jog */
    if (kid == KK_LEFT || kid == KK_RIGHT || kid == KK_S_LEFT || kid == KK_S_RIGHT) {
        if (down) {
            int coarse = (kid == KK_S_LEFT || kid == KK_S_RIGHT);
            int d = (kid == KK_RIGHT || kid == KK_S_RIGHT) ? 1 : -1;
            do_jog(active_deck, d * (coarse ? 8 : 1));
        }
        return;
    }

    /* mixer layer swallows the letters */
    if (mixer_layer) {
        if (!down)
            return;
        for (int i = 0; i < NVAL; i++)
            if (valmap[i].kid == kid) {
                do_valmap(&valmap[i]);
                return;
            }
        return;
    }

    for (int i = 0; i < NBTN; i++) {
        int ch, key;
        if (btnmap[i].kid != kid)
            continue;
        ch = btnmap[i].ch ? btnmap[i].ch : active_deck;
        key = btnmap[i].key;
        if (key >= K_HOTCUE && key <= K_BEATJUMP) {     /* bank select = tap */
            if (down)
                tap(key, ch);
        } else {
            send_btn(key, ch, down ? 1 : 0);
        }
        if (opt_verbose && down)
            logmsg("  %s -> 0x%04x ch%d\n", btnmap[i].name, key, ch);
        return;
    }
    if (opt_verbose && down)
        logmsg("  (unmapped key id 0x%x)\n", kid);
}

/* ---------------- evdev ---------------- */
static int is_keyboard(int fd)
{
    unsigned char bits[KEY_MAX / 8 + 1];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
        return 0;
    return (bits[KEY_A / 8] & (1 << (KEY_A % 8))) &&
           (bits[KEY_Z / 8] & (1 << (KEY_Z % 8))) &&
           (bits[KEY_ENTER / 8] & (1 << (KEY_ENTER % 8))) &&
           (bits[KEY_SPACE / 8] & (1 << (KEY_SPACE % 8)));
}

static const char *opt_want_name = NULL;

/* A device can be opened by several readers, but a reader that grabbed it
 * (EVIOCGRAB, e.g. keyd remapping the physical keyboard) receives the events
 * exclusively - everyone else silently gets nothing.  Probe by grabbing and
 * releasing: EBUSY means somebody else owns it, so skip it. */
static int is_grabbed(int fd)
{
    if (ioctl(fd, EVIOCGRAB, 1) == 0) {
        ioctl(fd, EVIOCGRAB, 0);
        return 0;
    }
    return 1;
}

/* A remapping daemon (keyd, on this image) re-emits keys on its own "virtual"
 * device, which may or may not mirror what was pressed.  Prefer a real,
 * ungrabbed keyboard and fall back to the virtual one. */
static int is_virtual_name(const char *name)
{
    return strstr(name, "virtual") != NULL;
}

struct kbd_choice { char path[128]; char name[64]; };

/* Pick the best free keyboard: first non-virtual one wins, else a virtual.
 * Returns 1 when a candidate was found. */
static int scan_keyboard(struct kbd_choice *out)
{
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    int have_virtual = 0, busy = 0;

    if (!d)
        return 0;
    while ((e = readdir(d))) {
        char path[128], name[64] = "?";
        int fd;
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        snprintf(path, sizeof(path), "/dev/input/%.32s", e->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        if (!is_keyboard(fd)) {
            close(fd);
            continue;
        }
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        if (opt_want_name && !strstr(name, opt_want_name)) {
            close(fd);
            continue;
        }
        if (is_grabbed(fd)) {
            busy++;
            close(fd);
            continue;
        }
        if (is_virtual_name(name)) {
            if (!have_virtual) {               /* remember as fallback only */
                snprintf(out->path, sizeof(out->path), "%s", path);
                snprintf(out->name, sizeof(out->name), "%s", name);
                have_virtual = 1;
            }
        } else {
            snprintf(out->path, sizeof(out->path), "%s", path);
            snprintf(out->name, sizeof(out->name), "%s", name);
            close(fd);                         /* else one fd leaked per rescan */
            closedir(d);
            return 1;                          /* physical: best possible */
        }
        close(fd);
    }
    closedir(d);
    if (!have_virtual && busy)
        logmsg("rbkeyd: %d keyboard device(s) grabbed by another program "
               "(keyd?) - waiting for a free one\n", busy);
    return have_virtual;
}

/* evdev key codes are NOT in alphabetical order: the QWERTY row is 16..25
 * (KEY_Q..KEY_P), the ASDF row is 30..38, the ZXCV row is 44..50.  Map
explicitly - do not compute from KEY_A. */
static const struct { unsigned short code; int kid; } letterkeys[] = {
    { KEY_Q, 'q' }, { KEY_W, 'w' }, { KEY_E, 'e' }, { KEY_R, 'r' }, { KEY_T, 't' },
    { KEY_Y, 'y' }, { KEY_U, 'u' }, { KEY_I, 'i' }, { KEY_O, 'o' }, { KEY_P, 'p' },
    { KEY_A, 'a' }, { KEY_S, 's' }, { KEY_D, 'd' }, { KEY_F, 'f' }, { KEY_G, 'g' },
    { KEY_H, 'h' }, { KEY_J, 'j' }, { KEY_K, 'k' }, { KEY_L, 'l' },
    { KEY_Z, 'z' }, { KEY_X, 'x' }, { KEY_C, 'c' }, { KEY_V, 'v' }, { KEY_B, 'b' },
    { KEY_N, 'n' }, { KEY_M, 'm' },
    { KEY_1, '1' }, { KEY_2, '2' }, { KEY_3, '3' }, { KEY_4, '4' }, { KEY_5, '5' },
    { KEY_6, '6' }, { KEY_7, '7' }, { KEY_8, '8' }, { KEY_9, '9' }, { KEY_0, '0' },
    { KEY_MINUS, '-' }, { KEY_EQUAL, '=' },
    { KEY_LEFTBRACE, '[' }, { KEY_RIGHTBRACE, ']' }, { KEY_BACKSLASH, '\\' },
    { KEY_SPACE, ' ' },
};

static int evdev_kid(unsigned short code, int shift)
{
    for (unsigned i = 0; i < sizeof(letterkeys) / sizeof(letterkeys[0]); i++)
        if (letterkeys[i].code == code)
            return letterkeys[i].kid;
    if (code >= KEY_F1 && code <= KEY_F12)
        return KK_F1 + (code - KEY_F1);
    switch (code) {
    case KEY_ENTER:
    case KEY_KPENTER:   return KK_ENTER;
    case KEY_BACKSPACE: return KK_BACKSPACE;
    case KEY_TAB:       return KK_TAB;
    case KEY_INSERT:    return KK_INSERT;
    case KEY_UP:        return shift ? KK_S_UP : KK_UP;
    case KEY_DOWN:      return shift ? KK_S_DOWN : KK_DOWN;
    case KEY_LEFT:      return shift ? KK_S_LEFT : KK_LEFT;
    case KEY_RIGHT:     return shift ? KK_S_RIGHT : KK_RIGHT;
    case KEY_PAGEUP:    return KK_PGUP;
    case KEY_PAGEDOWN:  return KK_PGDN;
    default:            return KK_NONE;
    }
}

/* ---------------- one-shot CLI ---------------- */
struct ctlname { const char *name; int key; int kind; };  /* kind 0 deck btn, 1 global btn, 2 ten-bit, 3 op5 */
static const struct ctlname ctlname[] = {
    { "play", K_PLAY, 0 }, { "cue", K_CUE, 0 }, { "sync", K_SYNC, 0 },
    { "master", K_MASTER, 0 }, { "temporange", K_TEMPO_RANGE, 0 },
    { "loopin", K_LOOPIN, 0 }, { "loopout", K_LOOPOUT, 0 }, { "reloop", K_RELOOP, 0 },
    { "load", K_LOAD, 0 }, { "jog", K_JOG_ROT, 0 }, { "jogtouch", K_JOG_TOUCH, 0 },
    { "pad", K_PAD1, 0 }, { "hotcue", K_HOTCUE, 0 }, { "autoloop", K_ALOOP, 0 },
    { "sliploop", K_SLIPLOOP, 0 }, { "beatjump", K_BEATJUMP, 0 },
    { "menu", K_MENU, 1 }, { "source", K_SOURCE, 1 }, { "browse", K_BROWSE, 1 },
    { "usb1", K_USB1, 1 }, { "rekordbox", K_REKORDBOX, 1 }, { "link", K_LINK, 1 },
    { "info", K_INFO, 1 }, { "taglist", K_TAGLIST, 1 }, { "select", K_SELECTOR, 1 },
    { "back", K_BACK, 1 }, { "bfx", K_BFX, 1 },
    { "fader", K_FADER, 2 }, { "trim", K_TRIM, 2 }, { "eqhi", K_EQH, 2 },
    { "eqmid", K_EQM, 2 }, { "eqlow", K_EQL, 2 }, { "crossfader", K_XFADER, 2 },
    { "color", K_COLOR, 3 }, { "depth", K_DEPTH, 3 },
};
#define NCTL ((int)(sizeof(ctlname) / sizeof(ctlname[0])))

static void list_map(void)
{
    printf("buttons:\n");
    for (int i = 0; i < NBTN; i++) {
        char k[12];
        int kid = btnmap[i].kid;
        if (kid == KK_ENTER)          snprintf(k, sizeof(k), "ENTER");
        else if (kid == KK_BACKSPACE) snprintf(k, sizeof(k), "BACKSPACE");
        else if (kid >= KK_F1)        snprintf(k, sizeof(k), "F%d", kid - KK_F1 + 1);
        else                          snprintf(k, sizeof(k), "'%c'", kid);
        printf("  %-9s -> 0x%04x  ch %-6s %s\n", k, btnmap[i].key,
               btnmap[i].ch ? (btnmap[i].ch == 1 ? "1" : "2") : "active",
               btnmap[i].name);
    }
    printf("arrows:  UP/DOWN browse +/-1   PGUP/PGDN browse +/-10\n"
           "         LEFT/RIGHT jog active deck (SHIFT = coarse)\n"
           "TAB switches active deck, INSERT toggles the mixer layer\n");
    printf("mixer layer (INSERT):\n");
    for (int i = 0; i < NVAL; i++)
        printf("  '%c' %-24s 0x%04x op %-6s step %+d\n", valmap[i].kid,
               valmap[i].name, valmap[i].key,
               valmap[i].op == OP_VALUE ? "VALUE" : "ROTATE", valmap[i].step);
    printf("one-shot: rbkeyd send <control> [deck] [0..1023]\n");
}

static int cli_send(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: rbkeyd send <control> [deck] [0..1023]\n");
        return 2;
    }
    if (strcmp(argv[0], "list") == 0) {
        for (int i = 0; i < NCTL; i++)
            printf("  %s\n", ctlname[i].name);
        return 0;
    }
    for (int i = 0; i < NCTL; i++) {
        int kind, key, deck, val;
        if (strcmp(argv[0], ctlname[i].name) != 0)
            continue;
        kind = ctlname[i].kind;
        key = ctlname[i].key;

        if (kind == 1) {
            tap(key, 1);
            logmsg("send %s (global)\n", argv[0]);
            return 0;
        }
        if (kind == 0) {
            if (strcmp(argv[0], "pad") == 0) {
                int pad = (argc > 1) ? atoi(argv[1]) : 1;
                deck = (argc > 2) ? atoi(argv[2]) : active_deck;
                if (pad < 1 || pad > 8) pad = 1;
                if (deck != 1 && deck != 2) deck = active_deck;
                tap(K_PAD1 + pad - 1, deck);
                logmsg("send pad %d deck %d\n", pad, deck);
                return 0;
            }
            if (strcmp(argv[0], "jog") == 0) {
                deck = (argc > 1) ? atoi(argv[1]) : active_deck;
                if (deck != 1 && deck != 2) deck = active_deck;
                do_jog(deck, (argc > 2) ? atoi(argv[2]) : 1);
                return 0;
            }
            deck = (argc > 1) ? atoi(argv[1]) : active_deck;
            if (deck != 1 && deck != 2) deck = active_deck;
            tap(key, deck);
            logmsg("send %s deck %d\n", argv[0], deck);
            return 0;
        }
        if (argc >= 3) { deck = atoi(argv[1]); val = atoi(argv[2]); }
        else           { deck = active_deck; val = (argc >= 2) ? atoi(argv[1]) : 512; }
        if (deck != 1 && deck != 2) deck = active_deck;
        if (val < 0) val = 0;
        if (val > 1023) val = 1023;
        if (kind == 2)
            send_ctrl(key, OP_ROTATE, deck, val, (float)val / 1023.0f, val);
        else
            send_ctrl(key, OP_VALUE, deck, val, (float)val / 1023.0f, val);
        logmsg("send %s deck %d value %d\n", argv[0], deck, val);
        return 0;
    }
    fprintf(stderr, "rbkeyd: unknown control '%s' (try: rbkeyd send list)\n", argv[0]);
    return 1;
}

/* ---------------- main ---------------- */
int main(int argc, char **argv)
{
    const char *forced_dev = NULL;
    int fd = -1, opt;

    if (argc > 1 && strcmp(argv[1], "list") == 0) { list_map(); return 0; }
    if (argc > 1 && strcmp(argv[1], "send") == 0) return cli_send(argc - 2, argv + 2);

    while ((opt = getopt(argc, argv, "vd:n:")) != -1) {
        switch (opt) {
        case 'v': opt_verbose = 1; break;
        case 'd': forced_dev = optarg; break;
        case 'n': opt_want_name = optarg; break;   /* match EVIOCGNAME substring */
        default:
            fprintf(stderr, "usage: %s [-v] [-n name] [-d /dev/input/eventN] | list | send ...\n",
                    argv[0]);
            return 2;
        }
    }

    if (access(fifo_key, F_OK) != 0)
        logmsg("rbkeyd: %s not there yet - will start feeding it as soon as rbp "
               "creates it (start-rb.sh)\n", fifo_key);

    unsigned char buf[256];
    int shift = 0;
    char cur_path[128] = "";
    long long last_scan = 0;

    if (forced_dev) {
        fd = open(forced_dev, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            logmsg("rbkeyd: cannot open %s: %s\n", forced_dev, strerror(errno));
            return 1;
        }
        snprintf(cur_path, sizeof(cur_path), "%s", forced_dev);
        logmsg("rbkeyd: using %s\n", forced_dev);
    }

    for (;;) {
        struct pollfd pfd;
        int pr;
        ssize_t n;
        long long now = now_ms();

        /* (re)scan for the best keyboard: a physical one that got plugged in
         * later replaces a virtual/remapped device, and an unplugged one is
         * dropped.  forced -d never rescans. */
        if (!forced_dev && (fd < 0 || now - last_scan >= 2000)) {
            struct kbd_choice c;
            last_scan = now;
            if (scan_keyboard(&c)) {
                if (fd < 0 || strcmp(c.path, cur_path) != 0) {
                    if (fd >= 0) {
                        logmsg("rbkeyd: switching keyboard %s -> %s\n",
                               cur_path, c.path);
                        close(fd);
                    }
                    fd = open(c.path, O_RDONLY | O_NONBLOCK);
                    if (fd < 0) {
                        logmsg("rbkeyd: cannot open %s: %s\n", c.path, strerror(errno));
                        fd = -1;
                    } else {
                        snprintf(cur_path, sizeof(cur_path), "%s", c.path);
                        logmsg("rbkeyd: keyboard %s (%s)\n", c.path, c.name);
                    }
                }
            } else if (fd < 0) {
                static int told = 0;
                if (!told) {
                    logmsg("rbkeyd: waiting for a keyboard on /dev/input ...\n");
                    told = 1;
                }
            }
        }
        if (fd < 0) {
            sleep(1);
            continue;
        }

        pfd.fd = fd;
        pfd.events = POLLIN;
        pr = poll(&pfd, 1, 40);
        jog_tick();
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            fd = -1;
            continue;
        }
        if (pr == 0)
            continue;

        n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR))
                continue;
            logmsg("rbkeyd: keyboard %s gone, rescanning ...\n", cur_path);
            close(fd);
            fd = -1;
            continue;
        }
        for (ssize_t i = 0; i + (ssize_t)sizeof(struct input_event) <= n;
             i += sizeof(struct input_event)) {
            struct input_event *ie = (struct input_event *)(buf + i);
            int down, kid;
            if (ie->type != EV_KEY)
                continue;
            if (ie->code == KEY_LEFTSHIFT || ie->code == KEY_RIGHTSHIFT) {
                shift = (ie->value != 0);
                continue;
            }
            down = (ie->value != 0);            /* 1 = press, 2 = auto-repeat */
            kid = evdev_kid(ie->code, shift);
            if (kid != KK_NONE)
                handle_key(kid, down);
        }
    }
    return 0;
}
