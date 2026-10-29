#pragma once
// USB serial JSON-lines link to the host daemon. See PROTOCOL.md.
#include <ArduinoJson.h>

// Host serial transport: the ESP32-S3 USB-Serial-JTAG peripheral through the IDF usb_serial_jtag driver, whose
// ISR moves a 64-byte packet into its ring buffer in one call. Arduino's HWCDC pushes every byte through a
// FreeRTOS queue from its ISR (~60-85 us per packet with interrupts masked on the LVGL core), which delayed the
// RGB panel's vsync ISR and shifted frames sideways (measured 2026-10-16). The driver is installed from core 0
// so its interrupt never runs on the LVGL core at all.
struct LinkSerial {
  size_t write(const void *buf, size_t n, uint32_t timeout_ms = 20);   // partial on timeout (host not reading)
  size_t print(const char *s) { return write(s, strlen(s)); }
  size_t print(char c) { return write(&c, 1); }
  size_t println(const char *s) { size_t n = print(s); return n + print('\n'); }
  size_t printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  size_t read(uint8_t *buf, size_t n);                                 // non-blocking
  void flush(uint32_t timeout_ms = 1000);
};
extern LinkSerial Link;

void link_serial_init();                   // install the USB serial driver (first thing in setup)
void link_init();
void link_poll();                          // read + dispatch incoming lines (call from loop)
void link_send(JsonDocument &doc);         // send one JSON line to host
void link_action(const char *action, const char *sid);  // {"t":"action",...}
