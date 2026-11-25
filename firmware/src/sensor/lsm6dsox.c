#include "lsm6dsox.h"
#include "board_pins.h"

/* Harmony names the generated SPI plib after whichever SERCOM instance it
 * allocates to PA7/PA8/PA9. Change this one line to match what it generated;
 * nothing else in the project refers to the instance. */
#ifndef BOARD_SPI_WriteRead
#define BOARD_SPI_WriteRead  SERCOM1_SPI_WriteRead
#define BOARD_SPI_IsBusy     SERCOM1_SPI_IsBusy
#endif

/* Long enough that a real transfer always finishes, short enough that a
 * misconfigured SERCOM gives a diagnosable state instead of a silent hang. */
#define LSM_SPI_TIMEOUT  2000000u

/* Every transfer is address byte first, then data, with CS held low across the
 * whole exchange. The LSM6DSOX auto-increments its address inside a burst, so
 * a block read is one transaction rather than N.
 *
 * The generated plib is interrupt driven: WriteRead only *starts* the transfer
 * and returns. CS must stay low until the ISR has moved every byte, and the
 * caller's tx/rx buffers have to stay alive that long -- they are stack locals,
 * so returning early let the ISR write into a dead frame. Hence the wait.
 */
static void xfer(lsm_cs_t cs, uint8_t *tx, uint8_t *rx, size_t len)
{
    GPIO_PinClear(cs);
    if (BOARD_SPI_WriteRead(tx, len, rx, len)) {
        uint32_t guard = 0u;
        while (BOARD_SPI_IsBusy() && (++guard < LSM_SPI_TIMEOUT)) {
            /* spin */
        }
    }
    GPIO_PinSet(cs);
}

void lsm_init_bus(void)
{
    /* Chip selects idle high. Harmony's generated GPIO_Initialize has already
     * set the direction; this only guarantees the level before the first
     * transfer, so a half-selected device cannot answer. */
    GPIO_PinSet(BOARD_CS1_PIN);
    GPIO_PinSet(BOARD_CS2_PIN);
    GPIO_PinSet(BOARD_CS9_PIN);
    GPIO_PinSet(BOARD_CS11_PIN);
    GPIO_PinSet(BOARD_CS13_PIN);
    GPIO_PinSet(BOARD_CS14_PIN);
    GPIO_PinSet(BOARD_CS15_PIN);
}

uint8_t lsm_read_reg(lsm_cs_t cs, uint8_t reg)
{
    uint8_t tx[2] = { LSM_READ(reg), 0x00u };
    uint8_t rx[2] = { 0u, 0u };
    xfer(cs, tx, rx, 2u);
    return rx[1];
}

void lsm_write_reg(lsm_cs_t cs, uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = { LSM_WRITE(reg), value };
    uint8_t rx[2] = { 0u, 0u };
    xfer(cs, tx, rx, 2u);
}

void lsm_read_block(lsm_cs_t cs, uint8_t start, void *dst, size_t count)
{
    uint8_t tx[1u + 16u] = { 0 };
    uint8_t rx[1u + 16u] = { 0 };
    if (count > 16u) { count = 16u; }
    tx[0] = LSM_READ(start);
    xfer(cs, tx, rx, count + 1u);
    for (size_t i = 0u; i < count; i++) {
        ((uint8_t *)dst)[i] = rx[i + 1u];
    }
}

/* A floating MISO reads as all zeros or all ones, so WHO_AM_I is a real test of
 * the wiring and not just of the device: nothing else returns 0x6C. */
bool lsm_present(lsm_cs_t cs)
{
    return lsm_read_reg(cs, LSM_WHO_AM_I) == LSM_WHO_AM_I_VALUE;
}

/* ---- the sixteen as one array -------------------------------------------- */

static const lsm_cs_t cs_tab[16] = {
    BOARD_CS0_PIN,  BOARD_CS1_PIN,  BOARD_CS2_PIN,  BOARD_CS3_PIN,
    BOARD_CS4_PIN,  BOARD_CS5_PIN,  BOARD_CS6_PIN,  BOARD_CS7_PIN,
    BOARD_CS8_PIN,  BOARD_CS9_PIN,  BOARD_CS10_PIN, BOARD_CS11_PIN,
    BOARD_CS12_PIN, BOARD_CS13_PIN, BOARD_CS14_PIN, BOARD_CS15_PIN
};

void lsm_write_multi(uint16_t mask, uint8_t reg, const uint8_t *data,
                     size_t count)
{
    uint8_t tx[1u + 8u];
    uint8_t rx[1u + 8u];
    if (count > 8u) { count = 8u; }
    tx[0] = LSM_WRITE(reg);
    for (size_t i = 0u; i < count; i++) { tx[i + 1u] = data[i]; }

    for (uint32_t i = 0u; i < 16u; i++) {
        if ((mask >> i) & 1u) { GPIO_PinClear(cs_tab[i]); }
    }
    if (BOARD_SPI_WriteRead(tx, count + 1u, rx, count + 1u)) {
        uint32_t guard = 0u;
        while (BOARD_SPI_IsBusy() && (++guard < LSM_SPI_TIMEOUT)) { }
    }
    for (uint32_t i = 0u; i < 16u; i++) { GPIO_PinSet(cs_tab[i]); }
}

void spin(uint32_t ms)
{
    for (uint32_t i = 0u; i < ms; i++) {
        for (volatile uint32_t j = 0u; j < 4000u; j++) { }
    }
}

void lsm_configure_all(void)
{
    uint8_t one;
    /* Settings that only need writing once. Sent to every device in one
     * transaction so no sensor is left on a different configuration. */
    one = LSM_INT1_DRDY_XL; lsm_write_multi(0xFFFFu, LSM_INT1_CTRL,    &one, 1u);
    one = LSM_DRDY_PULSED;  lsm_write_multi(0xFFFFu, LSM_COUNTER_BDR1, &one, 1u);
    one = LSM_TIMESTAMP_EN; lsm_write_multi(0xFFFFu, LSM_CTRL10_C,     &one, 1u);
}

/* Sample all sixteen at the same instant, then read them out at leisure.
 *
 * The array cannot be *read* simultaneously -- one data line, one talker. But
 * it can be *started* and *stopped* simultaneously, and that is what decides
 * whether a frame is one coherent snapshot. Between the stop at step 3 and the
 * reads at step 4 nothing is updating, so a sequential readout cannot smear the
 * frame across orientations the way reading sixteen running sensors would.
 */
void lsm_capture(lsm_frame_t *out)
{
    static const uint8_t run[2]  = { LSM_CTRL1_XL_VALUE, LSM_CTRL2_G_VALUE };
    static const uint8_t halt[2] = { 0x00u, 0x00u };
    uint8_t buf[6];

    out->present = 0u;
    out->count   = 0u;
    out->waits   = 0u;

    lsm_write_multi(0xFFFFu, LSM_CTRL1_XL, halt, 2u);   /* 1. all stop   */
    lsm_write_multi(0xFFFFu, LSM_CTRL1_XL, run,  2u);   /* 2. all start  */

    /* 3. Discard any sample that predates the synchronised start.
     *
     * XLDA clears only when the output registers are read, so the tap
     * watcher -- left running at 416 Hz -- arrives here with its flag already
     * set from an earlier, unsynchronised conversion. Reading every sensor
     * once clears all sixteen flags, so whatever sets them next is certainly
     * post-start and certainly part of this frame.
     *
     * Both banks: XLDA clears on reading the accelerometer, GDA on reading the
     * gyroscope. Clearing only one left sensor 0 reporting the tap it was
     * still ringing from. */
    for (uint32_t i = 0u; i < 16u; i++) {
        lsm_read_block(cs_tab[i], LSM_OUTX_L_A, buf, 6u);
        lsm_read_block(cs_tab[i], LSM_OUTX_L_G, buf, 6u);
    }

    /* 4. Wait until every sensor has converted, not just one of them.
     *
     * Polling a single sensor was the bug: it reported ready immediately on a
     * stale flag and the other fifteen were frozen before their first
     * conversion, reading back zero. Requiring all sixteen costs a few
     * hundred SPI reads and removes the assumption entirely. */
    /* Two rounds, because the first conversion after power-up is not settled:
     * every sensor read ~0.95 g instead of 1.00 g, all sixteen agreeing, which
     * is the signature of a systematic settling error rather than part-to-part
     * scale error. The first sample is read and discarded; the second is the
     * frame. Costs one extra ODR period. */
    for (uint32_t round = 0u; round < 2u; round++) {
        uint16_t ready = 0u;
        for (uint32_t t = 0u; (t < 600u) && (ready != 0xFFFFu); t += 5u) {
            for (uint32_t i = 0u; i < 16u; i++) {
                if (((ready >> i) & 1u) != 0u) { continue; }
                /* Both banks: the gyroscope turns on more slowly than the
                 * accelerometer, so waiting on XLDA alone froze the part
                 * before the gyroscope had converted and we read whatever its
                 * output registers still held -- for the tap watcher, the tap
                 * itself. */
                if ((lsm_read_reg(cs_tab[i], LSM_STATUS_REG)
                     & (LSM_STATUS_XLDA | LSM_STATUS_GDA))
                    == (LSM_STATUS_XLDA | LSM_STATUS_GDA)) {
                    ready |= (uint16_t)(1u << i);
                }
            }
            if (ready != 0xFFFFu) {
                out->waits++;
                spin(5u);
            }
        }
        if (round == 0u) {                 /* discard the unsettled sample */
            for (uint32_t i = 0u; i < 16u; i++) {
                lsm_read_block(cs_tab[i], LSM_OUTX_L_A, buf, 6u);
                lsm_read_block(cs_tab[i], LSM_OUTX_L_G, buf, 6u);
            }
        }
    }

    lsm_write_multi(0xFFFFu, LSM_CTRL1_XL, halt, 2u);   /* 5. all freeze */

    for (uint32_t i = 0u; i < 16u; i++) {               /* 6. read out   */
        lsm_sample_t *s = &out->s[i];
        if (!lsm_present(cs_tab[i])) { continue; }

        s->status = lsm_read_reg(cs_tab[i], LSM_STATUS_REG);

        lsm_read_block(cs_tab[i], LSM_OUTX_L_G, buf, 6u);
        for (uint32_t k = 0u; k < 3u; k++) {
            s->gyro[k] = (int16_t)((uint16_t)buf[2u*k] |
                                   ((uint16_t)buf[2u*k + 1u] << 8));
        }
        lsm_read_block(cs_tab[i], LSM_OUTX_L_A, buf, 6u);
        for (uint32_t k = 0u; k < 3u; k++) {
            s->accel[k] = (int16_t)((uint16_t)buf[2u*k] |
                                    ((uint16_t)buf[2u*k + 1u] << 8));
        }
        lsm_read_block(cs_tab[i], LSM_OUT_TEMP_L, buf, 2u);
        s->temp = (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));

        s->freq_fine = (int8_t)lsm_read_reg(cs_tab[i], LSM_INTERNAL_FREQ);

        lsm_read_block(cs_tab[i], LSM_TIMESTAMP0, buf, 4u);
        s->timestamp = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                       ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);

        out->present |= (uint16_t)(1u << i);
        out->count++;
    }
}

/* ---- double tap as the capture trigger ----------------------------------- */

void lsm_tap_configure(lsm_cs_t cs)
{
    /* Thresholds are in units of full-scale/32, so at +/-2 g each count is
     * 62.5 mg; 8 counts is a 500 mg tap.
     *
     * Back to 8 from 5. 312 mg was asked for and set, but with ALL SIXTEEN
     * sensors armed it is sixteen chances for a desk bump or a cable tug to be
     * read as a double tap, and the board self-triggered continuously. 500 mg
     * with sixteen watchers was the original proposal and it is the right one:
     * the breadth buys the sensitivity, so the threshold does not have to.
     *
     * Back to 8 from 5 on request, 2026-10-07. 5 made the tap easier, which was
     * the point, but it also means the engine fires on smaller knocks -- and
     * these four registers are now the ONLY difference between this firmware
     * and the 1 October image that was producing clean keys. Returning them
     * makes the two identical apart from a command nothing sends, so if the
     * errors persist the cause is not in the firmware at all. */
    lsm_write_reg(cs, LSM_CTRL1_XL,   LSM_CTRL1_XL_TAP);
    /* The gyroscope stays off while merely armed. It is only needed once a tap
     * has happened, and leaving it running made this sensor measurably warmer
     * than the other fifteen -- 2 K, which at the datasheet's 4.5 mg/degC on Z
     * is tens of milli-g of offset on one of the sixteen. A bias that grows
     * with how long the operator waited before tapping is the last thing a
     * key derived from inter-sensor differences needs. */
    lsm_write_reg(cs, LSM_TAP_CFG0,   0x0Eu);  /* tap enabled on X, Y and Z   */
    lsm_write_reg(cs, LSM_TAP_CFG1,   0x08u);  /* X threshold                 */
    lsm_write_reg(cs, LSM_TAP_CFG2,   0x88u);  /* Y threshold, interrupts on  */
    lsm_write_reg(cs, LSM_TAP_THS_6D, 0x08u);  /* Z threshold                 */

    /* INT_DUR2 packs three fields, and none of them is in units of 1/ODR:
     *
     *   SHOCK [1:0]  8/ODR per count  -- how long the axis may stay over
     *                threshold and still count as a tap rather than a shove
     *   QUIET [3:2]  4/ODR per count  -- dead time after a tap, so the board
     *                ringing from the first knock is not read as the second
     *   DUR   [7:4] 32/ODR per count  -- the largest gap allowed BETWEEN the
     *                two taps of a double tap
     *
     * At the 416 Hz armed ODR: SHOCK 3 = 58 ms, QUIET 3 = 29 ms, and DUR is
     * 77 ms per count, so the window comes in 77 ms steps and 615 ms (DUR 8)
     * is the nearest one to the 600 ms wanted.
     *
     * A wide window is the forgiving choice: the operator can tap at their
     * own pace and still be understood. What it gives up is telling two taps
     * meant as separate events from one double tap -- and here that costs
     * nothing, because a capture is the only thing a tap can cause and the
     * board is armed only when one was asked for. Erring towards "understood
     * the operator" is right in a demo. QUIET and SHOCK stay at maximum for
     * the same reason: both only make detection more forgiving. */
    lsm_write_reg(cs, LSM_INT_DUR2,   0x7Fu);  /* DUR 7, QUIET 3, SHOCK 3     */
    lsm_write_reg(cs, LSM_WAKE_UP_THS, 0x80u); /* double tap, not single      */
}

void lsm_gyro_enable(lsm_cs_t cs)
{
    lsm_write_reg(cs, LSM_CTRL2_G, LSM_CTRL2_G_TAP);
}

bool lsm_tap_occurred(lsm_cs_t cs)
{
    /* TAP_SRC clears on read, so each event is reported once. */
    return (lsm_read_reg(cs, LSM_TAP_SRC) & LSM_TAP_SRC_DOUBLE) != 0u;
}

/* Wait until the board stops moving. This is what the gyroscope is genuinely
 * good for here: it reads zero at rest whatever the orientation, so it cannot
 * contribute key bits, but it says plainly whether the board is still. Capturing
 * while the tap is still ringing would sample an orientation the user never
 * intended to present. */
bool lsm_wait_still(lsm_cs_t cs, uint32_t timeout_ms)
{
    /* 250 dps full scale over 32768 counts gives 131 counts per deg/s, so
     * 5 deg/s is 655. Tight enough that a hand-held board only clears it when
     * genuinely settled, loose enough that it clears at all. */
    const int32_t QUIET = 655;
    uint32_t quiet_runs = 0u;
    uint8_t buf[6];

    for (uint32_t t = 0u; t < timeout_ms; t += 20u) {
        lsm_read_block(cs, LSM_OUTX_L_G, buf, 6u);
        int32_t worst = 0;
        for (uint32_t k = 0u; k < 3u; k++) {
            int32_t v = (int16_t)((uint16_t)buf[2u*k] |
                                  ((uint16_t)buf[2u*k + 1u] << 8));
            if (v < 0) { v = -v; }
            if (v > worst) { worst = v; }
        }
        quiet_runs = (worst < QUIET) ? (quiet_runs + 1u) : 0u;
        if (quiet_runs >= 10u) {    /* 200 ms of stillness */
            return true;
        }
        spin(20u);
    }
    return false;                   /* gave up; capture anyway */
}

void lsm_still_reset(lsm_still_t *st)
{
    st->quiet_runs = 0u;
    st->attempts   = 0u;
    st->last_worst = 0u;
}

lsm_still_result_t lsm_still_poll(lsm_cs_t cs, lsm_still_t *st)
{
    const int32_t QUIET    = 655;    /* 5 deg/s at 250 dps full scale */
    const uint32_t MAX_TRY = 300u;   /* 300 calls at the caller's 10 ms tick = 3 s */
    uint8_t buf[6];
    int32_t worst = 0;

    lsm_read_block(cs, LSM_OUTX_L_G, buf, 6u);
    for (uint32_t k = 0u; k < 3u; k++) {
        int32_t v = (int16_t)((uint16_t)buf[2u*k] |
                              ((uint16_t)buf[2u*k + 1u] << 8));
        if (v < 0) { v = -v; }
        if (v > worst) { worst = v; }
    }
    st->last_worst = (worst > 0xFFFF) ? 0xFFFFu : (uint16_t)worst;
    st->quiet_runs = (worst < QUIET) ? (st->quiet_runs + 1u) : 0u;
    st->attempts++;

    if (st->quiet_runs >= 10u) { return LSM_STILL_YES; }
    if (st->attempts >= MAX_TRY) { return LSM_STILL_TIMEOUT; }
    return LSM_STILL_WAITING;
}
