#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>

namespace zzz_ao {

enum class ShadingPass { composite, preview };

class CharacterAo {
public:
    static CharacterAo Create(ID3D12Device *device);
    ID3D12RootSignature *Root() const { return root_.Get(); }
    bool HasPipeline(ShadingPass pass) const { return State(pass) != nullptr; }
    void Attach(ShadingPass pass, ID3D12PipelineState *pipeline);

    void Bind(ShadingPass pass, ID3D12GraphicsCommandList *list, D3D12_GPU_DESCRIPTOR_HANDLE vertex_table,
              D3D12_GPU_DESCRIPTOR_HANDLE depth) const;
    static void Restore(ID3D12GraphicsCommandList *list, ID3D12PipelineState *original_pipeline,
                        ID3D12RootSignature *original_root,
                        const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> &original_tables);

private:
    ID3D12PipelineState *State(ShadingPass pass) const;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> composite_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> preview_;
};

} // namespace zzz_ao
