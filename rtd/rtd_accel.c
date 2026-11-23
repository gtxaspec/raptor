/*
 * rtd_accel.c -- MiraMEMS DA221 accelerometer over i2c-dev
 */

#include "rtd.h"

#include <rss_common.h>

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DA221_REG_CHIP_ID 0x01
#define DA221_REG_ACC_X	  0x02 /* X, Y, Z: LSB then MSB, left-justified */
#define DA221_REG_RANGE	  0x0f /* bits 1:0: +-2/4/8/16 g */
#define DA221_REG_MODE	  0x11 /* bit 7: suspend */
#define DA221_CHIP_ID	  0x13

static int reg_read(rtd_accel_t *a, uint8_t reg, uint8_t *buf, uint16_t len)
{
	struct i2c_msg msgs[2] = {
		{.addr = (uint16_t)a->addr, .flags = 0, .len = 1, .buf = &reg},
		{.addr = (uint16_t)a->addr, .flags = I2C_M_RD, .len = len, .buf = buf},
	};
	struct i2c_rdwr_ioctl_data xfer = {.msgs = msgs, .nmsgs = 2};
	return ioctl(a->fd, I2C_RDWR, &xfer) == 2 ? 0 : -1;
}

static int reg_write(rtd_accel_t *a, uint8_t reg, uint8_t val)
{
	uint8_t b[2] = {reg, val};
	struct i2c_msg msg = {.addr = (uint16_t)a->addr, .flags = 0, .len = 2, .buf = b};
	struct i2c_rdwr_ioctl_data xfer = {.msgs = &msg, .nmsgs = 1};
	return ioctl(a->fd, I2C_RDWR, &xfer) == 1 ? 0 : -1;
}

int rtd_accel_open(rtd_accel_t *a, int bus, int addr)
{
	char dev[32];
	snprintf(dev, sizeof(dev), "/dev/i2c-%d", bus);
	a->addr = addr;
	a->fd = open(dev, O_RDWR | O_CLOEXEC);
	if (a->fd < 0) {
		RSS_ERROR("g-sensor: %s: %m (kernel without i2c-dev?)", dev);
		return -1;
	}

	uint8_t id = 0, range = 0, mode = 0;
	if (reg_read(a, DA221_REG_CHIP_ID, &id, 1) < 0 || id != DA221_CHIP_ID) {
		RSS_ERROR("g-sensor: no DA221 at %s 0x%02x (id 0x%02x)", dev, addr, id);
		rtd_accel_close(a);
		return -1;
	}
	if (reg_read(a, DA221_REG_RANGE, &range, 1) < 0 ||
	    reg_read(a, DA221_REG_MODE, &mode, 1) < 0) {
		rtd_accel_close(a);
		return -1;
	}
	if ((mode & 0x80) && reg_write(a, DA221_REG_MODE, mode & 0x7f) < 0) {
		RSS_ERROR("g-sensor: cannot leave suspend");
		rtd_accel_close(a);
		return -1;
	}
	/* Full scale +-16 g: the chip powers up at +-2 g, where a crash
	 * clips. The other bits of the register are left as found. */
	if ((range & 3) != 3) {
		uint8_t want = (uint8_t)(range | 3);
		if (reg_write(a, DA221_REG_RANGE, want) < 0 ||
		    reg_read(a, DA221_REG_RANGE, &range, 1) < 0)
			RSS_WARN("g-sensor: cannot set +-16 g");
	}
	int fs_g = 2 << (range & 3);
	a->scale = (float)fs_g / 32768.0f;
	RSS_INFO("g-sensor: DA221 at %s 0x%02x, +-%d g", dev, addr, fs_g);
	return 0;
}

int rtd_accel_read(rtd_accel_t *a, float g[3])
{
	/* One 2-byte read per axis: a 6-byte burst on the NT98529 I2C
	 * controller ends in 0xff now and then (~1% of reads at 200 Hz),
	 * with no error reported; 2-byte reads never did. */
	uint8_t b[6];
	for (int i = 0; i < 3; i++)
		if (reg_read(a, (uint8_t)(DA221_REG_ACC_X + 2 * i), b + 2 * i, 2) < 0)
			return -1;
	for (int i = 0; i < 3; i++)
		g[i] = (float)(int16_t)(b[2 * i] | b[2 * i + 1] << 8) * a->scale;
	return 0;
}

void rtd_accel_close(rtd_accel_t *a)
{
	if (a->fd >= 0)
		close(a->fd);
	a->fd = -1;
}
