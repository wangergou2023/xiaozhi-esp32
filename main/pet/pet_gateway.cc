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

#include "board.h"
#include "pet/pet_audio.h"
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

std::string Page() {
    auto& core = PetCore::Get();
    const int start = core.sleep_start_minute();
    const int end = core.sleep_end_minute();
    char sleep_start[16] = {};
    char sleep_end[16] = {};
    snprintf(sleep_start, sizeof(sleep_start), "%02d:%02d", start / 60, start % 60);
    snprintf(sleep_end, sizeof(sleep_end), "%02d:%02d", end / 60, end % 60);

    const int volume = PetAudio::Get().volume();
    const int difficulty = core.difficulty();
    int brightness = 75;
    if (auto* backlight = Board::GetInstance().GetBacklight()) {
        brightness = backlight->brightness();
    }

    std::string html;
    html.reserve(4096);
    html +=
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Kitty</title><style>"
        "body{background:#141414;color:#eee;font-family:system-ui;margin:0;padding:24px}"
        "h1{font-size:20px;font-weight:600;margin:0 0 4px}"
        "p.hint{color:#888;font-size:13px;margin:0 0 20px}"
        "label{display:block;margin:16px 0 6px;font-size:14px;color:#bbb}"
        "input,select{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;"
        "border:1px solid #333;background:#1e1e1e;color:#eee;font-size:16px}"
        "button{margin-top:24px;width:100%;padding:14px;border:0;border-radius:10px;"
        "background:#ffb26b;color:#1a1a1a;font-size:16px;font-weight:600}"
        "</style></head><body><h1>Kitty</h1>"
        "<p class=\"hint\">改完点保存，热点会自己关掉。</p><form method=\"post\" action=\"/save\">";

    html += "<label>名字</label><input name=\"name\" value=\"" + Escape(core.name()) + "\">";
    html += "<label>音量 " + std::to_string(volume) + "</label>"
            "<input name=\"volume\" type=\"range\" min=\"0\" max=\"100\" value=\"" +
            std::to_string(volume) + "\">";
    html += "<label>亮度</label><input name=\"brightness\" type=\"range\" min=\"5\" max=\"100\" value=\"" +
            std::to_string(brightness) + "\">";
    html += "<label>难度</label><select name=\"difficulty\">" +
            SelectOption(0, difficulty, "轻松") + SelectOption(1, difficulty, "正常") +
            SelectOption(2, difficulty, "硬核") + "</select>";
    html += "<label>性格</label><select name=\"personality\">" +
            SelectOption(0, core.personality(), "黏人") + SelectOption(1, core.personality(), "标准") +
            SelectOption(2, core.personality(), "冷淡") + SelectOption(3, core.personality(), "活泼") +
            "</select>";
    html += "<label>作息：几点睡</label><input name=\"sleep_start\" type=\"time\" value=\"" +
            std::string(sleep_start) + "\">";
    html += "<label>作息：几点起</label><input name=\"sleep_end\" type=\"time\" value=\"" +
            std::string(sleep_end) + "\">";
    html += "<label>对时（留空就不改）</label>"
            "<input name=\"clock\" type=\"datetime-local\">";
    html += "<button type=\"submit\">保存</button></form></body></html>";
    return html;
}

std::string SavedPage() {
    return "<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Kitty</title><style>"
           "body{background:#141414;color:#eee;font-family:system-ui;padding:48px 24px;text-align:center}"
           "h1{font-size:20px;font-weight:600}"
           "</style></head><body><h1>已保存</h1>"
           "<p>热点马上关闭，她继续过日子。</p></body></html>";
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

}  // namespace

Gateway& Gateway::Get() {
    static Gateway instance;
    return instance;
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

    server_ = server;
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
