#include "glm_realtime.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include <cJSON.h>
#include <mbedtls/base64.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "board.h"
#include "application.h"
#include "audio_service.h"
#include "settings.h"
#include "system_info.h"
#include "web_socket.h"

#if __has_include("glm_key.h")
#include "glm_key.h"
#endif

#define TAG "GlmRealtime"

#define RT_URL "wss://open.bigmodel.cn/api/paas/v4/realtime"
#define IN_RATE 16000      // what we upload (pcm16)
#define OUT_RATE 24000     // what the model returns
#define CHUNK 320          // 20 ms of 16 kHz mono
#define MAX_RECORD_MS 8000
#define SILENCE_STOP_MS 900

static std::string Base64Encode(const uint8_t* data, size_t len) {
    size_t out_len = 0;
    mbedtls_base64_encode(nullptr, 0, &out_len, data, len);
    std::string out(out_len, '\0');
    if (mbedtls_base64_encode((unsigned char*)out.data(), out.size(), &out_len, data, len) != 0) {
        return {};
    }
    out.resize(out_len);
    return out;
}

static std::vector<uint8_t> Base64Decode(const std::string& s) {
    size_t out_len = 0;
    mbedtls_base64_decode(nullptr, 0, &out_len, (const unsigned char*)s.data(), s.size());
    std::vector<uint8_t> out(out_len);
    if (mbedtls_base64_decode(out.data(), out.size(), &out_len,
                              (const unsigned char*)s.data(), s.size()) != 0) {
        return {};
    }
    out.resize(out_len);
    return out;
}

// Cheap linear resampler: good enough for speech at prototype quality.
static std::vector<int16_t> Resample(const int16_t* in, size_t count, int src, int dst) {
    if (src == dst || count == 0) {
        return std::vector<int16_t>(in, in + count);
    }
    size_t out_count = (size_t)((uint64_t)count * dst / src);
    std::vector<int16_t> out(out_count);
    for (size_t i = 0; i < out_count; i++) {
        double pos = (double)i * src / dst;
        size_t i0 = (size_t)pos;
        size_t i1 = i0 + 1 < count ? i0 + 1 : i0;
        double frac = pos - (double)i0;
        out[i] = (int16_t)((1.0 - frac) * in[i0] + frac * in[i1]);
    }
    return out;
}

GlmRealtime::GlmRealtime() {
    Settings settings("glm", false);
    api_key_ = settings.GetString("api_key", "");
#ifdef GLM_API_KEY
    if (api_key_.empty()) {
        api_key_ = GLM_API_KEY;
    }
#endif
    if (api_key_.empty()) {
        ESP_LOGW(TAG, "No GLM API key configured");
    } else {
        ESP_LOGI(TAG, "GLM realtime ready (key %u chars)", (unsigned)api_key_.size());
    }
}

GlmRealtime::~GlmRealtime() { Close(); }

bool GlmRealtime::Connect() {
    if (websocket_ && websocket_->IsConnected()) {
        return true;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "No network interface");
        return false;
    }
    websocket_ = std::make_shared<WebSocket>(network, 1);
    websocket_->SetHeader("Authorization", ("Bearer " + api_key_).c_str());
    websocket_->SetReceiveBufferSize(16 * 1024);

    if (!websocket_->Connect(RT_URL)) {
        ESP_LOGE(TAG, "Connect failed");
        websocket_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "Connected to %s", RT_URL);
    return true;
}

void GlmRealtime::Close() {
    if (websocket_) {
        websocket_->Close();
        websocket_ = nullptr;
    }
}

bool GlmRealtime::StartTurn() {
    if (busy_) {
        ESP_LOGW(TAG, "Turn already running");
        return false;
    }
    if (api_key_.empty()) {
        return false;
    }
    busy_ = true;
    stop_requested_ = false;
    if (xTaskCreate(TurnTaskEntry, "glm_turn", 6 * 1024, this, 4, &task_) != pdPASS) {
        busy_ = false;
        return false;
    }
    return true;
}

void GlmRealtime::TurnTaskEntry(void* arg) {
    auto* self = static_cast<GlmRealtime*>(arg);
    self->TurnTask();
    self->task_ = nullptr;
    self->busy_ = false;
    vTaskDelete(nullptr);
}

void GlmRealtime::TurnTask() {
    auto& audio = Application::GetInstance().GetAudioService();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int out_rate = codec ? codec->output_sample_rate() : IN_RATE;

    if (!Connect()) {
        return;
    }

    // Receive queue: the websocket callback runs on its own task. It is held by
    // a shared_ptr so a frame that arrives after this turn returns can never
    // touch a destroyed mutex.
    struct Inbox {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::string> messages;
    };
    auto inbox = std::make_shared<Inbox>();
    websocket_->OnData([inbox](const char* data, size_t len, bool binary) {
        if (binary) {
            return;
        }
        std::lock_guard<std::mutex> lock(inbox->mutex);
        inbox->messages.emplace_back(data, len);
        inbox->cv.notify_all();
    });
    // Detach the callback on every exit path.
    struct Detach {
        WebSocket* ws;
        ~Detach() {
            if (ws != nullptr) {
                ws->OnData([](const char*, size_t, bool) {});
            }
        }
    } detach{websocket_.get()};

    auto send = [this](const std::string& s) { return websocket_->Send(s); };
    auto wait_event = [&](int timeout_ms) -> std::string {
        std::unique_lock<std::mutex> lock(inbox->mutex);
        if (!inbox->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [&]() { return !inbox->messages.empty(); })) {
            return {};
        }
        std::string msg = std::move(inbox->messages.front());
        inbox->messages.pop_front();
        return msg;
    };

    // 1) Session configuration. beta_fields.chat_mode is required by the server.
    {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "session.update");
        cJSON* session = cJSON_AddObjectToObject(root, "session");
        cJSON_AddStringToObject(session, "model", "glm-realtime");
        cJSON* modalities = cJSON_AddArrayToObject(session, "modalities");
        cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
        cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
        cJSON_AddStringToObject(session, "instructions",
                                "你是一只叫 Kitty 的桌面小宠物，说话简短、活泼、口语化，"
                                "每次回复不超过两句话。永远不要说自己是小智或其他助手。");
        cJSON_AddStringToObject(session, "voice", "tongtong");
        cJSON_AddStringToObject(session, "input_audio_format", "pcm16");
        cJSON_AddStringToObject(session, "output_audio_format", "pcm");
        cJSON* beta = cJSON_AddObjectToObject(session, "beta_fields");
        cJSON_AddStringToObject(beta, "chat_mode", "audio");
        char* text = cJSON_PrintUnformatted(root);
        send(text);
        cJSON_free(text);
        cJSON_Delete(root);
    }
    bool ready = false;
    for (int i = 0; i < 40 && !ready; i++) {
        std::string msg = wait_event(500);
        if (msg.empty()) {
            continue;
        }
        cJSON* root = cJSON_Parse(msg.c_str());
        if (root == nullptr) {
            continue;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (cJSON_IsString(type)) {
            if (strcmp(type->valuestring, "session.created") == 0 ||
                strcmp(type->valuestring, "session.updated") == 0) {
                ready = true;
            } else if (strcmp(type->valuestring, "error") == 0) {
                ESP_LOGE(TAG, "Session error: %s", msg.c_str());
            }
        }
        cJSON_Delete(root);
    }
    if (!ready) {
        ESP_LOGE(TAG, "Session was not accepted");
        return;
    }
    ESP_LOGI(TAG, "Session ready, recording...");

    // Take the codec away from AudioService for the duration of the turn: two
    // tasks reading the same I2S channel crashes the driver.
    audio.Stop();
    vTaskDelay(pdMS_TO_TICKS(30));

    // The codec is usually open for output only; cycling it forces the driver
    // to bring the I2S RX channel up before we read from the microphone.
    if (codec != nullptr) {
        codec->EnableInput(false);
        codec->EnableOutput(false);
        vTaskDelay(pdMS_TO_TICKS(20));
        codec->EnableInput(true);
        codec->EnableOutput(true);
    }

    // 2) Record until silence (or the hard cap).
    std::vector<int16_t> pending;   // audio waiting to be shipped up
    std::vector<int16_t> buf(CHUNK);
    bool speech_started = false;
    int silent_ms = 0;
    int total_ms = 0;
    int read_fail = 0;
    int chunk_fill = 0;
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)MAX_RECORD_MS * 1000;
    while (esp_timer_get_time() < deadline_us && !stop_requested_) {
        if (!audio.ReadAudioData(buf, IN_RATE, CHUNK)) {
            if (++read_fail % 50 == 0) {
                ESP_LOGW(TAG, "Mic read failed %d times", read_fail);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        pending.insert(pending.end(), buf.begin(), buf.end());
        total_ms += 20;

        // Ship audio while the user is still talking, so committing the turn
        // costs nothing at the end.
        if (++chunk_fill >= 5) {
            chunk_fill = 0;
            std::string b64 = Base64Encode((const uint8_t*)pending.data(),
                                           pending.size() * sizeof(int16_t));
            cJSON* up = cJSON_CreateObject();
            cJSON_AddStringToObject(up, "type", "input_audio_buffer.append");
            cJSON_AddStringToObject(up, "audio", b64.c_str());
            char* up_text = cJSON_PrintUnformatted(up);
            send(up_text);
            cJSON_free(up_text);
            cJSON_Delete(up);
            pending.clear();
        }

        double sum = 0;
        for (auto s : buf) {
            sum += (double)s * s;
        }
        double rms = std::sqrt(sum / buf.size());
        if (rms > 300) {
            speech_started = true;
            silent_ms = 0;
        } else if (speech_started) {
            silent_ms += 20;
        }
        if (speech_started && silent_ms > SILENCE_STOP_MS) {
            break;
        }
    }
    if (!pending.empty()) {
        std::string b64 = Base64Encode((const uint8_t*)pending.data(),
                                       pending.size() * sizeof(int16_t));
        cJSON* up = cJSON_CreateObject();
        cJSON_AddStringToObject(up, "type", "input_audio_buffer.append");
        cJSON_AddStringToObject(up, "audio", b64.c_str());
        char* up_text = cJSON_PrintUnformatted(up);
        send(up_text);
        cJSON_free(up_text);
        cJSON_Delete(up);
        pending.clear();
    }
    int record_ms = total_ms;
    ESP_LOGI(TAG, "Recorded %d ms (speech=%d)", record_ms, (int)speech_started);
    if (record_ms < 300) {
        ESP_LOGW(TAG, "Too short, aborting");
        if (codec != nullptr) {
            codec->EnableInput(false);
        }
        audio.Start();
        return;
    }

    send(R"({"type":"input_audio_buffer.commit"})");
    send(R"({"type":"response.create"})");
    ESP_LOGI(TAG, "Audio committed, waiting for the answer");

    // 4) Play the answer as it streams in.
    std::string tail;  // leftover bytes when a chunk splits a sample
    size_t audio_bytes = 0;
    std::string text_out;
    // Hold the first ~1.2 s before starting playback: the server delivers audio
    // in uneven bursts, and writing straight through makes the speech stutter.
    std::vector<int16_t> prebuf;
    bool playing = false;
    const size_t prebuf_target = (size_t)out_rate * 12 / 10;
    while (!stop_requested_) {
        std::string msg = wait_event(30000);
        if (msg.empty()) {
            break;
        }
        cJSON* root = cJSON_Parse(msg.c_str());
        if (root == nullptr) {
            continue;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        const char* t = cJSON_IsString(type) ? type->valuestring : "";
        if (strcmp(t, "response.audio.delta") == 0) {
            cJSON* delta = cJSON_GetObjectItem(root, "delta");
            if (cJSON_IsString(delta)) {
                auto raw = Base64Decode(delta->valuestring);
                audio_bytes += raw.size();
                // Keep sample alignment across chunks.
                std::vector<uint8_t> merged(tail.begin(), tail.end());
                merged.insert(merged.end(), raw.begin(), raw.end());
                size_t usable = merged.size() & ~(size_t)1;
                tail.assign((char*)merged.data() + usable, merged.size() - usable);
                if (usable > 0) {
                    auto pcm = Resample((const int16_t*)merged.data(), usable / 2, OUT_RATE, out_rate);
                    if (!playing) {
                        prebuf.insert(prebuf.end(), pcm.begin(), pcm.end());
                        if (prebuf.size() >= prebuf_target) {
                            playing = true;
                        }
                    }
                    if (playing && codec != nullptr) {
                        if (!prebuf.empty()) {
                            codec->OutputData(prebuf);
                            prebuf.clear();
                        } else {
                            // I2S write blocks while the DMA buffer is full, which
                            // paces playback in real time.
                            codec->OutputData(pcm);
                        }
                    }
                }
            }
        } else if (strcmp(t, "response.audio_transcript.delta") == 0 ||
                   strcmp(t, "response.text.delta") == 0) {
            cJSON* d = cJSON_GetObjectItem(root, "delta");
            if (cJSON_IsString(d)) {
                text_out += d->valuestring;
            }
        } else if (strcmp(t, "response.done") == 0) {
            cJSON_Delete(root);
            break;
        } else if (strcmp(t, "error") == 0) {
            ESP_LOGE(TAG, "Realtime error: %s", msg.c_str());
            cJSON_Delete(root);
            break;
        }
        cJSON_Delete(root);
    }
    ESP_LOGI(TAG, "Turn finished: %u bytes audio, text=\"%s\"", (unsigned)audio_bytes,
             text_out.c_str());

    // Hand the codec back to the application: mic off, speaker on, audio
    // service running again.
    if (codec != nullptr) {
        codec->EnableInput(false);
    }
    audio.Start();
}
