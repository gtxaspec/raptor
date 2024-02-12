#include <fcntl.h>       // for off_t, fcntl
#include <stdio.h>       // for fprintf, fgetc, stderr, size_t, FILE
#include <stdlib.h>      // for abort
//#include <sys/mman.h>    // for mmap
#include <sys/stat.h>    // for fstat, stat
#include "bits/fcntl.h"  // for F_GETFL
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdint.h>

/**
 * Shim to create missing function calls in the ingenic libimp library
 */

#define DEBUG 1  // Set this to 1 to enable debug output or 0 to disable

#if DEBUG
#define DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...) (void)0
#endif

void __pthread_register_cancel(void *buf) {
    fprintf(stderr, "[WARNING] Called __pthread_register_cancel. This is a shim and does nothing.\n");
}

void __pthread_unregister_cancel(void *buf) {
    fprintf(stderr, "[WARNING] Called __pthread_unregister_cancel. This is a shim and does nothing.\n");
}

void __assert(const char *msg, const char *file, int line) {
    fprintf(stderr, "Assertion failed: %s (%s: %d)\n", msg, file, line);
    abort();
}

int __fgetc_unlocked(FILE *stream) {
    fprintf(stderr, "[WARNING] Called __fgetc_unlocked. This is a shim and does nothing.\n");
    return fgetc(stream);
}

void __ctype_b(void) {
    fprintf(stderr, "[WARNING] Called __ctype_b. This is a shim and does nothing.\n");
}
void __ctype_tolower(void) {
    fprintf(stderr, "[WARNING] Called __ctype_tolower. This is a shim and does nothing.\n");
}



// mmap begin

void* mmap(void *start, size_t len, int prot, int flags, int fd, uint32_t off) {
  return (void *)syscall(SYS_mmap2, start, len, prot, flags, fd, off >> 12);
}
