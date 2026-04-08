/*
 * rac.h -- Raptor Audio Client shared header
 */

#ifndef RAC_H
#define RAC_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#include <rss_ipc.h>
#include <rss_common.h>

/* Global run flag — defined in rac.c, set by signal handler */
extern volatile sig_atomic_t g_running;

/* Format enum for playback format detection */
enum play_fmt { FMT_PCM, FMT_MP3, FMT_AAC, FMT_OPUS };

/* Record mic audio from ring to file or stdout */
int cmd_record(const char *dest, int max_seconds);

/* Play audio file to speaker ring */
int cmd_play(const char *src, int sample_rate);

/* Send control command to RAD */
int cmd_ctrl(const char *cmd_json);

#endif /* RAC_H */
