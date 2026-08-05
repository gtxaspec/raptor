/*
 * ric_daynight.c -- IR-cut filter and day/night mode control
 *
 * Supports single GPIO (one pin toggles) and dual GPIO (two pins
 * pulsed for motor-driven IR-cut filters).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "ric.h"

/* Backoff for day attempts after a failed verification, in polls
 * (seconds at the default 1s poll): first hold, then doubling. */
#define RIC_DAY_LOCKOUT_FIRST 30
#define RIC_DAY_LOCKOUT_MAX   300

/* ── ADC via kernel device nodes ── */

static int adc_fd = -1;

static int adc_open_channel(int channel)
{
	char path[64];

	snprintf(path, sizeof(path), "/dev/ingenic_adc_aux_%d", channel);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0)
		return fd;

	snprintf(path, sizeof(path), "/dev/jz_adc_aux_%d", channel);
	return open(path, O_RDONLY | O_CLOEXEC);
}

bool ric_adc_start(ric_state_t *st)
{
	int channel = st->settings.adc_channel;

	adc_fd = adc_open_channel(channel);
	if (adc_fd < 0) {
		RSS_WARN("ADC: no device for channel %d", channel);
		return false;
	}

	if (ioctl(adc_fd, 0) < 0) {
		RSS_WARN("ADC: enable channel %d failed: %s", channel, strerror(errno));
		close(adc_fd);
		adc_fd = -1;
		return false;
	}

	RSS_DEBUG("ADC: channel %d initialized", channel);
	return true;
}

static int adc_read(int channel)
{
	(void)channel;
	if (adc_fd < 0)
		return -1;
	int value;
	return (read(adc_fd, &value, sizeof(value)) == sizeof(value)) ? value : -1;
}

/*
 * A photoresistor that stops answering is silent by nature: the read
 * fails, the poll is skipped, and day/night freezes wherever it stood
 * with nothing in the log to explain it. A start that fails is already
 * reported and demoted to the luma trigger, so what is left uncovered
 * is the device that opens and then goes quiet.
 *
 * Report a sustained run of failed reads rather than a single one --
 * one skipped poll costs nothing and says nothing -- and repeat rarely
 * enough that a log carrying the message for hours stays readable.
 */
#define ADC_FAIL_WARN_SEC   2
#define ADC_FAIL_REPEAT_SEC 60

/* rvd restarts routinely take a few seconds; only longer is an outage. */
#define RVD_FAIL_WARN_SEC   10
#define RVD_FAIL_REPEAT_SEC 60

static int polls_per(const ric_state_t *st, int seconds)
{
	int ms = st->settings.poll_interval_ms > 0 ? st->settings.poll_interval_ms : 1000;
	int polls = (seconds * 1000 + ms - 1) / ms;
	return polls > 0 ? polls : 1;
}

static int fail_secs(const ric_state_t *st, int run)
{
	int ms = st->settings.poll_interval_ms > 0 ? st->settings.poll_interval_ms : 1000;
	return (int)((long)run * ms / 1000);
}

static void adc_note_failed_read(ric_state_t *st)
{
	int first = polls_per(st, ADC_FAIL_WARN_SEC);
	int repeat = polls_per(st, ADC_FAIL_REPEAT_SEC);
	int run = ++st->adc_fail_run;

	if (run < first || (run - first) % repeat != 0)
		return;

	RSS_WARN("ADC: channel %d has read nothing for %ds -- day/night is frozen in "
		 "%s mode. Check the photoresistor wiring, or set [ircut] trigger to "
		 "something this board can measure",
		 st->settings.adc_channel, fail_secs(st, st->adc_fail_run),
		 st->current_mode == RIC_MODE_NIGHT ? "night" : "day");
	st->adc_fail_warned = true;
}

static void adc_note_good_read(ric_state_t *st)
{
	if (st->adc_fail_warned)
		RSS_INFO("ADC: channel %d reading again after %ds", st->settings.adc_channel,
			 fail_secs(st, st->adc_fail_run));
	st->adc_fail_run = 0;
	st->adc_fail_warned = false;
}

/*
 * The same silence, one layer up: rvd dying mid-run fails the poll's
 * query, the poll returns, and day/night freezes with nothing at
 * default log level -- the per-poll line below it is DEBUG. A routine
 * rvd restart is a few seconds and stays quiet.
 */
static void rvd_note_failed_query(ric_state_t *st)
{
	int first = polls_per(st, RVD_FAIL_WARN_SEC);
	int repeat = polls_per(st, RVD_FAIL_REPEAT_SEC);
	int run = ++st->rvd_fail_run;

	if (run < first || (run - first) % repeat != 0)
		return;

	RSS_WARN("rvd has not answered for %ds -- day/night is frozen in %s mode",
		 fail_secs(st, st->rvd_fail_run),
		 st->current_mode == RIC_MODE_NIGHT ? "night" : "day");
	st->rvd_fail_warned = true;
}

static void rvd_note_good_query(ric_state_t *st)
{
	if (st->rvd_fail_warned)
		RSS_INFO("rvd answering again after %ds", fail_secs(st, st->rvd_fail_run));
	st->rvd_fail_run = 0;
	st->rvd_fail_warned = false;
}

void ric_adc_cleanup(ric_state_t *st)
{
	if (adc_fd >= 0) {
		ioctl(adc_fd, 1);
		close(adc_fd);
		adc_fd = -1;
	}
	st->adc_initialized = false;
}

static void gpio_export(int pin)
{
	if (pin < 0)
		return;

	char path[64];
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
	if (access(path, F_OK) == 0) {
		RSS_DEBUG("gpio %d already exported", pin);
	} else {
		char buf[16];
		int len = snprintf(buf, sizeof(buf), "%d", pin);
		int fd = open("/sys/class/gpio/export", O_WRONLY);
		if (fd >= 0) {
			if (write(fd, buf, len) < 0)
				RSS_WARN("gpio %d export: %s", pin, strerror(errno));
			close(fd);
		}
	}

	int fd = open(path, O_WRONLY);
	if (fd >= 0) {
		if (write(fd, "out", 3) < 0)
			RSS_WARN("gpio %d direction: %s", pin, strerror(errno));
		close(fd);
	}
}

static void gpio_set(int pin, int value)
{
	if (pin < 0)
		return;
	char path[64];
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
	int fd = open(path, O_WRONLY);
	if (fd < 0) {
		/* As loud as a failed write: a pin that cannot open is an
		 * ircut or LED silently not moving. */
		RSS_WARN("gpio %d set: cannot open: %s", pin, strerror(errno));
		return;
	}
	if (write(fd, value ? "1" : "0", 1) < 0)
		RSS_WARN("gpio %d set: write failed: %s", pin, strerror(errno));
	close(fd);
}

void ric_gpio_init(ric_state_t *st)
{
	gpio_export(st->settings.gpio_ircut);
	if (st->settings.gpio_ircut2 >= 0)
		gpio_export(st->settings.gpio_ircut2);
	if (st->settings.gpio_irled >= 0)
		gpio_export(st->settings.gpio_irled);
	if (st->settings.gpio_irled2 >= 0)
		gpio_export(st->settings.gpio_irled2);
}

/*
 * Set ISP running mode only (day/night) via RVD control socket.
 * Does not toggle GPIO/IR-cut hardware.
 */
void ric_set_isp_mode(ric_mode_t mode)
{
	char resp[128];
	rss_ctrl_send_command(RSS_RUN_DIR "/rvd.sock",
			      mode == RIC_MODE_NIGHT
				      ? "{\"cmd\":\"set-running-mode\",\"value\":\"night\"}"
				      : "{\"cmd\":\"set-running-mode\",\"value\":\"day\"}",
			      resp, sizeof(resp), 2000);
}

/*
 * Drive the IR-cut filter alone. Exported for raptorctl's manual
 * control; a manual position lasts until the next mode switch
 * reasserts the automatic one. Returns -1 when no ircut pin exists.
 */
int ric_ircut_drive(ric_state_t *st, ric_mode_t pos)
{
	ric_config_t *c = &st->settings;

	if (c->gpio_ircut < 0)
		return -1;

	if (pos == RIC_MODE_NIGHT) {
		if (c->gpio_ircut2 >= 0) {
			gpio_set(c->gpio_ircut, 0);
			gpio_set(c->gpio_ircut2, 1);
			usleep((useconds_t)c->pulse_ms * 1000);
			gpio_set(c->gpio_ircut, 0);
			gpio_set(c->gpio_ircut2, 0);
			RSS_INFO("ircut: gpio %d=0, gpio %d=0 (night)", c->gpio_ircut,
				 c->gpio_ircut2);
		} else {
			gpio_set(c->gpio_ircut, 0);
			RSS_INFO("ircut: gpio %d=0 (night)", c->gpio_ircut);
		}
	} else {
		if (c->gpio_ircut2 >= 0) {
			gpio_set(c->gpio_ircut, 1);
			gpio_set(c->gpio_ircut2, 0);
			usleep((useconds_t)c->pulse_ms * 1000);
			gpio_set(c->gpio_ircut, 0);
			gpio_set(c->gpio_ircut2, 0);
			RSS_INFO("ircut: gpio %d=0, gpio %d=0 (day)", c->gpio_ircut,
				 c->gpio_ircut2);
		} else {
			gpio_set(c->gpio_ircut, 1);
			RSS_INFO("ircut: gpio %d=1 (day)", c->gpio_ircut);
		}
	}
	return 0;
}

/*
 * Drive one IR LED bank alone (bank940 selects 940nm). Exported for
 * raptorctl's manual control, which deliberately ignores the
 * ir850/ir940 enable flags: those gate what the AUTOMATIC transitions
 * do, while a manual command is explicit human intent -- lighting a
 * disabled bank from the shell is exactly what bench debugging needs.
 * Returns -1 when the bank has no pin.
 */
int ric_irled_drive(ric_state_t *st, bool bank940, bool on)
{
	ric_config_t *c = &st->settings;
	int pin = bank940 ? c->gpio_irled2 : c->gpio_irled;

	if (pin < 0)
		return -1;
	gpio_set(pin, on ? 1 : 0);
	RSS_INFO("%s: gpio %d=%d (%s)", bank940 ? "ir940" : "ir850", pin, on ? 1 : 0,
		 on ? "on" : "off");
	return 0;
}

static void ric_set_gpio(ric_state_t *st, ric_mode_t mode)
{
	ric_config_t *c = &st->settings;
	bool night = mode == RIC_MODE_NIGHT;

	ric_ircut_drive(st, mode);
	if (c->ir850_enabled)
		ric_irled_drive(st, false, night);
	if (c->ir940_enabled)
		ric_irled_drive(st, true, night);
}

void ric_set_mode(ric_state_t *st, ric_mode_t mode)
{
	if (mode == st->current_mode)
		return;

	ric_set_gpio(st, mode);
	ric_set_isp_mode(mode);
	RSS_INFO("switched to %s mode", mode == RIC_MODE_NIGHT ? "NIGHT" : "DAY");

	st->current_mode = mode;
	st->day_count = 0;
	st->night_count = 0;

	/* Cooldown: wait 3 polls for IR LEDs / ISP to stabilize before
	 * evaluating transitions. After cooldown in night mode, the gain
	 * baseline is sampled for the night→day transition algorithm. */
	st->cooldown_remaining = 3;
	if (mode == RIC_MODE_DAY)
		st->night_gain_baseline = 0;
}

/*
 * Debounce, shared by every trigger: hysteresis_sec consecutive polls
 * agreeing before the filter moves. `why` is the trigger's own account of
 * the decision, since the numbers that justify it differ per trigger.
 */
static void ric_debounce(ric_state_t *st, bool want_night, bool want_day, uint32_t total_gain,
			 const char *why)
{
	if (st->current_mode == RIC_MODE_DAY) {
		if (want_night) {
			st->night_count++;
			st->day_count = 0;
			if (st->night_count >= st->settings.hysteresis_sec) {
				RSS_DEBUG("night detected (%s for %ds)", why,
					  st->settings.hysteresis_sec);
				st->night_detect_gain = total_gain;
				ric_set_mode(st, RIC_MODE_NIGHT);
			}
		} else {
			st->night_count = 0;
		}
	} else {
		if (want_day) {
			st->day_count++;
			st->night_count = 0;
			if (st->day_count >= st->settings.hysteresis_sec) {
				RSS_DEBUG("day detected (%s for %ds)", why,
					  st->settings.hysteresis_sec);
				if (st->settings.trigger == RIC_TRIGGER_LUMA)
					st->day_verify_pending = true;
				ric_set_mode(st, RIC_MODE_DAY);
			}
		} else {
			st->day_count = 0;
		}
	}
}

/*
 * Poll ISP exposure and decide day/night transition.
 * Uses total_gain with hysteresis debounce.
 */
/* Extract unsigned integer from parsed cJSON object */
static uint32_t json_get_uint(const cJSON *root, const char *key)
{
	const cJSON *item = cJSON_GetObjectItem(root, key);
	return cJSON_IsNumber(item) ? (uint32_t)item->valuedouble : 0;
}

void ric_poll_exposure(ric_state_t *st)
{
	if (st->settings.opmode != RIC_AUTO)
		return;

	/* Query RVD for ISP exposure data via control socket */
	char resp[512];
	int ret = rss_ctrl_send_command(RSS_RUN_DIR "/rvd.sock", "{\"cmd\":\"get-exposure\"}", resp,
					sizeof(resp), 1000);
	if (ret < 0) {
		RSS_DEBUG("RVD query failed (%d)", ret);
		rvd_note_failed_query(st);
		return;
	}
	rvd_note_good_query(st);

	uint32_t total_gain = 0, ae_luma = 0;
	uint32_t ev = 0;
	uint16_t wb_rgain = 0, wb_bgain = 0;
	cJSON *parsed = cJSON_Parse(resp);
	if (!parsed) {
		RSS_DEBUG("RVD response parse failed: %.80s", resp);
		return;
	}
	total_gain = json_get_uint(parsed, "total_gain");
	ae_luma = json_get_uint(parsed, "ae_luma");
	ev = json_get_uint(parsed, "ev");
	wb_rgain = (uint16_t)json_get_uint(parsed, "wb_rgain");
	wb_bgain = (uint16_t)json_get_uint(parsed, "wb_bgain");
	cJSON_Delete(parsed);

	/*
	 * Zero means "this backend cannot answer for that field" -- the
	 * exposure contract raptor-hal documents in hal_isp.c, not a
	 * convention invented here. A live sensor never reports a mean luma
	 * of exactly 0, so treating it as absent costs nothing real.
	 *
	 * The ADC trigger reads its own sensor and must not be starved by
	 * missing exposure data -- it is the very fallback recommended
	 * below for platforms without a readback.
	 */
	bool have_gain = total_gain > 0;
	bool have_luma = ae_luma > 0;
	bool have_ev = ev > 0;

	if (st->settings.trigger != RIC_TRIGGER_ADC && !have_gain && !have_luma && !have_ev) {
		if (!st->no_exposure_warned) {
			RSS_WARN("no exposure data from rvd (gain, luma and ev all zero) -- "
				 "holding %s mode. This platform's HAL has no exposure "
				 "readback; use `raptorctl ric mode day|night`, or "
				 "trigger=adc if the board has a photoresistor",
				 st->current_mode == RIC_MODE_NIGHT ? "night" : "day");
			st->no_exposure_warned = true;
		}
		return;
	}

	/* Photo mode has its own state machine — bypass cooldown/hysteresis */
	if (st->settings.trigger == RIC_TRIGGER_PHOTO) {
		if (have_ev) {
			ric_photo_poll(st, ev, wb_rgain, wb_bgain);
			return;
		}
		/* Every phase of the photo state machine compares against ev,
		 * so with no ev there is nothing to run. Changing the trigger
		 * is what keeps this to one line. */
		RSS_WARN("photo trigger needs ev, which this platform does not report -- "
			 "falling back to the luma trigger");
		st->settings.trigger = RIC_TRIGGER_LUMA;
	}

	/* Cooldown after mode switch: wait for IR LEDs / ISP to stabilize.
	 * After cooldown in night mode, sample total_gain as baseline for
	 * the auto-calibrating night→day transition. */
	if (st->cooldown_remaining > 0) {
		st->cooldown_remaining--;
		if (st->cooldown_remaining == 0 && st->current_mode == RIC_MODE_DAY &&
		    st->day_verify_pending) {
			st->day_verify_pending = false;
			if (have_luma && ae_luma < (uint32_t)st->settings.night_luma) {
				/* The scene reads night-dark with the IR off: the
				 * "day" was the LED's own light bouncing back (a
				 * covered lens, a point-blank surface). Revert and
				 * back off so this cannot oscillate; a covered
				 * lens re-checks rarely instead of blinking. */
				int hold = st->day_lockout_next ? st->day_lockout_next
								: RIC_DAY_LOCKOUT_FIRST;
				RSS_WARN("day verification failed: scene reads night-dark "
					 "without IR (luma %u < %d) -- the day was IR "
					 "reflection; reverting, next attempt in %d polls",
					 ae_luma, st->settings.night_luma, hold);
				st->day_lockout_polls = hold;
				st->day_lockout_next = hold * 2 > RIC_DAY_LOCKOUT_MAX
							       ? RIC_DAY_LOCKOUT_MAX
							       : hold * 2;
				st->night_detect_gain = total_gain;
				ric_set_mode(st, RIC_MODE_NIGHT);
			} else {
				st->day_lockout_next = 0;
			}
		}
		if (st->cooldown_remaining == 0 && st->current_mode == RIC_MODE_NIGHT) {
			/* A baseline far below the gain that triggered night is
			 * self-contradictory (night through the IR reading
			 * brighter than the darkness that caused it) -- the IR
			 * LED bouncing off a covered lens or a point-blank
			 * surface crashes AE during this window and would make
			 * the day trigger unreachable. */
			uint32_t floor_gain = st->night_detect_gain / 10;
			if (total_gain < floor_gain) {
				RSS_WARN("night baseline %u is under 10%% of the gain that "
					 "triggered night (%u) -- IR reflection off a covered "
					 "lens? clamped to %u",
					 total_gain, st->night_detect_gain, floor_gain);
				st->night_gain_baseline = floor_gain;
			} else {
				st->night_gain_baseline = total_gain;
			}
			RSS_DEBUG("night baseline: gain=%u (day trigger < %u)",
				  st->night_gain_baseline,
				  st->night_gain_baseline * (uint32_t)st->settings.day_gain_pct /
					  100);
		}
		return;
	}

	bool want_night = false, want_day = false;
	char why[128];

	if (st->settings.trigger == RIC_TRIGGER_LUMA) {
		/*
		 * Hybrid luma+gain algorithm (sensor-independent):
		 *
		 * Day → Night: ae_luma < night_luma.
		 *   No IR LEDs on in day mode, so ae_luma directly reflects
		 *   ambient light. Works identically across all sensors.
		 *
		 * Night → Day: total_gain < day_gain_pct% of night baseline.
		 *   When ambient light returns (dawn, lights on), the ISP
		 *   drops gain because the scene is bright — even with IR
		 *   LEDs still on. This ratio is sensor-independent because
		 *   we compare against the same sensor's own night baseline.
		 *   ae_luma is NOT used here because IR illumination inflates
		 *   it regardless of ambient light level.
		 *
		 * Each term is gated on its field being reported, so a
		 * platform missing one runs on the other alone.
		 */
		want_night = (have_luma && ae_luma < (uint32_t)st->settings.night_luma) ||
			     (have_gain && total_gain > (uint32_t)st->settings.night_gain);

		if (have_gain && st->night_gain_baseline > 0) {
			if (st->current_mode == RIC_MODE_NIGHT &&
			    total_gain > st->night_gain_baseline) {
				RSS_DEBUG("night baseline raised %u -> %u (scene showed its "
					  "real dark reading)",
					  st->night_gain_baseline, total_gain);
				st->night_gain_baseline = total_gain;
			}
			uint32_t day_thr =
				st->night_gain_baseline * (uint32_t)st->settings.day_gain_pct / 100;
			want_day = (total_gain < day_thr);
		} else if (!have_gain) {
			/* No gain means no baseline and so no ratio; the
			 * inverse of the day→night test is all that is left.
			 * On a board that lights IR LEDs at night that test
			 * is an oscillator, not a fallback: IR lifts luma
			 * over the threshold, day mode cuts the LEDs, the
			 * scene goes dark again, and the filter clicks all
			 * night. Recover on luma only when no IR bank is
			 * active; with one, holding night is the lesser
			 * failure. */
			bool ir_lit =
				(st->settings.gpio_irled >= 0 && st->settings.ir850_enabled) ||
				(st->settings.gpio_irled2 >= 0 && st->settings.ir940_enabled);
			want_day = !ir_lit && have_luma &&
				   ae_luma >= (uint32_t)st->settings.night_luma;
		} else {
			want_day = false;
		}

		if (st->day_lockout_polls > 0) {
			st->day_lockout_polls--;
			want_day = false;
		}

		snprintf(why, sizeof(why), "luma=%u/%d gain=%u/%d night baseline=%u x %d%%",
			 ae_luma, st->settings.night_luma, total_gain, st->settings.night_gain,
			 st->night_gain_baseline, st->settings.day_gain_pct);
	} else if (st->settings.trigger == RIC_TRIGGER_ADC) {
		/*
		 * ADC mode: read photoresistor via SU_ADC.
		 * Direct ambient light measurement — unaffected by IR LEDs
		 * or camera sensor. No flip-flop, no calibration needed.
		 * High ADC value = bright, low = dark.
		 */
		int adc_val = adc_read(st->settings.adc_channel);
		if (adc_val < 0) {
			adc_note_failed_read(st);
			return;
		}
		adc_note_good_read(st);
		want_night = (adc_val < st->settings.adc_night);
		want_day = (adc_val > st->settings.adc_day);
		snprintf(why, sizeof(why), "adc=%d, night<%d day>%d", adc_val,
			 st->settings.adc_night, st->settings.adc_day);
	} else {
		/* Gain mode (legacy): fixed thresholds, sensor-dependent. */
		want_night = have_gain && total_gain > (uint32_t)st->settings.night_threshold;
		want_day = have_gain && total_gain < (uint32_t)st->settings.day_threshold;
		snprintf(why, sizeof(why), "gain=%u, night>%d day<%d", total_gain,
			 st->settings.night_threshold, st->settings.day_threshold);
	}

	ric_debounce(st, want_night, want_day, total_gain, why);
}
