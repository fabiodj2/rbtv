#include <asm/ioctl.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

__asm__(".symver dlsym, dlsym@GLIBC_2.4");

#define RX3_WIDTH 1280
#define RX3_HEIGHT 800
#define RX3_BYTES_PER_PIXEL 4
#define RX3_FRAME_BYTES (RX3_WIDTH * RX3_HEIGHT * RX3_BYTES_PER_PIXEL)

static void clear_bytes(void *pointer, unsigned size)
{
    unsigned char *output = pointer;

    while (size--)
        *output++ = 0;
}

static int klog_fd = -1;

static void klog_open(void)
{
    if (klog_fd < 0) {
        klog_fd = syscall(SYS_openat, AT_FDCWD,
                          "/tmp/fbshim32.log",
                          O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
}

static void klog_hex(const char *prefix, unsigned long v)
{
    char buf[64];
    int i;
    static const char hex[] = "0123456789abcdef";

    klog_open();
    if (klog_fd < 0)
        return;

    for (i = 0; prefix[i] && i < 32; i++)
        buf[i] = prefix[i];
    buf[i++] = '0';
    buf[i++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4)
        buf[i++] = hex[(v >> shift) & 0xf];
    buf[i++] = '\n';

    syscall(SYS_write, klog_fd, buf, i);
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    void *argument;

    va_start(arguments, request);
    argument = va_arg(arguments, void *);
    va_end(arguments);


    /* ALSA_IOCTL_PASSTHROUGH
     *
     * This preload also sees ioctls issued by libasound. Never treat them as
     * framebuffer requests. Pass the ALSA ioctl families directly to Linux:
     * PCM(A), control(U), hwdep(H), raw MIDI(W), timer(T), sequencer(S).
     */
    {
        unsigned int magic = (unsigned int)((request >> 8) & 0xff);

        if (magic == 'A' ||
            magic == 'U' ||
            magic == 'H' ||
            magic == 'W' ||
            magic == 'T' ||
            magic == 'S')
            return syscall(SYS_ioctl, fd, request, argument);
    }

    if (((request >> 8) & 255) == 0x70) {
        if ((request & 0x80000000UL) && argument) {
            unsigned size = (request >> 16) & 0x3fff;

            if (size <= 64)
                clear_bytes(argument, size);
        }
        return 0;
    }

    if (request == 0x80046b00) {
        *(unsigned *)argument = 3;
        return 0;
    }

    if (request == 0x80026b01) {
        *(unsigned *)argument = 3900;
        return 0;
    }

    if (request == 0x40046b00 || request == 0x40026b01)
        return 0;

    if (request == FBIOGET_FSCREENINFO) {
        struct fb_fix_screeninfo *info = argument;

        clear_bytes(info, sizeof(*info));
        info->id[0] = 'R';
        info->id[1] = 'X';
        info->id[2] = '3';
        info->smem_len = RX3_FRAME_BYTES;
        info->type = FB_TYPE_PACKED_PIXELS;
        info->visual = FB_VISUAL_TRUECOLOR;
        info->line_length = RX3_WIDTH * RX3_BYTES_PER_PIXEL;
        return 0;
    }

    if (request == FBIOGET_VSCREENINFO) {
        struct fb_var_screeninfo *info = argument;

        clear_bytes(info, sizeof(*info));
        info->xres = RX3_WIDTH;
        info->yres = RX3_HEIGHT;
        info->xres_virtual = RX3_WIDTH;
        info->yres_virtual = RX3_HEIGHT;
        info->bits_per_pixel = 32;
        info->red.offset = 16;
        info->red.length = 8;
        info->green.offset = 8;
        info->green.length = 8;
        info->blue.offset = 0;
        info->blue.length = 8;
        info->height = 135;
        info->width = 216;
        info->pixclock = 20000;
        info->left_margin = 40;
        info->right_margin = 40;
        info->upper_margin = 10;
        info->lower_margin = 10;
        info->hsync_len = 20;
        info->vsync_len = 3;
        return 0;
    }

    if (request == FBIOPUT_VSCREENINFO) {
        struct fb_var_screeninfo *info = argument;

        if (!info || info->bits_per_pixel != 32) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }

    /* Ioctls que o DirectFB chama e precisamos responder consistentemente.
     * NUNCA fazer passthrough para o kernel: o framebuffer real do host
     * tem geometria e capacidades diferentes, e o DirectFB se confunde. */
    if (request == FBIOPAN_DISPLAY ||
        request == FBIOPUTCMAP ||
        request == FBIOGETCMAP ||
        request == FBIOBLANK ||
        request == FBIO_WAITFORVSYNC)
        return 0;

    /* FBIOGET_VBLANK (0x8927): simular um vblank real.
     *
     * O DirectFB aguarda que o contador de vblank incremente entre chamadas
     * para sincronizar o refresh da tela.  Retornar zeros faz ele entrar
     * em loop esperando um vblank que nunca vem, e a layer DirectFB fica
     * em estado inválido, levando ao SIGSEGV em init_Resource.
     *
     * Simulamos um vblank a ~60 Hz baseado em CLOCK_MONOTONIC. */
    if (request == 0x8927UL) {
        struct fb_vblank *vb = argument;
        struct timespec ts;

        if (!vb)
            return -1;

        clear_bytes(vb, sizeof(*vb));

        syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);

        /* 60 Hz: 16.666 ms por frame. */
        unsigned long long ns = (unsigned long long)ts.tv_sec * 1000000000ULL
                              + (unsigned long long)ts.tv_nsec;
        unsigned long count = (unsigned long)(ns / 16666666ULL);

        /* FB_VBLANK_HAVE_VBLANK (0x1) | FB_VBLANK_HAVE_COUNT (0x2) */
        vb->flags  = 0x1 | 0x2;
        vb->count  = count;
        vb->vcount = count % 1080;
        vb->hcount = 0;

        return 0;
    }

    /* Qualquer outro ioctl: recusar com ENOTTY.
     * Isso evita que dados do framebuffer real do host vazem para o player. */
    {
        static int warned[8];
        static unsigned long warned_ioctls[8];
        int slot = -1;

        for (int i = 0; i < 8; i++) {
            if (warned_ioctls[i] == request) { slot = i; break; }
            if (warned_ioctls[i] == 0 && slot < 0) slot = i;
        }
        if (slot >= 0 && warned[slot] < 3) {
            klog_hex("UNKNOWN ioctl req=", request);
            warned[slot]++;
            if (warned_ioctls[slot] == 0)
                warned_ioctls[slot] = request;
        }
    }
    errno = ENOTTY;
    return -1;

    /* Nunca deveria chegar aqui — todos os caminhos acima retornam. */
    (void)fd;
    errno = ENOTTY;
    return -1;
}

void *dlopen(const char *name, int flags)
{
    static void *(*real_dlopen)(const char *, int);

    if (!real_dlopen)
        real_dlopen = dlsym((void *)-1, "dlopen");

    return real_dlopen(name, flags & ~8);
}
