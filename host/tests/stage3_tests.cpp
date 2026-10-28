#include "display/monitor_enum.h"
#include "capture/gdi_monitor_capture.h"
#include "capture/wgc_capture.h"
#include "util/timer.h"
#include "util/log.h"
#include <roapi.h>
#include <stdexcept>
#include <thread>
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
pds::MonitorInfo Monitor(const wchar_t* name, const wchar_t* id, bool primary = false) {
    pds::MonitorInfo result;
    result.friendly_name = name; result.monitor_device_id = id; result.is_primary = primary;
    result.is_virtual = pds::IsVirtualDisplayName(name);
    result.width = 1920; result.height = 1200;
    return result;
}
void SelectionTest() {
    for (const auto* name : {L"Virtual Display Driver", L"my vDd panel", L"INDIRECT DISPLAY", L"Virtual Monitor"})
        Require(pds::IsVirtualDisplayName(name), "case-insensitive virtual keywords");
    Require(!pds::IsVirtualDisplayName(L"Generic PnP Monitor"), "physical monitor is not virtual");
    std::vector<pds::MonitorInfo> monitors{Monitor(L"GameViewer Virtual Display Adapter / physical panel", L"remote-primary", true),
        Monitor(L"Office LCD", L"physical"), Monitor(L"Virtual Display Driver", L"vdd-a"), Monitor(L"VDD other", L"vdd-b")};
    Require(pds::SelectTargetMonitor(monitors, L"", false)->monitor_device_id == L"vdd-a", "default selects secondary virtual screen");
    Require(pds::SelectTargetMonitor(monitors, L"other", false)->monitor_device_id == L"vdd-b", "explicit friendly-name substring");
    Require(pds::SelectTargetMonitor(monitors, L"", true)->is_primary, "explicit primary fallback");
    Require(pds::SelectTargetMonitor(monitors, L"", false, L"VDD-B")->monitor_device_id == L"vdd-b", "stable ID survives display index changes");
    Require(!pds::SelectTargetMonitor(monitors, L"missing", false), "no match is an error");
    monitors.resize(1);
    Require(!pds::SelectTargetMonitor(monitors, L"", false), "virtual session primary cannot masquerade as second screen");
    LOG_INFO("PASS stage 3: monitor matching, explicit selection, stable ID and primary isolation");
}
void ResolutionTest() {
    auto size = pds::NegotiateResolution(1920, 1200, 1280, 800, 3840, 2160);
    Require(size.width == 1920 && size.height == 1200, "close aspect uses monitor resolution");
    size = pds::NegotiateResolution(1920, 1200, 1230, 800, 3840, 2160);
    Require(size.width == 1920 && size.height == 1200, "aspect error below five percent retains native mode");
    size = pds::NegotiateResolution(1920, 1200, 1200, 800, 3840, 2160);
    Require(size.width == 1200 && size.height == 750, "aspect error above five percent fits preferred bounds");
    size = pds::NegotiateResolution(1920, 1080, 1280, 800, 3840, 2160);
    Require(size.width == 1280 && size.height == 720, "mismatched aspect fits preferred bounds without stretching");
    size = pds::NegotiateResolution(1920, 1200, 1280, 800, 1280, 720);
    Require(size.width == 1152 && size.height == 720, "decoder limits override native resolution");
    size = pds::NegotiateResolution(1920, 1200, 1280, 800, 3840, 2160, 1280, 800);
    Require(size.width == 1280 && size.height == 800, "explicit CLI bounds respected");
    size = pds::NegotiateResolution(1921, 1201, 0, 0, 0, 0);
    Require(size.width == 1920 && size.height == 1200, "even output dimensions");
    size = pds::NegotiateResolution(0, 1200, 0, 0, 0, 0);
    Require(!size.width && !size.height, "invalid monitor rejected");
    LOG_INFO("PASS stage 3: native/aspect/decoder/CLI/even resolution negotiation");
}
void CaptureTest() {
    const auto monitors = pds::EnumerateMonitors();
    Require(!monitors.empty(), "live monitor enumeration");
    for (const auto& monitor : monitors) Require(monitor.hmonitor && monitor.width >= 2 && monitor.height >= 2 && !monitor.device_name.empty(), "monitor handles and dimensions");
    const auto primary = pds::SelectTargetMonitor(monitors, L"", true);
    Require(primary.has_value(), "primary exists for explicit monitor API test");
    pds::LogMonitors(monitors, primary->hmonitor);
    auto gdi = pds::CreateGdiMonitorCapture(*primary, true);
    pds::CapturedFrame cpu;
    Require(gdi->Initialize(640, 400) && gdi->Capture(cpu) && cpu.bgra.size() == 640u * 400u * 4u && cpu.capture_ts_us > 0,
        "selected-monitor JPEG fallback capture");
    pds::WgcCapture capture(true);
    Require(capture.InitializeForMonitor(primary->hmonitor, 640, 400), "WGC InitializeForMonitor explicit handle");
    pds::GpuFrame frame;
    const auto deadline = pds::NowUs() + 3000000;
    while (!capture.Capture(frame) && pds::NowUs() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Require(frame.texture && frame.width == primary->width && frame.height == primary->height, "selected monitor native WGC dimensions");
    LOG_INFO("PASS stage 3: live enumeration and explicit HMONITOR GDI/WGC APIs (primary test target; not virtual-screen acceptance)");
}
}
int main() {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) { LOG_HR(pds::LastErrorHr(), "stage3 DPI awareness"); return 1; }
    const auto initialized = RoInitialize(RO_INIT_MULTITHREADED);
    if (!LOG_HR(initialized, "stage3 RoInitialize")) return 1;
    int result = 0;
    try { SelectionTest(); ResolutionTest(); CaptureTest(); }
    catch (const std::exception& error) { LOG_ERROR("FAIL stage 3: ", error.what()); result = 1; }
    RoUninitialize(); return result;
}
