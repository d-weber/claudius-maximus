#pragma once
// LVGL heap -> PSRAM. Internal RAM is reserved for DMA draw buffers, RGB bounce buffers and stacks;
// hundreds of widgets would otherwise exhaust it (seen: 388 B free).
#include <stddef.h>
#include <esp_heap_caps.h>

static inline void *lv_psram_malloc(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}
static inline void *lv_psram_realloc(void *p, size_t n) {
  void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return q ? q : heap_caps_realloc(p, n, MALLOC_CAP_8BIT);
}
static inline void lv_psram_free(void *p) { heap_caps_free(p); }
