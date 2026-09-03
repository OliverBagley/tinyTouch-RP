cmake_minimum_required(VERSION 3.18)

# Local hardware recovery only. Release builds leave this OFF and therefore
# require a matching enrolled fingerprint for every protected operation.
option(TINYTOUCH_DEVELOPMENT_SKIP_FINGERPRINT_AUTH
       "Allow local development firmware to bypass fingerprint authorization"
       OFF)

set(PICO_BOARD waveshare_rp2040_zero CACHE STRING "Target board")
set(PICO_MBEDTLS_CONFIG_FILE "tt_mbedtls_config.h")
include(pico_sdk_import.cmake)

set(TINYTOUCH_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../../VERSION")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TINYTOUCH_VERSION_FILE}")
file(READ "${TINYTOUCH_VERSION_FILE}" TINYTOUCH_VERSION)
string(STRIP "${TINYTOUCH_VERSION}" TINYTOUCH_VERSION)

if(DEFINED ENV{GITHUB_SHA} AND NOT "$ENV{GITHUB_SHA}" STREQUAL "")
  string(SUBSTRING "$ENV{GITHUB_SHA}" 0 12 TINYTOUCH_BUILD_ID)
else()
  execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../.."
    OUTPUT_VARIABLE TINYTOUCH_BUILD_ID
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()
if(NOT TINYTOUCH_BUILD_ID)
  set(TINYTOUCH_BUILD_ID "development")
endif()

project(tiny_touch_unified C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# FreeRTOS kernel with its RP2040 port. Set FETCHCONTENT_SOURCE_DIR_FREERTOS_KERNEL
# to build from a local checkout instead of fetching.
include(FetchContent)
set(FREERTOS_CONFIG_FILE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/main")
FetchContent_Declare(freertos_kernel
  GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
  GIT_TAG V11.2.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR portable/ThirdParty/GCC/RP2040
)
FetchContent_MakeAvailable(freertos_kernel)

# OTA images carry an RSA-3072 signature that the firmware checks before it
# stages an update. Release builds provide the production key; every other
# build signs with a throwaway key generated in the build tree.
set(TINYTOUCH_SIGNING_KEY "${CMAKE_CURRENT_LIST_DIR}/secure_boot_signing_key.pem"
    CACHE FILEPATH "RSA-3072 private key that signs OTA images")
if(NOT EXISTS "${TINYTOUCH_SIGNING_KEY}")
  set(TINYTOUCH_SIGNING_KEY "${CMAKE_BINARY_DIR}/development_signing_key.pem")
  if(NOT EXISTS "${TINYTOUCH_SIGNING_KEY}")
    message(WARNING "secure_boot_signing_key.pem is missing; signing with a development key")
    execute_process(COMMAND openssl genrsa -out "${TINYTOUCH_SIGNING_KEY}" 3072
                    RESULT_VARIABLE openssl_result OUTPUT_QUIET ERROR_QUIET)
    if(NOT openssl_result EQUAL 0)
      message(FATAL_ERROR "openssl could not generate a development signing key")
    endif()
  endif()
endif()
execute_process(COMMAND openssl rsa -in "${TINYTOUCH_SIGNING_KEY}" -pubout
                        -out "${CMAKE_BINARY_DIR}/signing_public_key.pem"
                RESULT_VARIABLE openssl_result OUTPUT_QUIET ERROR_QUIET)
if(NOT openssl_result EQUAL 0)
  message(FATAL_ERROR "openssl could not read ${TINYTOUCH_SIGNING_KEY}")
endif()
file(READ "${CMAKE_BINARY_DIR}/signing_public_key.pem" TINYTOUCH_SIGNING_PUBLIC_KEY)
string(REPLACE "\n" "\\n\"\n  \"" TINYTOUCH_SIGNING_PUBLIC_KEY "${TINYTOUCH_SIGNING_PUBLIC_KEY}")
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/generated/signing_public_key.h"
     CONTENT "#pragma once\nstatic const char TINYTOUCH_SIGNING_PUBLIC_KEY_PEM[] =\n  \"${TINYTOUCH_SIGNING_PUBLIC_KEY}\";\n")

add_executable(tiny_touch_unified
  main/main.c
  main/config_console.c
  main/device_config.c
  main/fingerprint.c
  main/firmware_update.c
  main/piv.c
  main/storage.c
  main/touch_pin_hid.c
  main/usb_ccid.c
  main/usb_descriptors.c
)
target_include_directories(tiny_touch_unified PRIVATE main "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(tiny_touch_unified PRIVATE
  TINYTOUCH_FIRMWARE_VERSION="${TINYTOUCH_VERSION}"
  TINYTOUCH_BUILD_ID="${TINYTOUCH_BUILD_ID}"
  TINYTOUCH_PROTOCOL_VERSION=6
)
# Optional sensor pin overrides, e.g. -DTINYTOUCH_FP_TX_PIN=4 -DTINYTOUCH_FP_RX_PIN=5.
foreach(pin TX RX INT)
  if(DEFINED TINYTOUCH_FP_${pin}_PIN)
    target_compile_definitions(tiny_touch_unified PRIVATE FP_${pin}_PIN=${TINYTOUCH_FP_${pin}_PIN})
  endif()
endforeach()
if(TINYTOUCH_DEVELOPMENT_SKIP_FINGERPRINT_AUTH)
  target_compile_definitions(tiny_touch_unified PRIVATE
    TINYTOUCH_DEVELOPMENT_SKIP_FINGERPRINT_AUTH=1)
endif()
target_link_libraries(tiny_touch_unified PRIVATE
  pico_stdlib
  pico_unique_id
  pico_rand
  pico_mbedtls
  hardware_flash
  hardware_gpio
  hardware_uart
  hardware_watchdog
  tinyusb_device
  FreeRTOS-Kernel
  FreeRTOS-Kernel-Heap4
)
# Both UART pins belong to the fingerprint sensor; no debug console.
pico_enable_stdio_uart(tiny_touch_unified 0)
pico_enable_stdio_usb(tiny_touch_unified 0)
pico_add_extra_outputs(tiny_touch_unified)

add_custom_command(TARGET tiny_touch_unified POST_BUILD
  COMMAND openssl dgst -sha256 -sign "${TINYTOUCH_SIGNING_KEY}"
          -out tiny_touch_unified.sig tiny_touch_unified.bin
  COMMAND sh -c "cat tiny_touch_unified.bin tiny_touch_unified.sig > tiny_touch_unified.signed.bin"
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  COMMENT "Signing the OTA image"
  VERBATIM
)
