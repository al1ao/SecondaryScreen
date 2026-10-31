#pragma once
#include <windows.h>
#include <cstdint>
#include "protocol/protocol.h"
namespace pds {
struct TouchMapping {
    RECT monitor{};
    int32_t desktop_x = 0, desktop_y = 0, desktop_w = 0, desktop_h = 0;
};
bool MapTouchToDesktop(const TouchMapping& mapping, uint32_t x_ppm, uint32_t y_ppm,
                       int32_t* out_dx, int32_t* out_dy);
class InputInjector {
public:
    void UpdateTarget(const RECT& monitor);
    void OnTouch(const TouchInput& input);
    void Release();
private:
    bool down_ = false;
    TouchMapping mapping_{};
};
}
