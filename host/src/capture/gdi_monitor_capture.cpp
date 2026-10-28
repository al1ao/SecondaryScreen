#include "capture/gdi_monitor_capture.h"
#include <algorithm>
#include <cstring>
#include "util/log.h"
#include "util/timer.h"
namespace pds {
namespace {
class GdiMonitorCapture final : public IDesktopCapture {
public:
    GdiMonitorCapture(MonitorInfo monitor, bool cursor) : monitor_(std::move(monitor)), cursor_(cursor) {}
    ~GdiMonitorCapture() override {
        if (previous_ && SelectObject(memory_, previous_) == HGDI_ERROR) LOG_HR(LastErrorHr(), "monitor restore bitmap");
        if (bitmap_ && !DeleteObject(bitmap_)) LOG_HR(LastErrorHr(), "monitor DeleteObject");
        if (memory_ && !DeleteDC(memory_)) LOG_HR(LastErrorHr(), "monitor DeleteDC memory");
        if (screen_ && ReleaseDC(nullptr, screen_) == 0) LOG_HR(LastErrorHr(), "monitor ReleaseDC");
    }
    bool Initialize(uint32_t width, uint32_t height) override {
        if (screen_ || width < 2 || height < 2 || width % 2 || height % 2) return false;
        width_ = width; height_ = height;
        screen_ = GetDC(nullptr);
        if (!screen_) return LOG_HR(LastErrorHr(), "monitor GetDC");
        memory_ = CreateCompatibleDC(screen_);
        if (!memory_) return LOG_HR(LastErrorHr(), "monitor CreateCompatibleDC");
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width_); info.bmiHeader.biHeight = -static_cast<LONG>(height_);
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(screen_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (!bitmap_) return LOG_HR(LastErrorHr(), "monitor CreateDIBSection");
        previous_ = SelectObject(memory_, bitmap_);
        if (!previous_ || previous_ == HGDI_ERROR) { previous_ = nullptr; return LOG_HR(LastErrorHr(), "monitor SelectObject"); }
        if (!SetStretchBltMode(memory_, COLORONCOLOR)) return LOG_HR(LastErrorHr(), "monitor stretch mode");
        return true;
    }
    bool Capture(CapturedFrame& out) override {
        MONITORINFOEXW current{}; current.cbSize = sizeof(current);
        if (!bits_ || !GetMonitorInfoW(monitor_.hmonitor, &current)) { LOG_WARN("Selected JPEG monitor is no longer available; reconnect required"); return false; }
        if (monitor_.device_name != current.szDevice ||
            !EqualRect(&monitor_.rect, &current.rcMonitor)) { LOG_WARN("Selected JPEG monitor configuration changed; reconnect required"); return false; }
        const auto& area = monitor_.rect;
        const double scale = (std::min)(static_cast<double>(width_) / monitor_.width, static_cast<double>(height_) / monitor_.height);
        const int width = (std::max)(1, static_cast<int>(monitor_.width * scale));
        const int height = (std::max)(1, static_cast<int>(monitor_.height * scale));
        const int x = (static_cast<int>(width_) - width) / 2, y = (static_cast<int>(height_) - height) / 2;
        if (!PatBlt(memory_, 0, 0, static_cast<int>(width_), static_cast<int>(height_), BLACKNESS) ||
            !StretchBlt(memory_, x, y, width, height, screen_, area.left, area.top,
                static_cast<int>(monitor_.width), static_cast<int>(monitor_.height), SRCCOPY | CAPTUREBLT)) return LOG_HR(LastErrorHr(), "monitor GDI capture");
        const auto captured = NowUs();
        if (cursor_) DrawCursor(x, y, scale);
        if (!GdiFlush()) return LOG_HR(LastErrorHr(), "monitor GdiFlush");
        out.width = width_; out.height = height_; out.capture_ts_us = captured;
        out.bgra.resize(static_cast<size_t>(width_) * height_ * 4);
        std::memcpy(out.bgra.data(), bits_, out.bgra.size());
        return true;
    }
    uint32_t Width() const override { return width_; }
    uint32_t Height() const override { return height_; }
    const char* Name() const override { return "GDI selected monitor"; }
private:
    void DrawCursor(int x, int y, double scale) {
        CURSORINFO cursor{}; cursor.cbSize = sizeof(cursor);
        if (!GetCursorInfo(&cursor)) { LOG_HR(LastErrorHr(), "monitor GetCursorInfo"); return; }
        if ((cursor.flags & CURSOR_SHOWING) == 0 || !PtInRect(&monitor_.rect, cursor.ptScreenPos)) return;
        ICONINFO icon{};
        if (!GetIconInfo(cursor.hCursor, &icon)) { LOG_HR(LastErrorHr(), "monitor GetIconInfo"); return; }
        if (!DrawIconEx(memory_, x + static_cast<int>((cursor.ptScreenPos.x - monitor_.rect.left - static_cast<double>(icon.xHotspot)) * scale),
            y + static_cast<int>((cursor.ptScreenPos.y - monitor_.rect.top - static_cast<double>(icon.yHotspot)) * scale), cursor.hCursor,
            (std::max)(1, static_cast<int>(GetSystemMetrics(SM_CXCURSOR) * scale)),
            (std::max)(1, static_cast<int>(GetSystemMetrics(SM_CYCURSOR) * scale)), 0, nullptr, DI_NORMAL)) LOG_HR(LastErrorHr(), "monitor DrawIconEx");
        if (icon.hbmColor && !DeleteObject(icon.hbmColor)) LOG_HR(LastErrorHr(), "monitor cursor color bitmap");
        if (icon.hbmMask && !DeleteObject(icon.hbmMask)) LOG_HR(LastErrorHr(), "monitor cursor mask bitmap");
    }
    MonitorInfo monitor_;
    bool cursor_;
    HDC screen_ = nullptr, memory_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    void* bits_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
};
}
std::unique_ptr<IDesktopCapture> CreateGdiMonitorCapture(const MonitorInfo& monitor, bool cursor) {
    return std::make_unique<GdiMonitorCapture>(monitor, cursor);
}
}
