#pragma once
// Board profile: which Waveshare 4" board the firmware is built for. Both carry the same ESP32-S3R8 module
// (8 MB OPI PSRAM, 16 MB flash), ST7701 480x480 RGB panel and GT911 touch; they differ in the pin map, the IO
// expander and the backlight driver. Selected by the PlatformIO env (platformio.ini):
//   -DCM_BOARD_4B  ESP32-S3-Touch-LCD-4B: TCA9554 expander @0x20 carries the panel's 3-wire SPI init bus, the
//                  panel reset and the touch reset/INT; backlight PWM on GPIO4 (AP3032, inverted); I2C 47/48.
//                  Host link = the "USB-OTG" port (native USB-Serial-JTAG). The "USB TO UART" port is a CH343
//                  on UART0 and is not used.
//   -DCM_BOARD_V4  ESP32-S3-Touch-LCD-4 V4: CH32V003 expander @0x24 (display power, resets, backlight PWM);
//                  I2C 15/7; the only USB port is the native one.
// Sources: 4B schematic + Waveshare BSP (waveshare/esp32_s3_touch_lcd_4b) + Arduino demo 06_LVGL_Arduino_v9.
#if defined(CM_BOARD_4B) && defined(CM_BOARD_V4)
#error "CM_BOARD_4B and CM_BOARD_V4 are exclusive"
#endif
#if !defined(CM_BOARD_4B) && !defined(CM_BOARD_V4)
#define CM_BOARD_4B 1
#endif
#ifndef CM_BOARD_4B
#define CM_BOARD_4B 0
#endif
#ifndef CM_BOARD_V4
#define CM_BOARD_V4 0
#endif

#if CM_BOARD_4B
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
#else
#define BOARD_NAME "ESP32-S3-Touch-LCD-4 V4"
#define BOARD_I2C_SDA 15
#define BOARD_I2C_SCL 7
#define BOARD_LCD_HSYNC 38
#define BOARD_LCD_VSYNC 39
#define BOARD_LCD_DE 40
#define BOARD_LCD_PCLK 41
#define BOARD_LCD_DATA {5, 45, 48, 47, 21, 14, 13, 12, 11, 10, 9, 46, 3, 8, 18, 17}
#endif

// Panel orientation: 1 = the UI is drawn rotated by 180 degrees (the picture upright with the board's
// connectors on the side the user wants). Confirmed on the V4; set per build with -DHAL_ROT180=0/1.
#ifndef HAL_ROT180
#define HAL_ROT180 1
#endif
