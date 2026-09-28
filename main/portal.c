/* portal.c - Einstellungen (NVS) und Web-Oberflaeche */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bridge.h"
#include "boards.h"

#define TAG "portal"
#define NS "cfg"

app_cfg_t g_cfg;
mesh_cfg_t g_mesh;
int g_board;

/* ====================== Speicher ====================== */

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t l = len;
    if (nvs_get_str(h, key, out, &l) != ESP_OK) out[0] = 0;
}

void cfg_load(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    memset(&g_mesh, 0, sizeof(g_mesh));
    g_cfg.mqtt_port = 1883;
    g_mesh.own_addr = MESH_OWN_ADDR_BASE + 1;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    get_str(h, "ssid", g_cfg.ssid, sizeof(g_cfg.ssid));
    get_str(h, "pass", g_cfg.pass, sizeof(g_cfg.pass));
    get_str(h, "mhost", g_cfg.mqtt_host, sizeof(g_cfg.mqtt_host));
    get_str(h, "muser", g_cfg.mqtt_user, sizeof(g_cfg.mqtt_user));
    get_str(h, "mpass", g_cfg.mqtt_pass, sizeof(g_cfg.mqtt_pass));
    nvs_get_u16(h, "mport", &g_cfg.mqtt_port);

    char bid[24];
    get_str(h, "board", bid, sizeof(bid));
    for (int i = 0; i < NUM_BOARDS; i++) {
        if (strcmp(BOARDS[i].id, bid) == 0) g_board = i;
    }

    size_t l1 = 16, l2 = 16;
    bool k1 = nvs_get_blob(h, "netkey", g_mesh.net_key, &l1) == ESP_OK && l1 == 16;
    bool k2 = nvs_get_blob(h, "appkey", g_mesh.app_key, &l2) == ESP_OK && l2 == 16;
    nvs_get_u32(h, "iv", &g_mesh.iv_index);
    nvs_get_u16(h, "own", &g_mesh.own_addr);
    get_str(h, "lamps", g_mesh.lamps, sizeof(g_mesh.lamps));
    g_mesh.valid = k1 && k2 && g_mesh.lamps[0];
    nvs_close(h);
}

bool cfg_save(const app_cfg_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, "ssid", c->ssid);
    nvs_set_str(h, "pass", c->pass);
    nvs_set_str(h, "mhost", c->mqtt_host);
    nvs_set_str(h, "muser", c->mqtt_user);
    nvs_set_str(h, "mpass", c->mqtt_pass);
    nvs_set_u16(h, "mport", c->mqtt_port);
    nvs_set_str(h, "board", BOARDS[g_board].id);
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

bool mesh_cfg_save(const uint8_t net_key[16], const uint8_t app_key[16], uint32_t iv, const char *lamps)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    /* Bei jeder neuen Mesh-Konfiguration eine neue eigene Adresse verwenden:
     * Der Mesh-Speicher (inkl. Sequenznummer) wird geloescht, und die Lampen
     * wuerden Nachrichten mit alter Adresse und kleiner Sequenznummer verwerfen. */
    uint16_t own = g_mesh.own_addr + 1;
    if (own <= MESH_OWN_ADDR_BASE || own >= MESH_OWN_ADDR_BASE + 0xF0) own = MESH_OWN_ADDR_BASE + 1;
    nvs_set_blob(h, "netkey", net_key, 16);
    nvs_set_blob(h, "appkey", app_key, 16);
    nvs_set_u32(h, "iv", iv);
    nvs_set_u16(h, "own", own);
    nvs_set_str(h, "lamps", lamps);
    nvs_set_u8(h, "mrst", 1);
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

bool mesh_reset_pending(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_get_u8(h, "mrst", &v);
    if (v) {
        nvs_erase_key(h, "mrst");
        nvs_commit(h);
    }
    nvs_close(h);
    return v != 0;
}

/* BOOT-Taste: nur WLAN/MQTT loeschen. Mesh-Daten und der Adresszaehler bleiben. */
void cfg_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    const char *keys[] = {"ssid", "pass", "mhost", "muser", "mpass", "mport"};
    for (int i = 0; i < 6; i++) nvs_erase_key(h, keys[i]);
    nvs_commit(h);
    nvs_close(h);
}

/* "0002=Name;0003=Name" -> g_lamps */
void lamps_from_cfg(void)
{
    g_num_lamps = 0;
    const char *p = g_mesh.lamps;
    while (*p && g_num_lamps < MAX_LAMPS) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char item[48];
        if (len >= sizeof(item)) len = sizeof(item) - 1;
        memcpy(item, p, len);
        item[len] = 0;
        char *eq = strchr(item, '=');
        unsigned addr = 0;
        if (eq && sscanf(item, "%x", &addr) == 1 && addr > 0 && addr < 0x8000) {
            lamp_t *l = &g_lamps[g_num_lamps++];
            memset(l, 0, sizeof(*l));
            l->addr = (uint16_t)addr;
            strncpy(l->name, eq + 1, sizeof(l->name) - 1);
            if (!l->name[0]) snprintf(l->name, sizeof(l->name), "Lampe %04X", addr);
        }
        if (!end) break;
        p = end + 1;
    }
}

/* ====================== HTML ====================== */

static const char *HEAD =
    "<!DOCTYPE html><html lang=de><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>SkyBridge</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:520px;margin:0 auto;padding:16px;background:#f4f4f5;color:#18181b}"
    "h1{font-size:1.4em}h2{font-size:1.05em;margin:1.4em 0 .2em}"
    ".card{background:#fff;border-radius:12px;padding:16px;margin:12px 0;box-shadow:0 1px 3px #0002}"
    "label{display:block;margin:10px 0 4px;font-size:.9em;color:#52525b}"
    "input,textarea,select{width:100%;box-sizing:border-box;padding:10px;border:1px solid #d4d4d8;border-radius:8px;font-size:1em;font-family:inherit}"
    "textarea{font-family:ui-monospace,monospace;min-height:90px}"
    "button{margin-top:16px;width:100%;padding:12px;border:0;border-radius:8px;background:#2563eb;color:#fff;font-size:1em}"
    ".ok{color:#16a34a}.bad{color:#dc2626}small{color:#71717a}#msg{margin-top:8px}"
    "@media(prefers-color-scheme:dark){body{background:#18181b;color:#f4f4f5}.card{background:#27272a}"
    "input,textarea,select{background:#18181b;color:#f4f4f5;border-color:#3f3f46}label{color:#a1a1aa}}"
    "</style></head><body><h1>&#128161; SkyBridge</h1>";

/* liest die Mesh-Export-Datei (Telink SIG Mesh / nRF Mesh) im Browser aus */
static const char *MESH_JS =
    "<script>"
    "function hx(u){return (u||'').replace(/-/g,'').toUpperCase()}"
    "document.getElementById('f').onchange=function(e){"
    "var F=document.getElementById('m'),M=document.getElementById('msg'),r=new FileReader();"
    "r.onload=function(){try{var j=JSON.parse(r.result);"
    "var nk=(j.netKeys||[]).filter(function(k){return k.index==0})[0]||(j.netKeys||[])[0];"
    "var ak=(j.appKeys||[]).filter(function(k){return k.boundNetKey==0})[0]||(j.appKeys||[])[0];"
    "if(!nk||!ak)throw 'keine Schl\\u00fcssel gefunden';"
    "var pv=(j.provisioners||[]).map(function(p){return hx(p.UUID)});"
    "var L=(j.nodes||[]).filter(function(n){return !n.excluded&&pv.indexOf(hx(n.UUID))<0&&!/^provisioner/i.test(n.name||'')})"
    ".map(function(n){return n.unicastAddress+' '+(n.name||'')});"
    "F.nk.value=nk.key;F.ak.value=ak.key;if(j.ivIndex!=null)F.iv.value=j.ivIndex;F.lamps.value=L.join('\\n');"
    "M.className='ok';M.textContent=L.length+' Lampe(n) gefunden. Namen anpassen, fehlende Adressen erg\\u00e4nzen, dann speichern.';"
    "}catch(x){M.className='bad';M.textContent='Datei nicht lesbar: '+x}};"
    "r.readAsText(e.target.files[0])};"
    "</script>";

static void html_escape(char *out, size_t len, const char *in)
{
    size_t o = 0;
    for (; *in && o + 6 < len; in++) {
        if (*in == '<') { memcpy(out + o, "&lt;", 4); o += 4; }
        else if (*in == '>') { memcpy(out + o, "&gt;", 4); o += 4; }
        else if (*in == '"' || *in == '\'') { memcpy(out + o, "&#39;", 5); o += 5; }
        else if (*in == '&') { memcpy(out + o, "&amp;", 5); o += 5; }
        else out[o++] = *in;
    }
    out[o] = 0;
}

static esp_err_t root_get(httpd_req_t *req)
{
    const size_t BL = 4096;
    char *buf = malloc(BL);
    char *e1 = malloc(200), *e2 = malloc(200), *e3 = malloc(200);
    if (!buf || !e1 || !e2 || !e3) { free(buf); free(e1); free(e2); free(e3); return ESP_FAIL; }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, HEAD);

    /* ---- Status ---- */
    int n = snprintf(buf, BL, "<div class=card><b>Status</b><br>"
        "WLAN: <span class=%s>%s</span><br>MQTT: <span class=%s>%s</span><br>Mesh: ",
        g_wifi_connected ? "ok" : "bad", g_wifi_connected ? "verbunden" : "nicht verbunden",
        g_mqtt_connected ? "ok" : "bad", g_mqtt_connected ? "verbunden" : "nicht verbunden");
    if (!g_mesh.valid) n += snprintf(buf + n, BL - n, "<span class=bad>nicht eingerichtet</span>");
    else if (!g_cfg.ssid[0]) n += snprintf(buf + n, BL - n, "<small>startet nach der WLAN-Einrichtung</small>");
    else if (mesh_is_ready()) n += snprintf(buf + n, BL - n, "<span class=ok>bereit</span> <small>(Adresse %04X, IV %" PRIu32 ")</small>",
                                            g_mesh.own_addr, mesh_current_iv());
    else n += snprintf(buf + n, BL - n, "<small>startet...</small>");
    for (int i = 0; i < g_num_lamps && n < (int)BL - 200; i++) {
        lamp_t *l = &g_lamps[i];
        html_escape(e1, 200, l->name);
        n += snprintf(buf + n, BL - n, "<br>%s (%04X): ", e1, l->addr);
        if (!l->have_state) n += snprintf(buf + n, BL - n, "<small>noch keine Antwort</small>");
        else if (!l->online) n += snprintf(buf + n, BL - n, "<span class=bad>nicht erreichbar</span>");
        else if (!l->on) n += snprintf(buf + n, BL - n, "<span class=ok>aus</span>");
        else n += snprintf(buf + n, BL - n, "<span class=ok>an, %u%%, %u K</span>",
                           (unsigned)((l->lightness * 100UL + 32767) / 65535), temp_to_kelvin(l->temp));
    }
    snprintf(buf + n, BL - n, "<br><small>Kennung: %s</small></div>", g_id);
    httpd_resp_sendstr_chunk(req, buf);

    /* ---- Board / WLAN / MQTT ---- */
    char opts[600];
    int on = 0;
    for (int i = 0; i < NUM_BOARDS && on < (int)sizeof(opts) - 120; i++) {
        on += snprintf(opts + on, sizeof(opts) - on, "<option value=%s%s>%s</option>",
                       BOARDS[i].id, i == g_board ? " selected" : "", BOARDS[i].name);
    }
    html_escape(e1, 200, g_cfg.ssid);
    html_escape(e2, 200, g_cfg.mqtt_host);
    html_escape(e3, 200, g_cfg.mqtt_user);
    snprintf(buf, BL,
        "<form class=card method=post action=/save>"
        "<b>1. Board, WLAN &amp; MQTT</b>"
        "<label>Board <small>(Chip: " CONFIG_IDF_TARGET ")</small></label><select name=board>%s</select>"
        "<label>WLAN-Name (SSID, 2,4&nbsp;GHz)</label><input name=ssid value='%s' required maxlength=32>"
        "<label>WLAN-Passwort</label><input name=pass type=password placeholder='%s' maxlength=64>"
        "<label>MQTT-Server (IP von Home Assistant)</label><input name=mhost value='%s' placeholder='192.168.1.10' maxlength=63>"
        "<label>MQTT-Port</label><input name=mport type=number value='%u'>"
        "<label>MQTT-Benutzer</label><input name=muser value='%s' maxlength=63>"
        "<label>MQTT-Passwort</label><input name=mpass type=password placeholder='%s' maxlength=63>"
        "<button>Speichern &amp; neu starten</button>"
        "<small>Leere Passwortfelder lassen das gespeicherte Passwort unver&auml;ndert.</small></form>",
        opts, e1, g_cfg.pass[0] ? "(gespeichert)" : "", e2, g_cfg.mqtt_port, e3,
        g_cfg.mqtt_pass[0] ? "(gespeichert)" : "");
    httpd_resp_sendstr_chunk(req, buf);

    /* ---- Mesh ---- */
    snprintf(buf, BL,
        "<form class=card id=m method=post action=/mesh autocomplete=off>"
        "<b>2. Lampen (Bluetooth Mesh)</b>"
        "<label>Export-Datei der Telink-SIG-Mesh-App (.json)</label><input type=file id=f accept='.json,application/json'>"
        "<div id=msg></div>"
        "<label>NetKey (32 Hex-Zeichen)</label><input name=nk maxlength=32 placeholder='%s'>"
        "<label>AppKey (32 Hex-Zeichen)</label><input name=ak maxlength=32 placeholder='%s'>"
        "<label>IV-Index</label><input name=iv type=number min=0 placeholder='%s'>"
        "<label>Lampen &ndash; eine pro Zeile: <i>Adresse Name</i></label><textarea name=lamps placeholder='0002 Wohnzimmer&#10;0003 Gang'>",
        g_mesh.valid ? "(gespeichert)" : "wird aus der Datei gelesen",
        g_mesh.valid ? "(gespeichert)" : "wird aus der Datei gelesen",
        g_mesh.valid ? "(unver&auml;ndert)" : "0 = automatisch");
    httpd_resp_sendstr_chunk(req, buf);
    for (int i = 0; i < g_num_lamps; i++) {
        html_escape(e1, 200, g_lamps[i].name);
        snprintf(buf, BL, "%04X %s\n", g_lamps[i].addr, e1);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req,
        "</textarea>"
        "<small>Die Schl&uuml;ssel werden nur im ESP gespeichert und hier nie wieder angezeigt. "
        "Leere Schl&uuml;sselfelder behalten die gespeicherten Schl&uuml;ssel.</small>"
        "<button>Lampen speichern &amp; neu starten</button></form>");
    httpd_resp_sendstr_chunk(req, MESH_JS);
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    free(buf); free(e1); free(e2); free(e3);
    return ESP_OK;
}

/* ====================== Formulare ====================== */

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

/* Feld aus application/x-www-form-urlencoded holen (beliebige Laenge) */
static void form_field(const char *body, const char *key, char *out, size_t len)
{
    out[0] = 0;
    size_t kl = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            p += kl + 1;
            const char *end = strchr(p, '&');
            size_t l = end ? (size_t)(end - p) : strlen(p);
            if (l >= len) l = len - 1;
            memcpy(out, p, l);
            out[l] = 0;
            url_decode(out);
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static char *read_body(httpd_req_t *req, size_t max)
{
    if (req->content_len >= max) return NULL;
    char *body = malloc(req->content_len + 1);
    if (!body) return NULL;
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) { free(body); return NULL; }
        got += r;
    }
    body[got] = 0;
    return body;
}

static void restart_task(void *a)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void send_result(httpd_req_t *req, bool ok, const char *ok_text, const char *err_text)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, HEAD);
    httpd_resp_sendstr_chunk(req, "<div class=card>");
    httpd_resp_sendstr_chunk(req, ok ? "<b class=ok>Gespeichert.</b><p>" : "<b class=bad>Fehler:</b> ");
    httpd_resp_sendstr_chunk(req, ok ? ok_text : err_text);
    httpd_resp_sendstr_chunk(req, ok ? "</p></div>" : " <a href=/>Zur&uuml;ck</a></div>");
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    if (ok) xTaskCreate(restart_task, "rst", 2048, NULL, 5, NULL);
}

static esp_err_t save_post(httpd_req_t *req)
{
    char *body = read_body(req, 1024);
    if (!body) return ESP_FAIL;

    app_cfg_t c = g_cfg;
    char port[8], pass[65], mpass[64];
    form_field(body, "ssid", c.ssid, sizeof(c.ssid));
    form_field(body, "pass", pass, sizeof(pass));
    form_field(body, "mhost", c.mqtt_host, sizeof(c.mqtt_host));
    form_field(body, "mport", port, sizeof(port));
    form_field(body, "muser", c.mqtt_user, sizeof(c.mqtt_user));
    form_field(body, "mpass", mpass, sizeof(mpass));
    char bid[24];
    form_field(body, "board", bid, sizeof(bid));
    for (int i = 0; i < NUM_BOARDS; i++) {
        if (strcmp(BOARDS[i].id, bid) == 0) g_board = i;
    }
    free(body);
    if (pass[0]) strcpy(c.pass, pass);
    if (mpass[0]) strcpy(c.mqtt_pass, mpass);
    c.mqtt_port = port[0] ? (uint16_t)atoi(port) : 1883;

    bool ok = c.ssid[0] && cfg_save(&c);
    send_result(req, ok,
        "Die Bridge startet neu und verbindet sich mit deinem WLAN. Ihre neue IP-Adresse findest du im Router. "
        "Die Lampen erscheinen danach in Home Assistant unter <i>Einstellungen &rarr; Ger&auml;te &amp; Dienste &rarr; MQTT</i>.",
        "WLAN-Name fehlt oder Speichern fehlgeschlagen.");
    return ESP_OK;
}

static bool parse_key(const char *hex, uint8_t out[16])
{
    char clean[40];
    int n = 0;
    for (; *hex && n < 33; hex++) {
        if (isxdigit((unsigned char)*hex)) clean[n++] = *hex;
        else if (*hex != ' ' && *hex != ':' && *hex != '-') return false;
    }
    if (n != 32) return false;
    for (int i = 0; i < 16; i++) {
        char b[3] = {clean[2 * i], clean[2 * i + 1], 0};
        out[i] = (uint8_t)strtol(b, NULL, 16);
    }
    return true;
}

/* Textfeld "0002 Name\n0x0003 Name" -> "0002=Name;0003=Name" */
static int parse_lamps(const char *in, char *out, size_t outlen)
{
    int count = 0;
    size_t o = 0;
    out[0] = 0;
    while (*in && count < MAX_LAMPS) {
        while (*in == '\r' || *in == '\n' || *in == ' ' || *in == '\t') in++;
        if (!*in) break;
        const char *eol = strpbrk(in, "\r\n");
        size_t len = eol ? (size_t)(eol - in) : strlen(in);
        char line[64];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, in, len);
        line[len] = 0;
        in += len;

        char *p = line;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        char *endp;
        unsigned long addr = strtoul(p, &endp, 16);
        if (endp == p || addr == 0 || addr >= 0x8000) continue;
        while (*endp == ' ' || *endp == '\t' || *endp == ':' || *endp == '=' || *endp == ',') endp++;
        char name[32];
        int k = 0;
        for (; *endp && k < 31; endp++) {
            char ch = *endp;
            if (ch == ';' || ch == '=') ch = ' ';
            name[k++] = ch;
        }
        while (k > 0 && name[k - 1] == ' ') k--;
        name[k] = 0;
        if (!name[0]) snprintf(name, sizeof(name), "Lampe %04lX", addr);
        int w = snprintf(out + o, outlen - o, "%s%04lX=%s", count ? ";" : "", addr, name);
        if (w < 0 || (size_t)w >= outlen - o) break;
        o += w;
        count++;
    }
    return count;
}

static esp_err_t mesh_post(httpd_req_t *req)
{
    char *body = read_body(req, 3072);
    if (!body) return ESP_FAIL;

    char nk[80], ak[80], iv[16];
    char *lamps_in = malloc(1400), *lamps = malloc(sizeof(g_mesh.lamps));
    if (!lamps_in || !lamps) { free(body); free(lamps_in); free(lamps); return ESP_FAIL; }
    form_field(body, "nk", nk, sizeof(nk));
    form_field(body, "ak", ak, sizeof(ak));
    form_field(body, "iv", iv, sizeof(iv));
    form_field(body, "lamps", lamps_in, 1400);
    free(body);

    uint8_t net_key[16], app_key[16];
    const char *err = NULL;
    if (nk[0] ? !parse_key(nk, net_key) : !g_mesh.valid) err = "NetKey fehlt oder ist ung&uuml;ltig (32 Hex-Zeichen).";
    else if (ak[0] ? !parse_key(ak, app_key) : !g_mesh.valid) err = "AppKey fehlt oder ist ung&uuml;ltig (32 Hex-Zeichen).";
    if (!nk[0]) memcpy(net_key, g_mesh.net_key, 16);
    if (!ak[0]) memcpy(app_key, g_mesh.app_key, 16);
    uint32_t ivx = iv[0] ? (uint32_t)strtoul(iv, NULL, 10) : g_mesh.iv_index;
    int count = parse_lamps(lamps_in, lamps, sizeof(g_mesh.lamps));
    if (!err && count == 0) err = "Keine Lampe eingetragen (Format: <i>0002 Name</i>).";

    bool ok = !err && mesh_cfg_save(net_key, app_key, ivx, lamps);
    send_result(req, ok,
        "Die Bridge startet neu und meldet sich mit den neuen Schl&uuml;sseln im Mesh an. "
        "Nach etwa einer Minute sollten die Lampen auf der Startseite als erreichbar angezeigt werden.",
        err ? err : "Speichern fehlgeschlagen.");
    free(lamps_in);
    free(lamps);
    return ESP_OK;
}

/* Captive-Portal: alle unbekannten Adressen auf die Startseite umleiten */
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static httpd_handle_t s_httpd;

void portal_start(bool ap_mode)
{
    if (s_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Webserver-Start fehlgeschlagen");
        return;
    }
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_uri_t mesh = { .uri = "/mesh", .method = HTTP_POST, .handler = mesh_post };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &mesh);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_404);
    ESP_LOGI(TAG, "Webseite aktiv%s", ap_mode ? " (Einrichtungsmodus)" : "");
}
