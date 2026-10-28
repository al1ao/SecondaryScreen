#pragma once
#include <mfidl.h>
#include <mfapi.h>
#include <wrl.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace pds {
// Only collects MF events. The encode thread retains all transform/GPU work.
// Stop must precede activation shutdown; pending callbacks never refer to Impl.
class MfEventWaiter final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFAsyncCallback> {
public:
    HRESULT Start(IMFMediaEventGenerator* generator);
    void Stop();
    HRESULT Pop(IMFMediaEvent** event);
    void WaitUntil(std::chrono::steady_clock::time_point deadline);
    STDMETHODIMP GetParameters(DWORD* flags, DWORD* queue) override {
        if (!flags || !queue) return E_POINTER;
        // Short, thread-safe notifications must not wait behind unrelated work
        // on MF's default serial queue. Transform work stays on the encode thread.
        *flags = 0;
        *queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
        return S_OK;
    }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override;
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> generator_;
    std::deque<Microsoft::WRL::ComPtr<IMFMediaEvent>> pending_;
    HRESULT status_ = S_OK;
    bool stopped_ = true;
    bool started_ = false;
};
}
