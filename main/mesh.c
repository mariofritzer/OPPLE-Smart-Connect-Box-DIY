/* mesh.c - Bluetooth-Mesh-Teil der Skylight-Bridge
 *
 * Die Bridge laeuft als (zweiter) Provisioner im bestehenden Mesh-Netz,
 * das mit der Telink-SIG-Mesh-App eingerichtet wurde. Sie kennt NetKey,
 * AppKey und IV-Index und schickt Standard-Nachrichten (Generic OnOff,
 * Light Lightness, Light CTL) an die Lampen.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_lighting_model_api.h"
#include "ble_mesh_example_init.h"

#include <inttypes.h>
#include "nvs.h"
#include "bridge.h"
#include "net.h"

#define TAG "mesh"

/* Schluessel und Lampen kommen aus der Konfiguration (Upload auf der Webseite) */
#define NET_IDX 0x0000
#define APP_IDX 0x0000

lamp_t g_lamps[MAX_LAMPS];
int g_num_lamps;

uint16_t g_temp_min = HA_KELVIN_MIN;
uint16_t g_temp_max = HA_KELVIN_MAX;

/* Interne ESP-IDF-Funktion bt_mesh_provisioner_provision(): traegt einen Knoten
 * in die Provisioner-Datenbank ein, ohne ihn neu anzulernen. Ohne Eintrag
 * verwirft der Stack die Antworten der Lampen. */
#include "pvnr_mgmt.h"
#include "settings.h"   /* bt_mesh_store_iv */

/* Adressbereich fuer die automatische Lampensuche */
#define SCAN_FIRST 0x0002
#define SCAN_LAST  0x0060

/* ---------------- Modelle der Bridge ---------------- */
static uint8_t dev_uuid[16];
static esp_ble_mesh_client_t config_client, onoff_client, lightness_client, ctl_client;

static esp_ble_mesh_cfg_srv_t config_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    /* keine eigenen Beacons: eine falsche IV-Index-Einstellung darf die Lampen nie beeinflussen */
    .beacon = ESP_BLE_MESH_BEACON_DISABLED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl = 7,
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&config_client),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(NULL, &onoff_client),
    ESP_BLE_MESH_MODEL_LIGHT_LIGHTNESS_CLI(NULL, &lightness_client),
    ESP_BLE_MESH_MODEL_LIGHT_CTL_CLI(NULL, &ctl_client),
};
#define M_ONOFF     (&root_models[2])
#define M_LIGHTNESS (&root_models[3])
#define M_CTL       (&root_models[4])

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, ESP_BLE_MESH_MODEL_NONE),
};

static esp_ble_mesh_comp_t composition = {
    .cid = 0x02E5,
    .element_count = ARRAY_SIZE(elements),
    .elements = elements,
};

static esp_ble_mesh_prov_t provision = {
    .prov_uuid           = dev_uuid,
    .prov_unicast_addr   = MESH_OWN_ADDR_BASE + 1,   /* wird in mesh_start() gesetzt */
    .prov_start_address  = 0x7FF0,
    .prov_attention      = 0x00,
    .prov_algorithm      = 0x00,
    .prov_pub_key_oob    = 0x00,
    .prov_static_oob_val = NULL,
    .prov_static_oob_len = 0x00,
    .flags               = 0x00,
    .iv_index            = 0,   /* wird in mesh_start() gesetzt */
};

/* ---------------- Zustand ---------------- */
static volatile bool s_ready;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_resp;
static volatile uint16_t s_wait_addr;
static volatile bool s_resp_ok;
static uint8_t s_tid;
static int s_bind_pending;
static volatile bool s_scanning, s_scan_request, s_discover_pending;
static uint16_t s_found[MAX_LAMPS];
static volatile int s_found_n;
char g_scan_result[160];

static void found_addr(uint16_t src)
{
    for (int i = 0; i < s_found_n; i++) if (s_found[i] == src) return;
    if (s_found_n < MAX_LAMPS) s_found[s_found_n++] = src;
}

bool mesh_is_ready(void) { return s_ready; }

int lamp_index(uint16_t addr)
{
    for (int i = 0; i < g_num_lamps; i++) {
        if (g_lamps[i].addr == addr) return i;
    }
    return -1;
}

/* ---------------- Umrechnungen ---------------- */
static uint16_t bri_to_lightness(int bri)
{
    if (bri <= 0) return 0;
    if (bri >= 255) return 0xFFFF;
    return (uint16_t)((bri * 65535 + 127) / 255);
}

uint8_t lightness_to_bri(uint16_t l)
{
    if (l == 0) return 0;
    uint32_t b = ((uint32_t)l * 255 + 32767) / 65535;
    return b < 1 ? 1 : (uint8_t)b;
}

static uint16_t kelvin_to_temp(int32_t k)
{
    if (k < HA_KELVIN_MIN) k = HA_KELVIN_MIN;
    if (k > HA_KELVIN_MAX) k = HA_KELVIN_MAX;
    int32_t t = g_temp_min + (int32_t)((int64_t)(k - HA_KELVIN_MIN) * (g_temp_max - g_temp_min) /
                                       (HA_KELVIN_MAX - HA_KELVIN_MIN));
    return (uint16_t)t;
}

uint16_t temp_to_kelvin(uint16_t t)
{
    if (g_temp_max <= g_temp_min) return t;
    if (t < g_temp_min) t = g_temp_min;
    if (t > g_temp_max) t = g_temp_max;
    return (uint16_t)(HA_KELVIN_MIN + (int64_t)(t - g_temp_min) * (HA_KELVIN_MAX - HA_KELVIN_MIN) /
                      (g_temp_max - g_temp_min));
}

static uint8_t encode_trans(int32_t ms)
{
    if (ms <= 0) return 0;
    if (ms <= 6200)   return (uint8_t)((ms + 50) / 100);
    if (ms <= 62000)  return (uint8_t)(0x40 | ((ms + 500) / 1000));
    if (ms <= 620000) return (uint8_t)(0x80 | ((ms + 5000) / 10000));
    int32_t s = (ms + 300000) / 600000;
    return (uint8_t)(0xC0 | (s > 62 ? 62 : s));
}

/* ---------------- Status von den Lampen ---------------- */
static void lamp_seen(int i)
{
    lamp_t *l = &g_lamps[i];
    l->fails = 0;
    l->online = true;
    l->have_state = true;
    if (l->lightness) l->last_lightness = l->lightness;
    mqtt_ha_publish_lamp(i);
}

static void resp_done(uint16_t addr, bool ok)
{
    if (addr == s_wait_addr) {
        s_resp_ok = ok;
        xSemaphoreGive(s_resp);
    }
}

static void generic_cb(esp_ble_mesh_generic_client_cb_event_t event,
                       esp_ble_mesh_generic_client_cb_param_t *param)
{
    uint16_t src = param->params->ctx.addr;
    if (event == ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT) {
        resp_done(src, false);
        return;
    }
    if (param->error_code) {
        resp_done(src, false);
        return;
    }
    if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET ||
        param->params->opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET ||
        param->params->opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS) {
        int i = lamp_index(src);
        if (i >= 0) {
            esp_ble_mesh_gen_onoff_status_cb_t *s = &param->status_cb.onoff_status;
            uint8_t v = s->op_en ? s->target_onoff : s->present_onoff;
            g_lamps[i].on = v != 0;
            if (!g_lamps[i].on) g_lamps[i].lightness = 0;
            else if (!g_lamps[i].lightness) g_lamps[i].lightness = g_lamps[i].last_lightness ? g_lamps[i].last_lightness : 0xFFFF;
            lamp_seen(i);
        }
    }
    resp_done(src, true);
}

static void light_cb(esp_ble_mesh_light_client_cb_event_t event,
                     esp_ble_mesh_light_client_cb_param_t *param)
{
    uint16_t src = param->params->ctx.addr;
    uint32_t op = param->params->opcode;
    if (event == ESP_BLE_MESH_LIGHT_CLIENT_TIMEOUT_EVT || param->error_code) {
        resp_done(src, false);
        return;
    }
    if (s_scanning && (op == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_STATUS || op == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_GET)) {
        found_addr(src);
    }
    int i = lamp_index(src);
    if (op == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_TEMPERATURE_RANGE_GET ||
        op == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_TEMPERATURE_RANGE_STATUS) {
        esp_ble_mesh_light_ctl_temperature_range_status_cb_t *r = &param->status_cb.ctl_temperature_range_status;
        if (r->status_code == 0 && r->range_max > r->range_min && r->range_min >= 800) {
            g_temp_min = r->range_min;
            g_temp_max = r->range_max;
            ESP_LOGI(TAG, "CTL-Bereich der Lampe: %u..%u", g_temp_min, g_temp_max);
        }
        if (i >= 0) { g_lamps[i].fails = 0; g_lamps[i].online = true; }
    } else if (op == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_GET || op == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_SET ||
               op == ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_STATUS) {
        if (i >= 0) {
            esp_ble_mesh_light_ctl_status_cb_t *s = &param->status_cb.ctl_status;
            uint16_t l = s->op_en ? s->target_ctl_lightness : s->present_ctl_lightness;
            uint16_t t = s->op_en ? s->target_ctl_temperature : s->present_ctl_temperature;
            g_lamps[i].lightness = l;
            g_lamps[i].temp = t;
            g_lamps[i].on = l > 0;
            lamp_seen(i);
        }
    } else if (op == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_GET || op == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET ||
               op == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS) {
        if (i >= 0) {
            esp_ble_mesh_light_lightness_status_cb_t *s = &param->status_cb.lightness_status;
            uint16_t l = s->op_en ? s->target_lightness : s->present_lightness;
            g_lamps[i].lightness = l;
            g_lamps[i].on = l > 0;
            lamp_seen(i);
        }
    }
    resp_done(src, true);
}

/* ---------------- Senden ---------------- */
static void fill_common(esp_ble_mesh_client_common_param_t *c, uint32_t opcode,
                        esp_ble_mesh_model_t *model, uint16_t dst)
{
    memset(c, 0, sizeof(*c));
    c->opcode = opcode;
    c->model = model;
    c->ctx.net_idx = NET_IDX;
    c->ctx.app_idx = APP_IDX;
    c->ctx.addr = dst;
    c->ctx.send_ttl = 7;
    c->msg_timeout = 3000;
}

/* sendet eine bestaetigte Nachricht und wartet auf die Antwort */
static bool send_wait(uint16_t dst, esp_err_t (*fn)(void *), void *arg)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        xSemaphoreTake(s_resp, 0);
        s_wait_addr = dst;
        s_resp_ok = false;
        if (fn(arg) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        if (xSemaphoreTake(s_resp, pdMS_TO_TICKS(4000)) == pdTRUE && s_resp_ok) {
            s_wait_addr = 0;
            return true;
        }
    }
    s_wait_addr = 0;
    return false;
}

typedef struct {
    esp_ble_mesh_client_common_param_t common;
    union {
        esp_ble_mesh_generic_client_set_state_t gset;
        esp_ble_mesh_light_client_set_state_t lset;
        esp_ble_mesh_light_client_get_state_t lget;
        esp_ble_mesh_generic_client_get_state_t gget;
    } u;
    bool generic;
    bool is_get;
} msg_t;

static esp_err_t do_send(void *p)
{
    msg_t *m = p;
    if (m->generic) {
        return m->is_get ? esp_ble_mesh_generic_client_get_state(&m->common, &m->u.gget)
                         : esp_ble_mesh_generic_client_set_state(&m->common, &m->u.gset);
    }
    return m->is_get ? esp_ble_mesh_light_client_get_state(&m->common, &m->u.lget)
                     : esp_ble_mesh_light_client_set_state(&m->common, &m->u.lset);
}

static void mark_fail(uint16_t addr)
{
    int i = lamp_index(addr);
    if (i < 0) return;
    if (g_lamps[i].fails < 255) g_lamps[i].fails++;
    if (g_lamps[i].fails >= 3 && g_lamps[i].online) {
        ESP_LOGW(TAG, "Lampe 0x%04x antwortet nicht", addr);
        g_lamps[i].online = false;
        mqtt_ha_publish_lamp(i);
    }
}

static bool poll_lamp(uint16_t addr)
{
    msg_t m = {0};
    fill_common(&m.common, ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_GET, M_CTL, addr);
    m.is_get = true;
    bool ok = send_wait(addr, do_send, &m);
    if (!ok) mark_fail(addr);
    return ok;
}

static void read_temp_range(uint16_t addr)
{
    msg_t m = {0};
    fill_common(&m.common, ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_TEMPERATURE_RANGE_GET, M_CTL, addr);
    m.is_get = true;
    send_wait(addr, do_send, &m);
}

static void handle_set(const mesh_cmd_t *c)
{
    bool group = c->addr == MESH_ALL_NODES;
    int li = group ? 0 : lamp_index(c->addr);
    if (li < 0) return;
    lamp_t *lamp = &g_lamps[li];

    bool opt = c->trans_ms >= 0;
    uint8_t trans = encode_trans(c->trans_ms);
    msg_t m = {0};

    if (c->onoff == 0) {
        fill_common(&m.common, group ? ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK : ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET,
                    M_ONOFF, c->addr);
        m.generic = true;
        m.u.gset.onoff_set = (esp_ble_mesh_gen_onoff_set_t){ .op_en = opt, .onoff = 0, .tid = s_tid++, .trans_time = trans, .delay = 0 };
    } else if (c->kelvin >= 0) {
        uint16_t l = c->brightness > 0 ? bri_to_lightness(c->brightness)
                   : (lamp->on && lamp->lightness ? lamp->lightness
                   : (lamp->last_lightness ? lamp->last_lightness : 0xFFFF));
        fill_common(&m.common, group ? ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_SET_UNACK : ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_SET,
                    M_CTL, c->addr);
        m.u.lset.ctl_set = (esp_ble_mesh_light_ctl_set_t){ .op_en = opt, .ctl_lightness = l,
            .ctl_temperature = kelvin_to_temp(c->kelvin), .ctl_delta_uv = 0, .tid = s_tid++, .trans_time = trans, .delay = 0 };
    } else if (c->brightness > 0) {
        fill_common(&m.common, group ? ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET_UNACK : ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET,
                    M_LIGHTNESS, c->addr);
        m.u.lset.lightness_set = (esp_ble_mesh_light_lightness_set_t){ .op_en = opt,
            .lightness = bri_to_lightness(c->brightness), .tid = s_tid++, .trans_time = trans, .delay = 0 };
    } else {
        fill_common(&m.common, group ? ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK : ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET,
                    M_ONOFF, c->addr);
        m.generic = true;
        m.u.gset.onoff_set = (esp_ble_mesh_gen_onoff_set_t){ .op_en = opt, .onoff = 1, .tid = s_tid++, .trans_time = trans, .delay = 0 };
    }

    if (group) {
        /* unbestaetigt, zweimal senden fuer Zuverlaessigkeit */
        do_send(&m);
        vTaskDelay(pdMS_TO_TICKS(150));
        do_send(&m);
        vTaskDelay(pdMS_TO_TICKS(1200 + (c->trans_ms > 0 ? c->trans_ms : 0)));
        for (int i = 0; i < g_num_lamps; i++) poll_lamp(g_lamps[i].addr);
        return;
    }

    if (!send_wait(c->addr, do_send, &m)) {
        ESP_LOGW(TAG, "Keine Antwort von 0x%04x", c->addr);
        mark_fail(c->addr);
        return;
    }
    /* nach Einschalten Helligkeit/Farbtemperatur nachlesen */
    if (m.generic && c->onoff != 0) poll_lamp(c->addr);
}

static int register_node(uint16_t unicast);

static void set_iv(uint32_t iv)
{
    bt_mesh_atomic_clear_bit(bt_mesh.flags, BLE_MESH_IVU_IN_PROGRESS);
    bt_mesh.iv_index = iv;
}

/* Sucht alle Lampen im Netz (Light CTL Get an "alle Knoten") und findet dabei
 * auch den richtigen IV-Index. Die Bridge sendet keine Beacons, daher kann ein
 * falscher Versuch die Lampen nicht beeinflussen. */
static void discovery(void)
{
    ESP_LOGW(TAG, "Suche Lampen (Adressen %04X-%04X)...", SCAN_FIRST, SCAN_LAST);
    snprintf(g_scan_result, sizeof(g_scan_result), "Suche l&auml;uft...");
    static uint16_t probe[SCAN_LAST - SCAN_FIRST + 1];
    int np = 0;
    for (uint16_t a = SCAN_FIRST; a <= SCAN_LAST; a++) {
        if (!bt_mesh_provisioner_get_node_with_addr(a) && register_node(a) == 0) probe[np++] = a;
    }

    uint32_t cands[12];
    int nc = 0;
    cands[nc++] = g_mesh.iv_index;
    for (uint32_t v = 0; v <= 10 && nc < 12; v++) if (v != g_mesh.iv_index) cands[nc++] = v;

    uint32_t orig = bt_mesh.iv_index, hit = UINT32_MAX;
    s_found_n = 0;
    s_scanning = true;
    for (int c = 0; c < nc && hit == UINT32_MAX; c++) {
        set_iv(cands[c]);
        msg_t m = {0};
        fill_common(&m.common, ESP_BLE_MESH_MODEL_OP_LIGHT_CTL_GET, M_CTL, MESH_ALL_NODES);
        m.is_get = true;
        for (int k = 0; k < 2 && s_found_n == 0; k++) {
            do_send(&m);
            vTaskDelay(pdMS_TO_TICKS(2500));
        }
        if (s_found_n > 0) {
            /* Treffer: noch zweimal nachfragen, damit auch Lampen erfasst werden,
             * deren erste Antwort verloren ging */
            vTaskDelay(pdMS_TO_TICKS(1500));
            for (int k = 0; k < 2; k++) {
                do_send(&m);
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            hit = cands[c];
        }
    }
    s_scanning = false;
    if (hit == UINT32_MAX) set_iv(orig);

    /* Platzhalter-Knoten wieder entfernen */
    for (int i = 0; i < np; i++) {
        bool keep = lamp_index(probe[i]) >= 0;
        for (int f = 0; f < s_found_n; f++) if (s_found[f] == probe[i]) keep = true;
        if (!keep) bt_mesh_provisioner_delete_node_with_node_addr(probe[i]);
    }

    if (hit == UINT32_MAX) {
        ESP_LOGW(TAG, "Keine Lampe gefunden");
        snprintf(g_scan_result, sizeof(g_scan_result),
                 "Keine Lampe gefunden. Haben die Lampen Strom? Stimmen die Schl&uuml;ssel (Export-Datei)?");
        return;
    }
    if (hit != g_mesh.iv_index) {
        ESP_LOGW(TAG, "IV-Index %" PRIu32 " erkannt", hit);
        g_mesh.iv_index = hit;
        bt_mesh_store_iv(false);
    }
    int added = 0;
    for (int f = 0; f < s_found_n; f++) {
        if (lamp_index(s_found[f]) >= 0 || g_num_lamps >= MAX_LAMPS) continue;
        lamp_t *l = &g_lamps[g_num_lamps++];
        memset(l, 0, sizeof(*l));
        l->addr = s_found[f];
        snprintf(l->name, sizeof(l->name), "Lampe %04X", s_found[f]);
        added++;
    }
    cfg_store_lamps_and_iv();
    int n = snprintf(g_scan_result, sizeof(g_scan_result), "%d Lampe(n) gefunden:", s_found_n);
    for (int f = 0; f < s_found_n && n < (int)sizeof(g_scan_result) - 20; f++) n += snprintf(g_scan_result + n, sizeof(g_scan_result) - n, " %04X", s_found[f]);
    n += snprintf(g_scan_result + n, sizeof(g_scan_result) - n, " (IV %" PRIu32 ")", hit);
    if (added) snprintf(g_scan_result + n, sizeof(g_scan_result) - n, ", %d neu hinzugef&uuml;gt", added);
    ESP_LOGW(TAG, "%s", g_scan_result);
    if (added) mqtt_ha_announce();
    mesh_poll_all();
}

void mesh_request_scan(void) { s_scan_request = true; }
bool mesh_is_scanning(void) { return s_scanning || s_scan_request; }

static void worker(void *arg)
{
    mesh_cmd_t c;
    int64_t next_poll = 0;
    bool range_read = false, first = true, auto_disc_done = false;
    int silent_rounds = 0;
    while (true) {
        if (!s_ready) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        if (s_scan_request || (first && (s_discover_pending || g_num_lamps == 0))) {
            s_scan_request = false;
            s_discover_pending = false;
            discovery();
            auto_disc_done = true;
            next_poll = 0;
        }
        first = false;
        if (!range_read && g_num_lamps > 0) {
            read_temp_range(g_lamps[0].addr);
            range_read = true;
        }
        if (xQueueReceive(s_queue, &c, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (c.type == CMD_SET) handle_set(&c);
            else poll_lamp(c.addr);
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_poll) {
            for (int i = 0; i < g_num_lamps; i++) poll_lamp(g_lamps[i].addr);
            next_poll = now + 30LL * 1000000;
            /* antwortet nach dem Start gar keine Lampe -> einmal automatisch suchen (IV-Index?) */
            bool any = false;
            for (int i = 0; i < g_num_lamps; i++) any |= g_lamps[i].have_state;
            if (!any && !auto_disc_done && ++silent_rounds >= 2) {
                auto_disc_done = true;
                s_scan_request = true;
            }
        }
    }
}

bool mesh_submit(const mesh_cmd_t *cmd)
{
    return s_queue && xQueueSend(s_queue, cmd, 0) == pdTRUE;
}

void mesh_poll_all(void)
{
    for (int i = 0; i < g_num_lamps; i++) {
        mesh_cmd_t c = { .type = CMD_POLL, .addr = g_lamps[i].addr, .onoff = -1, .brightness = -1, .kelvin = -1, .trans_ms = -1 };
        mesh_submit(&c);
    }
}

/* ---------------- Einrichtung ---------------- */
/* Knoten in die Provisioner-Datenbank eintragen (sonst verwirft der Stack seine Antworten) */
static int register_node(uint16_t unicast)
{
    if (bt_mesh_provisioner_get_node_with_addr(unicast)) return 0;
    bt_mesh_addr_t addr = {0};
    uint8_t uuid[16] = {0x53, 0x4B, 0x59, 0x42, 0x52, 0x44, 0x47, 0x45}; /* "SKYBRDGE" */
    uuid[14] = unicast >> 8;
    uuid[15] = unicast & 0xFF;
    uint16_t idx = 0;
    /* Der Geraeteschluessel wird nur fuer Konfigurationsnachrichten gebraucht,
     * die die Bridge nie sendet -> Platzhalter */
    uint8_t dev_key[16] = {0};
    return bt_mesh_provisioner_provision(&addr, uuid, 0, unicast, 1, NET_IDX, 0,
                                         g_mesh.iv_index, dev_key, &idx, false);
}

static void register_lamps(void)
{
    for (int i = 0; i < g_num_lamps; i++) {
        int err = register_node(g_lamps[i].addr);
        ESP_LOGW(TAG, "Lampe %s (0x%04x) registriert: %d", g_lamps[i].name, g_lamps[i].addr, err);
    }
    s_ready = true;
    ESP_LOGW(TAG, "Mesh bereit (eigene Adresse 0x%04x, IV-Index %" PRIu32 ")", g_mesh.own_addr, mesh_current_iv());
    mesh_poll_all();
}

static void bind_models(void)
{
    const uint16_t ids[] = {
        ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI,
        ESP_BLE_MESH_MODEL_ID_LIGHT_LIGHTNESS_CLI,
        ESP_BLE_MESH_MODEL_ID_LIGHT_CTL_CLI,
    };
    s_bind_pending = ARRAY_SIZE(ids);
    for (int i = 0; i < ARRAY_SIZE(ids); i++) {
        esp_ble_mesh_provisioner_bind_app_key_to_local_model(g_mesh.own_addr, APP_IDX, ids[i], ESP_BLE_MESH_CID_NVAL);
    }
}

static void prov_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "Provisioner aktiv (%d), setze NetKey", param->provisioner_prov_enable_comp.err_code);
        esp_ble_mesh_provisioner_update_local_net_key(g_mesh.net_key, NET_IDX);
        break;
    case ESP_BLE_MESH_PROVISIONER_UPDATE_LOCAL_NET_KEY_COMP_EVT:
        ESP_LOGI(TAG, "NetKey gesetzt (%d), setze AppKey", param->provisioner_update_net_key_comp.err_code);
        esp_ble_mesh_provisioner_add_local_app_key(g_mesh.app_key, NET_IDX, APP_IDX);
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
        if (param->provisioner_add_app_key_comp.err_code) {
            /* existiert schon (Neustart) -> sicherheitshalber aktualisieren */
            esp_ble_mesh_provisioner_update_local_app_key(g_mesh.app_key, NET_IDX, APP_IDX);
        } else {
            bind_models();
        }
        break;
    case ESP_BLE_MESH_PROVISIONER_UPDATE_LOCAL_APP_KEY_COMP_EVT:
        ESP_LOGI(TAG, "AppKey aktualisiert (%d)", param->provisioner_update_app_key_comp.err_code);
        bind_models();
        break;
    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT:
        if (--s_bind_pending == 0) register_lamps();
        break;
    default:
        break;
    }
}

static void config_client_cb(esp_ble_mesh_cfg_client_cb_event_t event, esp_ble_mesh_cfg_client_cb_param_t *param)
{
    (void)event; (void)param;
}

void mesh_start(void)
{
    if (!g_mesh.valid) {
        ESP_LOGW(TAG, "Keine Mesh-Konfiguration -> bitte auf der Webseite die Export-Datei hochladen");
        return;
    }
    provision.iv_index = g_mesh.iv_index;

    if (mesh_reset_pending()) {
        /* neue Schluessel -> gespeicherten Mesh-Zustand (alte Schluessel, Zaehler) verwerfen */
        nvs_handle_t h;
        if (nvs_open("mesh_core", NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGW(TAG, "Mesh-Speicher zurueckgesetzt, eigene Adresse jetzt 0x%04x", g_mesh.own_addr);
        s_discover_pending = true;
    }

    s_queue = xQueueCreate(16, sizeof(mesh_cmd_t));
    s_resp = xSemaphoreCreateBinary();
    s_tid = (uint8_t)(esp_timer_get_time() & 0xFF);

    if (bluetooth_init() != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth-Start fehlgeschlagen");
        return;
    }
    ble_mesh_get_dev_uuid(dev_uuid);

    esp_ble_mesh_register_prov_callback(prov_cb);
    esp_ble_mesh_register_config_client_callback(config_client_cb);
    esp_ble_mesh_register_generic_client_callback(generic_cb);
    esp_ble_mesh_register_light_client_callback(light_cb);

    if (esp_ble_mesh_init(&provision, &composition) != ESP_OK) {
        ESP_LOGE(TAG, "Mesh-Init fehlgeschlagen");
        return;
    }
    if (esp_ble_mesh_provisioner_set_primary_elem_addr(g_mesh.own_addr) != ESP_OK) {
        ESP_LOGE(TAG, "Eigene Adresse 0x%04x konnte nicht gesetzt werden", g_mesh.own_addr);
    }
    /* UUID-Filter, der auf kein Geraet passt: die Bridge lernt nie selbst Lampen an */
    uint8_t match[2] = {0xFE, 0xED};
    esp_ble_mesh_provisioner_set_dev_uuid_match(match, sizeof(match), 0, false);
    esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV);

    xTaskCreate(worker, "mesh_worker", 4096, NULL, 5, NULL);
}

uint32_t mesh_current_iv(void)
{
    return bt_mesh.iv_index;
}
