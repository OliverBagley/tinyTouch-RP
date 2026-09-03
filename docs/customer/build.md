# Build

Use a Waveshare RP2040-Zero with a ZW101-style UART fingerprint sensor. Use 3.3 V power and logic.

## Wire the sensor

| Sensor pin | Signal | RP2040-Zero |
|---:|---|---|
| 1 | VTouch | 3V3 |
| 2 | TouchOut | GP2 |
| 3 | VCC | 3V3 |
| 4 | TX | GP1 (RX) |
| 5 | RX | GP0 (TX) |
| 6 | GND | GND |

The UART pair is crossed: sensor TX goes to board RX. Sensor RX goes to board TX.

Check continuity. Confirm that 3V3 and GND are not shorted before connecting USB.

Next: [Flash factory firmware](/flash).
