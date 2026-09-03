#pragma once

#include "hardware/flash.h"
#include "pico.h"

// 2 MB flash on the Waveshare RP2040-Zero. The application runs from offset 0.
// An OTA image is written to the staging region, verified, and copied over the
// application on the next boot. Persistent records live in the last sectors.
#define FLASH_APP_OFFSET 0u
#define FLASH_APP_SIZE (992u * 1024u)
#define FLASH_STAGING_OFFSET FLASH_APP_SIZE
#define FLASH_STAGING_SIZE FLASH_APP_SIZE
#define FLASH_UPDATE_MARKER_OFFSET (FLASH_STAGING_OFFSET + FLASH_STAGING_SIZE)
#define FLASH_PIV_OFFSET (FLASH_UPDATE_MARKER_OFFSET + FLASH_SECTOR_SIZE)
#define FLASH_PIV_SIZE (3u * FLASH_SECTOR_SIZE)
#define FLASH_CONFIG_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define FLASH_CONFIG_SIZE FLASH_SECTOR_SIZE

_Static_assert(FLASH_PIV_OFFSET + FLASH_PIV_SIZE <= FLASH_CONFIG_OFFSET, "flash layout overlaps");
_Static_assert(FLASH_CONFIG_OFFSET + FLASH_CONFIG_SIZE == PICO_FLASH_SIZE_BYTES, "config sector must end the flash");
