#pragma once
#include <d3d11.h>
#include <array>
#include <cstdio>

namespace dos2dlss {
// Opt-in, asynchronous timestamps. Never flush or wait for GPU completion.
class DlssGpuProfiler {
    struct Slot {
        ID3D11Query *disjoint = nullptr, *start = nullptr, *prepared = nullptr, *finish = nullptr;
        bool pending = false, valid = false;
        unsigned key = 0;
    };
    struct Sum { double prep = 0, dlss = 0; unsigned count = 0; };
    std::array<Slot, 8> slots_ = {};
    std::array<Sum, 10> sums_ = {};
    unsigned cursor_ = 0;
    Slot *active_ = nullptr;
    bool enabled_ = false, legacy_ = false;
public:
    using LogFn = void (*)(const char *);
    void configure(bool enabled, bool legacy) { enabled_ = enabled; legacy_ = legacy; }
    void begin(ID3D11Device *device, ID3D11DeviceContext *context, unsigned mode, LogFn log) {
        if (!enabled_ || active_ || mode >= 5) return;
        for (auto &slot : slots_) {
            if (!slot.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
            UINT64 a=0,b=0,c=0;
            if (context->GetData(slot.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                context->GetData(slot.start, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                context->GetData(slot.prepared, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                context->GetData(slot.finish, &c, sizeof(c), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            slot.pending = false;
            if (!slot.valid || disjoint.Disjoint || !disjoint.Frequency || b<a || c<b) continue;
            auto &sum = sums_[slot.key];
            sum.prep += 1000.0 * static_cast<double>(b-a) / disjoint.Frequency;
            sum.dlss += 1000.0 * static_cast<double>(c-b) / disjoint.Frequency;
            if (++sum.count >= 120) {
                char message[256] = {};
                snprintf(message, sizeof(message), "PERF GPU mode=%u legacy=%u samples=%u prepare_ms=%.3f dlss_ms=%.3f",
                    slot.key%5, slot.key/5, sum.count, sum.prep/sum.count, sum.dlss/sum.count);
                if (log) log(message);
                sum = {};
            }
        }
        auto &slot = slots_[cursor_++ % slots_.size()];
        if (slot.pending) return;
        if (!slot.disjoint) {
            D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            if (FAILED(device->CreateQuery(&desc, &slot.disjoint))) return;
            desc.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(device->CreateQuery(&desc, &slot.start)) ||
                FAILED(device->CreateQuery(&desc, &slot.prepared)) ||
                FAILED(device->CreateQuery(&desc, &slot.finish))) { shutdown(); return; }
        }
        slot.key = mode + (legacy_ ? 5 : 0);
        slot.valid = false;
        active_ = &slot;
        context->Begin(slot.disjoint);
        context->End(slot.start);
    }
    void prepared(ID3D11DeviceContext *context) {
        if (active_) { context->End(active_->prepared); active_->valid = true; }
    }
    void finish(ID3D11DeviceContext *context) {
        if (!active_) return;
        if (!active_->valid) context->End(active_->prepared);
        context->End(active_->finish);
        context->End(active_->disjoint);
        active_->pending = true;
        active_ = nullptr;
    }
    struct Scope {
        DlssGpuProfiler &owner; ID3D11DeviceContext *context;
        Scope(DlssGpuProfiler &p, ID3D11Device *d, ID3D11DeviceContext *c, unsigned mode, LogFn log)
            : owner(p), context(c) { owner.begin(d,c,mode,log); }
        ~Scope() { owner.finish(context); }
    };
    void shutdown() {
        for (auto &slot : slots_) {
            for (auto *query : {slot.disjoint,slot.start,slot.prepared,slot.finish}) if (query) query->Release();
            slot = {};
        }
        active_ = nullptr; sums_ = {}; cursor_ = 0;
    }
};
}
