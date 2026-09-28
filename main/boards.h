#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"

/* Board-Profile. Der Chip wird beim Flashen automatisch erkannt,
 * das Board waehlt man auf der Einrichtungsseite (nur passende werden angezeigt). */
typedef struct {
    const char *id;
    const char *name;
    int8_t boot_gpio;     /* Taste: 5 s halten = WLAN/MQTT zuruecksetzen */
    bool boot_pullup;
    uint8_t xiao_rf;      /* XIAO ESP32-C6: 0 = kein HF-Schalter, 1 = interne, 2 = externe Antenne */
} board_t;

static const board_t BOARDS[] = {
#if CONFIG_IDF_TARGET_ESP32
    { "esp32-devkit",   "ESP32 DevKit V1 / WROOM",          0,  true,  0 },
    { "lolin-d32",      "LOLIN / Wemos D32",                0,  true,  0 },
    { "m5-atom-lite",   "M5Stack Atom Lite",                39, false, 0 },
#elif CONFIG_IDF_TARGET_ESP32S3
    { "esp32s3-devkitc","ESP32-S3 DevKitC",                 0,  true,  0 },
    { "nano-esp32",     "Arduino Nano ESP32",               0,  true,  0 },
    { "xiao-esp32s3",   "Seeed XIAO ESP32-S3",              0,  true,  0 },
    { "lolin-s3-mini",  "LOLIN / Wemos S3 Mini",            0,  true,  0 },
    { "m5-atoms3",      "M5Stack AtomS3",                   41, true,  0 },
#elif CONFIG_IDF_TARGET_ESP32C3
    { "esp32c3-mini",   "ESP32-C3 SuperMini / Zero",        9,  true,  0 },
    { "xiao-esp32c3",   "Seeed XIAO ESP32-C3",              9,  true,  0 },
#elif CONFIG_IDF_TARGET_ESP32C6
    { "esp32c6-devkit", "ESP32-C6 DevKit / SuperMini",      9,  true,  0 },
    { "xiao-esp32c6",   "Seeed XIAO ESP32-C6 (interne Antenne)", 9, true, 1 },
    { "xiao-esp32c6-ext","Seeed XIAO ESP32-C6 (externe Antenne)", 9, true, 2 },
#elif CONFIG_IDF_TARGET_ESP32C5
    { "esp32c5-devkit", "ESP32-C5 DevKit (experimentell)",  28, true,  0 },
#else
    { "generic",        "ESP32 (allgemein)",                0,  true,  0 },
#endif
};
#define NUM_BOARDS ((int)(sizeof(BOARDS) / sizeof(BOARDS[0])))

extern int g_board;   /* Index in BOARDS */
