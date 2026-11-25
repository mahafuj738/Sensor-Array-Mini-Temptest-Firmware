# Mini sensor board — WBZ451 pin map

Derived from `16_Sensor_Board_Stage2.pdf` (Rev 1.0, 2026-06-10) by extracting the
PDF text layer for the module symbol and rendering the wire fan-out at 600 and
1200 dpi to follow each stepped net to its pin. The module pinout was confirmed
independently against the WBZ451 data-sheet symbol in `zoom.png`.

Every signal except `User_BTN` and `User_LED` reaches the module through a 330 R
series resistor; the designator is given so each can be checked on the board.

## Signals needed to bring the board up

| net | WBZ451 pin | port | via |
|-----|-----------|------|-----|
| SPI_SCK   | 29 | PA8  | R8  |
| SPI_MISO  | 30 | PA9  | R10 |
| SPI_MOSI  | 31 | PA7  | R9  |
| IMU_INT   | 39 | PB4  | R21 |
| User_BTN  | 36 | PA3  | direct |
| User_LED  | 24 | PA5  | direct |
| Reset     |  3 | nMCLR | R11 |

## Chip selects confirmed from the schematic

| net | pin | port | via |   | net | pin | port | via |
|-----|-----|------|-----|---|-----|-----|------|-----|
| CS1  | 38 | PB1  | R20 |   | CS13 | 32 | PA10 | R15 |
| CS2  | 37 | PB2  | R19 |   | CS14 | 33 | PA13 | R16 |
| CS9  | 25 | PA6  | R14 |   | CS15 | 34 | PA14 | R17 |
| CS11 | 35 | PA2  | R18 |   |      |    |      |     |

`CS0`, `CS3`-`CS8`, `CS10` and `CS12` leave the left-hand side of the module and
were not traced. They do not need to be: bring-up discovers them by driving each
candidate pin low and reading `WHO_AM_I`, where `0x6C` identifies a chip select.
The only other free signals on that side are the LED and the SWD pads, so probing
is harmless.

## No-connect pins

5 (PB3), 9 (PA11), 10 (PA12), 13 (PB7) carry no-connect flags on the schematic.

## Verified on hardware, 2026-10-01

**All sixteen sensors answered `WHO_AM_I` = `0x6C`.** Every line in the tables
above is confirmed on silicon, not inferred from the drawing: the SPI bus, all
sixteen chip selects, and the LED.

Two settings are needed and each one fails as a completely silent bus, with
identical symptoms: `SCOM1_HSEN = DIRECT` in DEVCFG1, and `DIPO = PAD2` on the
SERCOM1 SPI component. Both default wrong and neither is visible in
`plib_gpio.c`.

## Caveat

Traced from a rendered PDF, not from a netlist. The seven signals in the first
table are the ones firmware depends on and each was followed individually at
1200 dpi. If a netlist ever becomes available, check them against it.
