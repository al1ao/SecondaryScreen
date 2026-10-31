#include "input/input_injector.h"
#include <algorithm>
namespace pds {
bool MapTouchToDesktop(const TouchMapping& m, uint32_t x, uint32_t y, int32_t* dx, int32_t* dy) {
    if (!dx || !dy || x > 1000000 || y > 1000000 || m.desktop_w <= 0 || m.desktop_h <= 0 ||
        m.monitor.right <= m.monitor.left || m.monitor.bottom <= m.monitor.top) return false;
    const auto px = static_cast<int64_t>(m.monitor.left) +
        (static_cast<int64_t>(x) * (m.monitor.right - m.monitor.left - 1) + 500000) / 1000000;
    const auto py = static_cast<int64_t>(m.monitor.top) +
        (static_cast<int64_t>(y) * (m.monitor.bottom - m.monitor.top - 1) + 500000) / 1000000;
    *dx = static_cast<int32_t>(std::clamp<int64_t>((px - m.desktop_x) * 65535 /
        std::max<int64_t>(1, m.desktop_w - 1), 0, 65535));
    *dy = static_cast<int32_t>(std::clamp<int64_t>((py - m.desktop_y) * 65535 /
        std::max<int64_t>(1, m.desktop_h - 1), 0, 65535));
    return true;
}
void InputInjector::UpdateTarget(const RECT& rect) {
    Release();
    mapping_ = {rect, GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}
void InputInjector::Release() {
    if (!down_) return;
    INPUT event{}; event.type = INPUT_MOUSE; event.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &event, sizeof(event)); down_ = false;
}
void InputInjector::OnTouch(const TouchInput& input) {
    if (input.pointer != 0 || input.action > 3) return;
    int32_t x = 0, y = 0;
    if (!MapTouchToDesktop(mapping_, input.x_ppm, input.y_ppm, &x, &y)) return;
    if (input.action == 3) { Release(); return; }
    if (input.action == 0 && down_) Release();
    if (input.action == 1 && !down_) return;
    INPUT event{}; event.type = INPUT_MOUSE; event.mi.dx = x; event.mi.dy = y;
    event.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    if (input.action == 0) event.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
    if (input.action == 2 && down_) event.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
    SendInput(1, &event, sizeof(event));
    down_ = input.action != 2;
}
}
