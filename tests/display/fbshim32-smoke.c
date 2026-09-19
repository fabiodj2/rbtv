#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#define WIDTH 1280
#define HEIGHT 800
#define FRAME_BYTES (WIDTH * HEIGHT * 4)

int main(void)
{
    static const uint32_t colors[8] = {
        0x00ffffff,
        0x00ffff00,
        0x0000ffff,
        0x0000ff00,
        0x00ff00ff,
        0x00ff0000,
        0x000000ff,
        0x00000000
    };

    struct fb_fix_screeninfo fixed = {0};
    struct fb_var_screeninfo variable = {0};
    int fd = open("/dev/fb0", O_RDWR);

    if (fd < 0) {
        perror("open /dev/fb0");
        return 1;
    }

    if (ioctl(fd, FBIOGET_FSCREENINFO, &fixed) ||
        ioctl(fd, FBIOGET_VSCREENINFO, &variable)) {
        perror("framebuffer ioctl");
        close(fd);
        return 1;
    }

    if (variable.xres != WIDTH ||
        variable.yres != HEIGHT ||
        variable.bits_per_pixel != 32 ||
        fixed.line_length != WIDTH * 4 ||
        fixed.smem_len != FRAME_BYTES) {
        printf("FAIL geometry %ux%u %ubpp stride=%u bytes=%u\n",
               variable.xres,
               variable.yres,
               variable.bits_per_pixel,
               fixed.line_length,
               fixed.smem_len);
        close(fd);
        return 1;
    }

    uint32_t *pixels = mmap(0,
                            FRAME_BYTES,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED,
                            fd,
                            0);

    if (pixels == MAP_FAILED) {
        perror("mmap framebuffer");
        close(fd);
        return 1;
    }

    for (unsigned y = 0; y < HEIGHT; y++) {
        for (unsigned x = 0; x < WIDTH; x++)
            pixels[y * WIDTH + x] = colors[x / 160];
    }

    if (msync(pixels, FRAME_BYTES, MS_SYNC)) {
        perror("msync framebuffer");
        munmap(pixels, FRAME_BYTES);
        close(fd);
        return 1;
    }

    munmap(pixels, FRAME_BYTES);
    close(fd);

    puts("PASS arm32-fbshim-1280x800x32");
    return 0;
}
