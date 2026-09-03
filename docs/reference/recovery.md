# Recovery

Recovery reinstalls the factory firmware and clears device state. Try this first:

```sh
tinytouch status --verbose
tinytouch factory-reset
```

## Procedure

You need a USB data cable and access to **BOOT** and **RESET** on the RP2040-Zero.

1. Disconnect tinyTouch.
2. Hold **BOOT** while reconnecting it. Or hold **BOOT**, tap **RESET**, and release **BOOT**. A drive named **RPI-RP2** appears.
3. To erase every setting and PIV key as well, first copy Raspberry Pi's [flash_nuke.uf2](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html#resetting-flash-memory) onto **RPI-RP2**, wait for the drive to reappear, then continue.
4. Open the [Flash center](/flash), download the factory UF2, and copy it onto **RPI-RP2**. The drive disappears when the board restarts.
5. Unplug and reconnect the device.
6. Wait 20 seconds, then run `tinytouch setup`.

Fingerprints are stored in the sensor. `tinytouch factory-reset` clears them; reflashing does not.

Use `tinytouch update` for routine firmware updates. Recovery deletes device state.
