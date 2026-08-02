/*
 * ric_adc_shim.c -- LD_PRELOAD fake for the SU_ADC device, test-only.
 *
 * ric's ADC trigger opens /dev/ingenic_adc_aux_N (or jz_adc_aux_N), enables
 * it with ioctl(fd, 0) and reads a 4-byte value per poll. None of that
 * exists on a build host, so redirect the open to the file named by
 * RIC_ADC_BACKING, accept the enable/disable ioctls, and serve reads from
 * offset 0 -- the harness rewrites the file to move the "photoresistor".
 *
 * Loaded after the ASan runtime; interceptor chains end here via
 * RTLD_NEXT either way.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int adc_fd = -1;

static int is_adc_path(const char *path)
{
	return strncmp(path, "/dev/ingenic_adc_aux_", 21) == 0 ||
	       strncmp(path, "/dev/jz_adc_aux_", 16) == 0;
}

static int shim_open(const char *fn, const char *path, int flags, mode_t mode)
{
	static int (*real)(const char *, int, ...);
	const char *backing;

	if (!real)
		real = dlsym(RTLD_NEXT, "open");

	backing = getenv("RIC_ADC_BACKING");
	if (backing && is_adc_path(path)) {
		int fd = real(backing, O_RDONLY);
		if (fd >= 0)
			adc_fd = fd;
		return fd;
	}
	(void)fn;
	return real(path, flags, mode);
}

int open(const char *path, int flags, ...)
{
	va_list ap;
	mode_t mode = 0;

	if (flags & (O_CREAT | O_TMPFILE)) {
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	return shim_open("open", path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
	va_list ap;
	mode_t mode = 0;

	if (flags & (O_CREAT | O_TMPFILE)) {
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	return shim_open("open64", path, flags, mode);
}

int ioctl(int fd, unsigned long request, ...)
{
	static int (*real)(int, unsigned long, ...);
	va_list ap;
	void *arg;

	if (fd >= 0 && fd == adc_fd)
		return 0; /* enable (0) and disable (1) both succeed */

	if (!real)
		real = dlsym(RTLD_NEXT, "ioctl");
	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	return real(fd, request, arg);
}

ssize_t read(int fd, void *buf, size_t count)
{
	static ssize_t (*real)(int, void *, size_t);

	if (fd >= 0 && fd == adc_fd)
		return pread(fd, buf, count < 4 ? count : 4, 0);

	if (!real)
		real = dlsym(RTLD_NEXT, "read");
	return real(fd, buf, count);
}

int close(int fd)
{
	static int (*real)(int);

	if (!real)
		real = dlsym(RTLD_NEXT, "close");
	if (fd >= 0 && fd == adc_fd)
		adc_fd = -1;
	return real(fd);
}
