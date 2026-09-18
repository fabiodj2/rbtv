/* fakekbd.c - create a virtual keyboard through uinput and inject key presses,
 * so the rbkeyd -> FIFO -> keyshim chain can be tested end-to-end without a
 * physical keyboard attached (the Chromebit has one USB-A port, usually taken
 * by the DDJ-400).
 *
 * Usage (on the Chromebit, as root):
 *   fakekbd p c 1 up enter f1        # press+release each in order
 *   fakekbd -w 200 w w w             # 200 ms between keys
 *   fakekbd -t 500 left              # hold: 500 ms press before release
 *
 * Arguments: a single character ('a'..'z', '0'..'9', '[', ']', '\\', '-', '='),
 * a decimal evdev key code, or one of the names below.  The device is created
 * as "rbkeyd-test-keyboard" and destroyed on exit, which also exercises
 * rbkeyd's hot-plug handling.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/uinput.h>
#include <linux/input.h>

/* evdev codes are not in alphabetical order (QWERTY row 16..25, ASDF row
 * 30..38, ZXCV row 44..50), so map character keys explicitly. */
struct namecode { const char *name; int code; };
static const struct namecode letters[] = {
    { "q", KEY_Q }, { "w", KEY_W }, { "e", KEY_E }, { "r", KEY_R }, { "t", KEY_T },
    { "y", KEY_Y }, { "u", KEY_U }, { "i", KEY_I }, { "o", KEY_O }, { "p", KEY_P },
    { "a", KEY_A }, { "s", KEY_S }, { "d", KEY_D }, { "f", KEY_F }, { "g", KEY_G },
    { "h", KEY_H }, { "j", KEY_J }, { "k", KEY_K }, { "l", KEY_L },
    { "z", KEY_Z }, { "x", KEY_X }, { "c", KEY_C }, { "v", KEY_V }, { "b", KEY_B },
    { "n", KEY_N }, { "m", KEY_M },
    { "1", KEY_1 }, { "2", KEY_2 }, { "3", KEY_3 }, { "4", KEY_4 }, { "5", KEY_5 },
    { "6", KEY_6 }, { "7", KEY_7 }, { "8", KEY_8 }, { "9", KEY_9 }, { "0", KEY_0 },
    { "-", KEY_MINUS }, { "=", KEY_EQUAL },
    { "[", KEY_LEFTBRACE }, { "]", KEY_RIGHTBRACE }, { "\\", KEY_BACKSLASH },
    { " ", KEY_SPACE },
};
static const struct namecode names[] = {
    { "enter", KEY_ENTER }, { "backspace", KEY_BACKSPACE }, { "tab", KEY_TAB },
    { "insert", KEY_INSERT }, { "space", KEY_SPACE },
    { "up", KEY_UP }, { "down", KEY_DOWN }, { "left", KEY_LEFT }, { "right", KEY_RIGHT },
    { "pgup", KEY_PAGEUP }, { "pgdn", KEY_PAGEDOWN },
    { "shift", KEY_LEFTSHIFT }, { "lshift", KEY_LEFTSHIFT }, { "rshift", KEY_RIGHTSHIFT },
    { "f1", KEY_F1 }, { "f2", KEY_F2 }, { "f3", KEY_F3 }, { "f4", KEY_F4 },
    { "f5", KEY_F5 }, { "f6", KEY_F6 }, { "f7", KEY_F7 }, { "f8", KEY_F8 },
    { "f9", KEY_F9 }, { "f10", KEY_F10 }, { "f11", KEY_F11 }, { "f12", KEY_F12 },
    { "minus", KEY_MINUS }, { "equal", KEY_EQUAL },
    { "lbracket", KEY_LEFTBRACE }, { "rbracket", KEY_RIGHTBRACE },
    { "backslash", KEY_BACKSLASH },
};

static int keycode_of(const char *s)
{
    unsigned i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(names[i].name, s) == 0)
            return names[i].code;
    for (i = 0; i < sizeof(letters) / sizeof(letters[0]); i++)
        if (strcmp(letters[i].name, s) == 0)
            return letters[i].code;
    if (strlen(s) == 1 && isupper((unsigned char)s[0])) {
        char lower[2];
        lower[0] = (char)tolower((unsigned char)s[0]);
        lower[1] = '\0';
        return keycode_of(lower);
    }
    if (isdigit((unsigned char)s[0]))
        return atoi(s);
    return -1;
}

static void emit(int fd, int type, int code, int val)
{
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    ie.type = type;
    ie.code = code;
    ie.value = val;
    if (write(fd, &ie, sizeof(ie)) < 0)
        fprintf(stderr, "fakekbd: write: %s\n", strerror(errno));
}

static void press(int fd, int code, int hold_ms)
{
    emit(fd, EV_KEY, code, 1);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(hold_ms * 1000);
    emit(fd, EV_KEY, code, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

int main(int argc, char **argv)
{
    int fd, i, wait_ms = 120, hold_ms = 60, settle_s = 1, keepalive_s = 0;
    struct uinput_setup us;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            wait_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            hold_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            settle_s = atoi(argv[++i]);   /* wait for rbkeyd to pick the device up */
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            keepalive_s = atoi(argv[++i]); /* hold the device open after the keys */
        } else {
            break;
        }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: %s [-w ms] [-t hold_ms] [-s settle_s] [-k keepalive_s] "
                        "<key>...  (chars, 'name' or keycode; try 'list')\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[i], "list") == 0) {
        unsigned k;
        for (k = 0; k < sizeof(names) / sizeof(names[0]); k++)
            printf("  %-10s %d\n", names[k].name, names[k].code);
        return 0;
    }

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "fakekbd: open /dev/uinput: %s\n", strerror(errno));
        return 1;
    }
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        fprintf(stderr, "fakekbd: UI_SET_EVBIT: %s\n", strerror(errno));
        return 1;
    }
    for (int k = 0; k < KEY_MAX; k++)
        ioctl(fd, UI_SET_KEYBIT, k);

    memset(&us, 0, sizeof(us));
    snprintf(us.name, sizeof(us.name), "rbkeyd-test-keyboard");
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x1234;
    us.id.product = 0x5678;
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "fakekbd: UI_DEV_SETUP/CREATE: %s\n", strerror(errno));
        return 1;
    }
    /* give devtmpfs / any udev a moment to create /dev/input/eventN, and
     * rbkeyd time to notice the new keyboard (it rescans every 2 s) */
    sleep(settle_s);

    for (; i < argc; i++) {
        int code = keycode_of(argv[i]);
        if (code < 0) {
            fprintf(stderr, "fakekbd: unknown key '%s'\n", argv[i]);
            continue;
        }
        printf("fakekbd: key %s (code %d)\n", argv[i], code);
        press(fd, code, hold_ms);
        usleep(wait_ms * 1000);
    }

    usleep(200000);
    if (keepalive_s > 0)
        sleep(keepalive_s);
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
