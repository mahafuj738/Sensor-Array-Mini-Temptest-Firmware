/* sensor_app.h -- the board's whole behaviour, as a state machine that never
 * blocks for long.
 *
 * Why a state machine and not the loop from step 3: a BLE peripheral has to
 * keep servicing its stack. The step-3 capture loop blocked for up to three
 * seconds waiting for the board to go still, which is long enough to lose a
 * connection. Everything here returns after about ten milliseconds.
 *
 * Temperature-test image: there is no double tap, because the board sits in
 * an oven where nobody can reach it. The tap image is the separate
 * sensor-array-mini-firmware repo.
 *
 *   host connects  ->  link pairs and encrypts (BLE stack, already proven)
 *   host sends CAP ->  board replies MS1-CAP
 *   gyroscope confirms the board is still (frame flag S), or times out (M)
 *   sixteen sensors sampled in one window, read out, sent as one line
 *   board returns to idle; the host sends CAP again for the next read
 *
 *   host sends TEMP -> MS1-TEMP line with the sixteen die temperatures, so the
 *   host can wait for the oven to settle without taking full frames
 *
 * The board holds no key and performs no crypto of its own. Link encryption is
 * the BLE stack's; the orientation key is the host's.
 */
#ifndef SENSOR_APP_H
#define SENSOR_APP_H

#include <stdint.h>
#include <stdbool.h>

/* Values kept from the tap image (where 1 was ARMED) so STATUS reads the same. */
typedef enum {
    SA_IDLE = 0,      /* connected or not, nothing requested          */
    SA_SETTLING = 2,  /* CAP received; waiting for the gyroscope      */
    SA_SENDING = 3    /* pushing one line out in chunks               */
} sa_state_t;

void sensor_app_init(void);

/* One iteration of the state machine. Never blocks beyond a single SPI
 * transaction, so the caller decides the rate -- see sensor_task.c, which calls
 * it every SENSOR_APP_TICK_MS from its own FreeRTOS task.
 *
 * The 10 ms contract is not cosmetic: the stillness gate counts calls, not
 * milliseconds, so a different tick changes how long "settled" means. */
#define SENSOR_APP_TICK_MS  10u


void sensor_app_step(void);

/* Bytes from the host. Safe to call from the BLE event callback. */
void sensor_app_rx(const uint8_t *data, uint16_t len);

/* Link state, so a disconnect abandons a capture in progress. */
void sensor_app_set_connected(bool up);

sa_state_t sensor_app_state(void);
uint32_t   sensor_app_captures(void);

#endif /* SENSOR_APP_H */
