#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <memory>
#include <string>

namespace zzz_ao {

// The caller serializes the callbacks and owns the job while the list can still
// be submitted. The recorded copies only run during an explicitly armed trace.
class DepthCapture {
public:
    static std::shared_ptr<DepthCapture> Record(ID3D12GraphicsCommandList *list,
                                                ID3D12Resource *early, ID3D12Resource *late);
    void Submitted(ID3D12CommandQueue *queue);
    void FinishPresent();
    bool Ready() const;
    bool HasSubmission() const { return submission_ != 0; }
    bool IsList(ID3D12GraphicsCommandList *list) const { return list_ == list; }
    bool IsRetired() const { return list_ == nullptr; }
    void RetireList() { list_ = nullptr; }
    std::string ReadRows() const;

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    ID3D12GraphicsCommandList *list_ = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT first_{};
    uint64_t stride_ = 0;
    uint32_t bytes_per_pixel_ = 0;
    DWORD submission_thread_ = 0;
    uint64_t submission_ = 0;
    uint64_t signalled_ = 0;
    bool mismatched_queue_ = false;
};

} // namespace zzz_ao
