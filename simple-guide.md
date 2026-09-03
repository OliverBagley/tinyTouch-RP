# tinyTouch simple guide

This is the "explain it like I'm 5" version. Follow it top to bottom. Every step tells you exactly what to type or do. You do not need to understand any of it.

What you end up with: a tiny fingerprint reader that logs you into your Mac and approves `sudo` when you touch it.

## What you need

- A **Waveshare RP2040-Zero** board (a small purple board with a USB-C port).
- A **ZW101 / ZW111 style fingerprint sensor** (the one with a 6-pin cable).
- A **USB-C cable that carries data** (some cheap cables only charge; if in doubt, use the one that came with a phone).
- A **Mac** (Apple silicon or Intel).
- Six wires and a way to connect them (soldering iron, or a breadboard and jumper wires).

## Part 1: Get the tools onto your Mac (once)

Open the **Terminal** app (press Cmd+Space, type `Terminal`, press Enter). Paste each block below and press Enter. Wait for each one to finish before the next.

1. Install Homebrew (a tool that installs other tools). If you already have it, this does nothing bad.

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

2. Install the build tools.

```bash
brew install cmake git python3
```

3. Install the compiler for the RP2040 chip. It asks for your Mac password once.

```bash
brew install --cask gcc-arm-embedded
```

4. Download the Pico SDK (Raspberry Pi's code for the chip).

```bash
git clone --branch 2.3.0 https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk && git -C ~/pico-sdk submodule update --init lib/tinyusb lib/mbedtls
```

5. Download this project.

```bash
git clone https://github.com/OliverBagley/tinyTouch-RP.git ~/tinyTouch-RP && cd ~/tinyTouch-RP
```

## Part 2: Build the firmware

There are two ways. Pick one.

### Way A: let GitHub build it (no tools needed)

Every push to the `main` branch of your fork builds the firmware on GitHub's computers.

1. Open your repository on github.com and click the **Actions** tab.
2. Click the newest **CI** run at the top. Wait until it has a green tick.
3. Scroll down to **Artifacts** and click **tinytouch-firmware-rp2040**. A zip downloads.
4. Unzip it. Inside is `tiny_touch_unified.uf2`. That is the firmware. Skip to Part 3.

If you change anything (like the pins in Part 3), commit and push, and a new build appears the same way. You still need Part 1 step 5 (downloading the project) and `python3` for Part 5, but you can skip steps 2 to 4 of Part 1.

### Way B: build on your Mac

Still in Terminal, inside the project folder:

```bash
cd ~/tinyTouch-RP && ./firmware/build-and-flash --build-only
```

The first run takes a few minutes. It ends with a line like:

```text
Factory image (UF2): .../firmware/tiny_touch_unified/build/tiny_touch_unified.uf2
```

That `.uf2` file is the firmware. You will copy it onto the board in Part 4.

You will also see a warning that `secure_boot_signing_key.pem is missing`. That is fine. It means the build made its own signing key for you. There are no passwords, accounts, or credentials to create anywhere in this guide.

## Part 3: Wire the sensor to the board

Unplug everything first. Connect the six sensor pins to the board like this. Look at the numbers printed next to the board's pins.

| Sensor pin | Sensor label | Board pin |
|---:|---|---|
| 1 | VTouch | **3V3** |
| 2 | TouchOut | **GP2** |
| 3 | VCC | **3V3** |
| 4 | TX | **GP1** |
| 5 | RX | **GP0** |
| 6 | GND | **GND** |

Two rules that catch everyone:

- The sensor's **TX goes to GP1** and the sensor's **RX goes to GP0**. They are crossed on purpose.
- Only use **3V3**, never 5V. The sensor is 3.3 volt.

Double check that 3V3 and GND do not touch each other before plugging in USB.

### My sensor or board uses different pins

The board is the boss, not the sensor: the sensor's six wires can go to any pins you like, as long as the firmware knows which ones. Only the three numbered pins matter. Power and ground can use any 3V3 and GND pin.

The RP2040 chip can only do serial (TX/RX) on certain pin pairs. Pick **one pair** from this table. TouchOut can be any other free GP pin.

| Board TX pin (sensor RX) | Board RX pin (sensor TX) |
|---|---|
| GP0 | GP1 (this is the default) |
| GP4 | GP5 |
| GP8 | GP9 |
| GP12 | GP13 |
| GP28 | GP29 |

To change them:

1. Open the file `firmware/tiny_touch_unified/main/fingerprint.c` in any text editor (TextEdit works; in Terminal, `open -e firmware/tiny_touch_unified/main/fingerprint.c`).
2. Near the top you will see these three lines. Change the numbers only.

```c
#define FP_TX_PIN 0
#define FP_RX_PIN 1
#define FP_INT_PIN 2
```

   For example, GP8/GP9 with TouchOut on GP10 becomes `8`, `9`, `10`.

3. Save the file, then build again (Part 2). If you picked a pair that the chip cannot do, the build stops and prints a message that lists the valid pairs, so you cannot accidentally make a broken firmware.

If you build on your Mac you can also skip editing the file and type the pins on the command line instead:

```bash
cd ~/tinyTouch-RP/firmware/tiny_touch_unified && cmake -S . -B build -DPICO_SDK_PATH=$HOME/pico-sdk -DTINYTOUCH_FP_TX_PIN=8 -DTINYTOUCH_FP_RX_PIN=9 -DTINYTOUCH_FP_INT_PIN=10 && cmake --build build --parallel
```

## Part 4: Put the firmware on the board

1. Find the tiny **BOOT** button on the RP2040-Zero.
2. **Hold BOOT down**, plug the USB cable into your Mac, then let go.
3. A drive called **RPI-RP2** appears on your desktop, like a USB stick.
4. Copy the firmware onto it. Either drag `tiny_touch_unified.uf2` from Finder onto the RPI-RP2 drive, or run:

```bash
cp -X ~/tinyTouch-RP/firmware/tiny_touch_unified/build/tiny_touch_unified.uf2 /Volumes/RPI-RP2/
```

5. The drive disappears by itself. That means it worked.
6. **Unplug the board and plug it back in** once. The sensor needs this.

Shortcut: instead of steps 2 to 5 you can run `./firmware/build-and-flash` without `--build-only` and it waits for the RPI-RP2 drive and copies the file for you.

## Part 5: Set up the Mac

Back in Terminal, in the project folder:

```bash
cd ~/tinyTouch-RP && python3 tinytouch setup
```

The setup walks you through everything and tells you when to touch the sensor. This is what it asks:

1. **Touch the sensor** to prove you are there.
2. **Choose a mode.** Type `p` for PIV or `h` for HID.
   - **PIV** (recommended): the board pretends to be a smart card. Works for the login screen and `sudo`. Your real password is never typed anywhere.
   - **HID**: the board pretends to be a keyboard and types your password. Works in more places, but it really does type your password into whatever is focused.
3. If you picked **PIV**, wait. The board makes its own keys. On the RP2040 this **takes several minutes**. Do not touch the sensor and do not unplug it. Get a coffee. When it finishes, macOS asks you to pair the smart card and may ask for your Mac password. Say yes.
4. If you picked **HID**, it asks for your Mac password once and stores it in your Keychain. The board never keeps it.
5. **Enroll your fingerprint.** Touch, lift, touch again, exactly as the messages say. Use the same finger.

When it says setup is complete, you are done.

## Part 6: Try it

```bash
sudo -k && sudo -v
```

When the password prompt appears, touch the sensor. If it says nothing else, it worked. Lock your Mac (Ctrl+Cmd+Q) and touch the sensor to unlock.

## If something goes wrong

- **Check the board is talking:** `python3 tinytouch status`. You want `sensor=ready` and `protocol=6`.
- **"sensor=offline":** the wiring is wrong, or the firmware thinks the sensor is on different pins. Recheck Part 3, especially TX/RX being crossed and the pin numbers in the firmware. Then unplug and replug the board.
- **The RPI-RP2 drive never appears:** you let go of BOOT too early, or the cable is charge-only. Try again with BOOT held the whole time you plug in.
- **Start over completely:** `python3 tinytouch factory-reset` erases fingerprints and keys. Then run setup again.
- **Firmware totally broken:** hold BOOT and plug in, then copy the `.uf2` again (Part 4). This always works. The chip cannot be bricked this way.
- **Do not run `tinytouch update`.** It downloads firmware for the original ESP32 version of this project, which this board rejects. Update by rebuilding (Part 2) and copying the new `.uf2` (Part 4).

## Words you might see

- **UF2:** the firmware file format the board accepts when it shows up as a drive.
- **BOOTSEL / BOOT:** the button that makes the board show up as the RPI-RP2 drive.
- **PIV:** the smart-card standard macOS uses for login.
- **HID:** "keyboard mode".
- **Helper:** a small program setup installs on your Mac that runs in the background and talks to the board. It starts automatically after login.
