#pragma once
#include <windows.h>
#include "capture/capture.h"
namespace pds {
class GdiCapture final : public IDesktopCapture {
public:
    ~GdiCapture() override;
    bool Initialize(uint32_t width, uint32_t height) override;
    bool Capture(CapturedFrame& out) override;
    uint32_t Width() const override { return width_; }
    uint32_t Height() const override { return height_; }
    const char* Name() const override { return "GDI primary monitor"; }
private:
    void Reset();
    void DrawCursor(int x, int y, int width, int height, int source_width, int source_height);
    HDC screen_dc_ = nullptr;
    HDC mem_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    void* bits_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};
}
