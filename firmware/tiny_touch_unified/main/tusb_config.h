#pragma once

// CFG_TUSB_MCU and CFG_TUSB_OS come from the Pico SDK build (OPT_OS_PICO). The
// USB task polls tud_task() under FreeRTOS.
#define CFG_TUD_ENABLED 1
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE)

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC 1
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 1
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

#define CFG_TUD_CDC_RX_BUFSIZE 4096
#define CFG_TUD_CDC_TX_BUFSIZE 64
#define CFG_TUD_CDC_EP_BUFSIZE 64
#define CFG_TUD_HID_EP_BUFSIZE 16
