/*
 * ric_shim_probe.c -- unit check for the adc preload shim, test-only.
 *
 * Exercises the shim through both entries a compiler may emit for
 * read(): the plain symbol and the _FORTIFY_SOURCE __read_chk variant
 * that bypassed it in issue #18. Both are called explicitly, so the
 * check does not depend on what this host's compiler happens to
 * fortify -- the exact blindness that let the bug through. Also pins
 * the rewind semantics the scenario relies on: a rewritten backing
 * file must be seen by the very next poll.
 *
 * Run under LD_PRELOAD of the shim with RIC_ADC_BACKING set.
 * Exits 0 when every read returns 4 bytes of the expected value.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern ssize_t __read_chk(int fd, void *buf, size_t nbytes, size_t buflen);

static int put(const char *backing, int value)
{
	FILE *f = fopen(backing, "wb");

	if (!f)
		return -1;
	fwrite(&value, sizeof(value), 1, f);
	fclose(f);
	return 0;
}

static int expect(const char *what, ssize_t n, int got, int want)
{
	if (n == (ssize_t)sizeof(int) && got == want)
		return 0;
	fprintf(stderr, "shim probe: %s returned n=%zd v=%d, want %d\n", what, n, got, want);
	return 1;
}

int main(void)
{
	const char *backing = getenv("RIC_ADC_BACKING");
	int fails = 0;
	ssize_t n;
	int v;

	if (!backing) {
		fprintf(stderr, "shim probe: RIC_ADC_BACKING not set\n");
		return 2;
	}
	if (put(backing, 700) < 0) {
		fprintf(stderr, "shim probe: cannot write %s\n", backing);
		return 2;
	}

	int fd = open("/dev/ingenic_adc_aux_0", O_RDONLY | O_CLOEXEC);

	if (fd < 0) {
		fprintf(stderr, "shim probe: open was not intercepted\n");
		return 1;
	}
	if (ioctl(fd, 0) < 0) {
		fprintf(stderr, "shim probe: enable ioctl was not accepted\n");
		close(fd);
		return 1;
	}

	v = -1;
	n = read(fd, &v, sizeof(v));
	fails += expect("read #1", n, v, 700);

	v = -1;
	n = read(fd, &v, sizeof(v));
	fails += expect("read #2 (must rewind, not hit EOF)", n, v, 700);

	v = -1;
	n = __read_chk(fd, &v, sizeof(v), sizeof(v));
	fails += expect("__read_chk (fortified entry, issue #18)", n, v, 700);

	if (put(backing, 100) < 0)
		return 2;
	v = -1;
	n = __read_chk(fd, &v, sizeof(v), sizeof(v));
	fails += expect("__read_chk after backing rewrite", n, v, 100);

	close(fd);
	return fails ? 1 : 0;
}
