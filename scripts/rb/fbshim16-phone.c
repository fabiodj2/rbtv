/* fbshim16-phone.c - LD_PRELOAD ioctl shim for rb on the phone.
 * Port of the Prime GO fbshim16 (final, corrected approach):
 * report the logical fb as 1280x800@16bpp RGB565 (line_length 2560),
 * accept FBIOPUT silently, so DirectFB creates RGB16 surfaces matching
 * rbp's native RX3 rendering.
 * Zero-dependency build (-nostdlib, direct svc) so the RX3 glibc-2.13
 * soft-float loader loads it without symbol-version complaints.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stddef.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIO_WAITFORVSYNC   0x4620

struct fb_var_screeninfo {
    unsigned int xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    unsigned int bits_per_pixel, grayscale;
    struct { unsigned int offset, length, msb_right; } red, green, blue, transp;
    unsigned int nonstd;
    unsigned int activate;
    unsigned int height, width;
    unsigned int accel_flags;
    unsigned int pixclock, left_margin, right_margin, upper_margin, lower_margin;
    unsigned int hsync_len, vsync_len, sync, vmode;
    unsigned int rotate;
    unsigned int colorspace;
    unsigned int reserved[4];
};

struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    unsigned int smem_len;
    unsigned int type;
    unsigned int type_aux;
    unsigned int visual;
    unsigned short xpanstep, ypanstep, ywrapstep;
    unsigned int line_length;
    unsigned long mmio_start;
    unsigned int mmio_len;
    unsigned int accel;
    unsigned short capabilities;
    unsigned short reserved[2];
};

static long raw_ioctl(int fd, unsigned long req, void *arg)
{
    register long r0 asm("r0") = fd;
    register long r1 asm("r1") = req;
    register long r2 asm("r2") = (long)arg;
    register long r7 asm("r7") = 54;  /* __NR_ioctl on ARM EABI (2 is fork()!) */
    asm volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory", "cc");
    return r0;
}

int ioctl(int fd, unsigned long request, ...)
{
    void *arg;
    va_list ap;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    switch (request) {
    case FBIOGET_VSCREENINFO: {
        struct fb_var_screeninfo *v = arg;
        int r = (int)raw_ioctl(fd, request, v);
        if (r == 0 && v) {
            v->xres = 1280; v->yres = 800;
            v->xres_virtual = 1280; v->yres_virtual = 800;
            v->bits_per_pixel = 16;
            v->grayscale = 0; v->nonstd = 0;
            v->red.offset = 11; v->red.length = 5; v->red.msb_right = 0;
            v->green.offset = 5; v->green.length = 6; v->green.msb_right = 0;
            v->blue.offset = 0; v->blue.length = 5; v->blue.msb_right = 0;
            v->transp.offset = 0; v->transp.length = 0; v->transp.msb_right = 0;
            v->rotate = 0;
        }
        return r;
    }
    case FBIOGET_FSCREENINFO: {
        struct fb_fix_screeninfo *f = arg;
        int r = (int)raw_ioctl(fd, request, f);
        if (r == 0 && f)
            f->line_length = 2560;      /* 1280 * 2 */
        return r;
    }
    case FBIOPUT_VSCREENINFO:
        return 0;
    default:
        return (int)raw_ioctl(fd, request, arg);
    }
}
