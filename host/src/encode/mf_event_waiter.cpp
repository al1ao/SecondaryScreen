#include "encode/mf_event_waiter.h"
#include <mferror.h>
#include <new>

namespace pds {
HRESULT MfEventWaiter::Start(IMFMediaEventGenerator* generator) {
    if (!generator) return E_POINTER;
    std::lock_guard lock(mutex_);
    // Single subscription lifetime: a callback from a stopped generation must
    // never be interpreted as an event from a later initialization.
    if (started_) return MF_E_INVALIDREQUEST;
    started_ = true;
    pending_.clear();
    generator_ = generator;
    stopped_ = false;
    status_ = generator_->BeginGetEvent(this, nullptr);
    if (FAILED(status_)) { stopped_ = true; generator_.Reset(); }
    return status_;
}
void MfEventWaiter::Stop() {
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        generator_.Reset();
        pending_.clear();
    }
    ready_.notify_all();
}
HRESULT MfEventWaiter::Pop(IMFMediaEvent** event) {
    if (!event) return E_POINTER;
    *event = nullptr;
    std::lock_guard lock(mutex_);
    if (stopped_) return MF_E_SHUTDOWN;
    if (FAILED(status_)) return status_;
    if (pending_.empty()) return MF_E_NO_EVENTS_AVAILABLE;
    *event = pending_.front().Detach();
    pending_.pop_front();
    return S_OK;
}
void MfEventWaiter::WaitUntil(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mutex_);
    ready_.wait_until(lock, deadline, [this] { return stopped_ || FAILED(status_) || !pending_.empty(); });
}
STDMETHODIMP MfEventWaiter::Invoke(IMFAsyncResult* result) {
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> generator;
    {
        std::lock_guard lock(mutex_);
        if (stopped_) return S_OK;
        generator = generator_;
    }
    Microsoft::WRL::ComPtr<IMFMediaEvent> event;
    const auto hr = generator->EndGetEvent(result, event.GetAddressOf());
    {
        std::lock_guard lock(mutex_);
        if (stopped_) return S_OK;
        status_ = SUCCEEDED(hr) && !event ? E_UNEXPECTED : hr;
        if (SUCCEEDED(status_)) {
            // A bounded mailbox also makes a broken event-producing MFT fail
            // explicitly rather than growing memory without limit.
            if (pending_.size() >= 64) status_ = E_UNEXPECTED;
            else {
                try { pending_.push_back(std::move(event)); }
                catch (const std::bad_alloc&) { status_ = E_OUTOFMEMORY; }
            }
            if (SUCCEEDED(status_)) status_ = generator->BeginGetEvent(this, nullptr);
        }
    }
    ready_.notify_all();
    return S_OK; // Failure is delivered to the encode thread through Pop.
}
}
