/* rbtv_common.h - helpers shared by the LD_PRELOAD shims (keyshim, audioshim).
 *
 *  - raw-syscall file helpers (no libc stdio, safe to use before main())
 *  - MD5 of a file, used to refuse to touch an `rbp` we were not written for
 *  - locating the real `rbp` image (rbp is started as
 *    `/lib/ld-linux.so.3 /root/pdj/rbp -a`, so /proc/self/exe is the loader)
 *  - the shared kill-switch (env var / flag file under /etc/rbtv)
 *
 * Header-only, `static`, C89-ish: it must build with the soft-float EABI5
 * toolchain against glibc 2.13 (only GLIBC_2.4 symbols).
 */
#ifndef RBTV_COMMON_H
#define RBTV_COMMON_H

#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>

#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0400000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif

static int rbtv_open(const char *p, int flags)
{
    return (int)syscall(SYS_openat, AT_FDCWD, p, flags | O_CLOEXEC, 0);
}
static ssize_t rbtv_read(int fd, void *buf, size_t n)
{
    return (ssize_t)syscall(SYS_read, fd, buf, n);
}
static void rbtv_close(int fd)
{
    (void)syscall(SYS_close, fd);
}

/* ------------------------------------------------------------------ MD5 --- */

typedef struct {
    uint32_t a, b, c, d;
    uint64_t len;
    unsigned char buf[64];
    size_t used;
} rbtv_md5_t;

#define RBTV_ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void rbtv_md5_block(rbtv_md5_t *m, const unsigned char *p)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };
    static const unsigned char S[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };
    uint32_t w[16], a = m->a, b = m->b, c = m->c, d = m->d, f, t;
    int i, g;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
               ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    for (i = 0; i < 64; i++) {
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
        t = d; d = c; c = b;
        b = b + RBTV_ROL(a + f + K[i] + w[g], S[i]);
        a = t;
    }
    m->a += a; m->b += b; m->c += c; m->d += d;
}

static void rbtv_md5_init(rbtv_md5_t *m)
{
    m->a = 0x67452301; m->b = 0xefcdab89; m->c = 0x98badcfe; m->d = 0x10325476;
    m->len = 0;
    m->used = 0;
}

static void rbtv_md5_update(rbtv_md5_t *m, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    m->len += n;
    while (n) {
        size_t take = 64 - m->used;
        if (take > n) take = n;
        memcpy(m->buf + m->used, p, take);
        m->used += take; p += take; n -= take;
        if (m->used == 64) { rbtv_md5_block(m, m->buf); m->used = 0; }
    }
}

/* writes 32 lowercase hex chars + NUL */
static void rbtv_md5_final(rbtv_md5_t *m, char out[33])
{
    static const char hex[] = "0123456789abcdef";
    uint64_t bits = m->len * 8;
    unsigned char pad = 0x80, z = 0, lenb[8];
    uint32_t v[4];
    int i, j;

    rbtv_md5_update(m, &pad, 1);
    while (m->used != 56) rbtv_md5_update(m, &z, 1);
    for (i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (8 * i));
    rbtv_md5_update(m, lenb, 8);
    v[0] = m->a; v[1] = m->b; v[2] = m->c; v[3] = m->d;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            unsigned char byte = (unsigned char)(v[i] >> (8 * j));
            out[(i * 4 + j) * 2]     = hex[byte >> 4];
            out[(i * 4 + j) * 2 + 1] = hex[byte & 15];
        }
    out[32] = 0;
}

/* md5 of a whole file; 0 on success, -1 if it cannot be read */
static int rbtv_md5_file(const char *path, char out[33])
{
    static unsigned char buf[65536];   /* one-shot caller (guarded by once) */
    rbtv_md5_t m;
    int fd = rbtv_open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) return -1;
    rbtv_md5_init(&m);
    while ((n = rbtv_read(fd, buf, sizeof(buf))) > 0)
        rbtv_md5_update(&m, buf, (size_t)n);
    rbtv_close(fd);
    if (n < 0) return -1;
    rbtv_md5_final(&m, out);
    return 0;
}

/* ------------------------------------------------------- locate the rbp --- */

static int rbtv_is_rbp_name(const char *s)
{
    const char *b = strrchr(s, '/');
    b = b ? b + 1 : s;
    return strcmp(b, "rbp") == 0;
}

/* Finds the path of the `rbp` image this process runs.  The player is started
 * through the loader (`ld-linux.so.3 /root/pdj/rbp -a`), so look for an argv
 * entry whose basename is exactly "rbp" (argv[0] or argv[1]); /proc/self/exe is
 * only used when argv[0] itself is rbp.  Returns 1 and fills `out`, else 0.
 * The match is exact on the basename, unlike a substring test, so a path such
 * as /tmp/rbp-test or `grep rbp` is not mistaken for the player. */
static int rbtv_find_rbp_path(char *out, size_t outsz)
{
    static char cmd[1024];
    int fd = rbtv_open("/proc/self/cmdline", O_RDONLY);
    ssize_t n;
    size_t off = 0;
    int idx = 0;

    if (fd < 0) return 0;
    n = rbtv_read(fd, cmd, sizeof(cmd) - 1);
    rbtv_close(fd);
    if (n <= 0) return 0;
    cmd[n] = 0;
    while (off < (size_t)n && idx < 2) {
        const char *arg = cmd + off;
        size_t len = strlen(arg);
        if (len && rbtv_is_rbp_name(arg)) {
            if (idx == 0) {
                /* argv[0] is rbp itself: the image is /proc/self/exe */
                if (outsz < 15) return 0;
                strcpy(out, "/proc/self/exe");
            } else {
                if (len >= outsz) return 0;
                memcpy(out, arg, len + 1);
            }
            return 1;
        }
        off += len + 1;
        idx++;
    }
    return 0;
}

/* ------------------------------------------------------- kill switch ----- */

static int rbtv_file_exists(const char *path)
{
    int fd = rbtv_open(path, O_RDONLY);
    if (fd < 0) return 0;
    rbtv_close(fd);
    return 1;
}

/* A shim is disabled when its own env var is "1", or either
 * /etc/rbtv/disable-<name> or /etc/rbtv/disable-shims exists (inside the
 * chroot; start-rb.sh maintains the host-side flag and the crash counter). */
static int rbtv_shim_disabled(const char *name, const char *envvar)
{
    char path[96];
    const char *e = getenv(envvar);

    if (e && e[0] == '1') return 1;
    if (rbtv_file_exists("/etc/rbtv/disable-shims")) return 1;
    if (strlen(name) + 24 < sizeof(path)) {
        strcpy(path, "/etc/rbtv/disable-");
        strcat(path, name);
        if (rbtv_file_exists(path)) return 1;
    }
    return 0;
}

#endif /* RBTV_COMMON_H */
