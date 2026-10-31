#include "input/input_injector.h"
#include "util/log.h"
#include <stdexcept>
namespace {
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
}
int main() {
    try {
        using namespace pds;
        // Virtual desktop 3840x1080 with this monitor left of the primary:
        // exercises negative DIP-less desktop coordinates as well as the far edge.
        const TouchMapping mapping{{-1920, 0, 0, 1080}, -1920, 0, 3840, 1080};
        int32_t x = -1, y = -1;
        Require(MapTouchToDesktop(mapping, 0, 0, &x, &y) && x == 0 && y == 0,
            "top-left maps to the virtual desktop origin");
        Require(MapTouchToDesktop(mapping, 1000000, 1000000, &x, &y) && x == 32758 && y == 65535,
            "bottom-right maps to the far virtual-desktop edge");
        Require(MapTouchToDesktop(mapping, 500000, 500000, &x, &y) && x == 16388 && y == 32797,
            "center maps to the monitor center");
        Require(!MapTouchToDesktop(mapping, 1000001, 0, &x, &y), "ppm above one million is rejected");
        const TouchMapping empty{{0, 0, 0, 0}, 0, 0, 1920, 1080};
        Require(!MapTouchToDesktop(empty, 0, 0, &x, &y), "empty monitor rect is rejected");
        const TouchMapping right{{1920, 0, 3840, 1080}, 0, 0, 3840, 1080};
        Require(MapTouchToDesktop(right, 0, 0, &x, &y) && x == 32776 && y == 0,
            "monitor right of the primary starts past the midpoint");
        const TouchInput input{0, 0, 500000, 500000};
        const auto wire = Serialize(input);
        TouchInput decoded{};
        Require(Deserialize(wire, decoded) && decoded.x_ppm == 500000 && decoded.y_ppm == 500000 &&
            decoded.action == 0 && decoded.pointer == 0, "TouchInput wire round-trip");
        LOG_INFO("PASS stage 7: touch mapping (negative origin, edges, rejection) and wire round-trip");
        return 0;
    } catch (const std::exception& error) { LOG_ERROR("FAIL stage 7: ", error.what()); return 1; }
}
