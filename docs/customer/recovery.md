# Recovery

Recovery reinstalls the factory firmware and clears fingerprints, keys, pairings, and settings.

Try this first:

```sh
tinytouch status --verbose
tinytouch factory-reset
```

If the device still cannot be set up, hold **BOOT** while reconnecting it so the **RPI-RP2** drive appears, then open the [Flash center](/flash), download the factory UF2, and copy it onto the drive. You need a USB data cable and access to **BOOT** and **RESET**.

After recovery, unplug and reconnect the device, wait 20 seconds, then run:

```sh
tinytouch setup
```

See the [Recovery reference](/reference/recovery) for details, including how to erase all stored keys first.
