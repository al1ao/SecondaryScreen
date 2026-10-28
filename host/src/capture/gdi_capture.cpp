#include "capture/gdi_capture.h"
#include <algorithm>
#include <cstring>
#include "util/log.h"
#include "util/timer.h"

namespace pds {
namespace {
void DeleteGdi(HGDIOBJ object) {
    if (object && !DeleteObject(object)) LOG_HR(LastErrorHr(), "DeleteObject");
}
}
GdiCapture::~GdiCapture() { Reset(); }
void GdiCapture::Reset() {
    if (previous_ && mem_dc_ && SelectObject(mem_dc_, previous_) == HGDI_ERROR)
        LOG_HR(LastErrorHr(), "restore DIB selection");
    DeleteGdi(dib_);
    if (mem_dc_ && !DeleteDC(mem_dc_)) LOG_HR(LastErrorHr(), "DeleteDC");
    if (screen_dc_ && ReleaseDC(nullptr, screen_dc_) == 0) LOG_HR(LastErrorHr(), "ReleaseDC");
    screen_dc_ = nullptr; mem_dc_ = nullptr; dib_ = nullptr; previous_ = nullptr; bits_ = nullptr;
    width_ = 0; height_ = 0;
}
bool GdiCapture::Initialize(uint32_t width, uint32_t height) {
    Reset();
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw < 2 || sh < 2 || width < 2 || height < 2) return false;
    // TODO(design): stage 1 uses CLI dimensions instead of preferred_*; bound them
    // to the primary display, preserve the requested ratio, and round down to even.
    const double scale = (std::min)({1.0, static_cast<double>(sw) / width,
        static_cast<double>(sh) / height});
    width_ = static_cast<uint32_t>(width * scale) & ~1u;
    height_ = static_cast<uint32_t>(height * scale) & ~1u;
    if (width_ < 2 || height_ < 2) return false;
    screen_dc_ = GetDC(nullptr);
    if (!screen_dc_) { LOG_HR(LastErrorHr(), "GetDC"); return false; }
    mem_dc_ = CreateCompatibleDC(screen_dc_);
    if (!mem_dc_) { LOG_HR(LastErrorHr(), "CreateCompatibleDC"); return false; }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width_);
    info.bmiHeader.biHeight = -static_cast<LONG>(height_); // Top-down BGRA, never upside-down.
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    dib_ = CreateDIBSection(screen_dc_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (!dib_) { LOG_HR(LastErrorHr(), "CreateDIBSection"); return false; }
    previous_ = SelectObject(mem_dc_, dib_);
    if (!previous_ || previous_ == HGDI_ERROR) {
        previous_ = nullptr;
        LOG_HR(LastErrorHr(), "SelectObject DIB"); return false;
    }
    if (SetStretchBltMode(mem_dc_, COLORONCOLOR) == 0) {
        LOG_HR(LastErrorHr(), "SetStretchBltMode"); return false;
    }
    return true;
}
void GdiCapture::DrawCursor(int x, int y, int width, int height, int source_width, int source_height) {
    // TODO(design): BitBlt excludes the system pointer; composite the visible primary-screen
    // pointer with GDI so the stage-1 mouse movement acceptance check is observable.
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    if (!GetCursorInfo(&cursor)) { LOG_HR(LastErrorHr(), "GetCursorInfo"); return; }
    if ((cursor.flags & CURSOR_SHOWING) == 0 || cursor.ptScreenPos.x < 0 || cursor.ptScreenPos.y < 0 ||
        cursor.ptScreenPos.x >= source_width || cursor.ptScreenPos.y >= source_height) return;
    ICONINFO icon{};
    if (!GetIconInfo(cursor.hCursor, &icon)) { LOG_HR(LastErrorHr(), "GetIconInfo"); return; }
    BITMAP bitmap{};
    int cw = GetSystemMetrics(SM_CXCURSOR), ch = GetSystemMetrics(SM_CYCURSOR);
    if (icon.hbmColor && GetObjectW(icon.hbmColor, sizeof(bitmap), &bitmap) != 0) {
        cw = bitmap.bmWidth; ch = bitmap.bmHeight;
    } else if (icon.hbmMask && GetObjectW(icon.hbmMask, sizeof(bitmap), &bitmap) != 0) {
        cw = bitmap.bmWidth; ch = bitmap.bmHeight / 2;
    }
    const double sx = static_cast<double>(width) / source_width;
    const double sy = static_cast<double>(height) / source_height;
    if (!DrawIconEx(mem_dc_, x + static_cast<int>((cursor.ptScreenPos.x - static_cast<double>(icon.xHotspot)) * sx),
        y + static_cast<int>((cursor.ptScreenPos.y - static_cast<double>(icon.yHotspot)) * sy), cursor.hCursor,
        (std::max)(1, static_cast<int>(cw * sx)), (std::max)(1, static_cast<int>(ch * sy)), 0, nullptr, DI_NORMAL))
        LOG_HR(LastErrorHr(), "DrawIconEx");
    DeleteGdi(icon.hbmColor);
    DeleteGdi(icon.hbmMask);
}
bool GdiCapture::Capture(CapturedFrame& out) {
    if (!bits_) return false;
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0 || sh <= 0) return false;
    const int dw = static_cast<int>(width_), dh = static_cast<int>(height_);
    const double scale = (std::min)(static_cast<double>(dw) / sw, static_cast<double>(dh) / sh);
    const int rw = (std::max)(1, static_cast<int>(sw * scale));
    const int rh = (std::max)(1, static_cast<int>(sh * scale));
    const int x = (dw - rw) / 2, y = (dh - rh) / 2;
    if (!PatBlt(mem_dc_, 0, 0, dw, dh, BLACKNESS)) {
        LOG_HR(LastErrorHr(), "PatBlt"); return false;
    }
    const BOOL copied = rw == sw && rh == sh
        ? BitBlt(mem_dc_, x, y, rw, rh, screen_dc_, 0, 0, SRCCOPY | CAPTUREBLT)
        : StretchBlt(mem_dc_, x, y, rw, rh, screen_dc_, 0, 0, sw, sh, SRCCOPY | CAPTUREBLT);
    const auto captured = NowUs(); // Immediately after the capture API returns.
    if (!copied) { LOG_HR(LastErrorHr(), "BitBlt/StretchBlt"); return false; }
    DrawCursor(x, y, rw, rh, sw, sh);
    if (!GdiFlush()) { LOG_HR(LastErrorHr(), "GdiFlush"); return false; }
    out.width = width_; out.height = height_; out.capture_ts_us = captured;
    out.bgra.resize(static_cast<size_t>(width_) * height_ * 4);
    std::memcpy(out.bgra.data(), bits_, out.bgra.size());
    return true;
}
std::unique_ptr<IDesktopCapture> CreateGdiCapture() { return std::make_unique<GdiCapture>(); }
}
