#include "usb_ccid.h"

#include <string.h>

#include "FreeRTOS.h"
#include "device/usbd_pvt.h"
#include "hardware/watchdog.h"
#include "task.h"
#include "tusb.h"

#include "device_config.h"
#include "piv.h"
#include "touch_pin_hid.h"
#include "usb_descriptors.h"

#define CCID_EP_OUT 0x01
#define CCID_EP_IN 0x81
#define CCID_BUF_SIZE 2048
#define CCID_TIME_EXTENSION_MS 700

static uint8_t rx_buf[CCID_BUF_SIZE];
static uint8_t tx_buf[CCID_BUF_SIZE];
static uint8_t rhport_active;
static ccid_apdu_handler_t apdu_handler;
static bool in_busy;
static volatile bool resume_reconnect_active;

// PIV commands run on a worker task: an RSA-2048 private operation takes
// about a second on the Cortex-M0+, and the USB task must keep serving CDC,
// HID, and CCID time extensions meanwhile.
static TaskHandle_t apdu_task_handle;
static uint8_t apdu_response[CCID_BUF_SIZE - 10];
static size_t apdu_response_len;
static volatile bool apdu_busy;
static volatile bool apdu_response_ready;
static uint8_t apdu_slot;
static uint8_t apdu_seq;
static size_t apdu_len;
static uint32_t bus_generation;
static uint32_t apdu_generation;
static TickType_t next_time_extension;

static void resume_reconnect_task(void *argument) {
  (void)argument;
  // macOS can retain the device node after wake while its composite endpoints
  // no longer transfer. Re-enumerate USB without restarting the firmware.
  vTaskDelay(pdMS_TO_TICKS(100));
  tud_disconnect();
  vTaskDelay(pdMS_TO_TICKS(250));
  tud_connect();
  resume_reconnect_active = false;
  vTaskDelete(NULL);
}

void tud_mount_cb(void) { touch_pin_hid_usb_attached(); }

void tud_resume_cb(void) {
  if (resume_reconnect_active) return;
  resume_reconnect_active = true;
  if (xTaskCreate(resume_reconnect_task, "usb_resume", 512, NULL, 2, NULL) != pdPASS) {
    resume_reconnect_active = false;
  }
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static bool send_ccid(uint8_t msg_type, uint8_t slot, uint8_t seq, uint8_t status,
                      uint8_t error, const uint8_t *data, size_t data_len) {
  if (data_len > sizeof(tx_buf) - 10 || in_busy) return false;
  tx_buf[0] = msg_type;
  put_le32(tx_buf + 1, data_len);
  tx_buf[5] = slot;
  tx_buf[6] = seq;
  tx_buf[7] = status;
  tx_buf[8] = error;
  tx_buf[9] = 0x00;
  if (data_len) memcpy(tx_buf + 10, data, data_len);
  bool queued = usbd_edpt_xfer(rhport_active, CCID_EP_IN, tx_buf, data_len + 10);
  if (queued) in_busy = true;
  return queued;
}

static bool send_parameters(uint8_t slot, uint8_t seq) {
  const uint8_t t1_params[] = {0x11, 0x10, 0x00, 0x45, 0x00, 0xfe, 0x00};
  return send_ccid(0x82, slot, seq, 0x00, 0x00, t1_params, sizeof(t1_params));
}

static void handle_message(uint8_t *msg, size_t msg_len) {
  if (msg_len < 10) return;

  uint8_t type = msg[0];
  uint32_t len = le32(msg + 1);
  uint8_t slot = msg[5];
  uint8_t seq = msg[6];
  if (slot != 0) {
    send_ccid(0x81, slot, seq, 0x42, 0x05, NULL, 0);
    return;
  }
  if (len != msg_len - 10 || len > sizeof(rx_buf) - 10) {
    send_ccid(0x81, slot, seq, 0x42, 0x01, NULL, 0);
    return;
  }

  switch (type) {
    case 0x62: {
      // TCK is the XOR of every byte from T0 through the final interface byte.
      // Strict readers, including Windows, reject the former 0x01 value.
      const uint8_t atr[] = {0x3b, 0x80, 0x01, 0x81};
      send_ccid(0x80, slot, seq, 0x00, 0x00, atr, sizeof(atr));
      break;
    }
    case 0x63:
    case 0x65:
      send_ccid(0x81, slot, seq, 0x00, 0x00, NULL, 0);
      break;
    case 0x61:
    case 0x6c:
    case 0x6d:
      send_parameters(slot, seq);
      break;
    case 0x6f: {
      if (device_config_mode() != DEVICE_MODE_PIV) {
        const uint8_t unavailable[] = {0x69, 0x85};
        send_ccid(0x80, slot, seq, 0x00, 0x00, unavailable, sizeof(unavailable));
        break;
      }
      if (apdu_busy) {
        send_ccid(0x80, slot, seq, 0x40, 0xe0, NULL, 0);
        break;
      }
      // rx_buf stays untouched until the worker's response has been sent: the
      // OUT endpoint is not re-armed while apdu_busy is set.
      apdu_slot = slot;
      apdu_seq = seq;
      apdu_len = len;
      apdu_generation = bus_generation;
      apdu_response_ready = false;
      apdu_busy = true;
      next_time_extension = xTaskGetTickCount() + pdMS_TO_TICKS(CCID_TIME_EXTENSION_MS);
      xTaskNotifyGive(apdu_task_handle);
      break;
    }
    default:
      send_ccid(0x81, slot, seq, 0x42, 0x00, NULL, 0);
      break;
  }
}

static void apdu_task(void *argument) {
  (void)argument;
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    size_t resp_len = sizeof(apdu_response);
    bool ok = apdu_handler &&
              apdu_handler(rx_buf + 10, apdu_len, apdu_response, &resp_len, sizeof(apdu_response));
    if (!ok) {
      apdu_response[0] = 0x6f;
      apdu_response[1] = 0x00;
      resp_len = 2;
    }
    apdu_response_len = resp_len;
    apdu_response_ready = true;
  }
}

// Called from the USB task next to tud_task(), so every endpoint transfer is
// issued from one context.
static void usb_ccid_poll(void) {
  if (!apdu_busy) return;
  if (apdu_response_ready) {
    if (apdu_generation != bus_generation) {
      // The bus was reset while the command ran; the reply has no reader.
      apdu_response_ready = false;
      apdu_busy = false;
      return;
    }
    if (in_busy) return;
    if (send_ccid(0x80, apdu_slot, apdu_seq, 0x00, 0x00, apdu_response, apdu_response_len)) {
      apdu_response_ready = false;
      apdu_busy = false;
    }
    return;
  }
  if (apdu_generation == bus_generation && !in_busy &&
      (int32_t)(xTaskGetTickCount() - next_time_extension) >= 0) {
    // RDR_to_PC_DataBlock with bmCommandStatus = time extension requested.
    if (send_ccid(0x80, apdu_slot, apdu_seq, 0x80, 0x01, NULL, 0)) {
      next_time_extension = xTaskGetTickCount() + pdMS_TO_TICKS(CCID_TIME_EXTENSION_MS);
    }
  }
}

static void ccid_init(void) {}
static void ccid_reset(uint8_t rhport) {
  (void)rhport;
  in_busy = false;
  bus_generation++;
  piv_reset_transport_state();
}

static uint16_t ccid_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc,
                          uint16_t max_len) {
  uint8_t const *p_desc = (uint8_t const *)itf_desc;
  uint16_t drv_len = sizeof(tusb_desc_interface_t) + 54;
  uint16_t required_len = drv_len + 2 * sizeof(tusb_desc_endpoint_t);
  if (max_len < required_len) return 0;
  p_desc += drv_len;

  tusb_desc_endpoint_t const *ep_out = (tusb_desc_endpoint_t const *)p_desc;
  tusb_desc_endpoint_t const *ep_in = (tusb_desc_endpoint_t const *)(p_desc + sizeof(tusb_desc_endpoint_t));
  if (!usbd_edpt_open(rhport, ep_out) ||
      !usbd_edpt_open(rhport, ep_in)) return 0;

  rhport_active = rhport;
  in_busy = false;
  usbd_edpt_xfer(rhport, CCID_EP_OUT, rx_buf, sizeof(rx_buf));
  return required_len;
}

static bool ccid_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
  (void)rhport;
  (void)stage;
  (void)request;
  return false;
}

static bool ccid_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result,
                         uint32_t xferred_bytes) {
  if (result != XFER_RESULT_SUCCESS) {
    if (ep_addr == CCID_EP_IN) in_busy = false;
    if ((ep_addr == CCID_EP_OUT || ep_addr == CCID_EP_IN) && !apdu_busy) {
      usbd_edpt_xfer(rhport, CCID_EP_OUT, rx_buf, sizeof(rx_buf));
    }
    return true;
  }
  if (ep_addr == CCID_EP_OUT) {
    handle_message(rx_buf, xferred_bytes);
    // Keep tx_buf immutable until the IN transfer completes. Only accept the
    // next command immediately if no response was queued or deferred.
    if (!in_busy && !apdu_busy) usbd_edpt_xfer(rhport, CCID_EP_OUT, rx_buf, sizeof(rx_buf));
  } else if (ep_addr == CCID_EP_IN) {
    in_busy = false;
    if (!apdu_busy) usbd_edpt_xfer(rhport, CCID_EP_OUT, rx_buf, sizeof(rx_buf));
  }
  return true;
}

static usbd_class_driver_t const ccid_driver = {
#if CFG_TUSB_DEBUG >= 2
  .name = "CCID",
#endif
  .init = ccid_init,
  .reset = ccid_reset,
  .open = ccid_open,
  .control_xfer_cb = ccid_control_xfer_cb,
  .xfer_cb = ccid_xfer_cb,
  .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
  *driver_count = 1;
  return &ccid_driver;
}

static void usb_task(void *argument) {
  (void)argument;
  // The only watchdog feed. A wedged USB stack restarts the device, matching
  // the ESP32 task watchdog this replaces.
  watchdog_enable(8000, true);
  while (true) {
    tud_task();
    usb_ccid_poll();
    watchdog_update();
    vTaskDelay(1);
  }
}

void usb_ccid_start(ccid_apdu_handler_t handler) {
  apdu_handler = handler;
  tiny_touch_init_serial();
  configASSERT(xTaskCreate(apdu_task, "ccid", 2048, NULL, 2, &apdu_task_handle) == pdPASS);
  configASSERT(tud_init(0));
  configASSERT(xTaskCreate(usb_task, "usb", 1024, NULL, 5, NULL) == pdPASS);
}

void usb_ccid_rescan(void) {
  // PIV keys can be created after macOS has already scanned this card. A
  // runtime USB detach/attach makes CryptoTokenKit rescan the new identity
  // without restarting the RP2040 or requiring the user to reconnect it.
  tud_disconnect();
  vTaskDelay(pdMS_TO_TICKS(250));
  tud_connect();
}
