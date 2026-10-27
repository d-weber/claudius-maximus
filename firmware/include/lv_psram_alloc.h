#pragma once
// LVGL heap -> PSRAM. Internal RAM is reserved for DMA draw buffers, RGB bounce buffers and stacks;
// hundreds of widgets would otherwise exhaust it (seen: 388 B free). With LV_MEM_CUSTOM == 0 only
// lv_psram_pool() is used (LVGL's own lock-free TLSF on one PSRAM block); the others serve LV_MEM_CUSTOM == 1.
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
/* LV_MEM_CUSTOM == 0: the one-time pool for LVGL's own allocator (see lv_conf.h). */
static inline void *lv_psram_pool(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
