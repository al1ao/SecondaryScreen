#include "capture/wgc_capture.h"
#include "capture/frame_pacer.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include "util/gpu.h"
#include "util/timer.h"

namespace pds {
namespace capture = winrt::Windows::Graphics::Capture;
struct WgcCapture::Impl {
    struct State {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        std::mutex mutex;
        std::deque<GpuFrame> frames;
        std::atomic<bool> stopped{false}, failed{false};
        std::atomic<uint64_t> dropped{0};
        std::atomic<uint64_t> throttled{0}, received{0}, copied{0};
        FramePacer pacer;
    };
    explicit Impl(bool cursor) : show_cursor(cursor) {}
    ~Impl() { StopCapture(); }
    void StopCapture() {
        state->stopped.store(true);
        // WinRT exposes Close rather than StopCapture. Revoke first; shared callback
        // state stays alive until any already-running callback finishes.
        try {
            if (pool && subscribed) pool.FrameArrived(arrived);
            if (item && closed_subscribed) item.Closed(closed);
            if (session) session.Close();
            if (pool) pool.Close();
        } catch (const winrt::hresult_error& error) { LOG_HR(error.code(), "WGC StopCapture/Close"); }
        session = nullptr; pool = nullptr; item = nullptr;
    }
    std::shared_ptr<State> state = std::make_shared<State>();
    capture::GraphicsCaptureItem item{nullptr};
    capture::Direct3D11CaptureFramePool pool{nullptr};
    capture::GraphicsCaptureSession session{nullptr};
    winrt::event_token arrived{}, closed{};
    bool show_cursor, subscribed = false, closed_subscribed = false;
    uint32_t width = 0, height = 0;
};
WgcCapture::WgcCapture(bool show_cursor) : impl_(std::make_unique<Impl>(show_cursor)) {}
WgcCapture::~WgcCapture() = default;
bool WgcCapture::ConfigureThrottle(bool enabled, uint32_t target_fps) {
    if (impl_->pool || target_fps == 0 || target_fps > 60) return false;
    impl_->state->pacer = FramePacer(enabled, target_fps);
    return true;
}
bool WgcCapture::Initialize(ID3D11Device* device, uint32_t width, uint32_t height) {
    const auto monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!monitor) { LOG_HR(LastErrorHr(), "MonitorFromPoint primary"); return false; }
    return InitializeForMonitor(device, monitor, width, height);
}
bool WgcCapture::InitializeForMonitor(HMONITOR monitor, uint32_t width, uint32_t height) {
    ComPtr<ID3D11Device> device;
    if (!CreateCaptureDevice(device)) return false;
    return InitializeForMonitor(device.Get(), monitor, width, height);
}
bool WgcCapture::InitializeForMonitor(ID3D11Device* device, HMONITOR monitor, uint32_t width, uint32_t height) {
    if (!monitor) return false;
    if (!device || width == 0 || height == 0 || impl_->pool) return false;
    try {
        if (!capture::GraphicsCaptureSession::IsSupported()) { LOG_WARN("WGC unavailable on this Windows session"); return false; }
        auto& p = *impl_;
        p.state->device = device;
        device->GetImmediateContext(p.state->context.GetAddressOf());
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IInspectable> inspectable;
        if (!LOG_HR(device->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf())), "WGC IDXGIDevice") ||
            !LOG_HR(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.GetAddressOf()), "WGC WinRT device")) return false;
        // WRL and C++/WinRT use different QueryInterface wrappers.
        auto device_rt = winrt::com_ptr<IInspectable>{};
        device_rt.copy_from(inspectable.Get());
        const auto interop = winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(p.item)));
        const auto size = p.item.Size();
        if (size.Width <= 0 || size.Height <= 0) return false;
        p.width = static_cast<uint32_t>(size.Width); p.height = static_cast<uint32_t>(size.Height);
        // TODO(design): WGC owns native-size copies; the encoder shader scales and
        // letterboxes into the separately negotiated even output size on the GPU.
        p.pool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            device_rt.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>(),
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        const auto state = p.state;
        p.arrived = p.pool.FrameArrived([state, size](auto&& pool, auto&&) {
            if (state->stopped.load()) return;
            try {
                auto frame = pool.TryGetNextFrame();
                if (!frame) return;
                state->received.fetch_add(1);
                const auto content = frame.ContentSize();
                if (content.Width != size.Width || content.Height != size.Height) {
                    if (!state->failed.exchange(true)) LOG_WARN("Selected display resized; reconnect to recreate WGC");
                    return;
                }
                {
                    std::lock_guard lock(state->mutex);
                    // Drain/release every pool frame, even when rejecting it.
                    // Gate before texture allocation, CopyResource or encoding.
                    if (!state->pacer.Accept(NowUs())) { state->throttled.fetch_add(1); return; }
                }
                auto access = frame.Surface().template as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                ComPtr<ID3D11Texture2D> source, owned;
                if (!LOG_HR(access->GetInterface(IID_PPV_ARGS(source.GetAddressOf())), "WGC surface texture")) return;
                D3D11_TEXTURE2D_DESC desc{};
                source->GetDesc(&desc);
                desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; desc.MiscFlags = 0;
                if (!LOG_HR(state->device->CreateTexture2D(&desc, nullptr, owned.GetAddressOf()), "WGC owned texture")) return;
                {
                    GpuContextLock lock(state->context.Get());
                    // A distinct owned texture per queued frame: never overwrite a
                    // texture that the encoder still owns, even when the WGC pool recycles.
                    state->context->CopyResource(owned.Get(), source.Get());
                }
                state->copied.fetch_add(1);
                GpuFrame output{owned, static_cast<uint32_t>(size.Width), static_cast<uint32_t>(size.Height), NowUs()};
                std::lock_guard lock(state->mutex);
                if (state->stopped.load()) return;
                if (state->frames.size() == 2) { state->frames.pop_front(); state->dropped.fetch_add(1); }
                state->frames.push_back(std::move(output)); // No encoding or network work in this callback.
            } catch (const winrt::hresult_error& error) {
                if (!state->stopped.load()) { LOG_HR(error.code(), "WGC FrameArrived"); state->failed.store(true); }
            } catch (const std::exception& error) {
                LOG_ERROR("WGC callback: ", error.what()); state->failed.store(true);
            }
        });
        p.subscribed = true;
        p.closed = p.item.Closed([state](auto&&, auto&&) { state->failed.store(true); });
        p.closed_subscribed = true;
        p.session = p.pool.CreateCaptureSession(p.item);
        // Cursor configuration arrived after the first WGC release; default-on is
        // still usable on older supported OS builds without this optional property.
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled"))
            p.session.IsCursorCaptureEnabled(p.show_cursor);
        else if (!p.show_cursor) LOG_WARN("This Windows build cannot disable the WGC cursor");
        p.session.StartCapture();
        LOG_INFO("WGC started: selected HMONITOR=", monitor, ' ', p.width, 'x', p.height, ", frame pool=2, cursor=", p.show_cursor);
        return true;
    } catch (const winrt::hresult_error& error) {
        LOG_HR(error.code(), "WGC Initialize");
        if (error.code() == E_ACCESSDENIED) {
            LOG_ERROR("WGC requires access to an interactive desktop session; this session cannot access desktop capture.");
            LOG_ERROR("Run from the locally logged-in desktop session. Check session/desktop capture permissions; this is not proof of unsupported GPU hardware.");
        }
        return false;
    }
}
bool WgcCapture::Capture(GpuFrame& out) {
    std::lock_guard lock(impl_->state->mutex);
    if (impl_->state->frames.empty()) return false;
    out = std::move(impl_->state->frames.front());
    impl_->state->frames.pop_front();
    return true;
}
uint32_t WgcCapture::Width() const { return impl_->width; }
uint32_t WgcCapture::Height() const { return impl_->height; }
uint64_t WgcCapture::Dropped() const { return impl_->state->dropped.load(); }
uint64_t WgcCapture::Throttled() const { return impl_->state->throttled.load(); }
uint64_t WgcCapture::Received() const { return impl_->state->received.load(); }
uint64_t WgcCapture::Copied() const { return impl_->state->copied.load(); }
bool WgcCapture::Failed() const { return impl_->state->failed.load(); }
std::unique_ptr<IGpuCapture> CreateWgcCapture() { return CreateWgcCapture(true); }
std::unique_ptr<IGpuCapture> CreateWgcCapture(bool show_cursor) { return std::make_unique<WgcCapture>(show_cursor); }
}
