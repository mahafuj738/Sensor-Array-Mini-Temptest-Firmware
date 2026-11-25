/* frame.h -- turn one coherent sixteen-sensor frame into a single text line.
 *
 * The board does no arithmetic on the readings. It sends raw sensor counts and
 * the host derives everything: normalisation, pair selection, the key, the
 * encryption. Nothing here knows what a key is, which is the point -- the board
 * holds no secret, so losing the board loses nothing.
 *
 * The line is hex rather than binary for one reason: it survives being read by
 * a human. One capture per tap is a few hundred bytes, so the doubling costs
 * nothing, and a newline makes reassembly across BLE notifications trivial.
 *
 *   MS1 <seq> <flag> <present> <skew> <waits> <tapped> <uptime> <settle>
 *       <gmax> <records...>\n
 *
 *   seq      8 hex digits, capture counter since boot
 *   flag     'S' the board had settled, 'M' it was still moving
 *   present  4 hex digits, bit n set = sensor n is in this frame
 *   skew     8 hex digits, microseconds between first and last timestamp
 *   waits    4 hex digits, data-ready poll iterations before readout
 *   tapped   4 hex digits, bit n set = sensor n latched the double tap.
 *            All sixteen watch, so this says WHERE on the board the knock
 *            landed, which is the thing worth correlating against a capture
 *            that drifted. Zero for a CAP capture, which has no tap.
 *   uptime   8 hex digits, milliseconds since boot at the moment of capture.
 *            A fixed-pose run that degrades over its length is the open
 *            question, and without this the log has no time axis at all.
 *   settle   4 hex digits, stillness polls the gate needed, 10 ms each.
 *            0000 means the board was already still on the first look.
 *   gmax     4 hex digits, worst |gyro| across the three axes in LSB on the
 *            final poll, 250 dps full scale. Says how still "still" was.
 *   records  one 38-digit group per present sensor, ascending index:
 *              ax ay az gx gy gz   int16, little endian, 2 counts/digit pair
 *              temp                int16, degC = temp/256 + 25
 *              freq_fine           int8, oscillator trim, 0.15 %/count
 *              status              uint8, STATUS_REG: bit0 XLDA, bit1 GDA
 *              timestamp           uint32, 25 us/count
 *
 * Temperature is sent for all sixteen elements and averaged by the host. Per
 * element costs two bytes and the thermal argument in Chapter 5 is about
 * element-to-element spread, so throwing that away at the board would be
 * discarding the interesting part.
 */
#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../sensor/lsm6dsox.h"

#define FRAME_RECORD_DIGITS  40u
#define FRAME_LINE_MAX       712u   /* 58 header + 16 x 40 + newline = 699 */

/* Returns the number of characters written, excluding any terminator, or 0 if
 * the buffer was too small. */
size_t frame_encode(const lsm_frame_t *f, uint32_t seq, bool was_still,
                    uint16_t tapped, const lsm_still_t *st, uint32_t uptime_ms,
                    uint32_t skew_us, char *out, size_t cap);

/* Microseconds between the earliest and latest timestamp in the frame. A
 * coherent frame should report a few hundred at most: the sensors are stopped
 * before readout, so this measures clock spread, not readout time. */
uint32_t frame_skew_us(const lsm_frame_t *f);

#endif /* FRAME_H */
