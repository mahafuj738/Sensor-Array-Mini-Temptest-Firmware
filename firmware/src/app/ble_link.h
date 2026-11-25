/* ble_link.h -- the only two things sensor_app needs from the radio.
 *
 * Kept behind this seam so sensor_app.c compiles without a single Harmony
 * header, and so the same application can run over the old board's serial link
 * by supplying a different ble_link.c.
 */
#ifndef BLE_LINK_H
#define BLE_LINK_H

#include <stdint.h>
#include <stdbool.h>

/* Hand len bytes to the transport. False means "not now" -- no connection, or
 * the stack has no buffer free -- and the caller should retry. Partial sends
 * are not a thing: either all len bytes are accepted or none are. */
bool ble_link_send(const uint8_t *data, uint16_t len);

bool ble_link_connected(void);

/* Largest chunk ble_link_send will accept, derived from the negotiated ATT
 * MTU. Callers split long lines on this. */
uint16_t ble_link_mtu(void);

/* Called from the stock example's event handlers; see
 * docs/BLE_INTEGRATION.md for the exact call sites.
 *
 * ble_link_tx_ready is the connect signal: it comes from the TX characteristic
 * CCCD opening, which is when notifications actually become possible. A GAP
 * connection alone is not enough -- the central has not subscribed yet. */
void ble_link_tx_ready(uint16_t connHandle, bool opened);
void ble_link_down(void);

/* Chooses this board's address and advertising name. Called once from
 * APP_BleConfigBasic, in place of the example's hard-coded address. */
void ble_identity_configure(void);

/* "ms1-xxxx", valid after ble_identity_configure(). */
const char *ble_identity_name(void);
void ble_link_mtu_set(uint16_t attMtu);
void ble_link_rx(uint16_t connHandle);

#endif /* BLE_LINK_H */
