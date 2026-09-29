#pragma once
#include "scene.h"
#include <cstdint>

/**
 * Power Off scene.
 *
 * Shows the embedded power-off logo (assets/swofflogo.jpg, decoded on the fly
 * to the 16-gray palette) fullscreen. The E-ink panel keeps the image after the
 * device loses power — the scene doubles as the "cover" of the powered-off
 * device.
 *
 * Win32: any click / ESC closes the scene and returns to the caller.
 * ESP32:  the scene waits kOffDelayMs after entering (letting the EPD fully
 * refresh) and then powers the device off hardware-wise via
 * Platform::power_off() (OFF:G44 pulses to the PMS150G, per M5Unified).
 */
class PowerOffScene : public Scene {
public:
    static const int kOffDelayMs = 2000;

    const char* name() const override { return "poweroff"; }
    ~PowerOffScene() override;
    void on_enter() override;
    void on_exit() override;
    void update() override;
    void on_mouse_down(int x, int y, int button) override;
    void on_key_down(int key) override;
    void render(uint8_t* fb, int fb_w, int fb_h) override;
    // Decode the power-off logo into PSRAM at boot (heap is large there;
    // power-off can follow heavy reading when only ~0.5MB is left).
    static void warmup();
private:
    bool decode();
    void free_image();

    uint8_t* img_ = nullptr;   // palette indices, row-major WxH (rotated 90° CW)
    int      img_w_ = 0;
    int      img_h_ = 0;
    bool     decoded_ = false;
    bool     cached_ = false;  // img_ points into the shared boot cache
    bool     time_base_set_ = false;
    uint32_t enter_ms_ = 0;
    bool     powering_off_ = false;
    bool     presented_ = false; // first frame actually presented
};