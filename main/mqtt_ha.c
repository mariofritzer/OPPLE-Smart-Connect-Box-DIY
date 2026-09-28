/* mqtt_ha.c - MQTT-Anbindung mit Home-Assistant-Discovery */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include "bridge.h"
#include "boards.h"

#define TAG "mqtt"

static esp_mqtt_client_handle_t s_client;
bool g_mqtt_connected;
static char s_base[40];      /* skybridge/<id> */
static char s_status[64];    /* skybridge/<id>/status */

static void topic_for(char *buf, size_t len, uint16_t addr, const char *leaf)
{
    snprintf(buf, len, "%s/%04x/%s", s_base, addr, leaf);
}

static void publish_discovery_one(uint16_t addr, const char *name, bool group)
{
    char topic[128], cmd[80], state[80], avail[80], uid[48];
    snprintf(uid, sizeof(uid), "skybridge_%s_%04x", g_id, addr);
    snprintf(topic, sizeof(topic), "homeassistant/light/%s/config", uid);
    topic_for(cmd, sizeof(cmd), addr, "set");
    topic_for(state, sizeof(state), addr, "state");
    topic_for(avail, sizeof(avail), addr, "avail");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "unique_id", uid);
    cJSON_AddStringToObject(o, "schema", "json");
    cJSON_AddStringToObject(o, "command_topic", cmd);
    cJSON_AddBoolToObject(o, "brightness", true);
    cJSON *modes = cJSON_AddArrayToObject(o, "supported_color_modes");
    cJSON_AddItemToArray(modes, cJSON_CreateString("color_temp"));
    cJSON_AddBoolToObject(o, "color_temp_kelvin", true);
    cJSON_AddNumberToObject(o, "min_kelvin", HA_KELVIN_MIN);
    cJSON_AddNumberToObject(o, "max_kelvin", HA_KELVIN_MAX);

    cJSON *av = cJSON_AddArrayToObject(o, "availability");
    cJSON *a1 = cJSON_CreateObject();
    cJSON_AddStringToObject(a1, "topic", s_status);
    cJSON_AddItemToArray(av, a1);
    if (group) {
        cJSON_AddBoolToObject(o, "optimistic", true);
    } else {
        cJSON_AddStringToObject(o, "state_topic", state);
        cJSON *a2 = cJSON_CreateObject();
        cJSON_AddStringToObject(a2, "topic", avail);
        cJSON_AddItemToArray(av, a2);
        cJSON_AddStringToObject(o, "availability_mode", "all");
    }

    cJSON *dev = cJSON_AddObjectToObject(o, "device");
    cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
    char devid[48];
    if (group) snprintf(devid, sizeof(devid), "skybridge_%s", g_id);
    else snprintf(devid, sizeof(devid), "skybridge_%s_%04x", g_id, addr);
    cJSON_AddItemToArray(ids, cJSON_CreateString(devid));
    if (group) {
        cJSON_AddStringToObject(dev, "name", "SkyBridge");
        cJSON_AddStringToObject(dev, "manufacturer", "DIY");
        cJSON_AddStringToObject(dev, "model", BOARDS[g_board].name);
    } else {
        char via[48];
        snprintf(via, sizeof(via), "skybridge_%s", g_id);
        cJSON_AddStringToObject(dev, "name", name);
        cJSON_AddStringToObject(dev, "manufacturer", "Bluetooth Mesh");
        cJSON_AddStringToObject(dev, "model", "Tunable-White-Leuchte");
        cJSON_AddStringToObject(dev, "via_device", via);
    }

    char *js = cJSON_PrintUnformatted(o);
    esp_mqtt_client_publish(s_client, topic, js, 0, 1, 1);
    cJSON_free(js);
    cJSON_Delete(o);
}

static void publish_discovery(void)
{
    /* Gruppe zuerst, damit das Bridge-Geraet existiert (via_device) */
    publish_discovery_one(MESH_ALL_NODES, "Alle Lampen", true);
    for (int i = 0; i < g_num_lamps; i++) {
        publish_discovery_one(g_lamps[i].addr, g_lamps[i].name, false);
        mqtt_ha_publish_lamp(i);
    }
}

void mqtt_ha_announce(void)
{
    if (s_client && g_mqtt_connected) publish_discovery();
}

void mqtt_ha_publish_lamp(int idx)
{
    if (!s_client || !g_mqtt_connected || idx < 0 || idx >= g_num_lamps) return;
    lamp_t *l = &g_lamps[idx];
    char topic[80];

    topic_for(topic, sizeof(topic), l->addr, "avail");
    esp_mqtt_client_publish(s_client, topic, l->online ? "online" : "offline", 0, 1, 1);
    if (!l->have_state) return;

    char js[128];
    int n = snprintf(js, sizeof(js), "{\"state\":\"%s\"", l->on ? "ON" : "OFF");
    if (l->on) {
        n += snprintf(js + n, sizeof(js) - n, ",\"brightness\":%u", lightness_to_bri(l->lightness));
    }
    if (l->temp) {
        n += snprintf(js + n, sizeof(js) - n, ",\"color_mode\":\"color_temp\",\"color_temp\":%u", temp_to_kelvin(l->temp));
    }
    snprintf(js + n, sizeof(js) - n, "}");
    topic_for(topic, sizeof(topic), l->addr, "state");
    esp_mqtt_client_publish(s_client, topic, js, 0, 1, 1);
}

static void handle_command(uint16_t addr, const char *data, int len)
{
    cJSON *o = cJSON_ParseWithLength(data, len);
    if (!o) return;
    mesh_cmd_t c = { .type = CMD_SET, .addr = addr, .onoff = -1, .brightness = -1, .kelvin = -1, .trans_ms = -1 };

    cJSON *st = cJSON_GetObjectItem(o, "state");
    if (cJSON_IsString(st)) c.onoff = strcmp(st->valuestring, "OFF") == 0 ? 0 : 1;
    cJSON *b = cJSON_GetObjectItem(o, "brightness");
    if (cJSON_IsNumber(b)) c.brightness = b->valueint;
    cJSON *ct = cJSON_GetObjectItem(o, "color_temp");
    if (cJSON_IsNumber(ct)) {
        /* HA schickt mit color_temp_kelvin=true Kelvin; alte Versionen Mired */
        c.kelvin = ct->valueint < 1000 ? 1000000 / (ct->valueint ? ct->valueint : 1) : ct->valueint;
    }
    cJSON *tr = cJSON_GetObjectItem(o, "transition");
    if (cJSON_IsNumber(tr)) c.trans_ms = (int32_t)(tr->valuedouble * 1000);

    if (c.brightness == 0) c.onoff = 0;
    if (c.onoff == -1) c.onoff = 1;
    cJSON_Delete(o);

    if (!mesh_submit(&c)) ESP_LOGW(TAG, "Befehlswarteschlange voll");
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "Mit MQTT verbunden");
        g_mqtt_connected = true;
        esp_mqtt_client_publish(s_client, s_status, "online", 0, 1, 1);
        char sub[64];
        snprintf(sub, sizeof(sub), "%s/+/set", s_base);
        esp_mqtt_client_subscribe(s_client, sub, 1);
        esp_mqtt_client_subscribe(s_client, "homeassistant/status", 1);
        publish_discovery();
        mesh_poll_all();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        g_mqtt_connected = false;
        break;
    case MQTT_EVENT_DATA: {
        char topic[96];
        int tl = e->topic_len < (int)sizeof(topic) - 1 ? e->topic_len : (int)sizeof(topic) - 1;
        memcpy(topic, e->topic, tl);
        topic[tl] = 0;
        if (strcmp(topic, "homeassistant/status") == 0) {
            if (e->data_len >= 6 && strncmp(e->data, "online", 6) == 0) publish_discovery();
            break;
        }
        size_t bl = strlen(s_base);
        unsigned addr;
        if (strncmp(topic, s_base, bl) == 0 && sscanf(topic + bl, "/%4x/set", &addr) == 1) {
            handle_command((uint16_t)addr, e->data, e->data_len);
        }
        break;
    }
    default:
        break;
    }
}

void mqtt_ha_start(void)
{
    if (s_client || !g_cfg.mqtt_host[0]) return;
    snprintf(s_base, sizeof(s_base), "skybridge/%s", g_id);
    snprintf(s_status, sizeof(s_status), "%s/status", s_base);

    static char client_id[32];
    snprintf(client_id, sizeof(client_id), "skybridge-%s", g_id);

    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname = g_cfg.mqtt_host,
        .broker.address.port = g_cfg.mqtt_port ? g_cfg.mqtt_port : 1883,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id = client_id,
        .credentials.username = g_cfg.mqtt_user[0] ? g_cfg.mqtt_user : NULL,
        .credentials.authentication.password = g_cfg.mqtt_pass[0] ? g_cfg.mqtt_pass : NULL,
        .session.last_will.topic = s_status,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .session.keepalive = 30,
        .buffer.size = 2048,
    };
    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_mqtt_client_start(s_client);
}
