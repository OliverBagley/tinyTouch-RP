#include "firmware_update.h"

#include <string.h>

#include "flash_layout.h"
#include "hardware/flash.h"
#include "hardware/regs/m0plus.h"
#include "hardware/structs/scb.h"
#include "hardware/sync.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "pico/platform.h"
#include "signing_public_key.h"
#include "storage.h"

// OTA image layout: application binary followed by an RSA-3072 PKCS#1 v1.5
// signature over SHA-256 of the application.
#define SIGNATURE_SIZE 384
#define UPDATE_MARKER_MAGIC 0x50555454u  // "TTUP"

typedef struct {
  uint32_t magic;
  uint32_t size;
  uint8_t digest[32];
} update_marker_t;

typedef enum {
  OTA_IDLE,
  OTA_WRITING,
  OTA_STAGED,
} ota_state_t;

static size_t expected_size;
static size_t written_size;
static size_t programmed_size;
static uint8_t expected_hash[32];
static mbedtls_sha256_context hash_context;
static bool hash_started;
static uint8_t page[FLASH_PAGE_SIZE];
static size_t page_fill;
static ota_state_t state;

static const uint8_t *staging(void) {
  return (const uint8_t *)(XIP_BASE + FLASH_STAGING_OFFSET);
}

static void program_page(void) {
  if (!page_fill) return;
  memset(page + page_fill, 0xff, sizeof(page) - page_fill);
  if (programmed_size % FLASH_SECTOR_SIZE == 0) {
    storage_flash_erase(FLASH_STAGING_OFFSET + programmed_size, FLASH_SECTOR_SIZE);
  }
  storage_flash_program(FLASH_STAGING_OFFSET + programmed_size, page, sizeof(page));
  programmed_size += sizeof(page);
  page_fill = 0;
}

void firmware_update_abort(void) {
  if (hash_started) mbedtls_sha256_free(&hash_context);
  expected_size = 0;
  written_size = 0;
  programmed_size = 0;
  page_fill = 0;
  memset(page, 0, sizeof(page));
  memset(expected_hash, 0, sizeof(expected_hash));
  hash_started = false;
  state = OTA_IDLE;
}

bool firmware_update_begin(size_t size, const uint8_t expected_sha256[32]) {
  if (!expected_sha256) return false;
  firmware_update_abort();
  if (size <= SIGNATURE_SIZE + FLASH_PAGE_SIZE || size > FLASH_STAGING_SIZE ||
      size - SIGNATURE_SIZE > FLASH_APP_SIZE) {
    return false;
  }
  // A previously staged image is void once a new upload starts.
  storage_flash_erase(FLASH_UPDATE_MARKER_OFFSET, FLASH_SECTOR_SIZE);
  mbedtls_sha256_init(&hash_context);
  if (mbedtls_sha256_starts(&hash_context, 0) != 0) {
    mbedtls_sha256_free(&hash_context);
    return false;
  }
  memcpy(expected_hash, expected_sha256, sizeof(expected_hash));
  expected_size = size;
  hash_started = true;
  state = OTA_WRITING;
  return true;
}

bool firmware_update_write(size_t offset, const uint8_t *data, size_t length) {
  if (state != OTA_WRITING || !data || offset != written_size || length == 0 ||
      length > FIRMWARE_UPDATE_CHUNK_MAX || length > expected_size - written_size ||
      mbedtls_sha256_update(&hash_context, data, length) != 0) {
    firmware_update_abort();
    return false;
  }
  while (length) {
    size_t take = sizeof(page) - page_fill;
    if (take > length) take = length;
    memcpy(page + page_fill, data, take);
    page_fill += take;
    data += take;
    length -= take;
    written_size += take;
    if (page_fill == sizeof(page)) program_page();
  }
  return true;
}

static bool signature_valid(size_t image_size) {
  size_t payload = image_size - SIGNATURE_SIZE;
  uint8_t digest[32];
  mbedtls_sha256(staging(), payload, digest, 0);
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  bool ok = mbedtls_pk_parse_public_key(
                &key, (const unsigned char *)TINYTOUCH_SIGNING_PUBLIC_KEY_PEM,
                sizeof(TINYTOUCH_SIGNING_PUBLIC_KEY_PEM)) == 0 &&
            mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest),
                              staging() + payload, SIGNATURE_SIZE) == 0;
  mbedtls_pk_free(&key);
  return ok;
}

// The marker is the only thing the next boot trusts. It is written last, after
// the staged bytes and their signature have been verified from flash.
static bool stage_update_marker(void) {
  static uint8_t record[FLASH_PAGE_SIZE];
  update_marker_t marker = {.magic = UPDATE_MARKER_MAGIC, .size = (uint32_t)expected_size};
  memcpy(marker.digest, expected_hash, sizeof(marker.digest));
  memset(record, 0xff, sizeof(record));
  memcpy(record, &marker, sizeof(marker));
  storage_flash_program(FLASH_UPDATE_MARKER_OFFSET, record, sizeof(record));
  return memcmp((const void *)(XIP_BASE + FLASH_UPDATE_MARKER_OFFSET), &marker,
                sizeof(marker)) == 0;
}

bool firmware_update_commit(void) {
  if (state != OTA_WRITING || written_size != expected_size) return false;
  program_page();
  uint8_t actual_hash[32];
  bool ok = mbedtls_sha256_finish(&hash_context, actual_hash) == 0 &&
            memcmp(actual_hash, expected_hash, sizeof(actual_hash)) == 0;
  mbedtls_sha256_free(&hash_context);
  hash_started = false;
  if (ok) {
    mbedtls_sha256(staging(), expected_size, actual_hash, 0);
    ok = memcmp(actual_hash, expected_hash, sizeof(actual_hash)) == 0;
  }
  if (ok) ok = signature_valid(expected_size);
  if (ok) ok = stage_update_marker();
  memset(actual_hash, 0, sizeof(actual_hash));
  expected_size = 0;
  written_size = 0;
  programmed_size = 0;
  memset(expected_hash, 0, sizeof(expected_hash));
  state = ok ? OTA_STAGED : OTA_IDLE;
  return ok;
}

bool firmware_update_active(void) { return state == OTA_WRITING; }
bool firmware_update_staged(void) { return state == OTA_STAGED; }
size_t firmware_update_written(void) { return written_size; }

// Runs entirely from RAM: it erases the application it was loaded from. The
// SDK flash routines and the boot2 copy it uses are RAM resident as well.
// Erasing sector 0 first means a power loss mid-copy leaves the ROM
// bootloader in BOOTSEL mode, which is the recovery path anyway.
// ponytail: a resident second-stage bootloader with A/B slots would survive a
// mid-copy power loss without BOOTSEL; add it if field reports need it.
static uint8_t __attribute__((aligned(4))) copy_buffer[FLASH_SECTOR_SIZE];

static void __attribute__((noreturn, optimize("no-tree-loop-distribute-patterns")))
__no_inline_not_in_flash_func(install_staged)(uint32_t size) {
  uint32_t total = (size + FLASH_SECTOR_SIZE - 1) & ~(uint32_t)(FLASH_SECTOR_SIZE - 1);
  const volatile uint8_t *source = (const volatile uint8_t *)(XIP_BASE + FLASH_STAGING_OFFSET);
  for (uint32_t offset = 0; offset < total; offset += FLASH_SECTOR_SIZE) {
    for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++) copy_buffer[i] = source[offset + i];
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, copy_buffer, FLASH_SECTOR_SIZE);
  }
  flash_range_erase(FLASH_UPDATE_MARKER_OFFSET, FLASH_SECTOR_SIZE);
  __dsb();
  scb_hw->aircr = (0x05fau << M0PLUS_AIRCR_VECTKEY_LSB) | M0PLUS_AIRCR_SYSRESETREQ_BITS;
  __dsb();
  for (;;) {}
}

void firmware_update_apply_pending(void) {
  const update_marker_t *marker =
      (const update_marker_t *)(XIP_BASE + FLASH_UPDATE_MARKER_OFFSET);
  if (marker->magic != UPDATE_MARKER_MAGIC) return;
  uint32_t size = marker->size;
  bool valid = size > SIGNATURE_SIZE && size <= FLASH_STAGING_SIZE &&
               size - SIGNATURE_SIZE <= FLASH_APP_SIZE;
  if (valid) {
    uint8_t digest[32];
    mbedtls_sha256(staging(), size, digest, 0);
    valid = memcmp(digest, marker->digest, sizeof(digest)) == 0;
  }
  if (!valid) {
    storage_flash_erase(FLASH_UPDATE_MARKER_OFFSET, FLASH_SECTOR_SIZE);
    return;
  }
  (void)save_and_disable_interrupts();
  install_staged(size);
}
