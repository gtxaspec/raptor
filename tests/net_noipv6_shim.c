/*
 * net_noipv6_shim.c -- LD_PRELOAD simulation of a kernel built without
 * IPv6, test-only.
 *
 * Such a kernel refuses socket(AF_INET6, ...) outright with EAFNOSUPPORT;
 * that single behavior is the whole simulation, and everything downstream
 * (the rss_net.h fallback, rsr's rebind, daemon logs) follows from it.
 * Real IPv6-less targets exist -- OpenIPC ships such kernels -- but the
 * build hosts do not, hence the shim.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <sys/socket.h>

int socket(int domain, int type, int protocol)
{
	static int (*real)(int, int, int);

	if (domain == AF_INET6) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	if (!real)
		real = dlsym(RTLD_NEXT, "socket");
	return real(domain, type, protocol);
}
