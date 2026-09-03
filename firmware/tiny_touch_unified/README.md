# tinyTouch unified firmware

This Pico SDK project provides the PIV, HID keyboard, CDC configuration, fingerprint, and authenticated update runtime for the Waveshare RP2040-Zero.

Requirements: Pico SDK 2.x (with the `lib/tinyusb` and `lib/mbedtls` submodules), the Arm GNU toolchain, CMake, and OpenSSL. FreeRTOS-Kernel V11.2.0 is fetched at configure time; set `FETCHCONTENT_SOURCE_DIR_FREERTOS_KERNEL` to use a local checkout.

```sh
./firmware/build-and-flash --build-only
```

Outputs in `build/`:

- `tiny_touch_unified.uf2`: factory image, copied to the `RPI-RP2` drive in BOOTSEL mode. It also erases the device-state sectors, so flashing it is a factory reset.
- `tiny_touch_unified.signed.bin`: OTA image (application followed by an RSA-3072 signature) served by `tinytouch update`.

Without `secure_boot_signing_key.pem` the build signs with a throwaway key kept in `build/`, so a device that has a release firmware will refuse locally built OTA images. Factory flashing over BOOTSEL always works.

Flash layout (2 MB): application at 0, OTA staging at 992 KB, update marker, PIV identity, and device configuration in the last sectors. A staged update is copied over the application on the next power cycle, before USB enumerates.

Customers update installed devices with `tinytouch update`. Factory flashing is for blank DIY boards and recovery. See `docs/` for wiring, protocol, release, and security details.
