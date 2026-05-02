/*
 * raptorctl_help.c -- Help text table and usage display
 */

#include <stdio.h>
#include <string.h>

#include "raptorctl.h"

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
	{NULL,
	 "<daemon> set-log-level <level>      Set log level (fatal|error|warn|info|debug|trace)"},
	{NULL, "<daemon> get-log-level              Show current log level"},
	{NULL, "<daemon> <cmd> [args...]            Send command"},
	{"rvd", "set-rc-mode <ch> <mode> [bps]       Change rate control mode"},
	{"rvd", "set-bitrate <ch> <bps>              Change bitrate"},
	{"rvd", "set-gop <ch> <length>               Change GOP length"},
	{"rvd", "set-fps <ch> <fps>                  Change frame rate"},
	{"rvd", "set-qp-bounds <ch> <min> <max>      Change QP range"},
	{"rvd", "set-qp-bounds-per-frame <ch> ...    Per-frame QP (iMin iMax pMin pMax)"},
	{"rvd", "set-max-pic-size <ch> <iK> <pK>     Max I/P frame size (kbits)"},
	{"rvd", "set-h264-trans <ch> <offset>        H.264 chroma QP offset [-12..12]"},
	{"rvd", "set-h265-trans <ch> <cr> <cb>       H.265 chroma QP offsets [-12..12]"},
	{"rvd", "set-roi <ch> <idx> ...              ROI region (en x y w h qp)"},
	{"rvd", "set-super-frame <ch> <mode> ...     Super frame (mode iThr pThr)"},
	{"rvd", "set-pskip <ch> <en> <maxf>          P-skip (enable max_frames)"},
	{"rvd", "set-srd <ch> <en> <level>           Static refresh (enable level)"},
	{"rvd", "set-enc-denoise <ch> ...             Encoder denoise (en type iQP pQP)"},
	{"rvd", "set-gdr <ch> <en> <cycle>           GDR (enable cycle)"},
	{"rvd", "set-enc-crop <ch> <en> <x y w h>    Encoder crop"},
	{"rvd", "enc-set <ch> <param> <value>        Set encoder parameter"},
	{"rvd", "enc-get <ch> <param>                Get encoder parameter"},
	{"rvd", "enc-list [ch]                       List encoder parameters"},
	{"rvd", "get-enc-caps                        Show encoder capabilities"},
	{"rvd", "get-h264-trans <ch>                 Show H.264 chroma QP offset"},
	{"rvd", "get-h265-trans <ch>                 Show H.265 chroma QP offsets"},
	{"rvd", "get-roi <ch> <idx>                  Show ROI region"},
	{"rvd", "get-super-frame <ch>                Show super frame config"},
	{"rvd", "get-pskip <ch>                      Show P-skip config"},
	{"rvd", "get-srd <ch>                        Show SRD config"},
	{"rvd", "get-enc-denoise <ch>                Show encoder denoise config"},
	{"rvd", "get-gdr <ch>                        Show GDR config"},
	{"rvd", "get-enc-crop <ch>                   Show encoder crop"},
	{"rvd", "get-bitrate <ch>                    Show target + avg bitrate"},
	{"rvd", "get-fps <ch>                        Show frame rate"},
	{"rvd", "get-gop <ch>                        Show GOP length"},
	{"rvd", "get-qp-bounds <ch>                  Show QP range"},
	{"rvd", "get-rc-mode <ch>                    Show rate control mode"},
	{"rvd", "request-idr [channel]               Request keyframe"},
	{"rvd", "stream-stop <ch>                    Stop stream pipeline"},
	{"rvd", "stream-start <ch>                   Start stopped stream"},
	{"rvd", "stream-restart <ch>                 Restart stream pipeline"},
	{"rvd", "set-codec <ch> <h264|h265>          Change codec (requires restart)"},
	{"rvd", "set-resolution <ch> <w> <h>         Change resolution (requires restart)"},
	{"rvd", "set-jpeg-quality <jpeg_ch> <1-100>    Change JPEG quality (restarts channel)"},
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
	{"rvd", "set-dpc <val>                       Dead pixel correction (0-255)"},
	{"rvd", "set-drc <val>                       Dynamic range compression (0-255)"},
	{"rvd", "set-highlight-depress <val>         Highlight suppression (0-255)"},
	{"rvd", "set-backlight-comp <val>            Backlight compensation (0-10)"},
	{"rvd", "set-defog <0|1>                     Defog enable"},
	{"rvd", "set-defog-strength <val>            Defog strength (0-255)"},
	{"rvd", "set-wb <mode> [r] [b]               White balance"},
	{"rvd", "get-wb                              Show white balance settings"},
	{"rvd", "get-isp                             Show all ISP settings"},
	{"rvd", "get-exposure                        Show exposure info"},
	{"rsd", "clients                             List connected clients"},
	{"rad", "set-codec <codec>                   Change audio codec (restart)"},
	{"rad", "set-sample-rate <Hz>                Input sample rate (restarts pipeline)"},
	{"rad", "set-volume <val>                    Input volume"},
	{"rad", "set-gain <val>                      Input gain"},
	{"rad", "set-alc-gain <0-7>                  ALC gain (T21/T31 only)"},
	{"rad", "mute                                Mute audio input"},
	{"rad", "unmute                              Unmute audio input"},
	{"rad", "ai-disable                          Disable audio input pipeline"},
	{"rad", "ai-enable                           Enable audio input pipeline"},
	{"rad", "set-aec <0|1>                        Acoustic echo cancellation"},
	{"rad", "set-ns <0|1> [0-3]                  Noise suppression level"},
	{"rad", "set-hpf <0|1>                       High-pass filter"},
	{"rad", "set-agc <0|1> [target] [comp]       Automatic gain control"},
	{"rad", "ao-set-volume <val>                 Speaker volume"},
	{"rad", "ao-set-gain <val>                   Speaker gain"},
	{"rad", "ao-set-sample-rate <Hz>             Speaker sample rate"},
	{"rad", "ao-mute                             Mute speaker (soft fade)"},
	{"rad", "ao-unmute                           Unmute speaker (soft fade)"},
	{"rad", "ao-disable                          Disable audio output pipeline"},
	{"rad", "ao-enable                           Enable audio output pipeline"},
	{"rod", "privacy [on|off] [channel]          Toggle privacy mode"},
	{"rod", "elements                            List all OSD elements"},
	{"rod", "add-element <name> [key=val]...     Create OSD element"},
	{"rod", "remove-element <name>               Remove OSD element"},
	{"rod", "set-element <name> [key=val]...     Modify element property"},
	{"rod", "show-element <name>                 Show element"},
	{"rod", "hide-element <name>                 Hide element"},
	{"rod", "set-var <name> <value>              Set template variable"},
	{"rod", "receipt [name] <text>               Append receipt line"},
	{"rod", "receipt-clear [name]                Clear receipt display"},
	{"rod", "set-position <elem> <pos>           Move element (named or x,y)"},
	{"rod", "set-font-size <10-72>               Global font size"},
	{"rod", "set-font-color <0xAARRGGBB>         Global text color"},
	{"rod", "set-stroke-color <0xAARRGGBB>       Global stroke color"},
	{"rod", "set-stroke-size <0-5>               Global stroke width"},
	{"ric", "mode <auto|day|night>               Set day/night mode (GPIO + ISP)"},
	{"ric", "isp-mode <day|night>                Set ISP mode only (no GPIO)"},
	{"rhd", "clients                             List connected clients"},
	{"rwd", "clients                             List connected clients"},
	{"rwd", "share                               Show WebTorrent share URL"},
	{"rwd", "share-rotate                        Generate new share key"},
	{"rmd", "sensitivity <0-4>                   Set motion sensitivity"},
	{"rmd", "skip-frames <N>                      Set IVS skip frame count"},
	{NULL, "test-motion [sec]                   Trigger clip recording (default 10s)"},
	{"rsp", "start                               Start push stream"},
	{"rsp", "stop                                Stop push stream"},
	{"rsr", "clients                             List connected SRT clients"},
	{NULL, "<daemon> set-affinity <cpu>          Pin daemon to CPU core"},
	{NULL, "<daemon> get-affinity                Show CPU affinity and sched policy"},
	{NULL, NULL}};

static int same_section(const char *a, const char *b)
{
	if (a == b)
		return 1;
	if (!a || !b)
		return 0;
	return strcmp(a, b) == 0;
}

void daemon_help(const char *name)
{
	printf("\nCommands:\n"
	       "  status                              Show status\n"
	       "  config                              Show running config\n");
	for (const struct help_entry *e = help_entries; e->text; e++) {
		if (e->daemon && strcmp(e->daemon, name) == 0)
			printf("  %s\n", e->text);
	}
}

void usage(FILE *out)
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
	fprintf(out,
		"\nJSON mode:\n"
		"  -j '{\"daemon\":\"rvd\",\"cmd\":\"...\"}'\n"
		"  -j "
		"'[{\"daemon\":\"rvd\",\"cmd\":\"...\"},{\"daemon\":\"rad\",\"cmd\":\"...\"}]'\n");
	fprintf(out,
		"\nDaemons: rvd, rsd, rad, rod, rhd, ric, rmr, rmd, rwd, rwc, rfs, rsp, rsr\n");
}
