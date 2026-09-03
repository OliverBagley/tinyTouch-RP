#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "mbedtls/platform.h"
#include "pico/stdlib.h"
#include "task.h"

#include "config_console.h"
#include "device_config.h"
#include "fingerprint.h"
#include "firmware_update.h"
#include "piv.h"
#include "touch_pin_hid.h"
#include "usb_ccid.h"

#ifndef TINYTOUCH_FIRMWARE_VERSION
#define TINYTOUCH_FIRMWARE_VERSION "development"
#endif
#ifndef TINYTOUCH_BUILD_ID
#define TINYTOUCH_BUILD_ID "development"
#endif

// packaging/release_integrity.py locates this descriptor inside the image.
const char tinytouch_image_descriptor[]
    __attribute__((used, section(".rodata.tinytouch_descriptor"))) =
    "\x7f" "TTFW" "\x01"
    "version=" TINYTOUCH_FIRMWARE_VERSION ";project=tiny_touch_unified;build="
    TINYTOUCH_BUILD_ID ";board=rp2040-zero;protocol=6;";

// mbedTLS allocates from the FreeRTOS heap so RSA work on two tasks never
// races the unguarded newlib allocator.
static void *rtos_calloc(size_t count, size_t size) {
  if (size && count > SIZE_MAX / size) return NULL;
  void *block = pvPortMalloc(count * size);
  if (block) memset(block, 0, count * size);
  return block;
}

static void app_task(void *argument) {
  (void)argument;
  device_config_init();
  fingerprint_init();
  // Prime the sensor's live-detection state before the HID task begins. This
  // is the same probe STATUS performs; doing it at boot avoids requiring a
  // host status command after USB reconnect before the first fingerprint.
  (void)fingerprint_count();
  piv_init();
  usb_ccid_start(piv_handle_apdu);
  config_console_start();
  touch_pin_hid_start();
  vTaskDelete(NULL);
}

int main(void) {
  // Reference the descriptor so --gc-sections keeps it in the image.
  __asm__ volatile("" : : "r"(tinytouch_image_descriptor));
  // A verified OTA image staged by the previous session is installed before
  // USB enumerates, so the host only ever sees the new firmware.
  firmware_update_apply_pending();
  mbedtls_platform_set_calloc_free(rtos_calloc, vPortFree);
  configASSERT(xTaskCreate(app_task, "app", 2048, NULL, 1, NULL) == pdPASS);
  vTaskStartScheduler();
  for (;;) {}
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
  (void)task;
  (void)name;
  // Park until the watchdog restarts the device.
  portDISABLE_INTERRUPTS();
  for (;;) {}
}
