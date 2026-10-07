# Smart Traffic Light Controller (STM32 Blue Pill)

This project implements a two-road traffic light controller with:

- Fixed traffic cycle (A green/yellow, B green/yellow)
- Adaptive green-time extension based on noise sensor input
- OLED status output (road states, timer, noise, timings)
- 1-second heartbeat on the onboard LED

The main source file is `pro.cpp`.

## Hardware assumptions

- STM32 Blue Pill (Arduino-compatible core)
- 6 LEDs for two traffic lights (A/B: red, yellow, green)
- Analog noise sensor on `PA6`
- SSD1306 OLED display over I2C (`0x3C`)

## Pin mapping

- A red: `PA0`
- A yellow: `PA1`
- A green: `PA2`
- B red: `PA3`
- B yellow: `PA4`
- B green: `PA5`
- Noise sensor: `PA6`
- Onboard LED: `PC13`

## Behavior summary

- Default green time: 15s
- Yellow time: 3s
- Green extension step: +3s when noise exceeds threshold
- Green limits: minimum 5s, maximum 30s (max enforced during adaptation)
- Adaptation cooldown: 1000 ms between extensions

Noise threshold is configured in code:

- `NOISE_THRESHOLD = 1200` (12-bit ADC scale 0–4095)

Tune this value for your sensor and environment.

## Software dependencies

- `Wire`
- `Adafruit_GFX`
- `Adafruit_SSD1306`

Install the Adafruit libraries in your Arduino environment before building.

## Build and upload

Use an Arduino-compatible STM32 toolchain (Arduino IDE or PlatformIO):

1. Configure target board as STM32 Blue Pill.
2. Ensure I2C and OLED wiring are correct.
3. Add/install required libraries.
4. Build and upload `pro.cpp` as your sketch source.

## Notes

- If OLED initialization fails, the onboard LED blinks forever.
- Traffic logic executes on a 1-second tick.
