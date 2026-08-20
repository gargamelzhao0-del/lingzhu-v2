/*
 * Miaopan 2.0 board pin map derived from the 2026-05-08/09 schematics.
 * The legacy header name is retained to preserve the established audio-board
 * interface while this V2 product tree targets the revised hardware.
 *
 * Audio topology: one ESP32-S3 I2S full-duplex port, ES8311 DAC TX and
 * ES7210 four-slot TDM RX. The codec rail is CODEC_3V3 from the schematic,
 * not a GPIO-controlled power switch.
 */
#ifndef NUWAX_BOARD_ESP_VOCAT_V1_0_PINS_H
#define NUWAX_BOARD_ESP_VOCAT_V1_0_PINS_H

/* ---- Shared I2C bus (touch / IMU / gauge / codec) ---- */
#define NUWAX_VOCAT_I2C_SCL_GPIO        1
#define NUWAX_VOCAT_I2C_SDA_GPIO        2
/* 7-bit I2C addresses on the shared bus */
#define NUWAX_VOCAT_I2C_ADDR_ES8311     0x18  /* speaker codec (DAC) */
#define NUWAX_VOCAT_I2C_ADDR_ES7210     0x40  /* mic codec (4ch ADC) */
#define NUWAX_VOCAT_CODEC_DEV_ADDR_ES8311 (NUWAX_VOCAT_I2C_ADDR_ES8311 << 1)
#define NUWAX_VOCAT_CODEC_DEV_ADDR_ES7210 (NUWAX_VOCAT_I2C_ADDR_ES7210 << 1)
#define NUWAX_VOCAT_I2C_ADDR_QMI8658A   0x6A  /* 6-axis IMU (原理图QMI8658A, 非BMI270) */
#define NUWAX_VOCAT_I2C_ADDR_BQ27220    0x55  /* fuel gauge (1200 mAh) */
#define NUWAX_VOCAT_I2C_ADDR_QMI8658A_ALT 0x6B /* SA0-dependent alternate address */
/* CST816S/CST816-family touch shares TP_SCL/TP_SDA. */
#define NUWAX_VOCAT_I2C_ADDR_TOUCH_PRIMARY 0x15
#define NUWAX_VOCAT_I2C_ADDR_TOUCH_ALT_2A  0x2A
#define NUWAX_VOCAT_I2C_ADDR_TOUCH_ALT_2D  0x2D

/* ---- Audio I2S (FINAL single-port mixed mode, P2-2; not for P1) ---- */
#define NUWAX_VOCAT_I2S_MCLK_GPIO       42    /* 256 x fs = 4.096 MHz */
#define NUWAX_VOCAT_I2S_BCK_GPIO        40
#define NUWAX_VOCAT_I2S_WS_GPIO         39
#define NUWAX_VOCAT_I2S_DOUT_GPIO       41    /* -> ES8311 */
#define NUWAX_VOCAT_I2S_DIN_GPIO        38    /* <- ES7210 I2S_DI/TDMOUT */
#define NUWAX_VOCAT_PA_CTRL_GPIO        4     /* GPIO4 -> NS4150B PA_CTRL */
#define NUWAX_VOCAT_CODEC_PWR_GPIO      (-1)  /* CODEC_3V3 DCDC; no GPIO switch in schematic */

/* ---- Power / control ----
 * The updated Miaopan schematic labels this net PG2.  It is driven by
 * ESP32-S3 GPIO7 and feeds the SS8050/Q2-Q3 hold circuit.  A high level
 * keeps the rail alive after SW3 is released. */
#define NUWAX_VOCAT_POWER_CTRL_GPIO     7     /* PG2 -> Q4 base, active-high hold */

/* ---- Touch signals ----
 * TP_INT is the CST816-family interrupt line. TOUCH is a separate wake/input net. */
#define NUWAX_VOCAT_TP_INT_GPIO         11    /* TP_INT */
#define NUWAX_VOCAT_TOUCH_WAKE_GPIO     8     /* separate TOUCH net */

/* ---- LCD QSPI (ST77916, 360x360) ----
 * QSPI has no DC line; command/data are encoded in the QSPI frame.
 * Old LCD_DC=GPIO45 / LCD_TE=GPIO8 assumptions do not apply to Miaopan 2.0. */
#define NUWAX_VOCAT_LCD_D0_GPIO         13    /* LCD_SDA0 */
#define NUWAX_VOCAT_LCD_D1_GPIO         9     /* LCD_SDA1 */
#define NUWAX_VOCAT_LCD_D2_GPIO         12    /* LCD_SDA2 */
#define NUWAX_VOCAT_LCD_D3_GPIO         46    /* LCD_SDA3 */
#define NUWAX_VOCAT_LCD_SCL_GPIO        14
#define NUWAX_VOCAT_LCD_CS_GPIO         10
#define NUWAX_VOCAT_LCD_RST_GPIO        21
#define NUWAX_VOCAT_LCD_BLK_GPIO        3     /* active-high NMOS backlight control */
#define NUWAX_VOCAT_LCD_TE_GPIO         17    /* ST77916 tearing-effect output */

/* ---- SD (1-bit) ---- */
#define NUWAX_VOCAT_SD_CLK_GPIO         48
#define NUWAX_VOCAT_SD_CMD_GPIO         45
#define NUWAX_VOCAT_SD_D0_GPIO          47

/* ---- Misc ---- */
#define NUWAX_VOCAT_LED_G_GPIO          18
#define NUWAX_VOCAT_POWER_KEY_GPIO      16    /* PG1/SW3, active-low */
#define NUWAX_VOCAT_UART1_TX_GPIO       43    /* ESP32-S3 module U0TXD -> U1TXD */
#define NUWAX_VOCAT_UART1_RX_GPIO       44    /* ESP32-S3 module U0RXD <- U1RXD */
#define NUWAX_VOCAT_4G_UART_RX_GPIO     5     /* module TX / 4G_TX net -> ESP RX */
#define NUWAX_VOCAT_4G_UART_TX_GPIO     6     /* ESP TX -> module RX / 4G_RX net */
#define NUWAX_VOCAT_4G_ENABLE_GPIO      15    /* high enables the 4.231 V rail */
#define NUWAX_VOCAT_BOOT_GPIO           0     /* [STRAP] ESP_BOOT */

/* ---- ESP32-S3 strap pins: validate reset-time external levels/eFuses ---- */
#define NUWAX_VOCAT_STRAP_BOOT_GPIO     0
#define NUWAX_VOCAT_STRAP_GPIO3         3     /* LCD backlight MOSFET gate */
#define NUWAX_VOCAT_STRAP_GPIO45        45    /* SD_CMD with external pull-up */
#define NUWAX_VOCAT_STRAP_LCD_D3_GPIO   46    /* LCD QSPI D3 */

#endif /* NUWAX_BOARD_ESP_VOCAT_V1_0_PINS_H */
