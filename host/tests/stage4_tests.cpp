#include "capture/frame_pacer.h"
#include "encode/mf_event_waiter.h"
#include "encode/mf_h264_encoder.h"
#include "util/bounded_queue.h"
#include "util/com_ptr.h"
#include "util/log.h"
#include <mfapi.h>
#include <mferror.h>
#include <array>
#include <future>
#include <stdexcept>
#include <thread>
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class EventSource final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFMediaEventGenerator> {
public:
    Microsoft::WRL::ComPtr<IMFMediaEventQueue> queue;
    ~EventSource() { if (queue) queue->Shutdown(); }
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override { return queue->GetEvent(flags, event); }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override { return queue->BeginGetEvent(callback, state); }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override { return queue->EndGetEvent(result, event); }
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended, HRESULT status, const PROPVARIANT* data) override {
        return queue->QueueEventParamVar(type, extended, status, data);
    }
};
void EventWaitTest() {
    pds::ComApartment apartment;
    Require(apartment.Ready(), "MF event test COM apartment");
    Require(pds::EnsureMediaFoundation(), "MF event test startup");
    auto source = Microsoft::WRL::Make<EventSource>();
    auto waiter = Microsoft::WRL::Make<pds::MfEventWaiter>();
    Require(source && waiter && SUCCEEDED(MFCreateEventQueue(source->queue.GetAddressOf())), "MF event test queue");
    Require(SUCCEEDED(waiter->Start(source.Get())), "subscribe to MF events");
    // Stop the mailbox before shutting down its source even if an assertion throws.
    struct Cleanup {
        pds::MfEventWaiter* waiter; EventSource* source;
        ~Cleanup() { waiter->Stop(); source->queue->Shutdown(); }
    } cleanup{waiter.Get(), source.Get()};
    Require(waiter->Start(source.Get()) == MF_E_INVALIDREQUEST, "only one outstanding event subscription");
    Microsoft::WRL::ComPtr<IMFMediaEvent> event;
    const auto began = std::chrono::steady_clock::now();
    waiter->WaitUntil(began + std::chrono::milliseconds(20));
    Require(waiter->Pop(event.GetAddressOf()) == MF_E_NO_EVENTS_AVAILABLE, "no-event wait has finite deadline");
    Require(std::chrono::steady_clock::now() - began >= std::chrono::milliseconds(10), "empty mailbox waits rather than spinning");
    Require(std::chrono::steady_clock::now() - began < std::chrono::seconds(1), "timeout does not wait indefinitely");
    for (const auto type : {METransformNeedInput, METransformHaveOutput}) {
        Require(SUCCEEDED(source->QueueEvent(type, GUID_NULL, S_OK, nullptr)), "queue transform event");
        waiter->WaitUntil(std::chrono::steady_clock::now() + std::chrono::seconds(1));
        Require(SUCCEEDED(waiter->Pop(event.ReleaseAndGetAddressOf())), "event wakes waiter and is retained");
        MediaEventType actual = MEUnknown;
        Require(SUCCEEDED(event->GetType(&actual)) && actual == static_cast<MediaEventType>(type), "event delivery order");
    }
    Require(SUCCEEDED(source->QueueEvent(MEError, GUID_NULL, E_FAIL, nullptr)), "queue error event");
    waiter->WaitUntil(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    HRESULT status = S_OK;
    Require(SUCCEEDED(waiter->Pop(event.ReleaseAndGetAddressOf())) && SUCCEEDED(event->GetStatus(&status)) && status == E_FAIL,
        "MFT failure status must not be hidden");
    // Event-queue Shutdown can cancel a pending async request without Invoke.
    // Exercise the production order: explicitly stop/wake before source shutdown.
    std::promise<void> waiting;
    auto entered = waiting.get_future();
    const auto closing = std::chrono::steady_clock::now();
    std::jthread blocked([&] {
        waiting.set_value();
        waiter->WaitUntil(std::chrono::steady_clock::now() + std::chrono::seconds(30));
    });
    entered.wait();
    waiter->Stop();
    Require(SUCCEEDED(source->queue->Shutdown()), "shutdown with pending event request");
    blocked.join();
    Require(std::chrono::steady_clock::now() - closing < std::chrono::seconds(1), "Stop wakes blocked waiter before shutdown");
    Require(waiter->Pop(event.ReleaseAndGetAddressOf()) == MF_E_SHUTDOWN, "stopped mailbox rejects reads");
    Require(waiter->Start(source.Get()) == MF_E_INVALIDREQUEST, "stopped generation cannot be reused by stale callbacks");
    auto dead_waiter = Microsoft::WRL::Make<pds::MfEventWaiter>();
    Require(dead_waiter && dead_waiter->Start(source.Get()) == MF_E_SHUTDOWN, "subscription failure is propagated");
    LOG_INFO("PASS stage 4: MF event notification, bounded wait, error status and pending-request shutdown");
}
uint64_t Simulate(unsigned source_hz, bool enabled, unsigned target_fps, bool jitter) {
    pds::FramePacer pacer(enabled, target_fps);
    uint64_t accepted = 0;
    for (uint64_t frame = 0; frame < source_hz * 10u; ++frame) {
        uint64_t now = 1000000 + frame * 1000000 / source_hz;
        if (jitter && frame % 3 == 0) now += 400;
        if (pacer.Accept(now)) ++accepted;
    }
    return accepted;
}
}
int main() {
    try {
        for (const auto source : {60u, 120u, 144u, 240u}) {
            const auto accepted = Simulate(source, true, 60, false);
            Require(accepted >= 599 && accepted <= 601, "source cadence must not quantize target 60fps down to 48fps");
            Require(Simulate(source, false, 60, false) == source * 10u, "off accepts every callback");
            LOG_INFO("PASS stage 4: source=", source, "Hz accepted=", accepted, " in 10s at target 60fps");
        }
        Require(Simulate(60, true, 60, true) == 600, "60Hz scheduling jitter must not halve frame rate");
        Require(Simulate(144, true, 30, true) >= 299 && Simulate(144, true, 30, true) <= 301, "explicit lower target respected");
        pds::FramePacer pacer(true, 60);
        Require(pacer.Accept(1000000) && !pacer.Accept(1000001), "no duplicate callback bursts");
        Require(pacer.Accept(9000000) && !pacer.Accept(9000001), "long stall rebases without catch-up burst");
        bool rejected = false;
        try { pds::FramePacer invalid(true, 0); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "invalid target rejected");
        for (size_t depth : {size_t{1}, size_t{2}}) {
            pds::BoundedQueue<unsigned> queue(depth);
            Require(queue.Push(1) == 0, "first queue item");
            Require(queue.Push(2) == (depth == 1 ? 1u : 0u), "selected queue capacity");
            unsigned frame = 0;
            Require(queue.Pop(frame, {}) && frame == (depth == 1 ? 2u : 1u), "depth one keeps newest, depth two preserves normal order");
        }
        LOG_INFO("PASS stage 4: cadence/jitter/stall/toggle/queue regressions");
        EventWaitTest();
        return 0;
    } catch (const std::exception& error) { LOG_ERROR("FAIL stage 4: ", error.what()); return 1; }
}
