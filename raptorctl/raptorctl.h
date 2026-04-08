/*
 * raptorctl.h -- Shared types and declarations for raptorctl
 */

#ifndef RAPTORCTL_H
#define RAPTORCTL_H

#include <stdio.h>

/* Daemon name list, NULL-terminated. Defined in raptorctl.c. */
extern const char *daemons[];

/* Per-daemon help entries. NULL daemon = global commands. */
struct help_entry {
	const char *daemon;
	const char *text;
};

extern const struct help_entry help_entries[];

/* Raptor threads use pthread_attr_setstacksize (128KB or 64KB).
 * SDK/library threads use the default (~2MB). Threshold to distinguish. */
#define RAPTOR_STACK_THRESHOLD 256 /* KB -- anything above is SDK */

typedef struct {
	long raptor_alloc, raptor_used;
	long sdk_alloc, sdk_used;
} stack_info_t;

/* raptorctl_info.c */
void cmd_status(void);
void cmd_memory(void);
void cmd_cpu(void);

#endif /* RAPTORCTL_H */
