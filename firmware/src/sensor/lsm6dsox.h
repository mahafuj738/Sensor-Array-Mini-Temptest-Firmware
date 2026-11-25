/* lsm6dsox.h -- register access for the LSM6DSOX array over one shared SPI bus.
 *
 * Register numbers and the configuration values are taken from the SAME51
 * project in reference/, which is this group's working driver for the same
 * sensor. Only the transport differs.
 */
#ifndef LSM6DSOX_H
#define LSM6DSOX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Register map, the subset this project uses. */
#define LSM_WHO_AM_I        0x0Fu
#define LSM_CTRL1_XL        0x10u
#define LSM_CTRL2_G         0x11u
#define LSM_INT1_CTRL       0x0Du
#define LSM_COUNTER_BDR1    0x0Bu
#define LSM_CTRL10_C        0x19u
#define LSM_OUT_TEMP_L      0x20u
#define LSM_OUTX_L_G        0x22u
#define LSM_OUTX_L_A        0x28u
#define LSM_TIMESTAMP0      0x40u
#define LSM_INTERNAL_FREQ   0x63u

#define LSM_WHO_AM_I_VALUE  0x6Cu

/* Tap detection. The button is unusable on this board revision, and a double
 * tap is arguably the better gesture anyway: nothing is touching the board at
 * the moment of capture, which is what an orientation-bound key wants. */
#define LSM_TAP_CFG0        0x56u
#define LSM_TAP_CFG1        0x57u
#define LSM_TAP_CFG2        0x58u
#define LSM_TAP_THS_6D      0x59u
#define LSM_INT_DUR2        0x5Au
#define LSM_WAKE_UP_THS     0x5Bu
#define LSM_TAP_SRC         0x1Cu
#define LSM_STATUS_REG      0x1Eu

#define LSM_TAP_SRC_DOUBLE  0x10u
/* STATUS_REG: accelerometer / gyroscope / temperature data ready. */
#define LSM_STATUS_XLDA     0x01u
#define LSM_STATUS_GDA      0x02u

/* Tap needs a fast accelerometer; 416 Hz is ST's recommendation. Capture sets
 * its own rate afterwards, so this only applies while waiting. */
#define LSM_CTRL1_XL_TAP    0x60u    /* ODR 416 Hz, +/-2 g   */
#define LSM_CTRL2_G_TAP     0x60u    /* ODR 416 Hz, 250 dps  */

/* Configuration proven on the SAME51 board: accelerometer and gyroscope both
 * at 12.5 Hz, +/-2 g and 250 dps. The +/-2 g range matches the ADXL345 work so
 * the thesis numbers stay comparable. */
#define LSM_CTRL1_XL_VALUE  0x10u
#define LSM_CTRL2_G_VALUE   0x10u
#define LSM_INT1_DRDY_XL    0x01u
#define LSM_DRDY_PULSED     0x80u
#define LSM_TIMESTAMP_EN    0x20u

#define LSM_ACCEL_FS_G      2.0f
#define LSM_GYRO_FS_DPS     250.0f

/* Read accesses set bit 7 of the address byte, writes clear it. */
#define LSM_READ(a)   ((uint8_t)((a) | 0x80u))
#define LSM_WRITE(a)  ((uint8_t)((a) & 0x7Fu))

/* A chip select is just a GPIO; the caller supplies it so this file needs no
 * knowledge of which sensor sits where -- step 2 is still discovering that. */
typedef uint32_t lsm_cs_t;

/* One frame: every sensor's reading from the same instant. */
typedef struct {
    int16_t  accel[3];        /* raw LSB, +/-2 g full scale   */
    int16_t  gyro[3];         /* raw LSB, 250 dps full scale  */
    int16_t  temp;            /* degC = temp/256 + 25         */
    int8_t   freq_fine;       /* oscillator trim, 0.15% /count*/
    uint8_t  status;          /* STATUS_REG at readout: bit0 XLDA, bit1 GDA */
    uint32_t timestamp;       /* 25 us per count              */
} lsm_sample_t;

typedef struct {
    lsm_sample_t s[16];
    uint16_t     present;     /* bit n set = sensor n answered  */
    uint8_t      count;
    uint16_t     waits;       /* data-ready poll iterations used */
} lsm_frame_t;

void    lsm_init_bus(void);
void    lsm_configure_all(void);
void    lsm_capture(lsm_frame_t *out);

/* Watch one sensor for a double tap. Polled, so the shared IMU_INT line -- which
 * cannot say which device fired -- is not needed. */
void    spin(uint32_t ms);
void    lsm_tap_configure(lsm_cs_t cs);

/* Power the gyroscope up for the stillness gate. Called when a tap has
 * happened, not while merely armed: running it continuously warms the tap
 * watcher about 2 K above the other fifteen and biases its readings. */
void    lsm_gyro_enable(lsm_cs_t cs);
bool    lsm_tap_occurred(lsm_cs_t cs);
bool    lsm_wait_still(lsm_cs_t cs, uint32_t timeout_ms);

/* Incremental form of the above, for a board that must keep servicing a BLE
 * stack. One call reads the gyroscope once and returns immediately; the caller
 * loops over it from its own task instead of blocking for three seconds.
 *
 * It does not pace itself -- the caller's delay is the pacing. Ten consecutive
 * quiet reads mean "still", and it gives up after 300 calls, so on a 10 ms tick
 * that is 100 ms of stillness within a 3 s budget. */
typedef struct {
    uint32_t quiet_runs;
    uint32_t attempts;
    /* Worst |gyro| of the most recent poll, in LSB. Goes into the frame so a
     * log says how still the board actually was, not merely that it passed. */
    uint16_t last_worst;
} lsm_still_t;

typedef enum {
    LSM_STILL_WAITING = 0,
    LSM_STILL_YES,
    LSM_STILL_TIMEOUT
} lsm_still_result_t;

void               lsm_still_reset(lsm_still_t *st);
lsm_still_result_t lsm_still_poll(lsm_cs_t cs, lsm_still_t *st);

uint8_t lsm_read_reg(lsm_cs_t cs, uint8_t reg);
void    lsm_write_reg(lsm_cs_t cs, uint8_t reg, uint8_t value);
void    lsm_read_block(lsm_cs_t cs, uint8_t start, void *dst, size_t count);
bool    lsm_present(lsm_cs_t cs);

/* Writes to several devices at once by asserting their chip selects together.
 * Only writes: sixteen devices would collide driving the data line on a read.
 * Safe here because every line carries a 330 R series resistor. */
void    lsm_write_multi(uint16_t mask, uint8_t reg, const uint8_t *data,
                        size_t count);

#endif /* LSM6DSOX_H */
