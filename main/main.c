/* main.c - SkyBridge: Bluetooth-Mesh-Leuchten (z. B. OPPLE BLE2) <-> WLAN/MQTT <-> Home Assistant */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "bridge.h"
#include "boards.h"

#define TAG "main"
#define AP_SSID     "SkyBridge-Setup"
#define AP_PASS     "skybridge"

char g_id[13];
bool g_wifi_connected;
static bool s_ap_on;
static int64_t s_boot_us;

static void start_ap(void)
{
    if (s_ap_on) return;
    s_ap_on = true;
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    esp_wifi_set_mode(mode == WIFI_MODE_STA ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    wifi_config_t ap = {0};
    strcpy((char *)ap.ap.ssid, AP_SSID);
    ap.ap.ssid_len = strlen(AP_SSID);
    strcpy((char *)ap.ap.password, AP_PASS);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    ESP_LOGW(TAG, "Einrichtungs-WLAN '%s' (Passwort '%s') aktiv -> http://192.168.4.1", AP_SSID, AP_PASS);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected = false;
        if (!s_ap_on && esp_timer_get_time() - s_boot_us > 90LL * 1000000) {
            start_ap();   /* WLAN klappt nicht -> Einrichtung wieder anbieten */
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        g_wifi_connected = true;
        ESP_LOGI(TAG, "WLAN verbunden, IP: " IPSTR "  (Status-Seite: http://" IPSTR ")",
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.ip));
        if (s_ap_on) {
            esp_wifi_set_mode(WIFI_MODE_STA);
            s_ap_on = false;
        }
        mqtt_ha_start();
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (g_cfg.ssid[0]) {
        wifi_config_t sta = {0};
        strncpy((char *)sta.sta.ssid, g_cfg.ssid, sizeof(sta.sta.ssid));
        strncpy((char *)sta.sta.password, g_cfg.pass, sizeof(sta.sta.password));
        sta.sta.threshold.authmode = g_cfg.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        ESP_LOGI(TAG, "Verbinde mit WLAN '%s'", g_cfg.ssid);
    } else {
        esp_wifi_set_mode(WIFI_MODE_AP);
        start_ap();
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Sparmodus aus: sonst werden MQTT-Befehle verzoegert */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

/* Board-spezifische Einstellungen */
static void board_apply(void)
{
    const board_t *b = &BOARDS[g_board];
    ESP_LOGI(TAG, "Board: %s", b->name);
    if (b->xiao_rf) {
        /* Seeed XIAO ESP32-C6: GPIO3 LOW = HF-Schalter an, GPIO14 waehlt die Antenne */
        gpio_config_t io = { .pin_bit_mask = (1ULL << 3) | (1ULL << 14), .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io);
        gpio_set_level(3, 0);
        gpio_set_level(14, b->xiao_rf == 2 ? 1 : 0);
    }
}

/* Reset-Taste 5 s halten -> WLAN/MQTT-Einstellungen loeschen */
static void button_task(void *arg)
{
    const board_t *b = &BOARDS[g_board];
    int pin = b->boot_gpio;
    gpio_config_t io = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT, .pull_up_en = b->boot_pullup };
    gpio_config(&io);
    int held = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        held = gpio_get_level(pin) == 0 ? held + 1 : 0;
        if (held == 50) {
            ESP_LOGW(TAG, "Einstellungen werden geloescht");
            cfg_erase();
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
    }
}

void app_main(void)
{
    s_boot_us = esp_timer_get_time();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_id, sizeof(g_id), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "SkyBridge startet (Kennung %s)", g_id);

    cfg_load();
    lamps_from_cfg();
    board_apply();
    xTaskCreate(button_task, "button", 2048, NULL, 3, NULL);
    wifi_start();
    portal_start(!g_cfg.ssid[0]);
    if (g_cfg.ssid[0] && g_mesh.valid) {
        mesh_start();
    } else if (g_cfg.ssid[0]) {
        ESP_LOGW(TAG, "Noch keine Lampen eingerichtet -> Export-Datei auf der Webseite hochladen");
    } else {
        /* Einrichtungsmodus: Bluetooth bleibt aus, damit das Einrichtungs-WLAN
         * das Funkteil fuer sich hat */
        ESP_LOGW(TAG, "Noch nicht eingerichtet -> Bluetooth-Mesh startet erst nach der Einrichtung");
    }
}
