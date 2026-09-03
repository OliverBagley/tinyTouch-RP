#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Small persistent records in dedicated flash sectors. Each record carries a
// SHA-256 so a torn write is read back as "empty" instead of as garbage.
typedef enum {
  STORAGE_CONFIG = 0,
  STORAGE_PIV = 1,
} storage_region_t;

bool storage_read(storage_region_t region, void *out, size_t capacity, size_t *length);
bool storage_write(storage_region_t region, const void *data, size_t length);
bool storage_erase_all(void);

// Raw sector helpers used by the firmware updater. Offsets are relative to the
// start of flash; erase lengths are sector multiples and program lengths are
// page multiples.
void storage_flash_erase(uint32_t offset, size_t length);
void storage_flash_program(uint32_t offset, const uint8_t *data, size_t length);
