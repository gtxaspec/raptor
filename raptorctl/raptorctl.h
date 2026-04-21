/*
 * raptorctl.h -- Shared types and declarations for raptorctl
 */

#ifndef RAPTORCTL_H
#define RAPTORCTL_H

#include <stdio.h>

#include <cJSON.h>

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

/* raptorctl_ipc.c */
int is_daemon(const char *name);
cJSON *jcmd(const char *cmd);
void jadd_s(cJSON *j, const char *key, const char *val);
void jadd_auto(cJSON *j, const char *key, const char *val);
void jadd_i(cJSON *j, const char *key, const char *val);
void jstr(cJSON *j, char *buf, int size);
int send_cmd(const char *daemon, const char *json);
int send_cmd_json(const char *daemon, const char *json, char *resp, int resp_size);
int handle_json_mode(const char *input);

/* raptorctl_config.c */
int handle_config(int argc, char **argv);

/* raptorctl_help.c */
void usage(FILE *out);
void daemon_help(const char *name);

/* raptorctl_dispatch.c */
int dispatch_daemon_cmd(const char *daemon, const char *cmd, int argc, char **argv, char *json,
			int json_size);
int build_generic_set(const char *cmd, int argc, char **argv, char *json, int json_size);

#endif /* RAPTORCTL_H */
