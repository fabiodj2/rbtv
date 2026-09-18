/* compat_nonshared.c — extra symbols for the RX3 glibc-2.13 sysroot.
 *
 * The RX3 runtime does not export `fstat` or `__fdelt_chk`, and modern
 * glibc moved `atexit` into libc_nonshared.a. We append this object to the
 * sysroot's libc_nonshared.a so DirectFB (and anything else) links cleanly
 * and carries only GLIBC_2.4/2.7 symbol versions.
 *
 * Build (soft-float):
 *   arm-linux-gnueabi-gcc -O2 -march=armv5t -mfloat-abi=soft -fPIC \
 *       -c compat_nonshared.c -o compat_nonshared.o
 *   ar r $SYSROOT/usr/lib/libc_nonshared.a compat_nonshared.o
 */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

int fstat(int fd, struct stat *buf)
{
	return (int)syscall(SYS_fstat64, fd, buf);
}

long int __fdelt_chk(long int d)
{
	return (long int)((unsigned long)d / (8 * sizeof(long)));
}
