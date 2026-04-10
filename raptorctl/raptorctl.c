/*
 * raptorctl.c -- Raptor control CLI
 *
 * Usage:
 *   raptorctl status                       Check which daemons are running
 *   raptorctl config get <section> [key]   Read config value(s)
 *   raptorctl config set <section> <key> <value>  Set a config value
 *   raptorctl config save                  Save running config to disk
 *   raptorctl <daemon> <command> [args]    Send command to daemon
 *
 * RVD commands:
 *   raptorctl rvd status                   Show encoder channel stats
 *   raptorctl rvd config                   Show running config
 *   raptorctl rvd request-idr [channel]    Request keyframe
 *   raptorctl rvd set-bitrate <ch> <bps>   Change bitrate
 *   raptorctl rvd set-gop <ch> <length>    Change GOP length
 *   raptorctl rvd set-fps <ch> <fps>       Change frame rate
 *   raptorctl rvd set-qp-bounds <ch> <min> <max>  Change QP range
 *   raptorctl rvd get-enc-caps             Show encoder capabilities
 *   ... plus ~30 advanced encoder commands (see --help)
 *
 * RAD commands:
 *   raptorctl rad status                   Show audio status
 *   raptorctl rad config                   Show running config
 *   raptorctl rad set-volume <val>         Change input volume
 *   raptorctl rad set-gain <val>           Change input gain
 *   raptorctl rad set-ns <0|1> [level]    Noise suppression on/off
 *   raptorctl rad set-hpf <0|1>           High-pass filter on/off
 *   raptorctl rad set-agc <0|1> [t] [c]   AGC on/off
 *
 * RSD commands:
 *   raptorctl rsd status                   Show client count
 *   raptorctl rsd config                   Show running config
 *
 * ROD commands:
 *   raptorctl rod status                   Show OSD region status
 *   raptorctl rod config                   Show running config
 *   raptorctl rod set-text <text>          Change OSD text string
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rss_ipc.h>
#include <rss_common.h>

#include "raptorctl.h"

const char *daemons[] = {"rvd", "rsd", "rad", "rod", "rhd", "ric",
			 "rmr", "rmd", "rwd", "rwc", NULL};

static const char *find_daemon_for_section(const char *section)
{
	static const struct {
		const char *section;
		const char *daemon;
	} map[] = {
		{"sensor", "rvd"}, {"stream0", "rvd"},	  {"stream1", "rvd"},	{"jpeg", "rvd"},
		{"ring", "rvd"},   {"audio", "rad"},	  {"rtsp", "rsd"},	{"http", "rhd"},
		{"osd", "rod"},	   {"ircut", "ric"},	  {"recording", "rmr"}, {"motion", "rmd"},
		{"webrtc", "rwd"}, {"webtorrent", "rwd"}, {"webcam", "rwc"},	{"log", "rvd"},
		{NULL, NULL},
	};
	for (int i = 0; map[i].section; i++) {
		if (strcmp(section, map[i].section) == 0)
			return map[i].daemon;
	}
	return NULL;
}

static void print_config_entry(const char *key, const char *value, void *userdata)
{
	int *count = userdata;
	printf("%s = %s\n", key, value);
	(*count)++;
}

/* Pretty-print a config-get-section JSON response as ini-style key = value.
 * Input: {"status":"ok","section":"audio","keys":{"codec":"aac","volume":"80"}}
 * Output: [audio]
 *         codec = aac
 *         volume = 80 */
static void print_section_json(const char *section, const char *json)
{
	printf("[%s]\n", section);
	const char *keys = strstr(json, "\"keys\":{");
	if (!keys)
		return;
	keys += 8; /* skip "keys":{ */

	while (*keys && *keys != '}') {
		/* Find key: "key" */
		const char *kstart = strchr(keys, '"');
		if (!kstart)
			break;
		kstart++;
		const char *kend = strchr(kstart, '"');
		if (!kend)
			break;

		/* Find value: "value" */
		const char *vstart = strchr(kend + 1, '"');
		if (!vstart)
			break;
		vstart++;
		const char *vend = strchr(vstart, '"');
		if (!vend)
			break;

		printf("%.*s = %.*s\n", (int)(kend - kstart), kstart, (int)(vend - vstart), vstart);

		keys = vend + 1;
		if (*keys == ',')
			keys++;
	}
}

const struct help_entry help_entries[] = {
	{NULL, "status                              Show daemon status"},
	{NULL, "memory                              Show memory usage (private/shared)"},
	{NULL, "cpu                                 Show CPU usage (1s sample)"},
	{NULL, "config get <section> <key>           Read config value"},
	{NULL, "config get <section>                Show all keys in section"},
	{NULL, "config set <section> <key> <value>  Set config value"},
	{NULL, "config save                         Save running config to disk"},
	{NULL, "<daemon> status                     Show daemon details"},
	{NULL, "<daemon> config                     Show running config"},
	{NULL, "<daemon> <cmd> [args...]            Send command"},
	{"rvd", "set-rc-mode <ch> <mode> [bps]       Change rate control mode"},
	{"rvd", "set-bitrate <ch> <bps>              Change bitrate"},
	{"rvd", "set-gop <ch> <length>               Change GOP length"},
	{"rvd", "set-fps <ch> <fps>                  Change frame rate"},
	{"rvd", "set-qp-bounds <ch> <min> <max>      Change QP range"},
	{"rvd", "set-qp <ch> <qp>                   Set fixed QP (all frames)"},
	{"rvd", "set-qp-ip-delta <ch> <delta>        I/P frame QP delta"},
	{"rvd", "set-qp-bounds-per-frame <ch> ...    Per-frame QP (iMin iMax pMin pMax)"},
	{"rvd", "set-gop-mode <ch> <0|1|2>           GOP mode (0=def 1=pyr 2=smartP)"},
	{"rvd", "set-rc-options <ch> <bitmask>       RC options bitmask"},
	{"rvd", "set-max-same-scene <ch> <count>     Max same-scene count"},
	{"rvd", "set-max-pic-size <ch> <iK> <pK>     Max I/P frame size (kbits)"},
	{"rvd", "set-color2grey <ch> <0|1>           Color to greyscale"},
	{"rvd", "set-mbrc <ch> <0|1>                 Macroblock rate control"},
	{"rvd", "set-entropy-mode <ch> <0|1>         0=CAVLC 1=CABAC"},
	{"rvd", "set-resize-mode <ch> <0|1>          Resize mode"},
	{"rvd", "set-stream-buf-size <ch> <bytes>    Stream buffer size"},
	{"rvd", "set-qpg-mode <ch> <mode>            QPG mode"},
	{"rvd", "set-h264-trans <ch> <offset>        H.264 chroma QP offset [-12..12]"},
	{"rvd", "set-h265-trans <ch> <cr> <cb>       H.265 chroma QP offsets [-12..12]"},
	{"rvd", "set-roi <ch> <idx> ...              ROI region (en x y w h qp)"},
	{"rvd", "set-super-frame <ch> <mode> ...     Super frame (mode iThr pThr)"},
	{"rvd", "set-pskip <ch> <en> <maxf>          P-skip (enable max_frames)"},
	{"rvd", "set-srd <ch> <en> <level>           Static refresh (enable level)"},
	{"rvd", "set-enc-denoise <ch> ...             Encoder denoise (en type iQP pQP)"},
	{"rvd", "set-gdr <ch> <en> <cycle>           GDR (enable cycle)"},
	{"rvd", "set-enc-crop <ch> <en> <x y w h>    Encoder crop"},
	{"rvd", "set-jpeg-qp <ch> <qp>              JPEG QP"},
	{"rvd", "get-enc-caps                        Show encoder capabilities"},
	{"rvd", "get-gop-mode <ch>                   Show GOP mode"},
	{"rvd", "get-rc-options <ch>                 Show RC options"},
	{"rvd", "get-max-same-scene <ch>             Show max same-scene count"},
	{"rvd", "get-color2grey <ch>                 Show color2grey state"},
	{"rvd", "get-mbrc <ch>                       Show macroblock RC state"},
	{"rvd", "get-qpg-mode <ch>                   Show QPG mode"},
	{"rvd", "get-stream-buf-size <ch>            Show stream buffer size"},
	{"rvd", "get-h264-trans <ch>                 Show H.264 chroma QP offset"},
	{"rvd", "get-h265-trans <ch>                 Show H.265 chroma QP offsets"},
	{"rvd", "get-roi <ch> <idx>                  Show ROI region"},
	{"rvd", "get-super-frame <ch>                Show super frame config"},
	{"rvd", "get-pskip <ch>                      Show P-skip config"},
	{"rvd", "get-srd <ch>                        Show SRD config"},
	{"rvd", "get-enc-denoise <ch>                Show encoder denoise config"},
	{"rvd", "get-gdr <ch>                        Show GDR config"},
	{"rvd", "get-enc-crop <ch>                   Show encoder crop"},
	{"rvd", "get-jpeg-qp <ch>                    Show JPEG QP"},
	{"rvd", "get-bitrate <ch>                    Show target + avg bitrate"},
	{"rvd", "get-fps <ch>                        Show frame rate"},
	{"rvd", "get-gop <ch>                        Show GOP length"},
	{"rvd", "get-qp-bounds <ch>                  Show QP range"},
	{"rvd", "get-rc-mode <ch>                    Show rate control mode"},
	{"rvd", "request-idr [channel]               Request keyframe"},
	{"rvd", "request-pskip <ch>                  Request P-skip"},
	{"rvd", "request-gdr <ch> <frames>           Request GDR"},
	{"rvd", "set-brightness <val>                ISP brightness (0-255)"},
	{"rvd", "set-contrast <val>                  ISP contrast (0-255)"},
	{"rvd", "set-saturation <val>                ISP saturation (0-255)"},
	{"rvd", "set-sharpness <val>                 ISP sharpness (0-255)"},
	{"rvd", "set-hue <val>                       ISP hue (0-255)"},
	{"rvd", "set-sinter <val>                    Spatial NR (0-255)"},
	{"rvd", "set-temper <val>                    Temporal NR (0-255)"},
	{"rvd", "set-hflip <0|1>                     Horizontal flip"},
	{"rvd", "set-vflip <0|1>                     Vertical flip"},
	{"rvd", "set-antiflicker <0|1|2>             Off/50Hz/60Hz"},
	{"rvd", "set-ae-comp <val>                   AE compensation"},
	{"rvd", "set-max-again <val>                 Max analog gain"},
	{"rvd", "set-max-dgain <val>                 Max digital gain"},
	{"rvd", "set-defog <0|1>                     Defog enable"},
	{"rvd", "set-wb <mode> [r] [b]               White balance"},
	{"rvd", "get-wb                              Show white balance settings"},
	{"rvd", "get-isp                             Show all ISP settings"},
	{"rvd", "get-exposure                        Show exposure info"},
	{"rsd", "clients                             List connected clients"},
	{"rad", "set-volume <val>                    Input volume"},
	{"rad", "set-gain <val>                      Input gain"},
	{"rad", "set-alc-gain <0-7>                  ALC gain (T21/T31 only)"},
	{"rad", "set-ns <0|1> [level]                Noise suppression"},
	{"rad", "set-hpf <0|1>                       High-pass filter"},
	{"rad", "set-agc <0|1> [target] [comp]       Automatic gain control"},
	{"rad", "ao-set-volume <val>                 Speaker volume"},
	{"rad", "ao-set-gain <val>                   Speaker gain"},
	{"rod", "privacy [on|off] [channel]          Toggle privacy mode"},
	{"rod", "set-text <text>                     Change OSD text"},
	{"rod", "set-font-color <0xAARRGGBB>         Text color"},
	{"rod", "set-stroke-color <0xAARRGGBB>       Stroke color"},
	{"rod", "set-stroke-size <0-5>               Stroke width"},
	{"ric", "mode <auto|day|night>               Set day/night mode (GPIO + ISP)"},
	{"ric", "isp-mode <day|night>                Set ISP mode only (no GPIO)"},
	{"rhd", "clients                             List connected clients"},
	{"rwd", "clients                             List connected clients"},
	{"rwd", "share                               Show WebTorrent share URL"},
	{"rwd", "share-rotate                        Generate new share key"},
	{"rmd", "sensitivity <0-4>                   Set motion sensitivity"},
	{NULL, "test-motion [sec]                   Trigger clip recording (default 10s)"},
	{NULL, NULL}};

static int same_section(const char *a, const char *b)
{
	if (a == b)
		return 1;
	if (!a || !b)
		return 0;
	return strcmp(a, b) == 0;
}

static void daemon_help(const char *name)
{
	printf("\nCommands:\n"
	       "  status                              Show status\n"
	       "  config                              Show running config\n");
	for (const struct help_entry *e = help_entries; e->text; e++) {
		if (e->daemon && strcmp(e->daemon, name) == 0)
			printf("  %s\n", e->text);
	}
}

static void usage(FILE *out)
{
	fprintf(out, "Usage: raptorctl <command>\n\nCommands:\n");
	const char *cur = NULL;
	for (const struct help_entry *e = help_entries; e->text; e++) {
		if (!same_section(e->daemon, cur)) {
			if (e->daemon)
				fprintf(out, "\n%s commands:\n", e->daemon);
			else if (cur)
				fprintf(out, "\n");
			cur = e->daemon;
		}
		if (e->daemon)
			fprintf(out, "  %s %s\n", e->daemon, e->text);
		else
			fprintf(out, "  %s\n", e->text);
	}
	fprintf(out, "\nDaemons: rvd, rsd, rad, rod, rhd, ric, rmr, rmd, rwd, rwc\n");
}

static int is_daemon(const char *name)
{
	for (int i = 0; daemons[i]; i++) {
		if (strcmp(name, daemons[i]) == 0)
			return 1;
	}
	return 0;
}

static int send_cmd(const char *daemon, const char *json)
{
	char sock_path[64];
	snprintf(sock_path, sizeof(sock_path), "/var/run/rss/%s.sock", daemon);

	char resp[2048];
	int ret = rss_ctrl_send_command(sock_path, json, resp, sizeof(resp), 5000);
	if (ret < 0) {
		fprintf(stderr, "Failed to send to %s: %s\n", daemon,
			ret == -2 ? "timeout" : "connection failed");
		return 1;
	}

	printf("%s\n", resp);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(stderr);
		return 1;
	}

	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		usage(stdout);
		return 0;
	}

	if (strcmp(argv[1], "status") == 0) {
		cmd_status();
		return 0;
	}

	if (strcmp(argv[1], "memory") == 0) {
		cmd_memory();
		return 0;
	}

	if (strcmp(argv[1], "cpu") == 0) {
		cmd_cpu();
		return 0;
	}

	/* Section-to-daemon mapping is in find_daemon_for_section() */

	/* raptorctl config <subcommand> */
	if (strcmp(argv[1], "config") == 0) {
		if (argc < 3) {
			fprintf(stderr, "Usage: raptorctl config <get|set|save> ...\n");
			return 1;
		}

		/* config save — tell all daemons to save */
		if (strcmp(argv[2], "save") == 0) {
			int saved = 0;
			for (int i = 0; daemons[i]; i++) {
				char sock_path[64];
				char resp[2048];
				snprintf(sock_path, sizeof(sock_path), "/var/run/rss/%s.sock",
					 daemons[i]);
				int ret = rss_ctrl_send_command(sock_path,
								"{\"cmd\":\"config-save\"}", resp,
								sizeof(resp), 2000);
				if (ret >= 0) {
					printf("%s: %s\n", daemons[i], resp);
					saved++;
				}
			}
			if (saved == 0) {
				fprintf(stderr, "No daemons responded\n");
				return 1;
			}
			return 0;
		}

		/* config get <section> [key] */
		if (strcmp(argv[2], "get") == 0) {
			if (argc < 4) {
				fprintf(stderr, "Usage: raptorctl config get <section> [key]\n");
				return 1;
			}
			const char *section = argv[3];
			const char *key = argc >= 5 ? argv[4] : NULL;

			/* Try daemon first for single-key get */
			if (key) {
				const char *target = find_daemon_for_section(section);
				if (target) {
					char sock_path[64];
					char resp[2048];
					char cmd_json[256];
					snprintf(sock_path, sizeof(sock_path),
						 "/var/run/rss/%s.sock", target);
					snprintf(cmd_json, sizeof(cmd_json),
						 "{\"cmd\":\"config-get\",\"section\":\"%s\""
						 ",\"key\":\"%s\"}",
						 section, key);
					int ret = rss_ctrl_send_command(sock_path, cmd_json, resp,
									sizeof(resp), 2000);
					if (ret >= 0) {
						printf("%s\n", resp);
						return 0;
					}
				}
			}

			if (!key) {
				/* Section dump: try daemon first */
				const char *target = find_daemon_for_section(section);
				if (target) {
					char sock_path[64];
					char resp[2048];
					char cmd_json[256];
					snprintf(sock_path, sizeof(sock_path),
						 "/var/run/rss/%s.sock", target);
					snprintf(cmd_json, sizeof(cmd_json),
						 "{\"cmd\":\"config-get-section\""
						 ",\"section\":\"%s\"}",
						 section);
					int ret = rss_ctrl_send_command(sock_path, cmd_json, resp,
									sizeof(resp), 2000);
					if (ret >= 0) {
						print_section_json(section, resp);
						return 0;
					}
				}
			}

			/* Fallback: read from config file */
			const char *cfgpath = "/etc/raptor.conf";
			rss_config_t *cfg = rss_config_load(cfgpath);
			if (!cfg) {
				fprintf(stderr, "Config not found\n");
				return 1;
			}

			if (key) {
				/* Single key */
				const char *val = rss_config_get_str(cfg, section, key, NULL);
				if (val)
					printf("%s\n", val);
				else
					fprintf(stderr, "Key not found: [%s] %s\n", section, key);
				rss_config_free(cfg);
				return val ? 0 : 1;
			}

			/* Section dump from file */
			printf("[%s]\n", section);
			int count = 0;
			rss_config_foreach(cfg, section, print_config_entry, &count);
			if (count == 0)
				fprintf(stderr, "Section not found: [%s]\n", section);
			rss_config_free(cfg);
			return count > 0 ? 0 : 1;
		}

		/* config set <section> <key> <value> */
		if (strcmp(argv[2], "set") == 0) {
			if (argc < 6) {
				fprintf(stderr,
					"Usage: raptorctl config set <section> <key> <value>\n");
				return 1;
			}
			const char *cfgpath = "/etc/raptor.conf";
			rss_config_t *cfg = rss_config_load(cfgpath);
			if (!cfg) {
				fprintf(stderr, "Failed to load %s\n", cfgpath);
				return 1;
			}
			rss_config_set_str(cfg, argv[3], argv[4], argv[5]);
			int ret = rss_config_save(cfg, cfgpath);
			rss_config_free(cfg);
			if (ret != 0) {
				fprintf(stderr, "Failed to save %s\n", cfgpath);
				return 1;
			}
			return 0;
		}

		fprintf(stderr, "Usage: raptorctl config <get|set|save> ...\n");
		return 1;
	}

	/* raptorctl test-motion [duration_sec] — trigger RMR clip recording */
	if (strcmp(argv[1], "test-motion") == 0) {
		int dur = 10;
		if (argc >= 3)
			dur = (int)strtol(argv[2], NULL, 10);
		if (dur < 1)
			dur = 1;
		if (dur > 300)
			dur = 300;

		printf("triggering motion clip for %d seconds...\n", dur);
		int ret = send_cmd("rmr", "{\"cmd\":\"start\"}");
		if (ret != 0)
			return ret;
		sleep(dur);
		printf("stopping motion clip...\n");
		return send_cmd("rmr", "{\"cmd\":\"stop\"}");
	}

	if (!is_daemon(argv[1])) {
		fprintf(stderr, "Unknown daemon: %s\n", argv[1]);
		usage(stderr);
		return 1;
	}

	if (argc < 3) {
		/* raptorctl <daemon> — show status + available commands */
		int pid = rss_daemon_check(argv[1]);
		if (pid > 0)
			printf("%s: running (pid %d)\n", argv[1], pid);
		else
			printf("%s: stopped\n", argv[1]);

		daemon_help(argv[1]);
		return 0;
	}

	const char *daemon = argv[1];
	const char *cmd = argv[2];
	char json[512];

	/* Privacy is implemented by RVD but exposed under ROD for UX */
	if (strcmp(daemon, "rod") == 0 && strcmp(cmd, "privacy") == 0)
		daemon = "rvd";

	if (strcmp(cmd, "status") == 0) {
		snprintf(json, sizeof(json), "{\"cmd\":\"status\"}");

	} else if (strcmp(cmd, "config") == 0) {
		snprintf(json, sizeof(json), "{\"cmd\":\"config-show\"}");

	} else if (strcmp(cmd, "clients") == 0) {
		snprintf(json, sizeof(json), "{\"cmd\":\"clients\"}");

	} else if (strcmp(cmd, "share-rotate") == 0) {
		snprintf(json, sizeof(json), "{\"cmd\":\"share-rotate\"}");

	} else if (strcmp(cmd, "share") == 0) {
		snprintf(json, sizeof(json), "{\"cmd\":\"share\"}");

	} else if (strcmp(cmd, "request-idr") == 0) {
		if (argc > 3)
			snprintf(json, sizeof(json), "{\"cmd\":\"request-idr\",\"channel\":%s}",
				 argv[3]);
		else
			snprintf(json, sizeof(json), "{\"cmd\":\"request-idr\"}");

	} else if (strcmp(cmd, "set-bitrate") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s set-bitrate <channel> <bps>\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-bitrate\",\"channel\":%s,\"value\":%s}", argv[3], argv[4]);

	} else if (strcmp(cmd, "set-gop") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s set-gop <channel> <length>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"set-gop\",\"channel\":%s,\"value\":%s}",
			 argv[3], argv[4]);

	} else if (strcmp(cmd, "set-fps") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s set-fps <channel> <fps>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"set-fps\",\"channel\":%s,\"value\":%s}",
			 argv[3], argv[4]);

	} else if (strcmp(cmd, "set-qp-bounds") == 0) {
		if (argc < 6) {
			fprintf(stderr, "Usage: raptorctl %s set-qp-bounds <channel> <min> <max>\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-qp-bounds\",\"channel\":%s,\"min\":%s,\"max\":%s}",
			 argv[3], argv[4], argv[5]);

	} else if (strcmp(cmd, "set-qp-bounds-per-frame") == 0) {
		if (argc < 8) {
			fprintf(stderr,
				"Usage: raptorctl %s set-qp-bounds-per-frame <ch> "
				"<min_i> <max_i> <min_p> <max_p>\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-qp-bounds-per-frame\",\"channel\":%s,"
			 "\"min_i\":%s,\"max_i\":%s,\"min_p\":%s,\"max_p\":%s}",
			 argv[3], argv[4], argv[5], argv[6], argv[7]);

	} else if (strcmp(cmd, "set-max-pic-size") == 0) {
		if (argc < 6) {
			fprintf(stderr,
				"Usage: raptorctl %s set-max-pic-size <ch> <i_kbits> <p_kbits>\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-max-pic-size\",\"channel\":%s,"
			 "\"i_kbits\":%s,\"p_kbits\":%s}",
			 argv[3], argv[4], argv[5]);

	} else if (strcmp(cmd, "set-h265-trans") == 0) {
		if (argc < 6) {
			fprintf(stderr,
				"Usage: raptorctl %s set-h265-trans <ch> <cr_offset> <cb_offset>\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-h265-trans\",\"channel\":%s,"
			 "\"cr_offset\":%s,\"cb_offset\":%s}",
			 argv[3], argv[4], argv[5]);

	} else if (strcmp(cmd, "set-roi") == 0) {
		if (argc < 11) {
			fprintf(stderr,
				"Usage: raptorctl %s set-roi <ch> <idx> <enable> "
				"<x> <y> <w> <h> <qp>\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-roi\",\"channel\":%s,\"index\":%s,\"enable\":%s,"
			 "\"x\":%s,\"y\":%s,\"w\":%s,\"h\":%s,\"qp\":%s}",
			 argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9], argv[10]);

	} else if (strcmp(cmd, "get-roi") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s get-roi <ch> <index>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"get-roi\",\"channel\":%s,\"index\":%s}",
			 argv[3], argv[4]);

	} else if (strcmp(cmd, "set-super-frame") == 0) {
		if (argc < 7) {
			fprintf(stderr,
				"Usage: raptorctl %s set-super-frame <ch> <mode> "
				"<i_thr> <p_thr>\n"
				"  modes: 0=none 1=discard 2=reencode\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-super-frame\",\"channel\":%s,\"mode\":%s,"
			 "\"i_thr\":%s,\"p_thr\":%s}",
			 argv[3], argv[4], argv[5], argv[6]);

	} else if (strcmp(cmd, "set-pskip") == 0) {
		if (argc < 6) {
			fprintf(stderr,
				"Usage: raptorctl %s set-pskip <ch> <enable> <max_frames>\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-pskip\",\"channel\":%s,\"enable\":%s,"
			 "\"max_frames\":%s}",
			 argv[3], argv[4], argv[5]);

	} else if (strcmp(cmd, "set-srd") == 0) {
		if (argc < 6) {
			fprintf(stderr, "Usage: raptorctl %s set-srd <ch> <enable> <level>\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-srd\",\"channel\":%s,\"enable\":%s,\"level\":%s}", argv[3],
			 argv[4], argv[5]);

	} else if (strcmp(cmd, "set-enc-denoise") == 0) {
		if (argc < 8) {
			fprintf(stderr,
				"Usage: raptorctl %s set-enc-denoise <ch> <enable> "
				"<type> <i_qp> <p_qp>\n"
				"  types: 0=off 1=I+P 2=I-only\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-enc-denoise\",\"channel\":%s,\"enable\":%s,"
			 "\"type\":%s,\"i_qp\":%s,\"p_qp\":%s}",
			 argv[3], argv[4], argv[5], argv[6], argv[7]);

	} else if (strcmp(cmd, "set-gdr") == 0) {
		if (argc < 6) {
			fprintf(stderr, "Usage: raptorctl %s set-gdr <ch> <enable> <cycle>\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-gdr\",\"channel\":%s,\"enable\":%s,\"cycle\":%s}", argv[3],
			 argv[4], argv[5]);

	} else if (strcmp(cmd, "request-pskip") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s request-pskip <channel>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"request-pskip\",\"channel\":%s}", argv[3]);

	} else if (strcmp(cmd, "request-gdr") == 0) {
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s request-gdr <channel> <frames>\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"request-gdr\",\"channel\":%s,\"value\":%s}", argv[3], argv[4]);

	} else if (strcmp(cmd, "set-enc-crop") == 0) {
		if (argc < 9) {
			fprintf(stderr,
				"Usage: raptorctl %s set-enc-crop <ch> <enable> "
				"<x> <y> <w> <h>\n",
				daemon);
			return 1;
		}
		snprintf(json, sizeof(json),
			 "{\"cmd\":\"set-enc-crop\",\"channel\":%s,\"enable\":%s,"
			 "\"x\":%s,\"y\":%s,\"w\":%s,\"h\":%s}",
			 argv[3], argv[4], argv[5], argv[6], argv[7], argv[8]);

	} else if (strcmp(cmd, "set-qp") == 0 || strcmp(cmd, "set-qp-ip-delta") == 0 ||
		   strcmp(cmd, "set-gop-mode") == 0 || strcmp(cmd, "set-rc-options") == 0 ||
		   strcmp(cmd, "set-max-same-scene") == 0 || strcmp(cmd, "set-qpg-mode") == 0 ||
		   strcmp(cmd, "set-entropy-mode") == 0 ||
		   strcmp(cmd, "set-stream-buf-size") == 0 || strcmp(cmd, "set-jpeg-qp") == 0 ||
		   strcmp(cmd, "set-color2grey") == 0 || strcmp(cmd, "set-mbrc") == 0 ||
		   strcmp(cmd, "set-resize-mode") == 0 || strcmp(cmd, "set-h264-trans") == 0) {
		/* Encoder set commands: <channel> <value> */
		if (argc < 5) {
			fprintf(stderr, "Usage: raptorctl %s %s <channel> <value>\n", daemon, cmd);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"%s\",\"channel\":%s,\"value\":%s}", cmd,
			 argv[3], argv[4]);

	} else if (strcmp(cmd, "get-enc-caps") == 0) {
		snprintf(json, sizeof(json), "{\"cmd\":\"get-enc-caps\"}");

	} else if (strcmp(cmd, "get-bitrate") == 0 || strcmp(cmd, "get-fps") == 0 ||
		   strcmp(cmd, "get-gop") == 0 || strcmp(cmd, "get-qp-bounds") == 0 ||
		   strcmp(cmd, "get-rc-mode") == 0 || strcmp(cmd, "get-gop-mode") == 0 ||
		   strcmp(cmd, "get-rc-options") == 0 || strcmp(cmd, "get-max-same-scene") == 0 ||
		   strcmp(cmd, "get-color2grey") == 0 || strcmp(cmd, "get-mbrc") == 0 ||
		   strcmp(cmd, "get-qpg-mode") == 0 || strcmp(cmd, "get-stream-buf-size") == 0 ||
		   strcmp(cmd, "get-h264-trans") == 0 || strcmp(cmd, "get-h265-trans") == 0 ||
		   strcmp(cmd, "get-super-frame") == 0 || strcmp(cmd, "get-pskip") == 0 ||
		   strcmp(cmd, "get-srd") == 0 || strcmp(cmd, "get-enc-denoise") == 0 ||
		   strcmp(cmd, "get-gdr") == 0 || strcmp(cmd, "get-enc-crop") == 0 ||
		   strcmp(cmd, "get-jpeg-qp") == 0) {
		/* Encoder get commands: <channel> */
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s %s <channel>\n", daemon, cmd);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"%s\",\"channel\":%s}", cmd, argv[3]);

	} else if (strcmp(cmd, "set-volume") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s set-volume <value>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"set-volume\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "set-gain") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s set-gain <value>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"set-gain\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "ao-set-volume") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s ao-set-volume <value>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"ao-set-volume\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "ao-set-gain") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s ao-set-gain <value>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"ao-set-gain\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "set-ns") == 0) {
		if (argc < 4) {
			fprintf(stderr,
				"Usage: raptorctl %s set-ns <0|1> [low|moderate|high|veryhigh]\n",
				daemon);
			return 1;
		}
		if (argc >= 5)
			snprintf(json, sizeof(json),
				 "{\"cmd\":\"set-ns\",\"value\":%s,\"level\":\"%s\"}", argv[3],
				 argv[4]);
		else
			snprintf(json, sizeof(json), "{\"cmd\":\"set-ns\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "set-hpf") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s set-hpf <0|1>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"set-hpf\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "set-agc") == 0) {
		if (argc < 4) {
			fprintf(stderr,
				"Usage: raptorctl %s set-agc <0|1> [target] [compression]\n",
				daemon);
			return 1;
		}
		if (argc >= 6)
			snprintf(json, sizeof(json),
				 "{\"cmd\":\"set-agc\",\"value\":%s,\"target\":%s,\"compression\":%"
				 "s}",
				 argv[3], argv[4], argv[5]);
		else
			snprintf(json, sizeof(json), "{\"cmd\":\"set-agc\",\"value\":%s}", argv[3]);

	} else if (strcmp(cmd, "privacy") == 0) {
		if (argc > 4)
			snprintf(json, sizeof(json),
				 "{\"cmd\":\"privacy\",\"value\":\"%s\",\"channel\":%ld}", argv[3],
				 strtol(argv[4], NULL, 10));
		else if (argc > 3)
			snprintf(json, sizeof(json), "{\"cmd\":\"privacy\",\"value\":\"%s\"}",
				 argv[3]);
		else
			snprintf(json, sizeof(json), "{\"cmd\":\"privacy\"}");

	} else if (strcmp(cmd, "set-text") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s set-text <text>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"set-text\",\"value\":\"%s\"}", argv[3]);

	} else if (strcmp(cmd, "mode") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s mode <auto|day|night>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"mode\",\"value\":\"%s\"}", argv[3]);

	} else if (strcmp(cmd, "isp-mode") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s isp-mode <day|night>\n", daemon);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"isp-mode\",\"value\":\"%s\"}", argv[3]);

	} else if (strcmp(cmd, "set-rc-mode") == 0) {
		if (argc < 5) {
			fprintf(stderr,
				"Usage: raptorctl %s set-rc-mode <ch> <mode> [bitrate]\n"
				"  modes: fixqp cbr vbr smart capped_vbr capped_quality\n",
				daemon);
			return 1;
		}
		int n = snprintf(json, sizeof(json),
				 "{\"cmd\":\"set-rc-mode\",\"channel\":%s,\"mode\":\"%s\"", argv[3],
				 argv[4]);
		if (argc >= 6)
			n += snprintf(json + n, sizeof(json) - n, ",\"bitrate\":%s", argv[5]);
		snprintf(json + n, sizeof(json) - n, "}");

	} else if (strcmp(cmd, "set-wb") == 0) {
		if (argc < 4) {
			fprintf(stderr,
				"Usage: raptorctl %s set-wb <auto|manual> [r_gain] [b_gain]\n",
				daemon);
			return 1;
		}
		int n = snprintf(json, sizeof(json), "{\"cmd\":\"set-wb\",\"mode\":\"%s\"",
				 argv[3]);
		if (argc >= 5)
			n += snprintf(json + n, sizeof(json) - n, ",\"r_gain\":%s", argv[4]);
		if (argc >= 6)
			n += snprintf(json + n, sizeof(json) - n, ",\"b_gain\":%s", argv[5]);
		snprintf(json + n, sizeof(json) - n, "}");

	} else if (strcmp(cmd, "set-font-color") == 0 || strcmp(cmd, "set-stroke-color") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: raptorctl %s %s <0xAARRGGBB>\n", daemon, cmd);
			return 1;
		}
		snprintf(json, sizeof(json), "{\"cmd\":\"%s\",\"value\":\"%s\"}", cmd, argv[3]);

	} else if (strncmp(cmd, "set-", 4) == 0 && argc >= 4) {
		/* Generic set-X <value> pass-through.
		 * Optional --sensor N flag for multi-sensor ISP tuning. */
		int sensor_idx = -1;
		const char *val_arg = argv[3];
		if (argc >= 6 && strcmp(argv[4], "--sensor") == 0)
			sensor_idx = (int)strtol(argv[5], NULL, 10);
		else if (argc >= 5 && strcmp(argv[3], "--sensor") == 0) {
			sensor_idx = (int)strtol(argv[4], NULL, 10);
			val_arg = argc >= 6 ? argv[5] : "0";
		}
		if (sensor_idx >= 0)
			snprintf(json, sizeof(json), "{\"cmd\":\"%s\",\"value\":%s,\"sensor\":%d}",
				 cmd, val_arg, sensor_idx);
		else
			snprintf(json, sizeof(json), "{\"cmd\":\"%s\",\"value\":%s}", cmd, val_arg);

	} else if (strncmp(cmd, "get-", 4) == 0) {
		/* Generic get-X pass-through */
		snprintf(json, sizeof(json), "{\"cmd\":\"%s\"}", cmd);

	} else {
		/* Generic pass-through */
		snprintf(json, sizeof(json), "{\"cmd\":\"%s\"}", cmd);
	}

	return send_cmd(daemon, json);
}
