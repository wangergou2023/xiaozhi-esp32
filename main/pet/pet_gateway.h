#ifndef KITTY_PET_GATEWAY_H_
#define KITTY_PET_GATEWAY_H_

#include <string>

namespace kitty {

// The configuration doorway from docs/pet-design.md §11: a short-lived SoftAP
// with one page on 192.168.4.1. WiFi is only a radio for editing settings --
// she still has no way to reach the internet.
//
// Long-press BOOT to open it. Three minutes later, or as soon as the form is
// saved, the radio goes off again.
class Gateway {
public:
    static Gateway& Get();

    bool Start();
    void Stop();
    bool active() const { return active_; }

    // Counts down the idle timeout while the AP is up.
    void Tick(float dt);

    const std::string& ssid() const { return ssid_; }
    bool dns_running() const { return dns_running_; }

    // Called by the HTTP handlers.
    void ApplyForm(const std::string& body);
    // Any request counts as activity; the three minute timer restarts.
    void Touch();
    void ScheduleStop(float seconds);

private:
    Gateway() = default;
    static int ParseMinutes(const std::string& text, int fallback);
    void* server_ = nullptr;    // httpd_handle_t
    void* dns_task_ = nullptr;  // TaskHandle_t
    volatile bool dns_running_ = false;
    void* ap_netif_ = nullptr;  // esp_netif_t*, created once and kept
    bool wifi_inited_ = false;
    bool active_ = false;
    float idle_ = 0.0f;
    float stop_in_ = 0.0f;
    std::string ssid_;
};

}  // namespace kitty

#endif  // KITTY_PET_GATEWAY_H_
