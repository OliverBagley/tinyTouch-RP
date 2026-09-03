#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pico/rand.h"
#include "pico/time.h"
#include "pico/unique_id.h"

// Debug logging is compiled out: no UART is free on the RP2040-Zero build.
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)

static inline int64_t now_us(void) { return (int64_t)time_us_64(); }

// ponytail: pico_rand mixes ROSC samples into a seeded xoroshiro stream; a
// dedicated TRNG conditioner would be the upgrade if key quality is audited.
static inline void fill_random(void *out, size_t length) {
  uint8_t *cursor = out;
  while (length) {
    uint64_t value = get_rand_64();
    size_t take = length < sizeof(value) ? length : sizeof(value);
    memcpy(cursor, &value, take);
    cursor += take;
    length -= take;
  }
}

// Six bytes of the flash unique ID, used where the ESP32 build used its MAC.
static inline void board_id(uint8_t out[6]) {
  pico_unique_board_id_t id;
  pico_get_unique_board_id(&id);
  memcpy(out, id.id + 2, 6);
}
