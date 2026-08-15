/*
 * rac.c -- Raptor Audio Client
 *
 * CLI tool for audio input/output:
 *   rac record <file|->         Record mic to file or stdout
 *   rac play <file|->           Play audio to speaker (PCM16; MP3/AAC/Opus if compiled)
 *   rac status                  Show audio daemon status
 *   rac ao-volume <val>         Set speaker volume
 *   rac ao-gain <val>           Set speaker gain
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>

#include "rac.h"

volatile sig_atomic_t g_running = 1;

static void sighandler(int sig)
{
	(void)sig;
	g_running = 0;
}

/* ── Status / control commands ── */

/* Send a verb (optionally one integer argument) to RAD and print the
 * answer. The wire JSON is built by the serializer helpers; no call
 * site carries JSON syntax. */
static int cmd_ctrl_send(const char *verb, const char *key, long val, bool has_val)
{
	char resp[1024] = {0};
	int ret = has_val ? rss_ctrl_cmd_int(RSS_RUN_DIR "/rad.sock", verb, key, (int)val, resp,
					     sizeof(resp), 2000)
			  : rss_ctrl_cmd(RSS_RUN_DIR "/rad.sock", verb, resp, sizeof(resp), 2000);
	if (ret < 0) {
		fprintf(stderr, "rac: failed to send to RAD: %s\n",
			ret == -ECONNREFUSED ? "not running" : "connection failed");
		return 1;
	}
	printf("%s\n", resp);
	return 0;
}

static int cmd_ctrl_verb(const char *verb)
{
	return cmd_ctrl_send(verb, NULL, 0, false);
}

static int cmd_ctrl_verb_int(const char *verb, long val)
{
	return cmd_ctrl_send(verb, "value", val, true);
}

/* ── Usage ── */

static void usage(void)
{
	fprintf(stderr, "Usage: rac <command> [options]\n"
			"\n"
			"Commands:\n"
			"  record [options] <file|->   Record mic audio (format from ring)\n"
			"    -d <seconds>              Duration limit\n"
			"    -r <rate>                 Expected sample rate (info only)\n"
			"  play [options] <file|->     Play audio to speaker\n"
			"    -r <rate>                 Sample rate (default: 16000)\n"
			"    Supported formats: PCM16 LE"
#ifdef RAPTOR_MP3
			", MP3"
#endif
#ifdef RAPTOR_AAC
			", AAC"
#endif
#ifdef RAPTOR_OPUS
			", Opus"
#endif
			"\n"
			"  beep [options]              Play a sine beep to the speaker (no file)\n"
			"    -f <hz>                   Frequency (default: 1000)\n"
			"    -d <ms>                   Duration (default: 200)\n"
			"    -r <rate>                 Sample rate (default: 16000)\n"
			"  status                      Show audio daemon status\n"
			"  ao-volume <val>             Set speaker volume\n"
			"  ao-gain <val>               Set speaker gain\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage();
		return 1;
	}

	if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
		fprintf(stderr, "Raptor Streaming System — rac [%s] built %s\n", rss_build_hash,
			rss_build_time);
		return 0;
	}

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);

	const char *cmd = argv[1];

	if (strcmp(cmd, "record") == 0) {
		int duration = 0;
		int opt;
		optind = 2;
		while ((opt = getopt(argc, argv, "d:r:")) != -1) {
			switch (opt) {
			case 'd':
				duration = (int)strtol(optarg, NULL, 10);
				break;
			case 'r':
				/* info only for record */
				break;
			default:
				usage();
				return 1;
			}
		}
		if (optind >= argc) {
			fprintf(stderr, "rac: record requires a destination (file or -)\n");
			return 1;
		}
		return cmd_record(argv[optind], duration);

	} else if (strcmp(cmd, "play") == 0) {
		int sample_rate = 16000;
		int opt;
		optind = 2;
		while ((opt = getopt(argc, argv, "r:")) != -1) {
			switch (opt) {
			case 'r':
				sample_rate = (int)strtol(optarg, NULL, 10);
				break;
			default:
				usage();
				return 1;
			}
		}
		if (sample_rate <= 0) {
			fprintf(stderr, "rac: invalid sample rate\n");
			return 1;
		}
		if (optind >= argc) {
			fprintf(stderr, "rac: play requires a source (file or -)\n");
			return 1;
		}
		return cmd_play(argv[optind], sample_rate);

	} else if (strcmp(cmd, "beep") == 0) {
		int freq = 1000;
		int duration_ms = 200;
		int sample_rate = 16000;
		int opt;
		optind = 2;
		while ((opt = getopt(argc, argv, "f:d:r:")) != -1) {
			switch (opt) {
			case 'f':
				freq = (int)strtol(optarg, NULL, 10);
				break;
			case 'd':
				duration_ms = (int)strtol(optarg, NULL, 10);
				break;
			case 'r':
				sample_rate = (int)strtol(optarg, NULL, 10);
				break;
			default:
				usage();
				return 1;
			}
		}
		if (sample_rate <= 0) {
			fprintf(stderr, "rac: invalid sample rate\n");
			return 1;
		}
		return cmd_beep(freq, duration_ms, sample_rate);

	} else if (strcmp(cmd, "status") == 0) {
		return cmd_ctrl_verb("status");

	} else if (strcmp(cmd, "ao-volume") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: rac ao-volume <value>\n");
			return 1;
		}
		return cmd_ctrl_verb_int("ao-set-volume", strtol(argv[2], NULL, 10));

	} else if (strcmp(cmd, "ao-gain") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: rac ao-gain <value>\n");
			return 1;
		}
		return cmd_ctrl_verb_int("ao-set-gain", strtol(argv[2], NULL, 10));

	} else if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
		usage();
		return 0;

	} else {
		fprintf(stderr, "rac: unknown command '%s'\n", cmd);
		usage();
		return 1;
	}
}
