#include "pet/agent_link.h"

#include <cstring>
#include <string>
#include <vector>

#include <esp_heap_caps.h>

#include <driver/usb_serial_jtag.h>
#include <mbedtls/base64.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>

#include "pet/pet_action.h"
#include "pet/pet_audio.h"
#include "pet/pet_core.h"
#include "pet/pet_ears.h"
#include "pet/pet_gateway.h"

namespace kitty {
namespace {

constexpr const char* TAG = "PetLink";
constexpr float kHeartbeatTimeout = 3.0f;
constexpr float kFaceHold = 4.0f;
constexpr const char* kFirmware = "kitty-pet 0.2.0";

const char* const kCapabilities[] = {
    "pet.face", "pet.look", "pet.attention", "pet.status", "pet.feed",
    "pet.clean", "pet.play", "pet.task", "pet.sleep", "pet.wake", "pet.night",
    "pet.time", "pet.config", "pet.volume", "pet.say", "pet.ping",
};

// The art has no emotion faces, so a requested emotion picks the closest
// motion row (docs/pet-design.md §16).
Clip ClipForEmotion(const char* emotion) {
    if (emotion == nullptr) {
        return Clip::kIdle;
    }
    const std::string e = emotion;
    if (e == "happy" || e == "laughing" || e == "funny" || e == "loving" ||
        e == "kissy" || e == "delicious" || e == "confident" || e == "cool" ||
        e == "relaxed" || e == "surprised" || e == "shocked") {
        return Clip::kJump;
    }
    if (e == "sad" || e == "crying" || e == "sleepy" || e == "embarrassed" ||
        e == "angry" || e == "tired") {
        return Clip::kFail;
    }
    if (e == "thinking" || e == "confused" || e == "curious") {
        return Clip::kReview;
    }
    return Clip::kIdle;
}

const char* GestureName(Gesture gesture) {
    switch (gesture) {
        case Gesture::kBootClick: return "boot_click";
        case Gesture::kBootDoubleClick: return "boot_double_click";
        case Gesture::kBootLongPress: return "boot_long_press";
        case Gesture::kPowerClick: return "power_click";
        case Gesture::kPowerLongPress: return "power_long_press";
    }
    return "unknown";
}

// The same driver the console logs go through, so a line is never cut in half
// by another writer.
void WriteAll(const char* data, size_t length) {
    while (length > 0) {
        const int written = usb_serial_jtag_write_bytes(data, length, pdMS_TO_TICKS(100));
        if (written <= 0) {
            return;
        }
        data += written;
        length -= static_cast<size_t>(written);
    }
}

cJSON* StatusJson(const Status& status) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "hunger", status.hunger);
    cJSON_AddNumberToObject(json, "energy", status.energy);
    cJSON_AddNumberToObject(json, "clean", status.clean);
    cJSON_AddNumberToObject(json, "affection", status.affection);
    cJSON_AddNumberToObject(json, "interactions", status.interactions);
    cJSON_AddStringToObject(json, "mood", ToName(status.mood));
    cJSON_AddStringToObject(json, "stage", ToName(status.stage));
    cJSON_AddStringToObject(json, "mode", ToName(status.mode));
    cJSON_AddBoolToObject(json, "sleeping", status.sleeping);
    cJSON_AddBoolToObject(json, "night", status.night);
    cJSON_AddNumberToObject(json, "calls", status.calls);
    return json;
}

}  // namespace

AgentLink& AgentLink::Get() {
    static AgentLink instance;
    return instance;
}

void AgentLink::Start(Action* action) {
    action_ = action;

    // stdin is the UART because the USB port is only the *secondary* console, so
    // reading the cable means installing the USB-Serial-JTAG driver ourselves.
    // Without a reader the host's writes sit in the 64 byte FIFO and block.
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        config.rx_buffer_size = 1024;
        config.tx_buffer_size = 1024;
        const esp_err_t err = usb_serial_jtag_driver_install(&config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not install the USB driver: %s", esp_err_to_name(err));
            return;
        }
    }
    usb_serial_jtag_vfs_use_driver();
    // One pet.say line can be ~100 KB of base64; PSRAM keeps that out of
    // internal RAM.
    // PSRAM is nearly full on this board (LVGL pool + image cache + frames +
    // the command model), so try a big enough buffer for one streamed chunk and
    // fall back to internal RAM.
    rx_capacity_ = 16 * 1024;
    rx_ = static_cast<char*>(heap_caps_malloc(rx_capacity_, MALLOC_CAP_SPIRAM));
    if (rx_ == nullptr) {
        rx_ = static_cast<char*>(heap_caps_malloc(rx_capacity_, MALLOC_CAP_8BIT));
    }
    if (rx_ == nullptr) {
        ESP_LOGE(TAG, "No memory for the line buffer");
        return;
    }
    ESP_LOGI(TAG, "Listening on the USB serial port for a host (line buffer %u)",
             static_cast<unsigned>(rx_capacity_));
    xTaskCreate(ReaderEntry, "pet_link", 4096, this, 6, (TaskHandle_t*)&reader_);
}

bool AgentLink::OwnsFace() const {
    return remote_ && face_hold_ > 0.0f;
}

bool AgentLink::TakeHello() {
    const bool pending = hello_pending_;
    hello_pending_ = false;
    return pending;
}

void AgentLink::Tick(float dt) {
    if (action_ == nullptr) {
        return;
    }
    since_rx_ += dt;
    if (face_hold_ > 0.0f) {
        face_hold_ -= dt;
    }

    std::string line;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(lines_mutex_);
            if (lines_.empty()) {
                break;
            }
            line = std::move(lines_.front());
            lines_.pop_front();
        }
        HandleLine(line.data());
    }

    if (remote_ && since_rx_ > kHeartbeatTimeout) {
        ExitRemote("heartbeat timeout");
    }
}

void AgentLink::ReaderEntry(void* arg) {
    static_cast<AgentLink*>(arg)->ReadLoop();
}

void AgentLink::ReadLoop() {
    char chunk[256];
    while (true) {
        const int received = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), 0);
        if (received <= 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        for (int i = 0; i < received; i++) {
            const char c = chunk[i];
            if (c == '\n' || c == '\r') {
                if (rx_len_ > 0) {
                    rx_[rx_len_] = '\0';
                    std::lock_guard<std::mutex> lock(lines_mutex_);
                    if (lines_.size() >= 2) {
                        lines_.pop_front();  // a slow body loop must not grow this
                    }
                    lines_.emplace_back(rx_);
                    rx_len_ = 0;
                }
            } else if (rx_len_ + 1 < rx_capacity_) {
                rx_[rx_len_++] = c;
            } else {
                rx_len_ = 0;  // oversized line: drop it and resynchronise
            }
        }
    }
}

void AgentLink::HandleLine(char* line) {
    // The port also carries the log output, so anything that is not JSON is
    // simply not ours.
    cJSON* root = cJSON_Parse(line);
    if (root == nullptr) {
        return;
    }
    since_rx_ = 0.0f;
    const cJSON* method = cJSON_GetObjectItem(root, "method");
    if (!cJSON_IsString(method) || method->valuestring == nullptr) {
        cJSON_Delete(root);
        return;
    }

    const cJSON* id = cJSON_GetObjectItem(root, "id");
    const std::string name = method->valuestring;
    const cJSON* params = cJSON_GetObjectItem(root, "params");
    auto& core = PetCore::Get();

    const auto reply = [&](cJSON* result) {
        if (id == nullptr) {
            cJSON_Delete(result);
            return;
        }
        cJSON* document = cJSON_CreateObject();
        cJSON_AddStringToObject(document, "jsonrpc", "2.0");
        cJSON_AddItemToObject(document, "id", cJSON_Duplicate(id, true));
        cJSON_AddItemToObject(document, "result", result);
        SendDocument(document);
    };
    const auto fail = [&](int code, const char* message) {
        if (id == nullptr) {
            return;
        }
        cJSON* document = cJSON_CreateObject();
        cJSON_AddStringToObject(document, "jsonrpc", "2.0");
        cJSON_AddItemToObject(document, "id", cJSON_Duplicate(id, true));
        cJSON* error = cJSON_CreateObject();
        cJSON_AddNumberToObject(error, "code", code);
        cJSON_AddStringToObject(error, "message", message);
        cJSON_AddItemToObject(document, "error", error);
        SendDocument(document);
    };
    const auto ok = [&]() {
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", true);
        reply(result);
    };

    if (name == "pet.hello") {
        remote_ = true;
        hello_pending_ = true;
        face_hold_ = 0.0f;
        core.SetMode(Mode::kRemote);
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddStringToObject(result, "name", core.name().c_str());
        cJSON_AddStringToObject(result, "firmware", kFirmware);
        cJSON* caps = cJSON_CreateArray();
        for (const char* cap : kCapabilities) {
            cJSON_AddItemToArray(caps, cJSON_CreateString(cap));
        }
        cJSON_AddItemToObject(result, "capabilities", caps);
        cJSON_AddItemToObject(result, "status", StatusJson(core.status()));
        reply(result);
        ESP_LOGI(TAG, "Host took over");
        return;
    }

    if (name == "pet.ping") {
        ok();
        return;
    }

    if (name == "pet.status") {
        reply(StatusJson(core.status()));
        return;
    }

    // At night she only takes part in the bare minimum: see her status, wake her,
    // end the night. Everything else is refused (docs/pet-design.md §10).
    if (core.night() &&
        (name == "pet.face" || name == "pet.look" || name == "pet.attention" ||
         name == "pet.say" || name == "pet.feed" || name == "pet.play" ||
         name == "pet.clean" || name == "pet.task")) {
        fail(-32001, "she is asleep; only pet.wake, pet.status, pet.ping and pet.night answer at night");
        return;
    }

    if (name == "pet.night") {
        const cJSON* on = cJSON_GetObjectItem(params, "on");
        const bool night = cJSON_IsBool(on) ? cJSON_IsTrue(on) : true;
        core.SetNight(night);
        reply(StatusJson(core.status()));
        return;
    }

    if (name == "pet.config") {
        // Not in the design's primitive list: it exists so a host can open the
        // same page the BOOT long press opens, which is also how it gets tested.
        const cJSON* on = cJSON_GetObjectItem(params, "on");
        const bool open = cJSON_IsBool(on) ? cJSON_IsTrue(on) : true;
        cJSON* result = cJSON_CreateObject();
        if (open) {
            const bool started = Gateway::Get().Start();
            cJSON_AddBoolToObject(result, "ok", started);
            if (started) {
                cJSON_AddStringToObject(result, "ssid", Gateway::Get().ssid().c_str());
                cJSON_AddStringToObject(result, "url", "http://192.168.4.1/");
            }
        } else {
            Gateway::Get().Stop();
            cJSON_AddBoolToObject(result, "ok", true);
        }
        reply(result);
        return;
    }

    if (name == "pet.time") {
        const cJSON* epoch = cJSON_GetObjectItem(params, "epoch");
        if (!cJSON_IsNumber(epoch)) {
            fail(-32602, "epoch (seconds since 1970) is required");
            return;
        }
        const cJSON* tz = cJSON_GetObjectItem(params, "tz_offset");
        if (cJSON_IsNumber(tz)) {
            core.SetTimezone(static_cast<int>(tz->valuedouble));
        }
        core.SetEpoch(static_cast<int64_t>(epoch->valuedouble));
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddNumberToObject(result, "minute_of_day", core.MinuteOfDay());
        cJSON_AddNumberToObject(result, "sleep_start_minute", core.sleep_start_minute());
        cJSON_AddNumberToObject(result, "sleep_end_minute", core.sleep_end_minute());
        cJSON_AddNumberToObject(result, "tz_offset", core.timezone());
        reply(result);
        return;
    }

    if (name == "pet.volume") {
        const cJSON* value = cJSON_GetObjectItem(params, "value");
        if (!cJSON_IsNumber(value)) {
            fail(-32602, "value (0-100) is required");
            return;
        }
        PetAudio::Get().SetVolume(static_cast<int>(value->valuedouble));
        cJSON* result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "volume", PetAudio::Get().volume());
        reply(result);
        return;
    }

    if (name == "pet.face") {
        const cJSON* emotion = cJSON_GetObjectItem(params, "emotion");
        const cJSON* intensity = cJSON_GetObjectItem(params, "intensity");
        const float strength = cJSON_IsNumber(intensity)
                                   ? static_cast<float>(intensity->valuedouble)
                                   : 0.6f;
        ApplyFace(cJSON_IsString(emotion) ? emotion->valuestring : "neutral", strength);
        ok();
        return;
    }

    if (name == "pet.look") {
        const cJSON* dx = cJSON_GetObjectItem(params, "dx");
        const cJSON* dy = cJSON_GetObjectItem(params, "dy");
        // dx/dy are -100..100, mapped onto the few pixels the face can move.
        const int x = cJSON_IsNumber(dx) ? static_cast<int>(dx->valuedouble) : 0;
        const int y = cJSON_IsNumber(dy) ? static_cast<int>(dy->valuedouble) : 0;
        look_dx_ = x / 12;
        look_dy_ = y / 12;
        action_->SetLook(look_dx_, look_dy_);
        face_hold_ = kFaceHold;
        ok();
        return;
    }

    if (name == "pet.attention") {
        const cJSON* on = cJSON_GetObjectItem(params, "on");
        const bool attention = cJSON_IsBool(on) ? cJSON_IsTrue(on) : true;
        if (attention) {
            action_->Play(Clip::kWave, 110);
            look_dy_ = -3;
            action_->SetLook(look_dx_, look_dy_);
        } else {
            look_dy_ = 0;
            action_->SetLook(look_dx_, look_dy_);
        }
        face_hold_ = kFaceHold;
        ok();
        return;
    }

    if (name == "pet.feed") {
        core.Feed(true);
        PetAudio::Get().Play(Voice::kChew);
        reply(StatusJson(core.status()));
        return;
    }
    if (name == "pet.clean") {
        core.Clean(true);
        PetAudio::Get().Play(Voice::kPurr);
        reply(StatusJson(core.status()));
        return;
    }
    if (name == "pet.play") {
        core.Play(true);
        PetAudio::Get().Play(Voice::kHappy);
        reply(StatusJson(core.status()));
        return;
    }
    if (name == "pet.sleep") {
        core.ForceSleep();
        ok();
        return;
    }
    if (name == "pet.wake") {
        // Waking her up also ends the night: the two are the same thing here.
        core.SetNight(false);
        core.ForceWake();
        ok();
        return;
    }
    if (name == "pet.task") {
        // docs/pet-design.md §6: every finished task costs her energy, and a
        // failed one costs more. This is the leash on the agent.
        const cJSON* result = cJSON_GetObjectItem(params, "result");
        const std::string outcome = cJSON_IsString(result) ? result->valuestring : "done";
        if (outcome == "error" || outcome == "failed" || outcome == "interrupted") {
            core.AgentTaskFailed();
            ESP_LOGI(TAG, "Host task failed");
        } else {
            core.AgentTaskDone();
            ESP_LOGI(TAG, "Host finished a task");
        }
        reply(StatusJson(core.status()));
        return;
    }

    if (name == "pet.say") {
        // Base64 mono PCM16 at 16 kHz. The host owns the resampling.
        const cJSON* pcm = cJSON_GetObjectItem(params, "pcm");
        if (!cJSON_IsString(pcm)) {
            fail(-32602, "pcm (base64 mono pcm16 at 16 kHz) is required");
            return;
        }
        const size_t encoded_len = strlen(pcm->valuestring);
        // One chunk per call; the host streams a sentence in pieces.
        if (encoded_len == 0 || encoded_len > 12 * 1024) {
            fail(-32602, "pcm must be at most 12 KB of base64 per call (about 0.3 s)");
            return;
        }
        std::vector<unsigned char> raw(encoded_len);
        size_t decoded_len = 0;
        if (mbedtls_base64_decode(raw.data(), raw.size(), &decoded_len,
                                  reinterpret_cast<const unsigned char*>(pcm->valuestring),
                                  encoded_len) != 0 ||
            decoded_len < 2) {
            fail(-32602, "pcm is not valid base64");
            return;
        }
        const size_t count = decoded_len / 2;
        const cJSON* first = cJSON_GetObjectItem(params, "first");
        const cJSON* last = cJSON_GetObjectItem(params, "last");
        if (say_buffer_ == nullptr) {
            // PSRAM is tight on this board (LVGL pool, image cache, frames and
            // the command model), so take what is still there.
            const size_t sizes[] = {4 * 16000, 2 * 16000, 16000};
            for (size_t samples : sizes) {
                say_buffer_ = static_cast<int16_t*>(
                    heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
                if (say_buffer_ != nullptr) {
                    say_capacity_ = samples;
                    break;
                }
            }
            if (say_buffer_ == nullptr) {
                fail(-32603, "no memory for speech");
                return;
            }
            ESP_LOGI(TAG, "Speech buffer: %u samples", static_cast<unsigned>(say_capacity_));
        }
        if (cJSON_IsBool(first) && cJSON_IsTrue(first)) {
            say_length_ = 0;
        }
        if (say_length_ + count > say_capacity_) {
            say_length_ = 0;
            fail(-32602, "speech longer than the buffer");
            return;
        }
        memcpy(say_buffer_ + say_length_, raw.data(), count * sizeof(int16_t));
        say_length_ += count;

        const bool finish = !cJSON_IsBool(last) || cJSON_IsTrue(last);
        size_t played = 0;
        const cJSON* test = cJSON_GetObjectItem(params, "test");
        if (finish && cJSON_IsBool(test) && cJSON_IsTrue(test)) {
            const cJSON* phrase = cJSON_GetObjectItem(params, "phrase");
            if (cJSON_IsString(phrase)) {
                Ears::Get().UsePhrase(phrase->valuestring);
            }
            // Feed it to the command model instead of the speaker.
            const std::string heard = Ears::Get().Recognize(say_buffer_, say_length_);
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "ok", true);
            cJSON_AddStringToObject(result, "heard", heard.c_str());
            cJSON_AddNumberToObject(result, "samples", static_cast<double>(say_length_));
            say_length_ = 0;
            reply(result);
            return;
        }
        if (finish) {
            std::vector<int16_t> samples(say_buffer_, say_buffer_ + say_length_);
            played = samples.size();
            PetAudio::Get().PlayPcm(std::move(samples));
            say_length_ = 0;
        }
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddNumberToObject(result, "chunk", static_cast<double>(count));
        cJSON_AddNumberToObject(result, "played", static_cast<double>(played));
        reply(result);
        return;
    }

    fail(-32601, "method not found");
}

void AgentLink::ApplyFace(const char* emotion, float intensity) {
    if (intensity < 0.0f) {
        intensity = 0.0f;
    }
    if (intensity > 1.0f) {
        intensity = 1.0f;
    }
    const int frame_ms = 170 - static_cast<int>(80.0f * intensity);
    action_->Play(ClipForEmotion(emotion), frame_ms);
    face_hold_ = kFaceHold;
}

void AgentLink::SendDocument(cJSON* document) {
    char* text = cJSON_PrintUnformatted(document);
    cJSON_Delete(document);
    if (text == nullptr) {
        return;
    }
    WriteAll(text, strlen(text));
    WriteAll("\n", 1);
    cJSON_free(text);
}

void AgentLink::SendEvent(const char* method, cJSON* params) {
    if (!remote_) {
        cJSON_Delete(params);
        return;
    }
    cJSON* document = cJSON_CreateObject();
    cJSON_AddStringToObject(document, "jsonrpc", "2.0");
    cJSON_AddStringToObject(document, "method", method);
    if (params != nullptr) {
        cJSON_AddItemToObject(document, "params", params);
    }
    SendDocument(document);
}

void AgentLink::ExitRemote(const char* reason) {
    ESP_LOGI(TAG, "Back on her own: %s", reason);
    remote_ = false;
    face_hold_ = 0.0f;
    look_dx_ = 0;
    look_dy_ = 0;
    if (action_ != nullptr) {
        action_->SetLook(0, 0);
    }
}

void AgentLink::OnGesture(Gesture gesture, const Status& status) {
    if (!remote_) {
        return;
    }
    (void)status;
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "source", GestureName(gesture));
    cJSON_AddNumberToObject(params, "count", 1);
    SendEvent("pet.poke", params);
}

void AgentLink::OnCalled() {
    if (!remote_) {
        return;
    }
    SendEvent("pet.called", cJSON_CreateObject());
}

void AgentLink::OnStatusChanged(const Status& status) {
    if (!remote_) {
        return;
    }
    SendEvent("pet.state_changed", StatusJson(status));

    if (status.energy < 15 && !tired_reported_) {
        tired_reported_ = true;
        cJSON* params = cJSON_CreateObject();
        cJSON_AddNumberToObject(params, "energy", status.energy);
        SendEvent("pet.tired", params);
    } else if (status.energy > 30) {
        tired_reported_ = false;
    }
}

}  // namespace kitty
