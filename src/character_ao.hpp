#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>

namespace zzz_ao {

class CharacterAo {
public:
    static CharacterAo Create(ID3D12Device *device);
    explicit operator bool() const { return pipeline_ != nullptr; }
    ID3D12RootSignature *Root() const { return root_.Get(); }
    void Attach(ID3D12PipelineState *pipeline) { pipeline_.Attach(pipeline); }

    void Bind(ID3D12GraphicsCommandList *list, D3D12_GPU_DESCRIPTOR_HANDLE vertex_table,
              D3D12_GPU_DESCRIPTOR_HANDLE depth) const;
    static void Restore(ID3D12GraphicsCommandList *list, ID3D12PipelineState *original_pipeline,
                        ID3D12RootSignature *original_root,
                        const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> &original_tables);

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};

} // namespace zzz_ao
