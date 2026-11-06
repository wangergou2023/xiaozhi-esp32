#include "pet/pet_gateway.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>

#include "board.h"
#include "pet/pet_audio.h"
#include "pet/pet_ears.h"
#include "pet/pet_core.h"

namespace kitty {
namespace {

constexpr const char* TAG = "PetGateway";
constexpr const char* kApPassword = "kitty1234";
constexpr float kIdleTimeout = 180.0f;  // three minutes, then the radio is off

std::string UrlDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < text.size()) {
            char hex[3] = {text[i + 1], text[i + 2], '\0'};
            out.push_back(static_cast<char>(strtol(hex, nullptr, 16)));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string FormField(const std::string& body, const std::string& key) {
    const std::string needle = key + "=";
    size_t start = 0;
    while (start < body.size()) {
        size_t end = body.find('&', start);
        if (end == std::string::npos) {
            end = body.size();
        }
        const std::string pair = body.substr(start, end - start);
        if (pair.compare(0, needle.size(), needle) == 0) {
            return UrlDecode(pair.substr(needle.size()));
        }
        start = end + 1;
    }
    return {};
}

std::string Escape(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

int64_t DaysFromCivil(int year, int month, int day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153u * (month + (month > 2 ? -3 : 9)) + 2u) / 5u + static_cast<unsigned>(day) - 1u;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097LL + static_cast<int>(day_of_era) - 719468LL;
}

// "2025-11-05T23:41"
bool ParseLocalTime(const std::string& text, int64_t& epoch_seconds, int tz_minutes) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    if (sscanf(text.c_str(), "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) != 5) {
        return false;
    }
    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    epoch_seconds = DaysFromCivil(year, month, day) * 86400LL + hour * 3600LL + minute * 60LL -
                    tz_minutes * 60LL;
    return true;
}

std::string SelectOption(int value, int current, const char* label) {
    std::string option = "<option value=\"" + std::to_string(value) + "\"";
    if (value == current) {
        option += " selected";
    }
    option += ">";
    option += label;
    option += "</option>";
    return option;
}

std::string Field(const char* label, const std::string& input) {
    return "<div class=\"field\"><span>" + std::string(label) + "</span>" + input + "</div>";
}

// One card, one topic. Cards keep the page from turning into one long column on
// a desktop screen and give the eye a place to land.
std::string Card(const char* title, const std::string& body) {
    return "<section class=\"card\"><h2>" + std::string(title) + "</h2>" + body + "</section>";
}

std::string Page() {
    auto& core = PetCore::Get();
    const int start = core.sleep_start_minute();
    const int end = core.sleep_end_minute();
    char sleep_start[16] = {};
    char sleep_end[16] = {};
    snprintf(sleep_start, sizeof(sleep_start), "%02d:%02d", start / 60, start % 60);
    snprintf(sleep_end, sizeof(sleep_end), "%02d:%02d", end / 60, end % 60);

    const int volume = PetAudio::Get().volume();
    int brightness = 75;
    if (auto* backlight = Board::GetInstance().GetBacklight()) {
        brightness = backlight->brightness();
    }

    std::string html;
    html.reserve(6144);
    html += R"HTML(<!doctype html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark">
<title>Kitty</title><style>
:root{--bg:#0b0b0c;--card:#141417;--line:#26262b;--fg:#e9e7e4;--muted:#93939b;--accent:#ffb26b}
*{box-sizing:border-box}
html,body{max-width:100%;overflow-x:hidden}
body{margin:0;background:var(--bg);color:var(--fg);
 font:15px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Noto Sans SC",sans-serif;
 -webkit-font-smoothing:antialiased}
.wrap{max-width:460px;margin:0 auto;padding:20px 14px 8px}
h1{margin:0;font-size:22px;font-weight:650;letter-spacing:.3px}
.sub{margin:4px 0 18px;color:var(--muted);font-size:13px}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;
 padding:15px 14px 1px;margin-bottom:12px}
.card h2{margin:0 0 13px;font-size:11px;font-weight:600;letter-spacing:.16em;
 text-transform:uppercase;color:var(--muted)}
.field{margin-bottom:15px}
.field>span{display:flex;justify-content:space-between;align-items:baseline;
 font-size:13px;color:var(--muted);margin-bottom:7px}
.field>span b{color:var(--accent);font-weight:650;font-variant-numeric:tabular-nums}
input,select{width:100%;min-width:0;padding:12px;border:1px solid var(--line);border-radius:12px;
 background:#1a1a1f;color:var(--fg);font:inherit;font-size:16px;appearance:none;
 -webkit-appearance:none;transition:border-color .15s}
input:focus,select:focus{outline:none;border-color:var(--accent)}
select{background-image:linear-gradient(45deg,transparent 50%,var(--muted) 50%),
 linear-gradient(135deg,var(--muted) 50%,transparent 50%);
 background-position:calc(100% - 19px) 54%,calc(100% - 14px) 54%;
 background-size:5px 5px,5px 5px;background-repeat:no-repeat;padding-right:38px}
input[type=range]{-webkit-appearance:none;appearance:none;padding:0;background:none;border:0;height:32px}
input[type=range]::-webkit-slider-runnable-track{height:6px;border-radius:999px;background:#2b2b31}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;margin-top:-8px;
 border-radius:50%;background:var(--accent);box-shadow:0 0 0 5px rgba(255,178,107,.14)}
input[type=range]::-moz-range-track{height:6px;border-radius:999px;background:#2b2b31}
input[type=range]::-moz-range-thumb{width:22px;height:22px;border:0;border-radius:50%;background:var(--accent)}
.row{display:flex;gap:10px}
.row .field{flex:1;min-width:0}
.bar{position:sticky;bottom:0;margin-top:-8px;padding:18px 0 calc(18px + env(safe-area-inset-bottom));
 background:linear-gradient(180deg,rgba(11,11,12,0) 0,rgba(11,11,12,.92) 34%,var(--bg) 100%)}
button{width:100%;padding:16px;border:0;border-radius:14px;background:var(--accent);
 color:#191207;font:650 16px/1 inherit;letter-spacing:.3px;cursor:pointer}
button:active{transform:scale(.995)}
.foot{margin:2px 2px 10px;color:var(--muted);font-size:12px;line-height:1.5}
</style></head><body><div class="wrap">
<h1>Kitty</h1>
<p class="sub">改完点保存，热点会自己关掉。</p>
<form method="post" action="/save">
)HTML";

    std::string basics;
    basics += Field("名字", "<input name=\"name\" value=\"" + Escape(core.name()) + "\">");
    basics += Field("唤醒词（拼音）",
                    "<input name=\"wake_phrase\" value=\"" + Escape(Ears::Get().phrase()) +
                        "\" autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\">");
    html += Card("她是谁", basics);

    std::string voice;
    voice += "<div class=\"field\"><span>音量 <b id=\"volumeValue\">" + std::to_string(volume) +
             "</b></span><input id=\"volume\" name=\"volume\" type=\"range\" min=\"0\" max=\"100\" "
             "value=\"" + std::to_string(volume) + "\"></div>";
    voice += "<div class=\"field\"><span>亮度 <b id=\"brightnessValue\">" +
             std::to_string(brightness) + "</b></span><input id=\"brightness\" name=\"brightness\" "
             "type=\"range\" min=\"5\" max=\"100\" value=\"" + std::to_string(brightness) + "\"></div>";
    voice += Field("唤醒灵敏度",
                   "<select name=\"wake_sensitivity\">" +
                       SelectOption(0, Ears::Get().sensitivity(), "低 · 环境安静") +
                       SelectOption(1, Ears::Get().sensitivity(), "中 · 日常") +
                       SelectOption(2, Ears::Get().sensitivity(), "高 · 小声也能叫醒") +
                       "</select>");
    html += Card("声音与灯", voice);

    std::string character;
    character += Field("难度",
                       "<select name=\"difficulty\">" +
                           SelectOption(0, core.difficulty(), "轻松 · 掉得慢") +
                           SelectOption(1, core.difficulty(), "正常") +
                           SelectOption(2, core.difficulty(), "硬核 · 掉得快") + "</select>");
    character += Field("性格",
                       "<select name=\"personality\">" +
                           SelectOption(0, core.personality(), "黏人 · 总来找你") +
                           SelectOption(1, core.personality(), "标准") +
                           SelectOption(2, core.personality(), "冷淡 · 自己待着") +
                           SelectOption(3, core.personality(), "活泼 · 闲不住") + "</select>");
    html += Card("脾气", character);

    std::string routine;
    routine += "<div class=\"row\">";
    routine += Field("几点睡", "<input name=\"sleep_start\" type=\"time\" value=\"" +
                                   std::string(sleep_start) + "\">");
    routine += Field("几点起", "<input name=\"sleep_end\" type=\"time\" value=\"" +
                                   std::string(sleep_end) + "\">");
    routine += "</div>";
    html += Card("作息", routine);

    std::string clock;
    clock += Field("对时", "<input name=\"clock\" type=\"datetime-local\">");
    html += Card("时间", clock);

    html += R"HTML(<div class="bar"><button type="submit">保存并关闭热点</button>
<p class="foot">只在这里改参数；她不联网，热点只是短暂的设置通道。</p></div>
</form></div><script>
for (const id of ['volume','brightness']) {
  const input = document.getElementById(id);
  const out = document.getElementById(id + 'Value');
  if (input && out) input.addEventListener('input', () => { out.textContent = input.value; });
}
</script></body></html>)HTML";
    return html;
}

std::string SavedPage() {
    return R"HTML(<!doctype html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark"><title>Kitty</title><style>
body{margin:0;min-height:100vh;display:grid;place-items:center;background:#0b0b0c;color:#e9e7e4;
 font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI","Noto Sans SC",sans-serif;text-align:center}
.box{padding:28px}
.mark{width:52px;height:52px;margin:0 auto 16px;border-radius:50%;background:#ffb26b;color:#191207;
 display:grid;place-items:center;font-size:26px;font-weight:700}
h1{margin:0 0 6px;font-size:19px;font-weight:650}
p{margin:0;color:#93939b;font-size:13px}
</style></head><body><div class="box"><div class="mark">✓</div>
<h1>已保存</h1><p>热点马上关闭，她继续过日子。</p></div></body></html>)HTML";
}

esp_err_t RootHandler(httpd_req_t* request) {
    Gateway::Get().Touch();
    const std::string html = Page();
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_send(request, html.c_str(), html.size());
    return ESP_OK;
}

esp_err_t SaveHandler(httpd_req_t* request) {
    char buffer[1024] = {};
    const int received = httpd_req_recv(request, buffer, sizeof(buffer) - 1);
    if (received <= 0) {
        httpd_resp_send_500(request);
        return ESP_FAIL;
    }
    Gateway::Get().Touch();
    const std::string body(buffer, received);
    Gateway::Get().ApplyForm(body);

    const std::string html = SavedPage();
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_send(request, html.c_str(), html.size());
    // Give the response time to leave the radio before it is switched off.
    Gateway::Get().ScheduleStop(1.5f);
    return ESP_OK;
}

// Phones decide whether a network needs a login by fetching a known URL and
// checking the answer (Apple: captive.apple.com/hotspot-detect.html, Android:
// connectivitycheck.gstatic.com/generate_204, Windows: msftconnecttest.com).
// A plain 200 with our HTML looks like "this network is fine", so the phone
// never offers the page. With DNS answering everything with our address, those
// probes land here: anything that is not our own page gets a redirect, which is
// what makes the system pop its "sign in" sheet.
esp_err_t CatchAllHandler(httpd_req_t* request) {
    // Every unmatched path is a captive-portal probe; send it to the page.
    Gateway::Get().Touch();
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_send(request, "<html><body>Kitty</body></html>", 33);
    return ESP_OK;
}

}  // namespace

// A one-answer DNS server: every A query resolves to 192.168.4.1 so the system
// probes never fail with "no DNS" (which also means "network is fine").
namespace {

constexpr const char* kPortalIp = "192.168.4.1";

void DnsTask(void* arg) {
    auto* gateway = static_cast<Gateway*>(arg);
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(nullptr);
        return;
    }
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(53);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(socket_fd);
        vTaskDelete(nullptr);
        return;
    }
    const uint8_t answer[4] = {192, 168, 4, 1};
    uint8_t buffer[512];
    while (gateway->dns_running()) {
        sockaddr_in from = {};
        socklen_t from_length = sizeof(from);
        const int received = recvfrom(socket_fd, buffer, sizeof(buffer), 0,
                                       reinterpret_cast<sockaddr*>(&from), &from_length);
        if (received < 12) {
            continue;
        }
        // Walk the question name to find where it ends.
        int offset = 12;
        while (offset < received && buffer[offset] != 0) {
            offset += buffer[offset] + 1;
        }
        offset += 1;
        if (offset + 4 > received) {
            continue;
        }
        const uint16_t query_type = (buffer[offset] << 8) | buffer[offset + 1];
        int length = offset + 4;
        if (query_type != 1) {
            continue;  // only A records matter here
        }
        buffer[2] = 0x81;  // QR=1, RD=1
        buffer[3] = 0x80;  // RA=1, rcode=0
        buffer[6] = 0;
        buffer[7] = 1;     // answer count
        buffer[8] = 0;
        buffer[9] = 0;
        buffer[10] = 0;
        buffer[11] = 0;
        buffer[length++] = 0xC0;
        buffer[length++] = 0x0C;  // pointer to the question name
        buffer[length++] = 0x00;
        buffer[length++] = 0x01;  // type A
        buffer[length++] = 0x00;
        buffer[length++] = 0x01;  // class IN
        buffer[length++] = 0x00;
        buffer[length++] = 0x00;
        buffer[length++] = 0x00;
        buffer[length++] = 0x3C;  // ttl 60
        buffer[length++] = 0x00;
        buffer[length++] = 0x04;  // rdlength
        memcpy(buffer + length, answer, 4);
        length += 4;
        sendto(socket_fd, buffer, length, 0, reinterpret_cast<sockaddr*>(&from), from_length);
    }
    close(socket_fd);
    vTaskDelete(nullptr);
}

}  // namespace

Gateway& Gateway::Get() {
    static Gateway instance;
    return instance;
}

const char* Gateway::password() const {
    return kApPassword;
}

bool Gateway::Start() {
    if (active_) {
        return true;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(err));
        return false;
    }
    if (ap_netif_ == nullptr) {
        ap_netif_ = esp_netif_create_default_wifi_ap();
    }

    // The driver is initialised once and never torn down: deinit while the
    // TCP/IP stack still owns the netif panicked and rebooted the board.
    if (!wifi_inited_) {
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&init);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
            return false;
        }
        wifi_inited_ = true;
    }

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32] = {};
    snprintf(ssid, sizeof(ssid), "Kitty-%02X%02X", mac[4], mac[5]);
    ssid_ = ssid;

    wifi_config_t config = {};
    strncpy(reinterpret_cast<char*>(config.ap.ssid), ssid, sizeof(config.ap.ssid) - 1);
    config.ap.ssid_len = strlen(ssid);
    strncpy(reinterpret_cast<char*>(config.ap.password), kApPassword, sizeof(config.ap.password) - 1);
    config.ap.channel = 6;
    config.ap.max_connection = 2;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &config);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return false;
    }

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = 80;
    http_config.lru_purge_enable = true;
    // The captive-portal probes arrive on arbitrary paths; without this the
    // catch-all below is registered but never matched (the default is exact
    // string comparison, which is why every probe got a 404).
    http_config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &http_config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        esp_wifi_stop();
        return false;
    }
    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = RootHandler;
    root.user_ctx = this;
    httpd_register_uri_handler(server, &root);

    httpd_uri_t save = {};
    save.uri = "/save";
    save.method = HTTP_POST;
    save.handler = SaveHandler;
    save.user_ctx = this;
    httpd_register_uri_handler(server, &save);

    httpd_uri_t catch_all = {};
    catch_all.uri = "/*";
    catch_all.method = HTTP_GET;
    catch_all.handler = CatchAllHandler;
    catch_all.user_ctx = this;
    httpd_register_uri_handler(server, &catch_all);

    esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                               &Gateway::OnStationJoined, this);

    server_ = server;
    dns_running_ = true;
    xTaskCreate(DnsTask, "pet_dns", 4096, this, 2, (TaskHandle_t*)&dns_task_);
    active_ = true;
    idle_ = kIdleTimeout;
    ESP_LOGI(TAG, "Configuration AP up: %s / %s, http://192.168.4.1/", ssid_.c_str(), kApPassword);
    return true;
}

void Gateway::Stop() {
    if (!active_) {
        return;
    }
    if (server_ != nullptr) {
        httpd_stop(static_cast<httpd_handle_t>(server_));
        server_ = nullptr;
    }
    dns_running_ = false;
    dns_task_ = nullptr;
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                 &Gateway::OnStationJoined);
    esp_wifi_stop();
    active_ = false;
    ESP_LOGI(TAG, "Configuration AP down");
}

void Gateway::Tick(float dt) {
    if (!active_) {
        return;
    }
    if (stop_in_ > 0.0f) {
        stop_in_ -= dt;
        if (stop_in_ <= 0.0f) {
            Stop();
        }
        return;
    }
    idle_ -= dt;
    if (idle_ <= 0.0f) {
        ESP_LOGI(TAG, "Nobody configured anything for three minutes");
        Stop();
    }
}

void Gateway::OnStationJoined(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)base;
    (void)id;
    (void)data;
    auto* gateway = static_cast<Gateway*>(arg);
    if (gateway != nullptr) {
        gateway->Touch();
    }
    ESP_LOGI(TAG, "A phone joined the hotspot; the three minute timer restarts");
}

void Gateway::Touch() {
    idle_ = kIdleTimeout;
}

void Gateway::ScheduleStop(float seconds) {
    stop_in_ = seconds;
}

void Gateway::ApplyForm(const std::string& body) {
    auto& core = PetCore::Get();

    const std::string name = FormField(body, "name");
    if (!name.empty()) {
        core.SetName(name);
    }

    const std::string volume = FormField(body, "volume");
    if (!volume.empty()) {
        PetAudio::Get().SetVolume(atoi(volume.c_str()));
    }

    const std::string brightness = FormField(body, "brightness");
    if (!brightness.empty()) {
        if (auto* backlight = Board::GetInstance().GetBacklight()) {
            backlight->SetBrightness(static_cast<uint8_t>(atoi(brightness.c_str())), true);
        }
    }

    const std::string difficulty = FormField(body, "difficulty");
    if (!difficulty.empty()) {
        core.SetDifficulty(atoi(difficulty.c_str()));
    }

    const std::string personality = FormField(body, "personality");
    if (!personality.empty()) {
        core.SetPersonality(atoi(personality.c_str()));
    }

    const int hours_start = ParseMinutes(FormField(body, "sleep_start"), core.sleep_start_minute());
    const int hours_end = ParseMinutes(FormField(body, "sleep_end"), core.sleep_end_minute());
    core.SetSleepWindow(hours_start, hours_end);

    const std::string wake_phrase = FormField(body, "wake_phrase");
    if (!wake_phrase.empty() && wake_phrase != Ears::Get().phrase()) {
        Ears::Get().Stop();
        Ears::Get().SetPhrase(wake_phrase);
        Ears::Get().Start();
    }

    const std::string sensitivity = FormField(body, "wake_sensitivity");
    if (!sensitivity.empty()) {
        Ears::Get().SetSensitivity(atoi(sensitivity.c_str()));
    }

    const std::string clock = FormField(body, "clock");
    if (!clock.empty()) {
        int64_t epoch = 0;
        if (ParseLocalTime(clock, epoch, core.timezone())) {
            core.SetEpoch(epoch);
        } else {
            ESP_LOGW(TAG, "Could not parse the time field '%s'", clock.c_str());
        }
    }
    ESP_LOGI(TAG, "Settings saved");
}

int Gateway::ParseMinutes(const std::string& text, int fallback) {
    int hour = 0;
    int minute = 0;
    if (text.empty() || sscanf(text.c_str(), "%d:%d", &hour, &minute) != 2) {
        return fallback;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return fallback;
    }
    return hour * 60 + minute;
}

}  // namespace kitty
