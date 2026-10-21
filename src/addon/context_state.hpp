#pragma once
#include <d3d11_1.h>

// NGX is allowed to change D3D11 pipeline bindings. Use the runtime's complete
// state swap so cached game state (including stencil and font SRVs) survives.
class PrivateContextState
{
    ID3D11DeviceContext1 *context_ = nullptr;
    ID3DDeviceContextState *state_ = nullptr;
public:
    bool initialize(ID3D11Device *device)
    {
        shutdown();
        ID3D11Device1 *device1 = nullptr;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) return false;
        ID3D11DeviceContext *context = nullptr;
        device->GetImmediateContext(&context);
        const HRESULT query = context->QueryInterface(IID_PPV_ARGS(&context_));
        context->Release();
        D3D_FEATURE_LEVEL level = device->GetFeatureLevel(), selected = {};
        const HRESULT create = SUCCEEDED(query) ? device1->CreateDeviceContextState(
            0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &selected, &state_) : query;
        device1->Release();
        if (FAILED(create)) { shutdown(); return false; }
        return true;
    }
    void shutdown()
    {
        if (state_) { state_->Release(); state_ = nullptr; }
        if (context_) { context_->Release(); context_ = nullptr; }
    }
    class Scope
    {
        ID3D11DeviceContext1 *context_;
        ID3DDeviceContextState *saved_ = nullptr;
    public:
        explicit Scope(PrivateContextState &owner) : context_(owner.context_)
        { if (context_) context_->SwapDeviceContextState(owner.state_, &saved_); }
        ~Scope()
        {
            if (saved_) { context_->SwapDeviceContextState(saved_, nullptr); saved_->Release(); }
        }
        bool active() const { return saved_ != nullptr; }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
    };
};
