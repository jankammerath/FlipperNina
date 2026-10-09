# FlipperNina

[Flipper app](https://lab.flipper.net/apps) using [WiFiNINA](https://github.com/arduino-libraries/WiFiNINA) on the Arduino Nano RP2040 Connect to scan for BLE devices and WiFi networks. It keeps track of contacts on the flipper and also allows making the flipper beep when a device is nearby that is marked as a favorite.

# Installation

## Install flipper app

To install and start it on a Flipper connected over USB, run this from the `flipper` folder:

```sh
python3 -m ufbt launch
```

# Building the Hardware Extension

## RP2040-NINA Wiring instructions

The Arduino Nano RP2040 Connect talks to the Flipper over UART and is powered from the Flipper's 5V GPIO pin. Both boards use 3.3V logic, so no level shifter is needed.

| Flipper Zero GPIO pin | Function    | Nano RP2040 Connect pin |
|-----------------------|-------------|-------------------------|
| 1                     | 5V          | VIN                     |
| 18 (or 8, 11)         | GND         | GND                     |
| 13                    | TX (USART1) | RX (D0)                 |
| 14                    | RX (USART1) | TX (D1)                 |

```
 Flipper Zero                 Nano RP2040 Connect
 ------------                 -------------------
 Pin 1  (5V)  --------------> VIN
 Pin 18 (GND) --------------- GND
 Pin 13 (TX)  --------------> D0 / RX
 Pin 14 (RX)  <-------------- D1 / TX
```

Notes:

- TX and RX are crossed: Flipper TX goes to Nano RX and vice versa.
- Use the Nano's **VIN** pin, not the `+5V` pin. On the Nano RP2040 Connect the `+5V` pin is not connected to the power input unless its solder jumper is closed.
- The 5V output on pin 1 is off by default. Turn it on under **GPIO → 5V on GPIO → ON** on the Flipper.
- The Arduino sketch must use `Serial1` (pins D0/D1) to talk to the Flipper. `Serial` is the USB port.

