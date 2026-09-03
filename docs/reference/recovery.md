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
3. Open the [Flash center](/flash), download the factory UF2, and copy it onto **RPI-RP2**. The drive disappears when the board restarts. The factory image erases the device configuration, PIV identity, and any staged update along with installing the firmware.
4. Unplug and reconnect the device.
5. Wait 20 seconds, then run `tinytouch setup`.

Fingerprints are stored in the sensor. Reflashing does not clear them; setup offers to erase them before enrolling, and `tinytouch factory-reset` clears them on a working device.

Use `tinytouch update` for routine firmware updates. Recovery deletes device state.
