#pragma once
// Board profile: the Waveshare ESP32-S3-Touch-LCD-4B (ESP32-S3R8 module, 8 MB OPI PSRAM, 16 MB flash, ST7701
// 480x480 RGB panel, GT911 touch). A TCA9554 expander @0x20 carries the panel's 3-wire SPI init bus, the panel
// reset and the touch reset/INT; backlight PWM on GPIO4 (AP3032, inverted); I2C on 47/48.
// Host link = the "USB-OTG" port (native USB-Serial-JTAG). The "USB TO UART" port is a CH343 on UART0 and is
// only used for flashing.
// Sources: 4B schematic + Waveshare BSP (waveshare/esp32_s3_touch_lcd_4b) + Arduino demo 06_LVGL_Arduino_v9.
// (The older ESP32-S3-Touch-LCD-4 V4 with its CH32V003 expander was supported until firmware 0.3.0.)

#define BOARD_NAME "ESP32-S3-Touch-LCD-4B"
#define BOARD_I2C_SDA 47
#define BOARD_I2C_SCL 48
#define BOARD_LCD_HSYNC 46
#define BOARD_LCD_VSYNC 3
#define BOARD_LCD_DE 17
#define BOARD_LCD_PCLK 9
// esp_lcd order: B0-4 G0-5 R0-4 (schematic nets B1-5 = IO40 41 42 2 1, G0-5 = IO21 8 18 45 38 39, R1-5 = IO10-14)
#define BOARD_LCD_DATA {40, 41, 42, 2, 1, 21, 8, 18, 45, 38, 39, 10, 11, 12, 13, 14}
#define BOARD_EXPANDER_ADDR 0x20
#define BOARD_EXIO_LCD_CS 0
#define BOARD_EXIO_LCD_SDA 1
#define BOARD_EXIO_LCD_SCL 2
// EXIO3 = audio amp enable, EXIO4 = PWR key sense (input): both left alone
#define BOARD_EXIO_TP_RST 5
#define BOARD_EXIO_TP_INT 6
#define BOARD_EXIO_LCD_RST 7
#define BOARD_BL_PIN 4

// Panel orientation: 1 = the UI is drawn rotated by 180 degrees (the picture upright with the board's
// connectors on the side the user wants). Set per build with -DHAL_ROT180=0/1.
#ifndef HAL_ROT180
#define HAL_ROT180 1
#endif
