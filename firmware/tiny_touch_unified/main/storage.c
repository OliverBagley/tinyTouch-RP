#include "storage.h"

#include <string.h>

#include "flash_layout.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "mbedtls/sha256.h"

#define STORAGE_MAGIC 0x31535454u  // "TTS1"

typedef struct {
  uint32_t magic;
  uint32_t length;
  uint8_t digest[32];
} storage_header_t;

typedef struct {
  uint32_t offset;
  uint32_t size;
} region_t;

static const region_t regions[] = {
  [STORAGE_CONFIG] = {FLASH_CONFIG_OFFSET, FLASH_CONFIG_SIZE},
  [STORAGE_PIV] = {FLASH_PIV_OFFSET, FLASH_PIV_SIZE},
};

// ponytail: one RAM image the size of the largest region (12 KB); a streaming
// writer would save RAM if regions ever grow.
static uint8_t __attribute__((aligned(4))) image[FLASH_PIV_SIZE];

void storage_flash_erase(uint32_t offset, size_t length) {
  uint32_t saved = save_and_disable_interrupts();
  flash_range_erase(offset, length);
  restore_interrupts(saved);
}

void storage_flash_program(uint32_t offset, const uint8_t *data, size_t length) {
  uint32_t saved = save_and_disable_interrupts();
  flash_range_program(offset, data, length);
  restore_interrupts(saved);
}

bool storage_read(storage_region_t region, void *out, size_t capacity, size_t *length) {
  if (region > STORAGE_PIV || !out) return false;
  const region_t *r = &regions[region];
  const storage_header_t *header = (const storage_header_t *)(XIP_BASE + r->offset);
  if (header->magic != STORAGE_MAGIC || header->length > capacity ||
      header->length > r->size - sizeof(*header)) return false;
  const uint8_t *payload = (const uint8_t *)(header + 1);
  uint8_t digest[32];
  mbedtls_sha256(payload, header->length, digest, 0);
  bool ok = memcmp(digest, header->digest, sizeof(digest)) == 0;
  if (ok) {
    memcpy(out, payload, header->length);
    if (length) *length = header->length;
  }
  return ok;
}

bool storage_write(storage_region_t region, const void *data, size_t length) {
  if (region > STORAGE_PIV || !data) return false;
  const region_t *r = &regions[region];
  if (length > r->size - sizeof(storage_header_t)) return false;
  storage_header_t header = {.magic = STORAGE_MAGIC, .length = (uint32_t)length};
  mbedtls_sha256(data, length, header.digest, 0);
  size_t total = sizeof(header) + length;
  size_t padded = (total + FLASH_PAGE_SIZE - 1) & ~(size_t)(FLASH_PAGE_SIZE - 1);
  memset(image, 0xff, padded);
  memcpy(image, &header, sizeof(header));
  memcpy(image + sizeof(header), data, length);
  storage_flash_erase(r->offset, r->size);
  storage_flash_program(r->offset, image, padded);
  memset(image, 0, padded);
  const storage_header_t *written = (const storage_header_t *)(XIP_BASE + r->offset);
  return written->magic == STORAGE_MAGIC && written->length == length &&
         memcmp(written + 1, data, length) == 0;
}

bool storage_erase_all(void) {
  for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
    storage_flash_erase(regions[i].offset, regions[i].size);
  }
  return true;
}
