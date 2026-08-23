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

/* Polls to let AE settle after an IR-off ambient probe lifts the
 * LEDs, before the luma reading is believed. */
#define RIC_PROBE_SETTLE_POLLS 3

/* A slow AE walk changes less than 10% per poll and fooled the old
 * pairwise test into accepting a transition as a night baseline. Two
 * percent still tolerates normal quantization/noise while rejecting the
 * measured post-IR walk. */
#define RIC_BASELINE_SETTLE_PCT		 2
#define RIC_BASELINE_SETTLE_EXTEND_POLLS 17

/* ── ADC via kernel device nodes ── */

/* jz_adc_aux ioctl contract: cmd 0 enables the channel, cmd 1
 * disables it. Vendor ABI, no header ships the names. */
#define ADC_IOC_ENABLE	0
#define ADC_IOC_DISABLE 1

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

	st->adc_fd = adc_open_channel(channel);
	if (st->adc_fd < 0) {
		RSS_WARN("ADC: no device for channel %d", channel);
		return false;
	}

	if (ioctl(st->adc_fd, ADC_IOC_ENABLE) < 0) {
		RSS_WARN("ADC: enable channel %d failed: %s", channel, strerror(errno));
		close(st->adc_fd);
		st->adc_fd = -1;
		return false;
	}

	RSS_DEBUG("ADC: channel %d initialized", channel);
	return true;
}

static int adc_read(ric_state_t *st)
{
	if (st->adc_fd < 0)
		return -1;
	int value;
	return (read(st->adc_fd, &value, sizeof(value)) == sizeof(value)) ? value : -1;
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
	if (st->rvd_fail_run > 0) {
		if (st->rvd_fail_warned)
			RSS_INFO("rvd answering again after %ds", fail_secs(st, st->rvd_fail_run));
		/* Any gap can hide an rvd restart, and the ISP running mode
		 * lives in rvd: the GPIO half of a night camera survives a
		 * restart, the ISP half boots back as day and streams
		 * wrong colors under lit IR until the next transition --
		 * potentially all night. Re-assert the current mode on
		 * every recovery; the call is idempotent and recoveries
		 * are rare. The night sensor rate rides along for the same
		 * reason: a restarted rvd is back at its boot rate. */
		ric_set_isp_mode(st->current_mode);
		ric_apply_night_fps(st, st->current_mode);
	}
	st->rvd_fail_run = 0;
	st->rvd_fail_warned = false;
}

void ric_adc_cleanup(ric_state_t *st)
{
	if (st->adc_fd >= 0) {
		ioctl(st->adc_fd, ADC_IOC_DISABLE);
		close(st->adc_fd);
		st->adc_fd = -1;
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
	/* Park each bank off right at export: direction "out" leaves the
	 * line raw-low, which on an active-low bank is lit until the
	 * first transition gets around to driving it. */
	if (st->settings.gpio_irled >= 0) {
		gpio_export(st->settings.gpio_irled);
		gpio_set(st->settings.gpio_irled, st->settings.irled_active_low ? 1 : 0);
	}
	if (st->settings.gpio_irled2 >= 0) {
		gpio_export(st->settings.gpio_irled2);
		gpio_set(st->settings.gpio_irled2, st->settings.irled2_active_low ? 1 : 0);
	}
}

/*
 * Set ISP running mode only (day/night) via RVD control socket.
 * Does not toggle GPIO/IR-cut hardware.
 */
void ric_set_isp_mode(ric_mode_t mode)
{
	char resp[128];
	int ret = rss_ctrl_cmd_str(RSS_RUN_DIR "/rvd.sock", "set-running-mode", "value",
				   mode == RIC_MODE_NIGHT ? "night" : "day", resp, sizeof(resp),
				   2000);
	/* The filter and LEDs have already moved; a failure here is a
	 * half-finished transition (color at night or B/W in day) that
	 * otherwise heals only at the next switch. */
	if (ret < 0)
		RSS_WARN("ISP %s mode not applied (rvd unreachable) -- image stays in the old "
			 "mode until the next transition",
			 mode == RIC_MODE_NIGHT ? "night" : "day");
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
			int raw = c->ircut_active_low ? 1 : 0;
			gpio_set(c->gpio_ircut, raw);
			RSS_INFO("ircut: gpio %d=%d (night)", c->gpio_ircut, raw);
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
			int raw = c->ircut_active_low ? 0 : 1;
			gpio_set(c->gpio_ircut, raw);
			RSS_INFO("ircut: gpio %d=%d (day)", c->gpio_ircut, raw);
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
	bool alow = bank940 ? c->irled2_active_low : c->irled_active_low;
	int raw = (on != alow) ? 1 : 0;
	gpio_set(pin, raw);
	RSS_INFO("%s: gpio %d=%d (%s)", bank940 ? "ir940" : "ir850", pin, raw, on ? "on" : "off");
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

/*
 * Optional night sensor rate: entering night applies the configured
 * rate, entering day restores rvd's boot baseline (value 0). Policy
 * lives here -- WHEN and TO WHAT -- while rvd's transient
 * set-sensor-fps knows HOW to move sensor timing, encoder rate
 * control and GOP together without persisting anything. An
 * unreachable rvd is transient (the recovery re-assert retries); an
 * error answer means the backend cannot do rates at all, which is
 * permanent for this boot: warn once and park the feature.
 */
void ric_apply_night_fps(ric_state_t *st, ric_mode_t mode)
{
	if (st->settings.night_fps <= 0 || st->night_fps_unusable)
		return;
	int value = (mode == RIC_MODE_NIGHT) ? st->settings.night_fps : 0;
	char resp[512];
	int ret = rss_ctrl_cmd_int(RSS_RUN_DIR "/rvd.sock", "set-sensor-fps", "value", value, resp,
				   sizeof(resp), 2000);
	if (ret < 0) {
		RSS_WARN("sensor rate %d not applied (rvd unreachable) -- retried on the "
			 "next recovery or transition",
			 value ? value : -1);
		return;
	}
	if (!rss_ctrl_resp_is_ok(resp)) {
		st->night_fps_unusable = true;
		RSS_WARN("night_fps disabled for this run: rvd cannot set the sensor rate "
			 "(%.100s)",
			 resp);
		return;
	}
	if (value)
		RSS_INFO("night sensor rate %d fps applied", value);
	else
		RSS_INFO("day sensor rate restored");
}

/*
 * A forced mode (raptorctl ric mode day|night) is an explicit hardware
 * assertion, not a state-machine hint. Bench verbs (ircut/ir850/ir940)
 * move rails without touching current_mode, so a force that matches the
 * bookkept state must still drive the hardware -- found on a Wyze V3 as
 * "day" with lit IR LEDs after ircut/LED verbs, because the equality
 * short-circuit below made the force a silent no-op.
 */
void ric_force_mode(ric_state_t *st, ric_mode_t mode)
{
	if (mode == st->current_mode) {
		ric_set_gpio(st, mode);
		ric_set_isp_mode(mode);
		ric_apply_night_fps(st, mode);
		return;
	}
	ric_set_mode(st, mode);
}

static void ric_baseline_settle_begin(ric_state_t *st)
{
	st->settle_prev_gain = 0;
	st->settle_prev_ev = 0;
	st->settle_agree_run = 0;
	st->settle_extend_left = RIC_BASELINE_SETTLE_EXTEND_POLLS;
}

static bool ric_baseline_value_within(uint32_t previous, uint32_t current)
{
	if (previous == 0 || current == 0)
		return false;

	uint32_t difference = previous > current ? previous - current : current - previous;
	uint32_t tolerance = (uint32_t)((uint64_t)previous * RIC_BASELINE_SETTLE_PCT / 100);
	if (tolerance == 0)
		tolerance = 1;
	return difference <= tolerance;
}

/*
 * Re-arm the trigger machinery for a live trigger switch: every counter,
 * baseline and probe in flight describes the OLD trigger's view of the
 * scene. Baselines zeroed here resample inside the CURRENT regime once
 * the cooldown lands — the same self-normalizing path a night entry
 * uses — so switching triggers mid-night needs no mode bounce. The mode
 * itself, the IR-cut position and the ISP tuning are untouched: the
 * regime is right, only the judge changed.
 *
 * A probe may have the LED banks lifted when the switch arrives, so
 * night re-asserts every bank to its policy state on the way out.
 */
void ric_trigger_rearm(ric_state_t *st)
{
	ric_config_t *c = &st->settings;

	st->day_count = 0;
	st->night_count = 0;
	st->cooldown_remaining = 3;
	ric_baseline_settle_begin(st);
	st->night_gain_baseline = 0;
	st->night_ev_baseline = 0;
	st->night_detect_gain = 0;
	st->day_verify_pending = false;
	st->day_lockout_polls = 0;
	st->day_lockout_next = 0;
	st->probe_active = false;
	st->probe_polls_left = 0;
	st->probe_holdoff_polls = 0;
	st->probe_dip_run = 0;
	st->probe_recheck_polls = 0;
	ric_photo_reset(&st->photo, PHOTO_PHASE_NIGHT_DETECT);

	if (st->current_mode == RIC_MODE_NIGHT) {
		ric_irled_drive(st, false, c->ir850_enabled);
		ric_irled_drive(st, true, c->ir940_enabled);
	}
}

void ric_set_mode(ric_state_t *st, ric_mode_t mode)
{
	if (mode == st->current_mode)
		return;

	ric_set_gpio(st, mode);
	ric_set_isp_mode(mode);
	ric_apply_night_fps(st, mode);
	RSS_INFO("switched to %s mode", mode == RIC_MODE_NIGHT ? "NIGHT" : "DAY");

	st->current_mode = mode;
	st->day_count = 0;
	st->night_count = 0;

	/* Cooldown: wait 3 polls for IR LEDs / ISP to stabilize before
	 * evaluating transitions. After cooldown in night mode, the gain
	 * baseline is sampled for the night→day transition algorithm; the
	 * sampling itself extends while AE is still walking (see the
	 * cooldown block in ric_poll_exposure). */
	st->cooldown_remaining = 3;
	ric_baseline_settle_begin(st);
	st->probe_recheck_polls = 0; /* re-armed when the baseline lands */
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

/* Extract unsigned integer from parsed cJSON object */
static uint32_t json_get_uint(const cJSON *root, const char *key)
{
	const cJSON *item = cJSON_GetObjectItem(root, key);
	return cJSON_IsNumber(item) ? (uint32_t)item->valuedouble : 0;
}

/* Poll ISP exposure once and decide the day/night transition. */
void ric_poll_exposure(ric_state_t *st)
{
	/* The rvd query doubles as the liveness probe, so it runs in
	 * every operating mode: a forced-night camera whose rvd restarts
	 * needs its ISP mode re-asserted exactly as an automatic one
	 * does, and skipping the query in forced modes would leave that
	 * restart invisible. */
	char resp[512];
	int ret = rss_ctrl_cmd(RSS_RUN_DIR "/rvd.sock", "get-exposure", resp, sizeof(resp), 1000);
	if (ret < 0) {
		RSS_DEBUG("RVD query failed (%d)", ret);
		rvd_note_failed_query(st);
		return;
	}
	rvd_note_good_query(st);

	if (st->settings.opmode != RIC_AUTO)
		return;

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
			/* The baseline must reflect a SETTLED reading: gc2053-class
			 * AE walks for many seconds after the IR lights the scene
			 * (measured on a Wyze V3: ceiling 45000 falling to 306,
			 * with 4502 sampled at the old fixed cooldown -- the
			 * settled gain then read as a permanent probe dip and the
			 * IR blinked at every holdoff). AE walks step and can
			 * hold a value briefly, so one quiet pair is not settled:
			 * adopt only after three consecutive gain and EV polls agree
			 * within 2%, and give up at a hard cap rather than wait
			 * forever. */
			bool gain_within = !have_gain || ric_baseline_value_within(
								 st->settle_prev_gain, total_gain);
			bool ev_within =
				!have_ev || ric_baseline_value_within(st->settle_prev_ev, ev);
			bool within = (have_gain || have_ev) && gain_within && ev_within;
			st->settle_agree_run = within ? st->settle_agree_run + 1 : 0;
			/* Settle every metric used by the dip detector. A platform
			 * reporting neither has no baseline to qualify. */
			if ((have_gain || have_ev) && st->settle_agree_run < 2 &&
			    st->settle_extend_left > 0) {
				st->settle_extend_left--;
				st->settle_prev_gain = total_gain;
				st->settle_prev_ev = ev;
				st->cooldown_remaining = 1;
				return;
			}
			/* Adopt the settled reading as-is. The old 10%-of-trigger
			 * floor clamp assumed a far-below-trigger baseline meant
			 * IR bouncing off a covered lens; a Wyze V3 disproved it
			 * (starlight gc2053 + strong IR settles at 0.7% of the
			 * trigger in a small room) and the clamp then manufactured
			 * a false ratio-day at every dusk. A genuinely covered
			 * lens now simply holds night on its own settled
			 * baseline: no dip, no probe, no blinking, and the
			 * day-verify latch still guards every real day attempt. */
			st->night_gain_baseline = total_gain;
			st->night_ev_baseline = ev;
			st->probe_recheck_polls =
				st->settings.probe_recheck_sec * 1000 /
				(st->settings.poll_interval_ms > 0 ? st->settings.poll_interval_ms
								   : 1000);
			RSS_DEBUG("night baseline: gain=%u ev=%u (day trigger < %u)",
				  st->night_gain_baseline, st->night_ev_baseline,
				  st->night_gain_baseline * (uint32_t)st->settings.day_gain_pct /
					  100);
		}
		/* Every cooldown poll seeds the settling comparison above,
		 * so the first evaluation has a real predecessor. */
		st->settle_prev_gain = total_gain;
		st->settle_prev_ev = ev;
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

		/* Luma is trustworthy for the day direction only while no
		 * IR bank pours light into the scene: none configured or
		 * enabled, or a probe has lifted them. On a board with lit
		 * IR a luma day test is an oscillator, not a fallback: IR
		 * lifts luma over the threshold, day mode cuts the LEDs,
		 * the scene goes dark again, and the filter clicks all
		 * night. */
		bool ir_banks = (st->settings.gpio_irled >= 0 && st->settings.ir850_enabled) ||
				(st->settings.gpio_irled2 >= 0 && st->settings.ir940_enabled);
		bool ir_lit = ir_banks && st->current_mode == RIC_MODE_NIGHT && !st->probe_active;

		if (have_gain && st->night_gain_baseline > 0) {
			/* The running max must not learn from a probe: with
			 * the IR lifted a dark room reads ceiling gain, and
			 * adopting it would both wreck the ratio and re-arm
			 * the dip trigger into a blink loop. */
			if (st->current_mode == RIC_MODE_NIGHT && !st->probe_active &&
			    total_gain > st->night_gain_baseline) {
				RSS_DEBUG("night baseline raised %u -> %u (scene showed its "
					  "real dark reading)",
					  st->night_gain_baseline, total_gain);
				st->night_gain_baseline = total_gain;
				st->night_ev_baseline = ev;
			}
			uint32_t day_thr =
				st->night_gain_baseline * (uint32_t)st->settings.day_gain_pct / 100;
			want_day = (total_gain < day_thr);
		}
		if (!ir_lit && have_luma && ae_luma >= (uint32_t)st->settings.night_luma)
			want_day = true;

		/* IR-off ambient probe: compressed-gain sensors (T20 class)
		 * floor total_gain in a lit scene long before the ratio can
		 * fire -- measured on a Wyze V2: night baseline 1299 with
		 * IR, lights-on 1024, ratio floor 324. The dip below the
		 * baseline is still a reliable brightness hint, so lift the
		 * LEDs and let the now-trustworthy luma decide. A truly
		 * dark night sits at its baseline and never probes; a probe
		 * that finds darkness restores the LEDs and backs off. */
		if (st->probe_active && st->current_mode != RIC_MODE_NIGHT) {
			/* The probe ended in a day switch; LEDs already match
			 * day mode. Arm the holdoff anyway: if day verification
			 * reverts this switch, an immediate re-probe would blink
			 * the IR again. */
			st->probe_active = false;
			st->probe_holdoff_polls = st->settings.probe_holdoff_sec * 1000 /
						  st->settings.poll_interval_ms;
		}
		if (st->current_mode == RIC_MODE_NIGHT && ir_banks &&
		    st->settings.probe_gain_pct > 0 && st->night_gain_baseline > 0 &&
		    st->day_lockout_polls == 0) {
			int hyst_polls = st->settings.hysteresis_sec;
			if (!st->probe_active) {
				/* AE can answer new light with exposure alone while
				 * gain sits pinned at its floor (Wyze V3: night+IR
				 * gain 306 = near-minimum, lights-on moves only the
				 * exposure time). EV carries both dimensions, so the
				 * dip watches EV where the platform reports it. */
				bool use_ev = have_ev && st->night_ev_baseline > 0;
				uint32_t dip_val = use_ev ? ev : total_gain;
				bool dip_have = use_ev || have_gain;
				uint32_t dip_thr =
					(use_ev ? st->night_ev_baseline : st->night_gain_baseline) *
					(uint32_t)st->settings.probe_gain_pct / 100;
				/* The interval recheck is its own clock: IR wash
				 * can hide ambient light from every AE metric
				 * (Wyze V3: lit reads 91% of the dark EV), so a
				 * long quiet night earns a probe regardless of
				 * dips. It ticks through dip holdoffs but not
				 * through an active probe. */
				bool recheck_due = false;
				if (st->settings.probe_recheck_sec > 0 &&
				    st->probe_recheck_polls > 0) {
					st->probe_recheck_polls--;
					recheck_due = st->probe_recheck_polls == 0;
				}
				if (st->probe_holdoff_polls > 0 && !recheck_due) {
					st->probe_holdoff_polls--;
				} else if (!recheck_due && (!dip_have || dip_val >= dip_thr)) {
					/* Not dipping (or no reading): the run
					 * must not accumulate across gaps. */
					st->probe_dip_run = 0;
				} else if (/* The dip must persist: a single low
					    * poll can be an AE transient. The
					    * interval recheck fires outright. */
					   recheck_due || ++st->probe_dip_run >= 3) {
					st->probe_dip_run = 0;
					if (st->settings.ir850_enabled)
						ric_irled_drive(st, false, false);
					if (st->settings.ir940_enabled)
						ric_irled_drive(st, true, false);
					st->probe_active = true;
					/* Decremented before the compare below, so
					 * +3 yields exactly SETTLE suppressed polls
					 * and hyst+2 evaluation polls. */
					st->probe_polls_left =
						RIC_PROBE_SETTLE_POLLS + hyst_polls + 3;
					if (recheck_due)
						RSS_INFO("interval recheck after %ds of quiet "
							 "night: IR off for an ambient probe",
							 st->settings.probe_recheck_sec);
					else
						RSS_INFO("%s %u dipped under %d%% of night "
							 "baseline %u: IR off for an ambient "
							 "probe",
							 use_ev ? "ev" : "gain", dip_val,
							 st->settings.probe_gain_pct,
							 dip_thr * 100 /
								 (uint32_t)st->settings
									 .probe_gain_pct);
				}
			} else if (st->probe_polls_left > 0) {
				st->probe_polls_left--;
				if (st->probe_polls_left > hyst_polls + 2) {
					/* AE still settling without IR */
					want_day = false;
					want_night = false;
				} else if (st->probe_polls_left == 0) {
					if (st->settings.ir850_enabled)
						ric_irled_drive(st, false, true);
					if (st->settings.ir940_enabled)
						ric_irled_drive(st, true, true);
					st->probe_active = false;
					st->probe_holdoff_polls = st->settings.probe_holdoff_sec *
								  1000 /
								  st->settings.poll_interval_ms;
					/* Cooldown resamples the baseline once the
					 * restored IR settles. */
					st->cooldown_remaining = 3;
					ric_baseline_settle_begin(st);
					RSS_INFO("probe found darkness; IR restored, next "
						 "probe in %ds",
						 st->settings.probe_holdoff_sec);
				}
			}
		}

		if (st->day_lockout_polls > 0) {
			st->day_lockout_polls--;
			want_day = false;
		}

		snprintf(why, sizeof(why), "luma=%u/%d gain=%u/%d night baseline=%u x %d%%%s",
			 ae_luma, st->settings.night_luma, total_gain, st->settings.night_gain,
			 st->night_gain_baseline, st->settings.day_gain_pct,
			 st->probe_active ? " (probing)" : "");
	} else if (st->settings.trigger == RIC_TRIGGER_ADC) {
		/*
		 * ADC mode: read photoresistor via SU_ADC.
		 * Direct ambient light measurement — unaffected by IR LEDs
		 * or camera sensor. No flip-flop, no calibration needed.
		 * High ADC value = bright, low = dark.
		 */
		int adc_val = adc_read(st);
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
