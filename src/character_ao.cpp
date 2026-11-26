#include "character_ao.hpp"

namespace zzz_ao {

CharacterAo CharacterAo::Create(ID3D12Device *device)
{
    CharacterAo ao;
    // Retain the game's vertex CBVs so the extra draw uses native scene UVs.
    const D3D12_DESCRIPTOR_RANGE ranges[]{
        {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
    };
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &ranges[0]};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &ranges[1]};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_SIGNATURE_DESC root_desc{2, parameters, 0, nullptr,
                                             D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &serialized, nullptr)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&ao.root_))))
        return {};
    return ao;
}

void CharacterAo::Bind(ID3D12GraphicsCommandList *list, D3D12_GPU_DESCRIPTOR_HANDLE vertex_table,
                       D3D12_GPU_DESCRIPTOR_HANDLE depth) const
{
    // Both handles are in the game's active heap. Compute bindings stay live.
    list->SetGraphicsRootSignature(root_.Get());
    list->SetGraphicsRootDescriptorTable(0, vertex_table);
    list->SetGraphicsRootDescriptorTable(1, depth);
    list->SetPipelineState(pipeline_.Get());
}

void CharacterAo::Restore(ID3D12GraphicsCommandList *list, ID3D12PipelineState *original_pipeline,
                          ID3D12RootSignature *original_root,
                          const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> &original_tables)
{
    list->SetPipelineState(original_pipeline);
    list->SetGraphicsRootSignature(original_root);
    for (UINT i = 0; i < original_tables.size(); ++i)
        list->SetGraphicsRootDescriptorTable(i, original_tables[i]);
}

} // namespace zzz_ao
