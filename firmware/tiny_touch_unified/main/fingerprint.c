#include "fingerprint.h"

#include <string.h>

#include "FreeRTOS.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "platform.h"
#include "semphr.h"
#include "task.h"

// RP2040-Zero: UART0 on GP0 (TX) and GP1 (RX), sensor TouchOut on GP2.
#define FP_UART uart0
static const uint FP_TX_PIN = 0;
static const uint FP_RX_PIN = 1;
static const uint FP_INT_PIN = 2;
static const int INT_ACTIVE_VALUE = 1;
static const uint16_t START_SLOT = 1;
static const uint16_t END_SLOT = 5;
static const uint32_t FINGER_WAIT_MS = 7000;
static const uint8_t FP_LED_BLUE = 0x01;
static const uint8_t FP_LED_GREEN = 0x02;
static const uint8_t FP_LED_RED = 0x04;
static const uint8_t FP_LED_FUNC_STEADY = 3;

static SemaphoreHandle_t fp_mutex;
static volatile bool prompted_authorization_active;
static bool sensor_ready;

static bool sensor_ready_snapshot(void) {
  taskENTER_CRITICAL();
  bool ready = sensor_ready;
  taskEXIT_CRITICAL();
  return ready;
}

static void set_sensor_ready(bool ready) {
  taskENTER_CRITICAL();
  sensor_ready = ready;
  taskEXIT_CRITICAL();
}

static void fp_uart_drain(void) {
  while (uart_is_readable(FP_UART)) (void)uart_getc(FP_UART);
}

// ponytail: polled at the 1 ms tick against the 32-byte RX FIFO (5.5 ms at
// 57600 baud); an RX interrupt feeding a stream buffer is the upgrade if a
// higher-priority task ever holds the CPU that long.
static size_t fp_uart_read(uint8_t *buffer, size_t capacity, uint32_t timeout_ms) {
  TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  size_t count = 0;
  while (true) {
    while (count < capacity && uart_is_readable(FP_UART)) {
      buffer[count++] = (uint8_t)uart_getc(FP_UART);
    }
    if (count || (int32_t)(xTaskGetTickCount() - deadline) >= 0) return count;
    vTaskDelay(1);
  }
}

static void note_transport_success(void) {
  set_sensor_ready(true);
}

static void note_transport_failure(void) { set_sensor_ready(false); }

static uint16_t fp_checksum(uint8_t packet_id, const uint8_t *payload, size_t payload_len) {
  uint16_t length = payload_len + 2;
  uint32_t total = packet_id + (length >> 8) + (length & 0xff);
  for (size_t i = 0; i < payload_len; i++) total += payload[i];
  return (uint16_t)total;
}

static bool fp_response_checksum_valid(const uint8_t *packet, size_t packet_len) {
  if (packet_len < 11) return false;
  uint16_t response_len = ((uint16_t)packet[7] << 8) | packet[8];
  if (response_len < 2 || packet_len != 9 + response_len) return false;
  size_t payload_len = response_len - 2;
  uint16_t expected = fp_checksum(packet[6], packet + 9, payload_len);
  uint16_t received = ((uint16_t)packet[packet_len - 2] << 8) |
                      packet[packet_len - 1];
  return received == expected;
}

static bool fp_command(uint8_t instruction, const uint8_t *params, size_t param_len,
                       uint8_t *confirm, uint8_t *data, size_t *data_len,
                       uint32_t timeout_ms) {
  fp_uart_drain();

  uint8_t payload[32];
  if (param_len + 1 > sizeof(payload)) return false;
  payload[0] = instruction;
  if (param_len) memcpy(payload + 1, params, param_len);

  const size_t payload_len = param_len + 1;
  const uint16_t length = payload_len + 2;
  const uint16_t sum = fp_checksum(0x01, payload, payload_len);
  const uint8_t header[] = {
    0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x01,
    (uint8_t)(length >> 8), (uint8_t)(length & 0xff)
  };

  uart_write_blocking(FP_UART, header, sizeof(header));
  uart_write_blocking(FP_UART, payload, payload_len);
  uint8_t sum_bytes[] = {(uint8_t)(sum >> 8), (uint8_t)(sum & 0xff)};
  uart_write_blocking(FP_UART, sum_bytes, sizeof(sum_bytes));

  uint8_t response[96];
  size_t pos = 0;
  const size_t data_cap = (data && data_len) ? *data_len : 0;
  size_t out_len = 0;
  bool saw_ack = false;
  TickType_t post_ack_until = 0;
  TickType_t start = xTaskGetTickCount();
  TickType_t deadline = pdMS_TO_TICKS(timeout_ms);
  if (data && data_len) *data_len = 0;

  while ((xTaskGetTickCount() - start) < deadline) {
    // Drain every complete packet already in memory before waiting for more
    // UART bytes. Sensors may return an ACK and its following data packet in a
    // single read; blocking between them can otherwise turn valid buffered
    // data into a timeout.
    while (true) {
      while (pos >= 2 && !(response[0] == 0xef && response[1] == 0x01)) {
        memmove(response, response + 1, --pos);
      }
      if (pos < 9) break;

      uint8_t packet_id = response[6];
      uint16_t resp_len = ((uint16_t)response[7] << 8) | response[8];
      size_t expected = 9 + resp_len;
      if (response[2] != 0xff || response[3] != 0xff ||
          response[4] != 0xff || response[5] != 0xff || resp_len < 2) {
        LOGW("fingerprint response has invalid address/length");
        note_transport_failure();
        return false;
      }
      if (expected > sizeof(response)) {
        note_transport_failure();
        return false;
      }
      if (pos < expected) break;

      size_t response_payload_len = resp_len - 2;
      if (!fp_response_checksum_valid(response, expected)) {
        LOGW("fingerprint response checksum mismatch");
        note_transport_failure();
        return false;
      }

      if (packet_id == 0x07) {
        if (response_payload_len < 1) {
          note_transport_failure();
          return false;
        }
        note_transport_success();
        *confirm = response[9];
        saw_ack = true;
        size_t actual_len = response_payload_len - 1;
        if (data && data_len && actual_len) {
          size_t copy_len = actual_len;
          if (copy_len > data_cap - out_len) copy_len = data_cap - out_len;
          memcpy(data + out_len, response + 10, copy_len);
          out_len += copy_len;
          *data_len = out_len;
        }
        if (*confirm != 0x00 || !data || !data_len || out_len >= data_cap) return true;
        post_ack_until = xTaskGetTickCount() + pdMS_TO_TICKS(120);
      } else if (packet_id == 0x02 && data && data_len) {
        size_t actual_len = response_payload_len;
        if (actual_len) {
          size_t copy_len = actual_len;
          if (copy_len > data_cap - out_len) copy_len = data_cap - out_len;
          memcpy(data + out_len, response + 9, copy_len);
          out_len += copy_len;
          *data_len = out_len;
        }
        if (saw_ack && out_len >= data_cap) return true;
      }

      size_t remaining = pos - expected;
      if (remaining) memmove(response, response + expected, remaining);
      pos = remaining;
    }

    if (saw_ack && post_ack_until && xTaskGetTickCount() > post_ack_until) return true;

    pos += fp_uart_read(response + pos, sizeof(response) - pos, 10);
  }

  if (!saw_ack) note_transport_failure();
  return saw_ack;
}

static bool fp_take(uint32_t timeout_ms) {
  return fp_mutex && xSemaphoreTake(fp_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void fp_give(void) {
  if (fp_mutex) xSemaphoreGive(fp_mutex);
}

static void set_aura(uint8_t color) {
  uint8_t params[] = {FP_LED_FUNC_STEADY, color, color, 0};
  uint8_t confirm = 0xff;
  fp_command(0x3c, params, sizeof(params), &confirm, NULL, NULL, 1000);
}

static void show_result(bool ok) {
  set_aura(ok ? FP_LED_GREEN : FP_LED_RED);
  vTaskDelay(pdMS_TO_TICKS(350));
  set_aura(FP_LED_BLUE);
}

void fingerprint_led_idle(void) {
  if (!fp_take(1000)) return;
  set_aura(FP_LED_BLUE);
  fp_give();
}

static bool finger_present(void) {
  return (int)gpio_get(FP_INT_PIN) == INT_ACTIVE_VALUE;
}

bool fingerprint_present_hint(void) {
  return finger_present();
}

static fingerprint_match_t fingerprint_match_captured(bool quiet) {
  fingerprint_match_t no_match = {0};
  uint8_t confirm = 0xff;
  uint8_t img2tz[] = {0x01};
  if (!fp_command(0x02, img2tz, sizeof(img2tz), &confirm, NULL, NULL, 2000) || confirm != 0x00) {
    if (!quiet) {
      LOGW("img2tz failed confirm=0x%02x", confirm);
      show_result(false);
    }
    return no_match;
  }

  uint16_t count = END_SLOT - START_SLOT + 1;
  uint8_t search_params[] = {
    0x01,
    (uint8_t)(START_SLOT >> 8), (uint8_t)(START_SLOT & 0xff),
    (uint8_t)(count >> 8), (uint8_t)(count & 0xff)
  };
  uint8_t search_data[4];
  size_t search_len = sizeof(search_data);
  if (!fp_command(0x04, search_params, sizeof(search_params), &confirm, search_data, &search_len, 2000)) {
    if (!quiet) LOGW("search command failed");
  } else if (confirm == 0x00 && search_len == sizeof(search_data)) {
    uint16_t score = ((uint16_t)search_data[2] << 8) | search_data[3];
    uint16_t slot = ((uint16_t)search_data[0] << 8) | search_data[1];
    bool ok = score > 0 && slot >= START_SLOT && slot <= END_SLOT;
    LOGI("fingerprint search: %s slot=%u score=%u", ok ? "ok" : "failed",
             slot, score);
    if (!quiet) {
      show_result(ok);
    }
    if (ok) return (fingerprint_match_t){.slot = slot, .score = score};
    return no_match;
  } else if (!quiet) {
    LOGW("search failed confirm=0x%02x len=%u", confirm, (unsigned)search_len);
  }

  for (uint16_t slot = START_SLOT; slot <= END_SLOT; slot++) {
    uint8_t load_params[] = {0x02, (uint8_t)(slot >> 8), (uint8_t)(slot & 0xff)};
    confirm = 0xff;
    if (!fp_command(0x07, load_params, sizeof(load_params), &confirm, NULL, NULL, 1000) ||
        confirm != 0x00) {
      if (!quiet) LOGW("load slot %u failed confirm=0x%02x", slot, confirm);
      continue;
    }

    uint8_t match_data[2];
    size_t match_len = sizeof(match_data);
    confirm = 0xff;
    if (!fp_command(0x03, NULL, 0, &confirm, match_data, &match_len, 1000)) {
      if (!quiet) LOGW("match slot %u command failed", slot);
      continue;
    }
    if (confirm == 0x00 && match_len == sizeof(match_data)) {
      uint16_t score = ((uint16_t)match_data[0] << 8) | match_data[1];
      if (score > 0) {
        LOGI("fingerprint match: ok slot=%u score=%u", slot, score);
        if (!quiet) show_result(true);
        return (fingerprint_match_t){.slot = slot, .score = score};
      }
    }
    if (!quiet) {
      LOGW("match slot %u failed confirm=0x%02x len=%u", slot, confirm, (unsigned)match_len);
    }
  }

  if (!quiet) show_result(false);
  return no_match;
}

fingerprint_match_t fingerprint_authorize_poll_match(void) {
  fingerprint_match_t no_match = {0};
  if (!fp_take(0)) return no_match;
  uint8_t confirm = 0xff;
  if (!fp_command(0x01, NULL, 0, &confirm, NULL, NULL, 350) || confirm != 0x00) {
    fp_give();
    return no_match;
  }
  fingerprint_match_t match = fingerprint_match_captured(true);
  if (match.slot) set_aura(FP_LED_GREEN);
  fp_give();
  return match;
}

void fingerprint_init(void) {
  gpio_init(FP_INT_PIN);
  gpio_set_dir(FP_INT_PIN, GPIO_IN);
  gpio_pull_down(FP_INT_PIN);

  uart_init(FP_UART, 57600);
  gpio_set_function(FP_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(FP_RX_PIN, GPIO_FUNC_UART);
  uart_set_format(FP_UART, 8, 1, UART_PARITY_NONE);
  uart_set_hw_flow(FP_UART, false, false);
  uart_set_fifo_enabled(FP_UART, true);
  fp_mutex = xSemaphoreCreateMutex();
  configASSERT(fp_mutex != NULL);

  uint8_t params[] = {0x00, 0x00, 0x00, 0x00};
  bool ok = false;
  for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
    uint8_t confirm = 0xff;
    configASSERT(fp_take(2000));
    ok = fp_command(0x13, params, sizeof(params), &confirm, NULL, NULL, 2000) &&
         confirm == 0x00;
    set_sensor_ready(ok);
    fp_give();
    if (!ok && attempt < 3) vTaskDelay(pdMS_TO_TICKS(250));
  }
  LOGI("sensor verify: %s", ok ? "ok" : "failed");
  if (ok) fingerprint_led_idle();
}

bool fingerprint_is_ready(void) {
  return sensor_ready_snapshot();
}

bool fingerprint_recover(void) {
  if (!fp_take(3000)) return false;

  // A USB reconnect must not be required to recover one interrupted UART
  // transaction. Flush stale bytes, reapply the known sensor baud rate, and
  // verify the sensor in place. This does not erase state or restart either
  // processor.
  fp_uart_drain();
  uart_set_baudrate(FP_UART, 57600);
  const uint8_t params[] = {0x00, 0x00, 0x00, 0x00};
  bool ok = false;
  for (int attempt = 0; attempt < 3 && !ok; attempt++) {
    uint8_t confirm = 0xff;
    ok = fp_command(0x13, params, sizeof(params), &confirm, NULL, NULL, 1200) &&
         confirm == 0x00;
    if (!ok && attempt < 2) vTaskDelay(pdMS_TO_TICKS(100));
  }
  set_sensor_ready(ok);
  fp_give();
  return ok;
}

bool fingerprint_authorize_prompted(void (*prompt)(void)) {
  // TOUCH_OUT is not reliable enough to gate a foreground capture on every
  // supported module. Reuse HID's quiet matcher and keep polling until the
  // user presents a valid enrolled finger or the authorization window ends.
  prompted_authorization_active = true;
  if (prompt) prompt();
  TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(FINGER_WAIT_MS);
  bool ok = false;
  while (xTaskGetTickCount() < deadline) {
    if (fingerprint_authorize_poll_match().slot != 0) {
      ok = true;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
  }
  prompted_authorization_active = false;
  return ok;
}

bool fingerprint_prompted_authorization_active(void) {
  return prompted_authorization_active;
}

int fingerprint_count(void) {
  for (unsigned attempt = 0; attempt < 3; attempt++) {
    if (fp_take(2000)) {
      uint8_t confirm = 0xff;
      uint8_t data[2];
      size_t data_len = sizeof(data);
      bool ok = fp_command(0x1d, NULL, 0, &confirm, data, &data_len, 2000) &&
                confirm == 0x00 && data_len == sizeof(data);
      fp_give();
      if (ok) return ((int)data[0] << 8) | data[1];
    }
    if (attempt < 2) {
      fingerprint_recover();
      vTaskDelay(pdMS_TO_TICKS(150));
    }
  }
  return -1;
}

static bool wait_capture_template(uint8_t buffer_id, uint32_t timeout_ms) {
  TickType_t start = xTaskGetTickCount();
  TickType_t deadline = pdMS_TO_TICKS(timeout_ms);
  while ((xTaskGetTickCount() - start) < deadline) {
    uint8_t confirm = 0xff;
    if (fp_command(0x01, NULL, 0, &confirm, NULL, NULL, 600) && confirm == 0x00) {
      uint8_t params[] = {buffer_id};
      if (fp_command(0x02, params, sizeof(params), &confirm, NULL, NULL, 2000) &&
          confirm == 0x00) {
        return true;
      }
      LOGW("enrollment conversion failed confirm=0x%02x; retrying", confirm);
    }
    vTaskDelay(pdMS_TO_TICKS(120));
  }
  return false;
}

static bool wait_finger_removed(uint32_t timeout_ms) {
  TickType_t start = xTaskGetTickCount();
  TickType_t deadline = pdMS_TO_TICKS(timeout_ms);
  unsigned absent_samples = 0;
  while ((xTaskGetTickCount() - start) < deadline) {
    uint8_t confirm = 0xff;
    bool answered = fp_command(0x01, NULL, 0, &confirm, NULL, NULL, 500);
    if (answered && confirm == 0x02) {
      if (++absent_samples >= 3) return true;
    } else if (answered && confirm == 0x00) {
      absent_samples = 0;
    } else {
      // A UART timeout or sensor error is not evidence that the finger lifted.
      absent_samples = 0;
      LOGW("finger-removal check failed confirm=0x%02x", confirm);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return false;
}

bool fingerprint_enroll(uint16_t slot, void (*prompt)(const char *message)) {
  if (slot < START_SLOT || slot > END_SLOT || !fp_take(1000)) return false;
  bool ok = false;
  set_aura(FP_LED_BLUE);
  if (prompt) prompt("TOUCH");
  if (!wait_capture_template(1, 15000)) goto done;
  if (prompt) prompt("LIFT");
  if (!wait_finger_removed(10000)) goto done;
  vTaskDelay(pdMS_TO_TICKS(250));
  if (prompt) prompt("TOUCH_AGAIN");
  if (!wait_capture_template(2, 15000)) goto done;

  uint8_t confirm = 0xff;
  if (!fp_command(0x05, NULL, 0, &confirm, NULL, NULL, 2000) || confirm != 0x00) goto done;
  uint8_t store[] = {0x01, (uint8_t)(slot >> 8), (uint8_t)slot};
  ok = fp_command(0x06, store, sizeof(store), &confirm, NULL, NULL, 2000) && confirm == 0x00;

done:
  show_result(ok);
  fp_give();
  return ok;
}

bool fingerprint_delete(uint16_t slot) {
  if (slot < START_SLOT || slot > END_SLOT || !fp_take(1000)) return false;
  uint8_t params[] = {(uint8_t)(slot >> 8), (uint8_t)slot, 0x00, 0x01};
  uint8_t confirm = 0xff;
  bool ok = fp_command(0x0c, params, sizeof(params), &confirm, NULL, NULL, 2000) && confirm == 0x00;
  fp_give();
  return ok;
}

bool fingerprint_delete_all(void) {
  if (!fp_take(1000)) return false;
  uint8_t confirm = 0xff;
  bool ok = fp_command(0x0d, NULL, 0, &confirm, NULL, NULL, 2000) && confirm == 0x00;
  fp_give();
  return ok;
}
