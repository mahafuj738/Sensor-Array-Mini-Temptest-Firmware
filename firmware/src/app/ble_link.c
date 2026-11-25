/* ble_link.c -- the Transparent UART service, behind the two-function seam.
 *
 * The only file in src/ that includes Harmony BLE headers. Everything written
 * against the real headers in this Harmony release, not guessed:
 *
 *   - BLE_TRSPS_GetDataLength returns void, not a status code.
 *   - There is no BLE_TRSPS_EVT_CONNECTED. The usable "ready" signal is
 *     BLE_TRSPS_EVT_TX_STATUS with status BLE_TRSPS_STATUS_TX_OPENED, which
 *     fires when the central enables the TX characteristic's CCCD -- exactly
 *     the moment notifications become possible, and not a moment before.
 *   - Disconnect has to come from the GAP handler; the profile has no such
 *     event.
 *
 * Thread note: ble_link_send is called from the sensor task, while the stack
 * runs in the app task. Harmony's BLE API is OSAL-based and documented as
 * callable from application tasks, so this is sanctioned rather than merely
 * lucky -- but it is the one cross-task assumption in this design, and the
 * first thing to question if sends start failing under load.
 */
#include "ble_link.h"
#include "sensor_app.h"
#include "ble_trsps/ble_trsps.h"
#include "ble_gap.h"
#include "mba_error_defs.h"
#include <stdbool.h>
#include <string.h>

/* Conservative until the central negotiates something larger. The Transparent
 * UART payload is the ATT MTU minus three bytes of notification header. */
#define MTU_DEFAULT   20u
#define MTU_CEILING   180u

static uint16_t s_conn;
static bool     s_up;            /* link is up AND the TX CCCD is enabled */
static uint16_t s_mtu = MTU_DEFAULT;

bool     ble_link_connected(void) { return s_up; }
uint16_t ble_link_mtu(void)       { return s_mtu; }

bool ble_link_send(const uint8_t *data, uint16_t len)
{
    if (!s_up || (len == 0u)) { return false; }
    return (BLE_TRSPS_SendData(s_conn, len, (uint8_t *)data) == MBA_RES_SUCCESS);
}

/* ---- called from the example's own event handlers ------------------------ */

void ble_link_tx_ready(uint16_t connHandle, bool opened)
{
    if (opened) {
        s_conn = connHandle;
        s_up   = true;
        sensor_app_set_connected(true);
    } else {
        s_up = false;
        sensor_app_set_connected(false);
    }
}

void ble_link_down(void)
{
    s_up  = false;
    s_mtu = MTU_DEFAULT;
    sensor_app_set_connected(false);

    /* Advertising has to be asked for again. The controller stops advertising
     * the moment a connectable advert wins a connection, and nothing restarts
     * it: the stock example calls BLE_GAP_SetAdvEnable exactly once, in
     * APP_STATE_INIT, and its DISCONNECTED handler does not. So after one
     * disconnect the board was invisible until it was reset -- the host could
     * see it had gone, scan for it, find nothing, and the only cure was the
     * power switch. Re-enabling here, where the link is already known to be
     * down, makes a reconnect a matter of asking twice rather than of walking
     * over to the board. */
    (void)BLE_GAP_SetAdvEnable(0x01, 0x00);
}

void ble_link_mtu_set(uint16_t attMtu)
{
    uint16_t payload = (attMtu > 3u) ? (uint16_t)(attMtu - 3u) : MTU_DEFAULT;
    s_mtu = (payload > MTU_CEILING) ? MTU_CEILING : payload;
}

/* Pull whatever the central wrote and hand it to the application. */
void ble_link_rx(uint16_t connHandle)
{
    uint8_t  buf[64];
    uint16_t len = 0u;

    BLE_TRSPS_GetDataLength(connHandle, &len);
    if (len == 0u) { return; }
    if (len > sizeof buf) { len = sizeof buf; }

    if (BLE_TRSPS_GetData(connHandle, buf) == MBA_RES_SUCCESS) {
        sensor_app_rx(buf, len);
    }
}

/* ---- this board's own Bluetooth identity -------------------------------
 *
 * Lives here rather than in a file of its own because the MPLAB project's
 * generated Makefile-default.mk is only rebuilt from configurations.xml when
 * the IDE notices the project changed. Adding a source file by editing
 * configurations.xml therefore links against nothing: the compile list still
 * came from the stale makefile and the build failed with "undefined reference
 * to ble_identity_configure". ble_link.c is already in the project, so putting
 * it here means a plain Clean and Build is enough.
 */

/* The example's address, kept as the fallback so behaviour is unchanged on a
 * board that cannot do better. */
static const uint8_t FALLBACK_ADDR[6] = {0xA1u, 0xA2u, 0xA3u, 0xA4u, 0xA5u, 0xA6u};

/* "HES_" plus an eight hex digit UID taken from this board's address, so a name
 * reads HES_6e4d43be: short, and unique per board without a lookup.
 *
 * The UID is the low four bytes of the BLE address, printed most significant
 * first so it reads as the tail of the address a host shows. The top two bytes
 * are dropped deliberately -- they are Microchip's 9C:95 OUI, identical on every
 * board, so they would cost eight characters and carry no information.
 *
 * Six boards on a bench all announcing one fixed string would be a scan list you
 * have to cross-reference against addresses to use, which is why the UID is in
 * the name at all rather than left to the address column.
 *
 * GAP_MAX_DEVICE_NAME_LEN is 32 and this is 12, so there is room to spare; the
 * advertising payload comes to 3 bytes of flags plus 2 + 12 for the name, 17 of
 * the 31 available. Buffers size themselves from the prefix and the digit count,
 * so changing either cannot overrun them. */
#define MS_NAME_PREFIX   "HES_"
#define MS_UID_DIGITS    8u
#define MS_NAME_LEN      (sizeof MS_NAME_PREFIX - 1u + MS_UID_DIGITS)

static char s_name[MS_NAME_LEN + 1u];
static uint8_t s_addr[6];

static void hex2(char *out, uint8_t v)
{
    static const char D[] = "0123456789abcdef";
    out[0] = D[(v >> 4) & 0x0Fu];
    out[1] = D[v & 0x0Fu];
}

/* An address is usable as an identity if it is neither all zero nor all ones.
 * Both are what unprogrammed storage reads as, and neither is a legal public
 * address. */
static bool addr_usable(const uint8_t *a)
{
    uint8_t and_all = 0xFFu, or_all = 0x00u;
    for (uint32_t i = 0u; i < 6u; i++) {
        and_all &= a[i];
        or_all  |= a[i];
    }
    return (or_all != 0x00u) && (and_all != 0xFFu);
}

const char *ble_identity_name(void) { return s_name; }

void ble_identity_configure(void)
{
    BLE_GAP_Addr_T addr;
    bool own = false;

    (void)memset(&addr, 0, sizeof addr);
    if (BLE_GAP_GetDeviceAddr(&addr) == 0u) {
        own = addr_usable(addr.addr);
    }

    if (!own) {
        addr.addrType = BLE_GAP_ADDR_TYPE_PUBLIC;
        (void)memcpy(addr.addr, FALLBACK_ADDR, sizeof FALLBACK_ADDR);
    }
    /* Set it either way: on the keep path this is a no-op that also confirms
     * the stack accepted the type, and on the fallback path it is the real
     * assignment. */
    (void)BLE_GAP_SetDeviceAddr(&addr);
    (void)memcpy(s_addr, addr.addr, sizeof s_addr);

    /* addr[0] is the least significant byte, so a host prints the six bytes as
     * addr[5] first and addr[0] last. Taking addr[3] down to addr[0] in that
     * order makes the eight digits read as the tail of the printed address. */
    (void)memcpy(s_name, MS_NAME_PREFIX, sizeof MS_NAME_PREFIX - 1u);
    for (uint32_t i = 0u; i < 4u; i++) {
        hex2(&s_name[sizeof MS_NAME_PREFIX - 1u + (i * 2u)], s_addr[3u - i]);
    }
    s_name[MS_NAME_LEN] = '\0';

    /* Flags, then the complete local name. 0x06 is general discoverable with
     * BR/EDR unsupported; the example used 0x05, limited discoverable, which
     * some scanners treat as a short-lived advert. */
    uint8_t adv[3u + 2u + MS_NAME_LEN];
    uint8_t n = (uint8_t)strlen(s_name);
    adv[0] = 0x02u; adv[1] = 0x01u; adv[2] = 0x06u;
    adv[3] = (uint8_t)(n + 1u);
    adv[4] = 0x09u;
    (void)memcpy(&adv[5], s_name, n);

    BLE_GAP_AdvDataParams_T p;
    p.advLen = (uint8_t)(5u + n);
    (void)memcpy(p.advData, adv, p.advLen);
    (void)BLE_GAP_SetAdvData(&p);

    /* The same name in the scan response, so a host that only caches the scan
     * response still sees it. */
    BLE_GAP_AdvDataParams_T r;
    r.advLen = (uint8_t)(2u + n);
    r.advData[0] = (uint8_t)(n + 1u);
    r.advData[1] = 0x09u;
    (void)memcpy(&r.advData[2], s_name, n);
    (void)BLE_GAP_SetScanRspData(&r);

    (void)BLE_GAP_SetDeviceName(n, (uint8_t *)s_name);
}
