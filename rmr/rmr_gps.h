/*
 * rmr_gps.h -- GPS and g-sensor records in recordings
 *
 * Each telemetry record from rtd becomes a top-level 'free' box in the
 * Novatek dashcam layout ("freeGPS "), which ExifTool, dashcam players
 * and GPX converters read:
 *
 *   0x00 u32 BE size (256)   0x04 'free'   0x08 'GPS '   0x0c u32 LE 0x2c
 *   0x10 i32 LE hour, minute, second, year - 2000, month, day (UTC)
 *   0x28 'A' or 'V', 'N'/'S', 'E'/'W', 0
 *   0x2c f32 LE latitude, longitude as NMEA ddmm.mmmm
 *   0x34 f32 LE speed (knots), 0x38 course (degrees)
 *   0x3c i32 LE acceleration x, y, z in g/256
 *   0x80 raptor extension: 'RTDx', then the rtd_record_t fields at full
 *        precision (see rmr_gps.c)
 *
 * Readers find the boxes through the 'gps ' index in moov (directly
 * after mvhd; readers ignore it anywhere else): BE u32 size, 'gps ',
 * u32 0x101, u32 count, then count x (u32 file offset, u32 size).
 * rmr reserves the index when it writes moov and fills it in when the
 * file closes, so the signature chain covers moov with the index
 * zeroed (rverify zeroes it before hashing). The records themselves
 * are ordinary signed bytes.
 */

#ifndef RMR_GPS_H
#define RMR_GPS_H

#include <stdint.h>

#include "../rtd/rtd.h"

#define RMR_GPS_BOX_SIZE 256

void rmr_gps_build(uint8_t box[RMR_GPS_BOX_SIZE], const rtd_record_t *r);

#endif /* RMR_GPS_H */
