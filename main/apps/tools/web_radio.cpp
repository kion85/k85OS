#include "web_radio.h"
#include "../../net/wifi.h"
#include "list_menu.h"
#include "text_input.h"
#include "common.h"
#include "input.h"
#include "log.h"
#include "theme.h"
#include "battery.h"

#include "M5Unified.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

struct K85WRPreset { const char *name; const char *host; int port; };
static const K85WRPreset K85_WR_PRESETS[] = {
    { "OK2KYJ (Czech)",       "sdr.ok2kyj.cz",              8073 },
    { "Warszawa (Poland)",    "warszawa.proxy.kiwisdr.com", 8073 },
    { "K1VL (USA)",           "sdr.k1vl.com",                8073 },
    { "F5NKP (France)",       "f5nkp.freeboxos.fr",          8073 },
    { "SDR.DDNS.NET",         "sdr.ddns.net",                8073 },
    { "Chantilly (France)",   "daviken.ddns.net",            8073 },
    { "Lippstadt (Germany)",  "df3af.ddns.net",              8073 },
    { "Filderstadt (Germany)","dl2sba.ddns.net",              8073 },
};
#define K85_WR_PRESET_COUNT (int)(sizeof(K85_WR_PRESETS) / sizeof(K85_WR_PRESETS[0]))

struct K85WRMode { const char *name; const char *kiwi_mod; int low_cut; int high_cut; };
static const K85WRMode K85_WR_MODES[] = {
    { "AM",  "am",  -4000, 4000 },
    { "USB", "usb",   100, 2700 },
    { "LSB", "lsb", -2700, -100 },
    { "CW",  "cw",    300,  700 },
};
#define K85_WR_MODE_COUNT (int)(sizeof(K85_WR_MODES) / sizeof(K85_WR_MODES[0]))

#define K85_WR_AUDIO_BUF_SAMPLES 2048
#define K85_WR_AUDIO_HEADER_SKIP 10

// ---------- Общее состояние (читается отовсюду, но МЕНЯЕТСЯ только из
// ws_background_task - HTTP-обработчик лишь выставляет "хочу" флаги) ----------
static volatile int s_freq_khz = 7200;
static volatile int s_mode_idx = 0;
static volatile bool s_muted = false;
static volatile int16_t s_smeter_raw = 0;

static esp_websocket_client_handle_t s_ws = nullptr;
static volatile bool s_connected = false;
static volatile int s_ws_state = 0; // 0=idle, 1=connecting, 2=connected, 3=error
static volatile bool s_need_handshake = false;

// Запросы от HTTP-обработчика к фоновому потоку - ТОЛЬКО так, никогда
// напрямую из HTTP-обработчика в esp_websocket_client_* (иначе гонка
// за внутренний мьютекс клиента и обрывы соединения).
static volatile bool s_want_connect = false;
static char s_want_host[64] = "";
static volatile int s_want_port = 8073;

static volatile bool s_want_retune = false;
static volatile int s_want_freq = 7200;
static volatile int s_want_mode = 0;

static volatile bool s_want_scan = false;
static volatile bool s_scan_ready = false;
static char s_scan_result[1024] = "[]";

static volatile bool s_ws_task_running = false;
static TaskHandle_t s_ws_task_handle = nullptr;

static int16_t s_audio_buf[2][K85_WR_AUDIO_BUF_SAMPLES];
static volatile int s_fill_idx = 0;
static volatile int s_fill_pos = 0;
static volatile bool s_buf_ready[2] = { false, false };

static esp_netif_t *s_ap_netif = nullptr;
static volatile bool s_server_running = false;

#define K85_WR_TASK_STACK_BYTES 8192
static_assert(sizeof(StackType_t) == 1,
    "StackType_t не uint8_t - пересчитай K85_WR_TASK_STACK_BYTES!");
static StaticTask_t s_wr_task_buf;
static StackType_t *s_wr_task_stack = nullptr;

static void ws_background_task(void *arg);
static bool start_ws_background_task(void);

// ---------- Внутренние функции - вызываются ТОЛЬКО из ws_background_task ----------
static void send_retune_internal(int freq_khz, int mode_idx) {
    if (!s_connected || !s_ws) return;
    const K85WRMode &m = K85_WR_MODES[mode_idx];
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "SET mod=%s low_cut=%d high_cut=%d freq=%d.000",
             m.kiwi_mod, m.low_cut, m.high_cut, freq_khz);
    esp_websocket_client_send_text(s_ws, cmd, strlen(cmd), pdMS_TO_TICKS(1000));
}

static void handle_audio_frame(const uint8_t *data, int len) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        char c0 = (data[0] >= 32 && data[0] < 127) ? (char)data[0] : '.';
        char c1 = (len > 1 && data[1] >= 32 && data[1] < 127) ? (char)data[1] : '.';
        char c2 = (len > 2 && data[2] >= 32 && data[2] < 127) ? (char)data[2] : '.';
        k85_log("wr: tag=%c%c%c len=%d", c0, c1, c2, len);
        k85_log("wr: hex %02x %02x %02x %02x %02x %02x",
                 data[0], len>1?data[1]:0, len>2?data[2]:0,
                 len>3?data[3]:0, len>4?data[4]:0, len>5?data[5]:0);
    }
    if (len <= K85_WR_AUDIO_HEADER_SKIP) return;
    if (memcmp(data, "SND", 3) != 0) return;

    int16_t smeter = (int16_t)((data[8] << 8) | data[9]);
    s_smeter_raw = smeter;

    // KiwiSDR шлёт PCM-сэмплы в big-endian, ESP32 - little-endian, поэтому
    // байты каждого сэмпла нужно поменять местами (как уже сделано для smeter выше).
    const uint8_t *sample_bytes = data + K85_WR_AUDIO_HEADER_SKIP;
    int n_samples = (len - K85_WR_AUDIO_HEADER_SKIP) / 2;
    for (int i = 0; i < n_samples; i++) {
        int16_t sample = (int16_t)((sample_bytes[i * 2] << 8) | sample_bytes[i * 2 + 1]);
        s_audio_buf[s_fill_idx][s_fill_pos] = sample;
        s_fill_pos++;
        if (s_fill_pos >= K85_WR_AUDIO_BUF_SAMPLES) {
            s_buf_ready[s_fill_idx] = true;
            s_fill_idx = 1 - s_fill_idx;
            s_fill_pos = 0;
        }
    }
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            s_connected = true;
            s_ws_state = 2;
            s_need_handshake = true;
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x02 && data->data_len > 0) {
                handle_audio_frame((const uint8_t *)data->data_ptr, data->data_len);
            } else if (data->op_code == 0x01 && data->data_len > 0) {
                k85_log("wr: TXT %.*s", data->data_len, (const char *)data->data_ptr);
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_ERROR:
            s_connected = false;
            s_ws_state = 3;
            break;
        default:
            break;
    }
}

// Вызывается ТОЛЬКО из фонового потока - единственный владелец s_ws
static void do_connect_internal(const char *host, int port) {
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = nullptr;
    }
    s_connected = false;
    s_ws_state = 1;

    k85_log("wr: ram=%uKB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    char uri[128];
    int64_t ts = esp_timer_get_time() / 1000;
    snprintf(uri, sizeof(uri), "ws://%s:%d/kiwi/%lld/SND", host, port, (long long)ts);

    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.reconnect_timeout_ms = 3000;
    cfg.network_timeout_ms = 8000;
    cfg.task_stack = 4096;
    cfg.buffer_size = 2048;
    cfg.disable_auto_reconnect = true;

    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) { k85_log("wr: init failed"); s_ws_state = 3; return; }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, nullptr);
    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        k85_log("wr: start err=%d", (int)err);
        s_ws_state = 3;
    }
}

#define K85_WR_SCAN_POINTS 21
#define K85_WR_SCAN_STEP_KHZ 5

// Вызывается ТОЛЬКО из фонового потока
static void do_scan_internal(char *json_out, size_t json_out_size) {
    int center = s_freq_khz;
    int saved_mode = s_mode_idx;
    size_t used = 0;
    used += snprintf(json_out + used, json_out_size - used, "[");

    for (int i = 0; i < K85_WR_SCAN_POINTS; i++) {
        int f = center + (i - K85_WR_SCAN_POINTS / 2) * K85_WR_SCAN_STEP_KHZ;
        if (f < 0) f = 0;
        send_retune_internal(f, saved_mode);
        vTaskDelay(pdMS_TO_TICKS(220));
        int level = s_smeter_raw;
        used += snprintf(json_out + used, json_out_size - used,
                          "%s{\"freq\":%d,\"level\":%d}", i == 0 ? "" : ",", f, level);
        if (used >= json_out_size - 64) break;
    }
    snprintf(json_out + used, json_out_size - used, "]");

    send_retune_internal(center, saved_mode);
}

static bool start_ws_background_task(void) {
    if (!s_wr_task_stack) {
        s_wr_task_stack = (StackType_t *)heap_caps_malloc(K85_WR_TASK_STACK_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_wr_task_stack) {
            k85_log("wr: PSRAM stack alloc failed");
            return false;
        }
    }
    s_ws_task_running = true;
    s_ws_task_handle = xTaskCreateStaticPinnedToCore(
        ws_background_task,
        "k85_wr_ws", K85_WR_TASK_STACK_BYTES, nullptr, 5,
        s_wr_task_stack, &s_wr_task_buf, tskNO_AFFINITY);
    return s_ws_task_handle != nullptr;
}

// ЕДИНСТВЕННЫЙ поток, который трогает esp_websocket_client_* - вся логика
// подключения/перестройки/скана/handshake выполняется строго здесь по очереди,
// никогда параллельно из HTTP-обработчика.
static void ws_background_task(void *arg) {
    while (s_ws_task_running) {
        if (s_want_connect) {
            s_want_connect = false;
            char host_copy[64];
            snprintf(host_copy, sizeof(host_copy), "%s", s_want_host);
            do_connect_internal(host_copy, s_want_port);
        }

        if (s_need_handshake) {
            s_need_handshake = false;
            const char *auth = "SET auth t=kiwi p=";
            esp_websocket_client_send_text(s_ws, auth, strlen(auth), pdMS_TO_TICKS(1000));
            vTaskDelay(pdMS_TO_TICKS(200));
            const char *ar = "SET AR OK in=12000 out=44100";
            esp_websocket_client_send_text(s_ws, ar, strlen(ar), pdMS_TO_TICKS(1000));
            vTaskDelay(pdMS_TO_TICKS(200));
            const char *comp = "SET compression=0";
            esp_websocket_client_send_text(s_ws, comp, strlen(comp), pdMS_TO_TICKS(1000));
            vTaskDelay(pdMS_TO_TICKS(200));
            send_retune_internal(s_freq_khz, s_mode_idx);
        }

        if (s_want_retune) {
            s_want_retune = false;
            s_freq_khz = s_want_freq;
            s_mode_idx = s_want_mode;
            send_retune_internal(s_freq_khz, s_mode_idx);
        }

        if (s_want_scan) {
            s_want_scan = false;
            do_scan_internal(s_scan_result, sizeof(s_scan_result));
            s_scan_ready = true;
        }

        for (int i = 0; i < 2; i++) {
            if (s_buf_ready[i]) {
                if (!s_muted) {
                    M5.Speaker.playRaw(s_audio_buf[i], K85_WR_AUDIO_BUF_SAMPLES, 12000, false, 1);
                }
                s_buf_ready[i] = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
    vTaskDelete(nullptr);
}

// ---------- HTTP ----------
static void send_http(int fd, const char *status, const char *ctype, const char *body) {
    char header[160];
    snprintf(header, sizeof(header), "HTTP/1.1 %s\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", status, ctype);
    send(fd, header, strlen(header), 0);
    if (body) send(fd, body, strlen(body), 0);
}

static bool get_param(const char *query, const char *key, char *out, size_t out_size) {
    char search[32];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(query, search);
    if (!p) return false;
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return true;
}

static const char *K85_WR_PAGE = R"HTML(<!DOCTYPE html><html><head><meta charset='UTF-8'>
<meta name=viewport content='width=device-width,initial-scale=1'>
<title>k85OS Radio</title>
<style>
body{background:#0a0a12;color:#e0e0e0;font-family:sans-serif;margin:0;padding:16px;max-width:480px;margin:auto}
h1{color:#4df;font-size:18px;margin-bottom:4px}
.card{background:#151525;border-radius:10px;padding:14px;margin:10px 0;border:1px solid #333}
select,input,button{font-size:15px;padding:8px;border-radius:6px;border:1px solid #444;background:#111;color:#eee;width:100%;box-sizing:border-box;margin-top:6px}
button{background:#2a6;color:#fff;border:none;cursor:pointer;font-weight:bold}
button.mute{background:#a33}
button.mute.active{background:#5a5}
.freqrow{display:flex;gap:6px;margin-top:6px}
.freqrow button{flex:1;background:#334;margin-top:0;font-size:13px}
.freq-input{font-size:26px;text-align:center;margin-top:8px;color:#4df;font-weight:bold}
.status{text-align:center;font-size:13px;color:#8f8}
.status.off{color:#f96}
#spectrum{width:100%;height:140px;background:#000;border-radius:8px;margin-top:8px}
.modes{display:flex;gap:6px}
.modes button{flex:1;background:#333;margin-top:0}
.modes button.active{background:#4af}
</style></head><body>
<h1>k85OS Internet Radio</h1>

<div class="card">
  <div class="freqrow">
    <button onclick="stepFreq(-100)">-100</button>
    <button onclick="stepFreq(-10)">-10</button>
    <button onclick="stepFreq(-1)">-1</button>
  </div>
  <input type="number" class="freq-input" id="freqInput" value="7200" onchange="setFreqDirect()">
  <div class="freqrow">
    <button onclick="stepFreq(1)">+1</button>
    <button onclick="stepFreq(10)">+10</button>
    <button onclick="stepFreq(100)">+100</button>
  </div>
  <div class="status" id="status">Not connected</div>
  <div class="modes" id="modes"></div>
  <button onclick="doMute()" class="mute" id="muteBtn">Mute</button>
</div>

<div class="card">
  <label>Server</label>
  <select id="preset" onchange="onPreset()"></select>
  <input id="manualHost" placeholder="or type host manually">
  <input id="manualPort" placeholder="port" value="8073">
  <button onclick="doConnect()">Connect</button>
</div>

<div class="card">
  <button onclick="doScan()">Scan nearby frequencies</button>
  <canvas id="spectrum"></canvas>
  <div style="font-size:11px;color:#888;margin-top:4px">Tap a bar to tune there</div>
</div>

<script>
let presets = [];
let modes = ["AM","USB","LSB","CW"];
let curMode = 0;
let curFreq = 7200;
let scanData = [];

function stepFreq(delta) {
  curFreq += delta;
  if (curFreq < 0) curFreq = 0;
  document.getElementById('freqInput').value = curFreq;
  tuneTo(curFreq);
}

function setFreqDirect() {
  const v = parseInt(document.getElementById('freqInput').value);
  if (!isNaN(v) && v >= 0) { curFreq = v; tuneTo(curFreq); }
}

async function api(path) {
  const r = await fetch(path);
  return await r.json();
}

async function refreshStatus() {
  try {
    const s = await api('/api/status');
    curFreq = s.freq;
    if (document.activeElement.id !== 'freqInput') {
      document.getElementById('freqInput').value = s.freq;
    }
    const st = document.getElementById('status');
    if (s.state === 'connected') { st.innerText = 'Connected'; st.className = 'status'; }
    else if (s.state === 'connecting') { st.innerText = 'Connecting...'; st.className = 'status off'; }
    else if (s.state === 'error') { st.innerText = 'Connection failed - check host/port'; st.className = 'status off'; }
    else { st.innerText = 'Not connected - pick a server'; st.className = 'status off'; }
    curMode = s.mode_idx;
    renderModes();
    document.getElementById('muteBtn').className = 'mute' + (s.muted ? ' active' : '');
    document.getElementById('muteBtn').innerText = s.muted ? 'Unmute' : 'Mute';
  } catch(e) {}
}

function renderModes() {
  const c = document.getElementById('modes');
  c.innerHTML = '';
  modes.forEach((m,i) => {
    const b = document.createElement('button');
    b.innerText = m;
    b.className = i === curMode ? 'active' : '';
    b.onclick = () => setMode(i);
    c.appendChild(b);
  });
}

async function setMode(i) {
  await fetch('/api/tune?mode=' + i);
  refreshStatus();
}

async function loadPresets() {
  presets = await api('/api/presets');
  const sel = document.getElementById('preset');
  sel.innerHTML = '';
  presets.forEach((p,i) => {
    const o = document.createElement('option');
    o.value = i; o.innerText = p.name;
    sel.appendChild(o);
  });
  if (presets.length > 0) onPreset();
}

function onPreset() {
  const i = document.getElementById('preset').value;
  document.getElementById('manualHost').value = presets[i].host;
  document.getElementById('manualPort').value = presets[i].port;
}

async function doConnect() {
  const host = document.getElementById('manualHost').value;
  const port = document.getElementById('manualPort').value;
  await fetch('/api/connect?host=' + encodeURIComponent(host) + '&port=' + port);
  refreshStatus();
}

async function doMute() {
  const isActive = document.getElementById('muteBtn').className.includes('active');
  await fetch('/api/mute?on=' + (isActive ? '0' : '1'));
  refreshStatus();
}

async function doScan() {
  document.getElementById('status').innerText = 'Scanning...';
  scanData = await api('/api/scan');
  drawSpectrum();
  refreshStatus();
}

function drawSpectrum() {
  const cv = document.getElementById('spectrum');
  cv.width = cv.clientWidth; cv.height = 140;
  const ctx = cv.getContext('2d');
  ctx.clearRect(0,0,cv.width,cv.height);
  if (scanData.length === 0) return;
  let maxL = Math.max(...scanData.map(d=>d.level), 1);
  let minL = Math.min(...scanData.map(d=>d.level), 0);
  let range = Math.max(maxL - minL, 1);
  const bw = cv.width / scanData.length;
  scanData.forEach((d,i) => {
    const norm = (d.level - minL) / range;
    const h = Math.max(4, norm * (cv.height - 20));
    const bright = norm > 0.6;
    ctx.fillStyle = bright ? '#4f6' : '#264';
    ctx.fillRect(i*bw+1, cv.height-h, bw-2, h);
  });
  let peakIdx = scanData.reduce((best,d,i,arr)=> d.level>arr[best].level?i:best, 0);
  const ax = peakIdx*bw + bw/2;
  ctx.fillStyle = '#fd0';
  ctx.beginPath();
  ctx.moveTo(ax-6, 10); ctx.lineTo(ax+6, 10); ctx.lineTo(ax, 18);
  ctx.fill();

  cv.onclick = (ev) => {
    const rect = cv.getBoundingClientRect();
    const x = ev.clientX - rect.left;
    const idx = Math.floor(x / bw);
    if (scanData[idx]) tuneTo(scanData[idx].freq);
  };
}

async function tuneTo(freq) {
  await fetch('/api/tune?freq=' + freq);
  refreshStatus();
}

loadPresets();
refreshStatus();
setInterval(refreshStatus, 2000);
</script>
</body></html>)HTML";

static void handle_connection(int conn_fd) {
    static char req_buf[2048];
    long req_len = 0;
    long headers_end = -1;

    while (headers_end < 0 && req_len < (long)sizeof(req_buf) - 1) {
        int r = recv(conn_fd, req_buf + req_len, sizeof(req_buf) - 1 - req_len, 0);
        if (r <= 0) break;
        req_len += r;
        req_buf[req_len] = 0;
        for (long i = 0; i + 3 < req_len; i++) {
            if (memcmp(req_buf + i, "\r\n\r\n", 4) == 0) { headers_end = i; break; }
        }
    }
    if (headers_end < 0) return;

    char method[8] = {0}, path_full[256] = {0};
    sscanf(req_buf, "%7s %255s", method, path_full);

    char path[256] = {0}, query[256] = {0};
    char *q = strchr(path_full, '?');
    if (q) {
        size_t plen = (size_t)(q - path_full);
        if (plen >= sizeof(path)) plen = sizeof(path) - 1;
        memcpy(path, path_full, plen); path[plen] = 0;
        snprintf(query, sizeof(query), "%s", q + 1);
    } else {
        snprintf(path, sizeof(path), "%s", path_full);
    }

    if (!strcmp(path, "/")) {
        send_http(conn_fd, "200 OK", "text/html", K85_WR_PAGE);
    } else if (!strcmp(path, "/api/status")) {
        const char *state_str = s_ws_state == 0 ? "idle" : s_ws_state == 1 ? "connecting" : s_ws_state == 2 ? "connected" : "error";
        char body[224];
        snprintf(body, sizeof(body),
                 "{\"freq\":%d,\"mode_idx\":%d,\"connected\":%s,\"state\":\"%s\",\"muted\":%s,\"smeter\":%d}",
                 s_freq_khz, s_mode_idx, s_connected ? "true" : "false", state_str,
                 s_muted ? "true" : "false", (int)s_smeter_raw);
        send_http(conn_fd, "200 OK", "application/json", body);
    } else if (!strcmp(path, "/api/presets")) {
        char body[512];
        size_t used = snprintf(body, sizeof(body), "[");
        for (int i = 0; i < K85_WR_PRESET_COUNT; i++) {
            used += snprintf(body + used, sizeof(body) - used,
                              "%s{\"name\":\"%s\",\"host\":\"%s\",\"port\":%d}",
                              i == 0 ? "" : ",", K85_WR_PRESETS[i].name, K85_WR_PRESETS[i].host, K85_WR_PRESETS[i].port);
        }
        snprintf(body + used, sizeof(body) - used, "]");
        send_http(conn_fd, "200 OK", "application/json", body);
    } else if (!strcmp(path, "/api/connect")) {
        char host[64] = {0}, port_str[8] = {0};
        get_param(query, "host", host, sizeof(host));
        get_param(query, "port", port_str, sizeof(port_str));
        int port = atoi(port_str);
        if (port <= 0) port = 8073;
        if (host[0]) {
            snprintf(s_want_host, sizeof(s_want_host), "%s", host);
            s_want_port = port;
            s_want_connect = true; // фоновый поток сам подключится
        }
        send_http(conn_fd, "200 OK", "application/json", "{\"ok\":true}");
    } else if (!strcmp(path, "/api/tune")) {
        char freq_str[16] = {0}, mode_str[8] = {0};
        int new_freq = s_freq_khz, new_mode = s_mode_idx;
        bool changed = false;
        if (get_param(query, "freq", freq_str, sizeof(freq_str))) {
            int f = atoi(freq_str);
            if (f > 0) { new_freq = f; changed = true; }
        }
        if (get_param(query, "mode", mode_str, sizeof(mode_str))) {
            int m = atoi(mode_str);
            if (m >= 0 && m < K85_WR_MODE_COUNT) { new_mode = m; changed = true; }
        }
        if (changed) {
            s_want_freq = new_freq;
            s_want_mode = new_mode;
            s_want_retune = true; // фоновый поток применит и отправит команду
        }
        send_http(conn_fd, "200 OK", "application/json", "{\"ok\":true}");
    } else if (!strcmp(path, "/api/mute")) {
        char on_str[4] = {0};
        get_param(query, "on", on_str, sizeof(on_str));
        s_muted = (on_str[0] == '1');
        send_http(conn_fd, "200 OK", "application/json", "{\"ok\":true}");
    } else if (!strcmp(path, "/api/scan")) {
        s_scan_ready = false;
        s_want_scan = true;
        int waited_ms = 0;
        while (!s_scan_ready && waited_ms < 8000) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited_ms += 100;
        }
        send_http(conn_fd, "200 OK", "application/json", s_scan_ready ? s_scan_result : "[]");
    } else {
        send_http(conn_fd, "404 Not Found", "text/plain", "Not found");
    }
}

static bool setup_ap_and_get_choice(bool *out_open, char *out_pass, size_t pass_size, int *out_channel) {
    const char *sec_items[] = { "Open (no password)", "WPA2 (set password)" };
    int sec = k85_run_list_menu("ENCRYPTION", sec_items, 2, nullptr);
    if (sec < 0) return false;
    *out_open = (sec == 0);

    if (!*out_open) {
        if (!k85_text_input("AP password\n(min 8 chars):", "", out_pass, pass_size)) return false;
        if (strlen(out_pass) < 8) {
            k85_show_message("Too short (min 8)\nA+B=back");
            vTaskDelay(pdMS_TO_TICKS(1500));
            return false;
        }
    } else {
        out_pass[0] = 0;
    }

    char ch_buf[4] = "1";
    if (!k85_text_input("Channel (1-13):", ch_buf, ch_buf, sizeof(ch_buf))) return false;
    int ch = atoi(ch_buf);
    if (ch < 1 || ch > 13) ch = 1;
    *out_channel = ch;
    return true;
}

void k85_run_web_radio(void) {
    const char *net_items[] = { "Use current WiFi", "Create own AP (k85os-radio)" };
    int net_choice = k85_run_list_menu("NETWORK MODE", net_items, 2, nullptr);
    if (net_choice < 0) return;

    bool use_own_ap = (net_choice == 1);
    char ip_str[16] = "";

    if (!use_own_ap) {
        if (!k85_wifi_is_connected()) {
            k85_show_message("Connecting to\nsaved WiFi...");
            bool ok = k85_wifi_connect_saved();
            if (!ok) {
                k85_show_message("Not connected\nSetup WiFi first\n(WiFi Manager)\nA+B=back");
                while (true) {
                    k85_input_update();
                    if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                return;
            }
        }
        snprintf(ip_str, sizeof(ip_str), "%s", k85_wifi_get_ip_str());
    } else {
        bool ap_open = true;
        char ap_pass[32] = "";
        int ap_channel = 1;
        if (!setup_ap_and_get_choice(&ap_open, ap_pass, sizeof(ap_pass), &ap_channel)) return;

        if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

        wifi_config_t ap_config = {};
        const char *ssid = "k85os-radio";
        snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s", ssid);
        ap_config.ap.ssid_len = strlen(ssid);
        if (!ap_open) {
            snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%s", ap_pass);
            ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            ap_config.ap.authmode = WIFI_AUTH_OPEN;
        }
        ap_config.ap.max_connection = 4;
        ap_config.ap.channel = (uint8_t)ap_channel;

        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        esp_err_t start_err = esp_wifi_start();
        if (start_err != ESP_OK && start_err != ESP_ERR_WIFI_STATE) {
            k85_show_message("AP start failed\nA+B=back");
            vTaskDelay(pdMS_TO_TICKS(1500));
            return;
        }

        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(s_ap_netif, &ip_info);
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }

    uint8_t wr_saved_volume = M5.Speaker.getVolume();
    M5.Speaker.setVolume(200); // погромче для сессии радио - раньше громкость вообще не трогалась

    if (!start_ws_background_task()) {
        M5.Speaker.setVolume(wr_saved_volume);
        k85_show_message("Task create failed\n(PSRAM OOM?)\nA+B=back");
        while (true) {
            k85_input_update();
            if (k85_ab_held(500)) { k85_wait_ab_release(); break; }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        return;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(80);
    bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_fd, 2);

    char msg[128];
    if (use_own_ap) {
        snprintf(msg, sizeof(msg), "Own AP mode\nIP: %s\nOpen browser to above IP\nA+B=stop", ip_str);
    } else {
        snprintf(msg, sizeof(msg), "Home network mode\nIP: %s\nOpen from same WiFi\nA+B=stop", ip_str);
    }
    k85_show_message(msg);

    s_server_running = true;
    while (s_server_running) {
        k85_input_update();
        if (k85_ab_held(500)) { k85_wait_ab_release(); s_server_running = false; break; }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        struct timeval tv = {0, 200000};
        int sel = select(listen_fd + 1, &fds, nullptr, nullptr, &tv);
        if (sel > 0 && FD_ISSET(listen_fd, &fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (conn_fd >= 0) {
                struct timeval sock_tv = { .tv_sec = 9, .tv_usec = 0 }; // >8с скана + запас
                setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &sock_tv, sizeof(sock_tv));
                setsockopt(conn_fd, SOL_SOCKET, SO_SNDTIMEO, &sock_tv, sizeof(sock_tv));
                handle_connection(conn_fd);
                close(conn_fd);
            }
        }
    }
    close(listen_fd);

    s_ws_task_running = false;
    vTaskDelay(pdMS_TO_TICKS(100));
    if (s_ws) { esp_websocket_client_stop(s_ws); esp_websocket_client_destroy(s_ws); s_ws = nullptr; }

    M5.Speaker.setVolume(wr_saved_volume);
    if (use_own_ap) esp_wifi_set_mode(WIFI_MODE_STA);
    k85_show_message("Web Radio stopped");
    vTaskDelay(pdMS_TO_TICKS(1000));
}