#include "character_ao.hpp"
#include "character_ao_ps.hpp"
#include "character_scene_ps.hpp"
#include "character_scene_vs.hpp"
#include "shading_view_ps.hpp"
#include "sphere_depth_ps.hpp"

#include <d3d12sdklayers.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

using Microsoft::WRL::ComPtr;
using zzz_ao::ShadingPass;

namespace {

constexpr UINT extent = 256;
constexpr UINT contact_edge = extent / 2;
constexpr DXGI_FORMAT color_format = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr D3D12_RESOURCE_DESC color_desc{
    D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, extent, extent, 1, 1, color_format,
    {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
};

bool Check(HRESULT result, const char *operation)
{
    if (SUCCEEDED(result))
        return true;
    std::cerr << operation << " failed with HRESULT 0x" << std::hex << static_cast<uint32_t>(result) << '\n';
    return false;
}

ComPtr<ID3D12Resource> Allocate(ID3D12Device *device, D3D12_HEAP_TYPE type,
                               const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES state)
{
    const D3D12_HEAP_PROPERTIES heap{type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    ComPtr<ID3D12Resource> result;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                         IID_PPV_ARGS(&result)), "CreateCommittedResource");
    return result;
}

ComPtr<ID3D12DescriptorHeap> MakeHeap(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count,
                                      D3D12_DESCRIPTOR_HEAP_FLAGS flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE)
{
    const D3D12_DESCRIPTOR_HEAP_DESC desc{type, count, flags, 0};
    ComPtr<ID3D12DescriptorHeap> result;
    Check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&result)), "CreateDescriptorHeap");
    return result;
}

void Transition(ID3D12GraphicsCommandList *list, ID3D12Resource *res,
                D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list->ResourceBarrier(1, &barrier);
}

ComPtr<ID3D12RootSignature> OriginalRoot(ID3D12Device *device)
{
    const D3D12_DESCRIPTOR_RANGE ranges[]{
        {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0},
    };
    D3D12_ROOT_PARAMETER params[3]{};
    for (UINT i = 0; i < 3; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable = {1, &ranges[i]};
        params[i].ShaderVisibility = i == 0 ? D3D12_SHADER_VISIBILITY_VERTEX : D3D12_SHADER_VISIBILITY_PIXEL;
    }
    const D3D12_ROOT_SIGNATURE_DESC desc{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3D12RootSignature> result;
    if (SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)))
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&result));
    return result;
}

enum class TestPass { game, composite, preview, depth };

ComPtr<ID3D12PipelineState> MakePipeline(ID3D12Device *device, ID3D12RootSignature *root, TestPass pass)
{
    const bool composite = pass == TestPass::composite;
    const bool depth_only = pass == TestPass::depth;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS = {character_scene_vs_bytecode, sizeof(character_scene_vs_bytecode)};
    switch (pass) {
    case TestPass::game:
        desc.PS = {character_scene_ps_bytecode, sizeof(character_scene_ps_bytecode)};
        break;
    case TestPass::composite:
        desc.PS = {character_ao_ps_bytecode, sizeof(character_ao_ps_bytecode)};
        break;
    case TestPass::preview:
        desc.PS = {shading_view_ps_bytecode, sizeof(shading_view_ps_bytecode)};
        break;
    case TestPass::depth:
        desc.PS = {sphere_depth_ps_bytecode, sizeof(sphere_depth_ps_bytecode)};
        break;
    }
    desc.BlendState.RenderTarget[0] = {composite, FALSE, composite ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE,
        composite ? D3D12_BLEND_SRC_COLOR : D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP,
        static_cast<UINT8>(pass == TestPass::game ? 0xf : 0x7)};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = depth_only;
    desc.DepthStencilState.DepthWriteMask = depth_only ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS
    };
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = depth_only ? 0 : 1;
    desc.RTVFormats[0] = depth_only ? DXGI_FORMAT_UNKNOWN : color_format;
    desc.DSVFormat = depth_only ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "Create test pipeline");
    return pso;
}

} // namespace

int main()
{
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        return 77;

    auto native_root = OriginalRoot(device.Get());
    auto ao = zzz_ao::CharacterAo::Create(device.Get());
    if (!native_root || !ao.Root()) {
        std::cerr << "Could not create the test root signatures.\n";
        return 1;
    }
    auto native = MakePipeline(device.Get(), native_root.Get(), TestPass::game);
    auto sphere = MakePipeline(device.Get(), native_root.Get(), TestPass::depth);
    ao.Attach(ShadingPass::composite, MakePipeline(device.Get(), ao.Root(), TestPass::composite).Detach());
    ao.Attach(ShadingPass::preview, MakePipeline(device.Get(), ao.Root(), TestPass::preview).Detach());
    if (!native || !sphere || !ao.HasPipeline(ShadingPass::composite) || !ao.HasPipeline(ShadingPass::preview))
        return 1;

    auto depth_desc = color_desc;
    depth_desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    auto depth = Allocate(device.Get(), D3D12_HEAP_TYPE_DEFAULT, depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    auto color = Allocate(device.Get(), D3D12_HEAP_TYPE_DEFAULT, color_desc, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto preview = Allocate(device.Get(), D3D12_HEAP_TYPE_DEFAULT, color_desc, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto later = Allocate(device.Get(), D3D12_HEAP_TYPE_DEFAULT, color_desc, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto rtvs = MakeHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3);
    auto dsvs = MakeHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
    auto resources = MakeHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3,
                              D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    auto samplers = MakeHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 size = 0;
    device->GetCopyableFootprints(&color_desc, 0, 1, 0, &layout, nullptr, nullptr, &size);
    const UINT64 stride = (size + 511) & ~511ull;
    const D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, stride * 3, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                                     {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    auto readback = Allocate(device.Get(), D3D12_HEAP_TYPE_READBACK, buffer, D3D12_RESOURCE_STATE_COPY_DEST);
    auto constants = Allocate(device.Get(), D3D12_HEAP_TYPE_UPLOAD, buffer, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!depth || !color || !preview || !later || !rtvs || !dsvs || !resources || !samplers || !readback || !constants)
        return 1;

    void *mapped = nullptr;
    if (FAILED(constants->Map(0, nullptr, &mapped)))
        return 1;
    static_cast<float *>(mapped)[0] = static_cast<float *>(mapped)[1] = 1.0f;
    constants->Unmap(0, nullptr);

    auto scene_rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    const auto rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_CPU_DESCRIPTOR_HANDLE preview_rtv{scene_rtv.ptr + rtv_increment};
    const D3D12_CPU_DESCRIPTOR_HANDLE later_rtv{preview_rtv.ptr + rtv_increment};
    device->CreateRenderTargetView(color.Get(), nullptr, scene_rtv);
    device->CreateRenderTargetView(preview.Get(), nullptr, preview_rtv);
    device->CreateRenderTargetView(later.Get(), nullptr, later_rtv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    const auto dsv_handle = dsvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(depth.Get(), &dsv, dsv_handle);

    auto resource_cpu = resources->GetCPUDescriptorHandleForHeapStart();
    const auto resource_gpu = resources->GetGPUDescriptorHandleForHeapStart();
    const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{constants->GetGPUVirtualAddress(), 256};
    for (int i = 0; i < 2; ++i) {
        device->CreateConstantBufferView(&cbv, resource_cpu);
        resource_cpu.ptr += increment;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(depth.Get(), &srv, resource_cpu);
    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&sampler, samplers->GetCPUDescriptorHandleForHeapStart());

    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    const D3D12_COMMAND_QUEUE_DESC queue_desc{};
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), native.Get(),
                                         IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return 1;
    const D3D12_RECT crease{contact_edge, 0, 224, 64};
    const D3D12_RECT silhouette{contact_edge, 64, 224, 128};
    list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, nullptr);
    list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 0.253f, 0, 1, &crease);
    list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 0.7f, 0, 1, &silhouette);
    for (LONG x = 0; x < static_cast<LONG>(extent); ++x) {
        const D3D12_RECT column{x, 128, x + 1, 192};
        list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 0.18f + 0.0012f * x, 0, 1, &column);
    }
    const float scene_color[]{0.8f, 0.6f, 0.4f, 0.25f};
    list->ClearRenderTargetView(scene_rtv, scene_color, 0, nullptr);
    list->ClearRenderTargetView(preview_rtv, scene_color, 0, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, extent, extent, 0, 1};
    const D3D12_RECT scissor{0, 0, extent, extent};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12DescriptorHeap *active_heaps[]{resources.Get(), samplers.Get()};
    list->SetDescriptorHeaps(2, active_heaps);
    const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> tables{
        resource_gpu, {resource_gpu.ptr + 2 * increment}, samplers->GetGPUDescriptorHandleForHeapStart()
    };
    list->SetGraphicsRootSignature(native_root.Get());
    for (UINT i = 0; i < tables.size(); ++i)
        list->SetGraphicsRootDescriptorTable(i, tables[i]);
    const D3D12_RECT sphere_scissor{0, 192, extent, extent};
    list->RSSetScissorRects(1, &sphere_scissor);
    list->OMSetRenderTargets(0, nullptr, FALSE, &dsv_handle);
    list->SetPipelineState(sphere.Get());
    list->DrawInstanced(3, 1, 0, 0);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &scene_rtv, FALSE, nullptr);
    Transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ);
    ao.Bind(ShadingPass::composite, list.Get(), tables[0], tables[1]);
    list->DrawInstanced(3, 1, 0, 0);
    zzz_ao::CharacterAo::Restore(list.Get(), native.Get(), native_root.Get(), tables);
    list->OMSetRenderTargets(1, &preview_rtv, FALSE, nullptr);
    ao.Bind(ShadingPass::preview, list.Get(), tables[0], tables[1]);
    list->DrawInstanced(3, 1, 0, 0);
    zzz_ao::CharacterAo::Restore(list.Get(), native.Get(), native_root.Get(), tables);
    list->OMSetRenderTargets(1, &later_rtv, FALSE, nullptr);
    list->DrawInstanced(3, 1, 0, 0);

    for (ID3D12Resource *result : {color.Get(), preview.Get(), later.Get()}) {
        Transition(list.Get(), result, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const D3D12_TEXTURE_COPY_LOCATION src{result, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        D3D12_TEXTURE_COPY_LOCATION dest{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dest.PlacedFootprint = layout;
        list->CopyTextureRegion(&dest, 0, 0, 0, &src, nullptr);
        layout.Offset += stride;
    }
    if (!Check(list->Close(), "Close command list"))
        return 1;
    ID3D12CommandList *lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence.Get(), 1)))
        return 1;
    for (int i = 0; i < 2000 && fence->GetCompletedValue() == 0; ++i)
        Sleep(5);
    const D3D12_RANGE read{0, static_cast<SIZE_T>(stride * 3)};
    if (fence->GetCompletedValue() != 1 || FAILED(readback->Map(0, &read, &mapped)))
        return 1;
    const auto pixel = [&](UINT x, UINT y, UINT64 offset = 0) {
        return static_cast<const uint8_t *>(mapped) + offset + y * layout.Footprint.RowPitch + x * 4;
    };
    constexpr UINT profile_y = 32;
    const int flat_color = pixel(24, profile_y)[0];
    int previous = flat_color;
    int shade_levels = 0;
    bool falls_off = true;
    for (UINT x = contact_edge - 25; x < contact_edge; ++x) {
        const int value = pixel(x, profile_y)[0];
        if (value < previous)
            ++shade_levels;
        falls_off &= value <= previous && previous - value <= 12;
        previous = value;
    }
    falls_off &= shade_levels >= 8 && pixel(contact_edge - 12, profile_y)[0] < flat_color &&
                 pixel(contact_edge - 1, profile_y)[0] < 175;

    bool neutral = flat_color >= 190 && pixel(contact_edge, profile_y)[0] == flat_color;
    const int tilted_color = pixel(contact_edge, 160)[0];
    for (UINT x = contact_edge - 25; x <= contact_edge + 25; ++x) {
        neutral &= pixel(x, 96)[0] == flat_color;
        neutral &= std::abs(int(pixel(x, 160)[0]) - tilted_color) <= 1;
    }
    constexpr UINT sphere_y = 222;
    bool form_shading = pixel(110, sphere_y)[0] - pixel(144, sphere_y)[0] >= 30 &&
                        pixel(128, 239)[0] - pixel(128, 208)[0] >= 25;
    bool matches_preview = pixel(40, sphere_y)[0] == 204 && pixel(40, sphere_y, stride)[0] == 0;
    int previous_gray = pixel(110, sphere_y, stride)[0];
    for (UINT x = 110; x <= 144; x += 2) {
        const auto *gray = pixel(x, sphere_y, stride);
        form_shading &= gray[0] <= previous_gray + 2 && previous_gray - gray[0] <= 30;
        previous_gray = gray[0];
        const float multiplier = 0.48f + 0.52f * gray[0] / 255.0f;
        const auto *game = pixel(x, sphere_y);
        matches_preview &= gray[0] == gray[1] && gray[0] == gray[2] && gray[3] == 64 &&
                           std::abs(game[0] - 204.0f * multiplier) < 1.6f &&
                           std::abs(game[1] - 153.0f * multiplier) < 1.6f &&
                           std::abs(game[2] - 102.0f * multiplier) < 1.6f;
    }
    const bool blend_and_restore = pixel(contact_edge - 1, profile_y)[3] == 64 &&
                                   pixel(4, profile_y, 2 * stride)[0] == 64 &&
                                   pixel(4, profile_y, 2 * stride)[1] == 128 &&
                                   pixel(4, profile_y, 2 * stride)[2] == 191;
    if (!falls_off) {
        std::cerr << "Unexpected contact profile.";
        for (UINT x = contact_edge - 25; x < contact_edge; ++x)
            std::cerr << ' ' << int(pixel(x, profile_y)[0]);
        std::cerr << '\n';
    }
    const D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    if (!neutral)
        std::cerr << "Occlusion leaked across a silhouette or shading varied along a plane.\n";
    if (!form_shading)
        std::cerr << "The sphere did not have a gradual directional shading profile.\n";
    if (!matches_preview)
        std::cerr << "Grayscale preview disagreed with the shading applied to scene color.\n";
    if (!blend_and_restore)
        std::cerr << "Unexpected scene-color alpha or restored draw.\n";
    if (!falls_off || !neutral || !form_shading || !matches_preview || !blend_and_restore)
        return 1;
    ComPtr<ID3D12InfoQueue> info;
    if (SUCCEEDED(device.As(&info))) {
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            if (FAILED(info->GetMessage(i, nullptr, &size)))
                return 1;
            auto bytes = std::make_unique<char[]>(size);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(bytes.get());
            if (FAILED(info->GetMessage(i, message, &size)))
                return 1;
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::cerr << message->pDescription << '\n';
                return 1;
            }
        }
    }
    std::cout << "Verified sphere shading, preview parity, AO falloff, alpha and graphics state through D3D12 WARP.\n";
    return 0;
}
