#pragma once

// ESP32-2432S028 / common CYD 2.8" ILI9341 + XPT2046
// Verify board revision if display/touch/SD initialization fails.

#define TFT_BL 21

// Touch controller uses its dedicated SPI pins.
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39

// microSD slot uses VSPI pins.
#define SD_CS   5
#define SD_SCK  18
#define SD_MISO 19
#define SD_MOSI 23

// Typical XPT2046 raw calibration values. Calibrate for your individual panel.
#define TOUCH_X_MIN  200
#define TOUCH_X_MAX  3900
#define TOUCH_Y_MIN  200
#define TOUCH_Y_MAX  3900
