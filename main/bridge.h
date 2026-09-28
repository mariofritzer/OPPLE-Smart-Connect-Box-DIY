#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ---------- Mesh ---------- */
#define MESH_OWN_ADDR_BASE 0x7E00      /* eigene Adresse = Basis + Zaehler (neu nach jeder Mesh-Konfiguration) */
#define MESH_ALL_NODES     0xFFFF
#define MAX_LAMPS          16

/* Home-Assistant-Farbtemperaturbereich (typisch fuer Tunable-White-Leuchten) */
#define HA_KELVIN_MIN     1800
#define HA_KELVIN_MAX     12000

typedef struct {
    uint16_t addr;
    char name[32];
    /* Zustand */
    bool online;
    uint8_t fails;
    bool have_state;
    bool on;
    uint16_t lightness;       /* 0..65535 */
    uint16_t last_lightness;  /* letzte Helligkeit > 0 */
    uint16_t temp;            /* Mesh-CTL-Wert */
} lamp_t;

extern lamp_t g_lamps[MAX_LAMPS];
extern int g_num_lamps;
extern uint16_t g_temp_min, g_temp_max;   /* CTL-Bereich der Lampe (vom Geraet gelesen) */

typedef enum { CMD_SET, CMD_POLL } cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint16_t addr;
    int8_t onoff;        /* -1 = unveraendert */
    int16_t brightness;  /* -1 = unveraendert, sonst 0..255 */
    int32_t kelvin;      /* -1 = unveraendert */
    int32_t trans_ms;    /* -1 = kein Uebergang */
} mesh_cmd_t;

void mesh_start(void);
bool mesh_submit(const mesh_cmd_t *cmd);
bool mesh_is_ready(void);
void mesh_poll_all(void);
int  lamp_index(uint16_t addr);
uint8_t  lightness_to_bri(uint16_t l);
uint16_t temp_to_kelvin(uint16_t t);
uint32_t mesh_current_iv(void);

/* ---------- Konfiguration ---------- */
typedef struct {
    char ssid[33];
    char pass[65];
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_user[64];
    char mqtt_pass[64];
} app_cfg_t;

typedef struct {
    bool valid;
    uint8_t net_key[16];
    uint8_t app_key[16];
    uint32_t iv_index;
    uint16_t own_addr;
    char lamps[MAX_LAMPS * 40];   /* "0002=Name;0003=Name" */
} mesh_cfg_t;

extern app_cfg_t g_cfg;
extern mesh_cfg_t g_mesh;
extern char g_id[13];          /* Teil der MAC als Hex, eindeutige Kennung */
extern bool g_wifi_connected;
extern bool g_mqtt_connected;

void cfg_load(void);
bool cfg_save(const app_cfg_t *c);
bool mesh_cfg_save(const uint8_t net_key[16], const uint8_t app_key[16], uint32_t iv, const char *lamps);
void cfg_erase(void);
void lamps_from_cfg(void);
bool mesh_reset_pending(void);
void portal_start(bool ap_mode);

/* ---------- MQTT / Home Assistant ---------- */
void mqtt_ha_start(void);
void mqtt_ha_publish_lamp(int idx);
