/* board_pins.h -- mini sensor board, WBZ451PE-I.
 *
 * Traced from 16_Sensor_Board_Stage2.pdf; see docs/PINMAP.md for how, and for
 * the 330R series resistor on each net so a pin can be checked on the board.
 *
 * The port constants come from Harmony's generated plib_gpio.h. Harmony names
 * each pin after whatever label is typed into the Pin Settings grid, so the
 * names below are what this project expects that grid to contain.
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include "definitions.h"

/* SPI on SERCOM1, confirmed against the Harmony pin table: the three nets land
 * on SERCOM1_PAD0, PAD1 and PAD2, which is the canonical data-out, clock,
 * data-in arrangement. The board was designed around it.
 *
 * MCC names the silicon pins RAn/RBn; the module data sheet calls the same pins
 * PAn/PBn. docs/PINMAP.md uses the data-sheet names, this file uses MCC's. */
#define BOARD_SPI_SCK_PORTPIN    GPIO_PIN_RA8    /* module pin 29, via R8  */
#define BOARD_SPI_MISO_PORTPIN   GPIO_PIN_RA9    /* module pin 30, via R10 */
#define BOARD_SPI_MOSI_PORTPIN   GPIO_PIN_RA7    /* module pin 31, via R9  */

/* All sixteen LSM6DSOX INT1 lines share one net, so the line says that some
 * device is ready, never which. Capture does not use it; see README. */
#define BOARD_IMU_INT_PIN        GPIO_PIN_RB4    /* module pin 39, via R21 */

#define BOARD_BTN_PIN            GPIO_PIN_RA3    /* module pin 36, direct  */
#define BOARD_LED_PIN            GPIO_PIN_RA5    /* module pin 24, direct  */

/* Active high: RA5 high lights the LED. An earlier guess of active-low made a
 * 60 ms on / 640 ms off heartbeat look like a steady lit LED, which is how the
 * polarity was caught. Patterns below are symmetric so this can never disguise
 * one again. */
#define BOARD_LED_ACTIVE_LOW     0

#if BOARD_LED_ACTIVE_LOW
#define BOARD_LED_ON()   GPIO_PinClear(BOARD_LED_PIN)
#define BOARD_LED_OFF()  GPIO_PinSet(BOARD_LED_PIN)
#else
#define BOARD_LED_ON()   GPIO_PinSet(BOARD_LED_PIN)
#define BOARD_LED_OFF()  GPIO_PinClear(BOARD_LED_PIN)
#endif

/* All sixteen chip selects. The right-hand side was traced individually; the
 * left-hand side was traced where the resistor sits in line with its pin (CS0,
 * CS3, CS4) and resolved by ordering for the rest -- the fan-out staircase does
 * not cross, and nine untraced nets matched nine free pins with none left over.
 *
 * Step 2 verifies every one of these empirically by driving it low and reading
 * WHO_AM_I, so a mistake here shows up immediately rather than silently.
 *
 * PB0, PB8 and PB9 are deliberately absent: two of them carry SWD_CLK and
 * SWD_IO and driving those as GPIO would cost the debugger.
 */
#define BOARD_CS0_PIN            GPIO_PIN_RB5    /* pin  6, via R30 */
#define BOARD_CS1_PIN            GPIO_PIN_RB1    /* pin 38, via R20 */
#define BOARD_CS2_PIN            GPIO_PIN_RB2    /* pin 37, via R19 */
#define BOARD_CS3_PIN            GPIO_PIN_RB6    /* pin 12, via R29 */
#define BOARD_CS4_PIN            GPIO_PIN_RA4    /* pin 16, via R28 */
#define BOARD_CS5_PIN            GPIO_PIN_RB10   /* pin 17, via R27 */
#define BOARD_CS6_PIN            GPIO_PIN_RB11   /* pin 18, via R26 */
#define BOARD_CS7_PIN            GPIO_PIN_RB12   /* pin 22, via R23 */
#define BOARD_CS8_PIN            GPIO_PIN_RB13   /* pin 23, via R22 */
#define BOARD_CS9_PIN            GPIO_PIN_RA6    /* pin 25, via R14 */
#define BOARD_CS10_PIN           GPIO_PIN_RA0    /* pin 20, via R25 */
#define BOARD_CS11_PIN           GPIO_PIN_RA2    /* pin 35, via R18 */
#define BOARD_CS12_PIN           GPIO_PIN_RA1    /* pin 21, via R24 */
#define BOARD_CS13_PIN           GPIO_PIN_RA10   /* pin 32, via R15 */
#define BOARD_CS14_PIN           GPIO_PIN_RA13   /* pin 33, via R16 */
#define BOARD_CS15_PIN           GPIO_PIN_RA14   /* pin 34, via R17 */

#define BOARD_CS_COUNT           16

/* Bring-up talks to one sensor only, the one on CS1. */
#define BRINGUP_CS_PIN           BOARD_CS1_PIN

#endif /* BOARD_PINS_H */
