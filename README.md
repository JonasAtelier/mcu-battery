# mcu-battery

Read a lithium battery over I2C. The MAX17048 estimates voltage and charge for one cell. The BQ76920, BQ76930 and BQ76940 measure individual cells in a 3–15 cell pack, depending on the model.

**Platforms** · ESP-IDF 5.2+ and STM32 HAL. Hardware testing is pending. The host tests use simulated devices.

**Before using BQ769x0 protection**, set limits for your pack and test with a bench supply and resistive load. Read the protection notes before wiring the charge or discharge switches. [Battery safety](docs/chips.md#safety)

## Contents

- [Quick start](#quick-start)
- [Drivers and examples](#drivers-and-examples)
- [Using the library in your project](#using-the-library-in-your-project)
- [License](#license)

## Quick start

Use a MAX17048 breakout and one lithium cell. Check connector polarity and set the I2C pins to match your board.

In a terminal with ESP-IDF activated, get the example and select the target for an ESP32. Change the target if your board uses another ESP32 chip.

```sh
git clone https://github.com/JonasAtelier/mcu-battery.git battery
cd battery/examples/esp/max17048
idf.py set-target esp32
```

Check the [wiring guide](docs/hardware.md) and edit the settings in [main.c](examples/esp/max17048/main/main.c) to match your board. Connect the board by USB, then build, upload and open the serial monitor.

```sh
idf.py build flash monitor
```

The monitor shows cell voltage and estimated charge. Give the gauge a few minutes to settle after power-up.

## Drivers and examples

Both drivers return a `battery_sample` through `battery_read()`. Use the BQ769x0 header for individual cell readings and protection settings. The pack examples configure protection thresholds and leave the charge and discharge switches alone. [Shared API](include/battery.h)

| Chip | Driver header | ESP-IDF project | STM32 HAL example |
|---|---|---|---|
| [MAX17048](docs/chips.md#max17048) | [max17048.h](include/max17048.h) | [Single-cell fuel gauge](examples/esp/max17048/) | [Single-cell fuel gauge](examples/stm/max17048/main.c) |
| [BQ76920, BQ76930, BQ76940](docs/chips.md#bq76920--bq76930--bq76940) | [bq769x0.h](include/bq769x0.h) | [BQ76930 six-cell pack](examples/esp/bq769x0/) | [BQ76930 six-cell pack](examples/stm/bq769x0/main.c) |

## Using the library in your project

- Add the component and enable the driver for your hardware. [Installation and build options](docs/installation.md)
- Open the ESP-IDF project for your hardware, or copy the STM32 example into a project with board setup already in place. [All ESP-IDF examples](examples/esp/) · [All STM32 examples](examples/stm/)
- Look up calls and chip-specific settings. [API usage](docs/usage.md) · [Hardware reference](docs/chips.md)

To run the simulated tests on your computer, run `make -C test test` from the library root. [Test requirements and coverage](docs/development.md)

## License

[MIT](LICENSE)

<p align="center">
  <img src="assets/max17048.jpg"
       alt="An Adafruit MAX17048 lithium fuel gauge breakout"
       width="520">
  <br><sub>Adafruit MAX17048 breakout &mdash; photo <a href="https://www.adafruit.com/product/5580">Adafruit Industries</a></sub>
</p>

[Browse the workshop catalog · ESP32 and STM32 drivers](https://github.com/JonasAtelier/jonas-atelier-libraries#esp32-and-stm32-drivers)
