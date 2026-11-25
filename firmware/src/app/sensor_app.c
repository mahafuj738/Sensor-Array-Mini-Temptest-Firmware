#include "sensor_app.h"
#include "ble_link.h"
#include "frame.h"
#include "../sensor/board_pins.h"
#include "../sensor/lsm6dsox.h"

/* Sensor 0's gyroscope runs the stillness gate, as it did in the tap image. */
#define GATE_CS         BOARD_CS0_PIN

#define RX_RING         64u          /* power of two */
#define CMD_MAX         16u

static sa_state_t  s_state;
static uint32_t    s_ticks;
static uint32_t    s_captures;
static bool        s_connected;

static lsm_frame_t s_frame;
static lsm_still_t s_still;

/* Which sensors answered in the most recent capture. Reported in STATUS so an
 * absent part shows up instead of being silently left out. */
static uint16_t    s_present;

/* A disconnect can land while the gate has sensor 0's gyroscope running. The
 * BLE task must not touch SPI, so it only raises this and the sensor task
 * switches the gyroscope off; left on, it warms sensor 0 about 2 K above the
 * other fifteen, which in a temperature test is the measurement itself. */
static volatile bool s_gyro_off_pending;


static char        s_out[FRAME_LINE_MAX];
static uint16_t    s_out_len;
static uint16_t    s_out_sent;
static uint32_t    s_send_stalls;

/* Single producer (BLE callback), single consumer (task). Volatile indices and
 * a power-of-two size make this safe without disabling interrupts. */
static volatile uint8_t  s_rx[RX_RING];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;

static char     s_cmd[CMD_MAX];
static uint16_t s_cmd_len;

/* ---- small helpers ------------------------------------------------------- */

static void say(const char *s)
{
    uint16_t n = 0u;
    while (s[n] != '\0') { n++; }
    (void)ble_link_send((const uint8_t *)s, n);
}

static bool cmd_is(const char *want)
{
    uint16_t i = 0u;
    while ((want[i] != '\0') && (i < s_cmd_len)) {
        char c = s_cmd[i];
        if ((c >= 'a') && (c <= 'z')) { c = (char)(c - 32); }
        if (c != want[i]) { return false; }
        i++;
    }
    return (want[i] == '\0') && (i == s_cmd_len);
}

static void start_capture(void)
{
    lsm_gyro_enable(GATE_CS);
    lsm_still_reset(&s_still);
    s_state = SA_SETTLING;
    say("MS1-CAP\n");
}

static void put_hex(char *dst, uint32_t v, uint32_t digits)
{
    static const char HEX[] = "0123456789ABCDEF";
    for (uint32_t i = 0u; i < digits; i++) {
        dst[i] = HEX[(v >> (4u * (digits - 1u - i))) & 0x0Fu];
    }
}

/* MS1-TEMP <present> <t0> ... <t15>, each int16 as 4 hex, degC = v/256 + 25.
 *
 * The array is powered down between captures, so the temperature registers
 * hold whatever the last capture left there. Running a full lsm_capture is the
 * only way to get fresh values without a second start/stop sequence, and that
 * sequence is the part of the driver that took the most debugging to get
 * right. It does not count as a capture. */
static void send_temps(void)
{
    uint16_t n = 0u;
    const char *tag = "MS1-TEMP ";

    lsm_capture(&s_frame);
    s_present = s_frame.present;

    while (tag[n] != '\0') { s_out[n] = tag[n]; n++; }
    put_hex(&s_out[n], s_frame.present, 4u); n += 4u;
    for (uint32_t i = 0u; i < 16u; i++) {
        uint16_t v = (((s_frame.present >> i) & 1u) != 0u)
                   ? (uint16_t)s_frame.s[i].temp : 0x8000u;
        s_out[n++] = ' ';
        put_hex(&s_out[n], v, 4u); n += 4u;
    }
    s_out[n++] = '\n';

    s_out_len     = n;
    s_out_sent    = 0u;
    s_send_stalls = 0u;
    s_state       = SA_SENDING;
}

static void dispatch(void)
{
    if (s_cmd_len == 0u) { return; }

    if (cmd_is("CAP")) {
        if (s_state == SA_IDLE) { start_capture(); }
        else                    { say("MS1-BUSY\n"); }
    } else if (cmd_is("TEMP")) {
        if (s_state == SA_IDLE) { send_temps(); }
        else                    { say("MS1-BUSY\n"); }
    } else if (cmd_is("PING")) {
        /* TT1 marks the temperature-test image, so a host can refuse a board
         * still carrying the tap image instead of waiting for a tap. */
        say("MS1-PONG TT1\n");
    } else if (cmd_is("STATUS")) {
        char line[48];
        static const char HEX[] = "0123456789ABCDEF";
        uint16_t n = 0u;
        const char *tag = "MS1-STAT ";
        while (tag[n] != '\0') { line[n] = tag[n]; n++; }
        line[n++] = (char)('0' + (char)s_state);
        line[n++] = ' ';
        for (uint32_t i = 0u; i < 4u; i++) {
            line[n++] = HEX[(s_present >> (4u * (3u - i))) & 0x0Fu];
        }
        line[n++] = ' ';
        for (uint32_t i = 0u; i < 8u; i++) {
            line[n++] = HEX[(s_captures >> (4u * (7u - i))) & 0x0Fu];
        }
        line[n++] = '\n';
        (void)ble_link_send((const uint8_t *)line, n);
    } else {
        say("MS1-ERR\n");
    }
    s_cmd_len = 0u;
}

static void drain_rx(void)
{
    while (s_rx_tail != s_rx_head) {
        uint8_t b = s_rx[s_rx_tail];
        s_rx_tail = (uint16_t)((s_rx_tail + 1u) & (RX_RING - 1u));

        if ((b == '\n') || (b == '\r')) {
            dispatch();
        } else if (s_cmd_len < CMD_MAX) {
            s_cmd[s_cmd_len++] = (char)b;
        } else {
            s_cmd_len = 0u;          /* overlong line; drop it whole */
        }
    }
}

/* The LED is the only output this board has without a host attached, so each
 * state gets a pattern that is unambiguous on its own -- no counting, because
 * counting blinks turned out to be how step 3 stayed ambiguous for a day. */
static void led_for_state(void)
{
    uint32_t phase;

    switch (s_state) {
    case SA_IDLE:
        phase = s_ticks % 100u;                  /* 100 x 10 ms = 1 s */
        if (s_connected) { if (phase < 5u) { BOARD_LED_ON(); } else { BOARD_LED_OFF(); } }
        else             { if (phase < 50u) { BOARD_LED_ON(); } else { BOARD_LED_OFF(); } }
        break;
    case SA_SETTLING:
        BOARD_LED_ON();                          /* solid: hold it still */
        break;
    case SA_SENDING:
    default:
        BOARD_LED_OFF();
        break;
    }
}

/* ---- public ------------------------------------------------------------- */

void sensor_app_init(void)
{
    s_state     = SA_IDLE;
    s_ticks     = 0u;
    s_captures  = 0u;
    s_connected = false;
    s_out_len   = 0u;
    s_out_sent  = 0u;
    s_cmd_len   = 0u;
    s_rx_head   = 0u;
    s_rx_tail   = 0u;
    s_present   = 0u;
    s_gyro_off_pending = false;

    lsm_init_bus();
    spin(50u);
    lsm_configure_all();
}

void sensor_app_rx(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0u; i < len; i++) {
        uint16_t next = (uint16_t)((s_rx_head + 1u) & (RX_RING - 1u));
        if (next == s_rx_tail) { return; }       /* full; drop the rest */
        s_rx[s_rx_head] = data[i];
        s_rx_head = next;
    }
}

void sensor_app_set_connected(bool up)
{
    s_connected = up;
    if (!up) {
        if (s_state == SA_SETTLING) { s_gyro_off_pending = true; }
        s_state   = SA_IDLE;
        s_out_len = 0u;
        s_cmd_len = 0u;
    }
}

sa_state_t sensor_app_state(void)    { return s_state; }
uint32_t   sensor_app_captures(void) { return s_captures; }

void sensor_app_step(void)
{
    if (s_gyro_off_pending) {
        s_gyro_off_pending = false;
        lsm_write_reg(GATE_CS, LSM_CTRL2_G, 0x00u);
    }

    drain_rx();
    led_for_state();

    switch (s_state) {
    case SA_IDLE:
        break;

    case SA_SETTLING: {
        /* The gyroscope IS the shock check. It is polled every tick and the
         * capture waits for all three axes under 5 deg/s for a continuous
         * 100 ms, so the board has demonstrably stopped before anything is
         * read. A blind fixed dwell was tried on top of this and removed: it
         * waits the same length whether the board settled instantly or is still
         * moving, which is strictly worse than asking the sensor. */
        lsm_still_result_t r = lsm_still_poll(GATE_CS, &s_still);
        if (r == LSM_STILL_WAITING) { break; }

        lsm_capture(&s_frame);
        s_present = s_frame.present;
        s_captures++;
        s_out_len = (uint16_t)frame_encode(&s_frame, s_captures,
                                           (r == LSM_STILL_YES), 0u,
                                           &s_still,
                                           s_ticks * SENSOR_APP_TICK_MS,
                                           frame_skew_us(&s_frame),
                                           s_out, sizeof s_out);
        s_out_sent    = 0u;
        s_send_stalls = 0u;
        s_state = (s_out_len > 0u) ? SA_SENDING : SA_IDLE;
        if (s_out_len == 0u) { say("MS1-ERR-ENCODE\n"); }
        break;
    }

    case SA_SENDING: {
        if (!ble_link_connected()) { s_state = SA_IDLE; break; }

        uint16_t left = (uint16_t)(s_out_len - s_out_sent);
        uint16_t mtu  = ble_link_mtu();
        uint16_t take = (left < mtu) ? left : mtu;

        if (ble_link_send((const uint8_t *)&s_out[s_out_sent], take)) {
            s_out_sent = (uint16_t)(s_out_sent + take);
            s_send_stalls = 0u;
            if (s_out_sent >= s_out_len) { s_state = SA_IDLE; }
        } else if (++s_send_stalls > 200u) {
            s_state = SA_IDLE;        /* two seconds of refusals; give up */
        }
        break;
    }

    default:
        s_state = SA_IDLE;
        break;
    }

    s_ticks++;
}
