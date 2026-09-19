#include <asm/ioctl.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <errno.h>
#include <dlfcn.h>

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

int ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    void *argument;

    va_start(arguments, request);
    argument = va_arg(arguments, void *);
    va_end(arguments);

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

    if (request == FBIOPAN_DISPLAY ||
        request == FBIOPUTCMAP ||
        request == FBIOGETCMAP ||
        request == FBIOBLANK ||
        request == FBIO_WAITFORVSYNC)
        return 0;

    register long r0 asm("r0") = fd;
    register long r1 asm("r1") = request;
    register void *r2 asm("r2") = argument;
    register long r7 asm("r7") = 54;

    asm volatile(
        "svc 0"
        : "+r"(r0)
        : "r"(r1), "r"(r2), "r"(r7)
        : "memory"
    );

    if (r0 < 0 && r0 >= -4095) {
        errno = (int)-r0;
        return -1;
    }

    return (int)r0;
}

void *dlopen(const char *name, int flags)
{
    static void *(*real_dlopen)(const char *, int);

    if (!real_dlopen)
        real_dlopen = dlsym((void *)-1, "dlopen");

    return real_dlopen(name, flags & ~8);
}
