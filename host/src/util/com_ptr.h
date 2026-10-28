#pragma once
#include <wrl/client.h>
#include "util/log.h"
namespace pds {
template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;
class ComApartment {
public:
    ComApartment() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) { LOG_HR(hr_, "CoInitializeEx"); }
    ~ComApartment() { if (SUCCEEDED(hr_)) CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    bool Ready() const { return SUCCEEDED(hr_); }
private:
    HRESULT hr_;
};
}
