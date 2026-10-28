#pragma once
#include "capture/capture.h"
#include "display/monitor_enum.h"
namespace pds {
// Separate stage-3 fallback: the frozen stage-1 primary capture is retained intact.
std::unique_ptr<IDesktopCapture> CreateGdiMonitorCapture(const MonitorInfo& monitor, bool show_cursor);
}
