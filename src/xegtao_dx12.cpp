#include "xegtao_dx12.hpp"

#include <wrl/client.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <vector>

#include "XeGTAO.h"

namespace zzz_ao {
namespace {

using Microsoft::WRL::ComPtr;
constexpr auto shader_read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr uint32_t max_extent = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
constexpr uint32_t table_size = 8; // t0–t2 and u0–u4.

enum Table : uint32_t { prefilter, horizon, blur_ab, blur_ba, final_a, final_b, table_count };
enum Shader : uint32_t { prefilter_cs, horizon_low_cs, horizon_medium_cs, horizon_high_cs,
                         horizon_ultra_cs, blur_cs, final_cs, shader_count };
constexpr std::array<const char *, shader_count> shader_names{
    "Prefilter", "HorizonLow", "HorizonMedium", "HorizonHigh", "HorizonUltra", "Denoise", "DenoiseFinal",
};

struct Constants {
    XeGTAO::GTAOConstants gtao;
    uint32_t active_width;
    uint32_t active_height;
    uint32_t origin_x;
    uint32_t origin_y;
    float intensity;
    float fade_start;
    float fade_end;
    float background_depth;
};

static_assert(sizeof(XeGTAO::GTAOConstants) == 96);
static_assert(sizeof(Constants) == 128);
static_assert(offsetof(Constants, intensity) == 112);

struct Slot {
    ComPtr<ID3D12Resource> output;
    bool has_timing = false;
};

uint32_t padded(uint32_t extent)
{
    return (extent + 15) & ~15u;
}

D3D12_RESOURCE_DESC texture_desc(uint32_t width, uint32_t height, DXGI_FORMAT format, uint16_t mips)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = mips;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return desc;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES from,
                                 D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    return barrier;
}

bool format_supported(ID3D12Device *device, DXGI_FORMAT format, D3D12_FORMAT_SUPPORT1 reads,
                      bool typed_store)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
           (support.Support1 & reads) == reads &&
           (!typed_store || (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0);
}

HRESULT load_pipeline(ID3D12Device *device, ID3D12RootSignature *root,
                      const std::filesystem::path &path, ID3D12PipelineState **pipeline)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    const auto size = file.tellg();
    if (size <= 0 || size > 4 * 1024 * 1024)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    std::vector<char> bytecode(static_cast<size_t>(size));
    if (!file.seekg(0).read(bytecode.data(), static_cast<std::streamsize>(size)))
        return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.CS = {bytecode.data(), bytecode.size()};
    return device->CreateComputePipelineState(&desc, IID_PPV_ARGS(pipeline));
}

bool rect_fits(const D3D12_RESOURCE_DESC &desc, const Frame &frame)
{
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1 &&
           desc.SampleDesc.Count == 1 && !(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
           uint64_t(frame.x) + frame.width <= desc.Width &&
           uint64_t(frame.y) + frame.height <= desc.Height;
}

bool valid_depth_srv(DXGI_FORMAT resource, DXGI_FORMAT srv)
{
    switch (srv) {
    case DXGI_FORMAT_R32_FLOAT:
        return resource == DXGI_FORMAT_R32_FLOAT || resource == DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return resource == DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return resource == DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_R16_UNORM:
        return resource == DXGI_FORMAT_R16_UNORM || resource == DXGI_FORMAT_R16_TYPELESS;
    default:
        return false;
    }
}

bool valid_normal_srv(DXGI_FORMAT resource, DXGI_FORMAT srv)
{
    switch (srv) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
        return resource == srv || resource == DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        return resource == srv || resource == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return resource == srv || resource == DXGI_FORMAT_R32G32B32A32_TYPELESS;
    case DXGI_FORMAT_R32G32B32_FLOAT:
        return resource == srv || resource == DXGI_FORMAT_R32G32B32_TYPELESS;
    default:
        return false;
    }
}

bool valid_projection(const Projection &p)
{
    return std::isfinite(p.depth_numerator) && p.depth_numerator != 0.0f &&
           std::isfinite(p.depth_bias) && std::isfinite(p.background_depth) &&
           p.background_depth >= 0.0f && p.background_depth <= 1.0f &&
           std::isfinite(p.uv_to_view_scale[0]) && p.uv_to_view_scale[0] != 0.0f &&
           std::isfinite(p.uv_to_view_scale[1]) && p.uv_to_view_scale[1] != 0.0f &&
           std::isfinite(p.uv_to_view_offset[0]) && std::isfinite(p.uv_to_view_offset[1]);
}

bool valid_settings(const Settings &s)
{
    return uint32_t(s.quality) <= uint32_t(Quality::ultra) && s.denoise_passes <= 3 &&
           std::isfinite(s.intensity) && s.intensity >= 0.0f &&
           std::isfinite(s.radius) && s.radius > 0.0f &&
           std::isfinite(s.falloff) && s.falloff > 0.0f && s.falloff <= 1.0f &&
           std::isfinite(s.fade_start) && s.fade_start >= 0.0f &&
           std::isfinite(s.fade_end) && s.fade_end > s.fade_start;
}

Constants constants(const Frame &frame, const Settings &settings, uint32_t width, uint32_t height)
{
    const auto &p = frame.projection;
    Constants result{};
    auto &g = result.gtao;
    g.ViewportSize = {static_cast<int>(width), static_cast<int>(height)};
    g.ViewportPixelSize = {1.0f / float(width), 1.0f / float(height)};
    g.DepthUnpackConsts = {p.depth_numerator, p.depth_bias};
    g.CameraTanHalfFOV = {std::abs(p.uv_to_view_scale[0] * 0.5f), std::abs(p.uv_to_view_scale[1] * 0.5f)};
    // Gather coordinates use the full padded storage; camera rays use the active extent.
    g.NDCToViewMul = {p.uv_to_view_scale[0] * float(width) / float(frame.width),
                      p.uv_to_view_scale[1] * float(height) / float(frame.height)};
    g.NDCToViewAdd = {p.uv_to_view_offset[0], p.uv_to_view_offset[1]};
    g.NDCToViewMul_x_PixelSize = {p.uv_to_view_scale[0] / float(frame.width),
                                 p.uv_to_view_scale[1] / float(frame.height)};
    g.EffectRadius = settings.radius;
    g.EffectFalloffRange = settings.falloff;
    g.RadiusMultiplier = XE_GTAO_DEFAULT_RADIUS_MULTIPLIER;
    g.FinalValuePower = XE_GTAO_DEFAULT_FINAL_VALUE_POWER;
    g.DenoiseBlurBeta = settings.denoise_passes != 0 ? 1.2f : 10000.0f;
    g.SampleDistributionPower = XE_GTAO_DEFAULT_SAMPLE_DISTRIBUTION_POWER;
    g.ThinOccluderCompensation = XE_GTAO_DEFAULT_THIN_OCCLUDER_COMPENSATION;
    g.DepthMIPSamplingOffset = XE_GTAO_DEFAULT_DEPTH_MIP_SAMPLING_OFFSET;
    g.NoiseIndex = static_cast<int>(frame.temporal_index % 64);
    result.active_width = frame.width;
    result.active_height = frame.height;
    result.origin_x = frame.x;
    result.origin_y = frame.y;
    result.intensity = settings.intensity;
    result.fade_start = settings.fade_start;
    result.fade_end = settings.fade_end;
    result.background_depth = p.background_depth;
    return result;
}

} // namespace

struct XeGtaoDx12::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, shader_count> shaders;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> timestamps;
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12Resource> edges;
    ComPtr<ID3D12Resource> work_a;
    ComPtr<ID3D12Resource> work_b;
    std::vector<Slot> slots;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t increment = 0;
    uint64_t texture_bytes = 0;

    uint64_t offset(uint32_t slot, Table table) const
    {
        return (slot * table_count + table) * table_size * uint64_t(increment);
    }

    void srv(uint32_t slot, Table table, uint32_t reg, ID3D12Resource *texture,
             DXGI_FORMAT format, uint32_t mips = 1) const
    {
        auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += offset(slot, table) + reg * uint64_t(increment);
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = mips;
        device->CreateShaderResourceView(texture, &desc, handle);
    }

    void uav_view(uint32_t slot, Table table, uint32_t reg, ID3D12Resource *texture,
                  DXGI_FORMAT format, uint32_t mip = 0) const
    {
        auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += offset(slot, table) + (3 + reg) * uint64_t(increment);
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = mip;
        device->CreateUnorderedAccessView(texture, nullptr, &desc, handle);
    }

    HRESULT make_texture(DXGI_FORMAT format, uint16_t mips, D3D12_RESOURCE_STATES state,
                         ID3D12Resource **texture)
    {
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                        D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        const auto desc = texture_desc(width, height, format, mips);
        const auto bytes = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        if (bytes == UINT64_MAX)
            return E_INVALIDARG;
        const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                            state, nullptr, IID_PPV_ARGS(texture));
        if (SUCCEEDED(hr))
            texture_bytes += bytes;
        return hr;
    }

    HRESULT initialize(const std::filesystem::path &shader_path)
    {
        constexpr auto gathered = D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_GATHER;
        if (!format_supported(device.Get(), DXGI_FORMAT_R32_FLOAT, gathered | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE, true) ||
            !format_supported(device.Get(), DXGI_FORMAT_R8_UNORM, gathered, true) ||
            !format_supported(device.Get(), DXGI_FORMAT_R8_UINT, gathered, true))
            return DXGI_ERROR_UNSUPPORTED;

        const D3D12_DESCRIPTOR_RANGE ranges[]{
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 0, 0, 3},
        };
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0, 0, sizeof(Constants) / sizeof(uint32_t)};
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = {2, ranges};

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;

        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 2;
        signature.pParameters = parameters;
        signature.NumStaticSamplers = 1;
        signature.pStaticSamplers = &sampler;

        ComPtr<ID3DBlob> root_bytes;
        HRESULT hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                                &root_bytes, nullptr);
        if (FAILED(hr))
            return hr;
        hr = device->CreateRootSignature(0, root_bytes->GetBufferPointer(), root_bytes->GetBufferSize(),
                                         IID_PPV_ARGS(&root));
        if (FAILED(hr))
            return hr;

        for (uint32_t i = 0; i < shader_count; ++i) {
            hr = load_pipeline(device.Get(), root.Get(), shader_path / (std::string(shader_names[i]) + ".dxil"),
                               shaders[i].ReleaseAndGetAddressOf());
            if (FAILED(hr))
                return hr;
        }

        const uint32_t slot_count = static_cast<uint32_t>(slots.size());
        const D3D12_DESCRIPTOR_HEAP_DESC descriptor_desc{
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, slot_count * table_count * table_size,
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0,
        };
        hr = device->CreateDescriptorHeap(&descriptor_desc, IID_PPV_ARGS(&descriptors));
        if (FAILED(hr))
            return hr;
        increment = device->GetDescriptorHandleIncrementSize(descriptor_desc.Type);

        const D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, slot_count * 2, 0};
        hr = device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries));
        if (FAILED(hr))
            return hr;

        D3D12_RESOURCE_DESC readback_desc{};
        readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_desc.Width = slot_count * 2 * sizeof(uint64_t);
        readback_desc.Height = 1;
        readback_desc.DepthOrArraySize = 1;
        readback_desc.MipLevels = 1;
        readback_desc.SampleDesc.Count = 1;
        readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                                 D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        hr = device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&timestamps));
        if (FAILED(hr))
            return hr;

        // The same DIRECT queue serializes the scratch passes across slots.
        hr = make_texture(DXGI_FORMAT_R32_FLOAT, 5, shader_read, &depth);
        if (SUCCEEDED(hr))
            hr = make_texture(DXGI_FORMAT_R8_UNORM, 1, shader_read, &edges);
        if (SUCCEEDED(hr))
            hr = make_texture(DXGI_FORMAT_R8_UINT, 1, shader_read, &work_a);
        if (SUCCEEDED(hr))
            hr = make_texture(DXGI_FORMAT_R8_UINT, 1, shader_read, &work_b);
        if (FAILED(hr))
            return hr;

        for (uint32_t i = 0; i < slot_count; ++i) {
            hr = initialize_slot(i);
            if (FAILED(hr))
                return hr;
        }
        return S_OK;
    }

    HRESULT initialize_slot(uint32_t i)
    {
        auto &slot = slots[i];
        const HRESULT hr = make_texture(DXGI_FORMAT_R8_UINT, 1, output_state, &slot.output);
        if (FAILED(hr))
            return hr;

        // Root signature 1.0 can require even unused table entries to be initialized.
        for (uint32_t table = 0; table < table_count; ++table) {
            for (uint32_t reg = 0; reg < 3; ++reg)
                srv(i, static_cast<Table>(table), reg, nullptr, DXGI_FORMAT_R32_FLOAT);
            for (uint32_t reg = 0; reg < 5; ++reg)
                uav_view(i, static_cast<Table>(table), reg, nullptr, DXGI_FORMAT_R32_FLOAT);
        }

        for (uint32_t mip = 0; mip < 5; ++mip)
            uav_view(i, prefilter, mip, depth.Get(), DXGI_FORMAT_R32_FLOAT, mip);
        srv(i, horizon, 0, depth.Get(), DXGI_FORMAT_R32_FLOAT, 5);
        uav_view(i, horizon, 0, work_a.Get(), DXGI_FORMAT_R8_UINT);
        uav_view(i, horizon, 1, edges.Get(), DXGI_FORMAT_R8_UNORM);
        for (Table table : {blur_ab, blur_ba, final_a, final_b})
            srv(i, table, 1, edges.Get(), DXGI_FORMAT_R8_UNORM);
        srv(i, blur_ab, 0, work_a.Get(), DXGI_FORMAT_R8_UINT);
        srv(i, blur_ba, 0, work_b.Get(), DXGI_FORMAT_R8_UINT);
        uav_view(i, blur_ab, 0, work_b.Get(), DXGI_FORMAT_R8_UINT);
        uav_view(i, blur_ba, 0, work_a.Get(), DXGI_FORMAT_R8_UINT);
        srv(i, final_a, 0, work_a.Get(), DXGI_FORMAT_R8_UINT);
        srv(i, final_b, 0, work_b.Get(), DXGI_FORMAT_R8_UINT);
        for (Table table : {final_a, final_b})
            uav_view(i, table, 0, slot.output.Get(), DXGI_FORMAT_R8_UINT);
        return S_OK;
    }

    void run(ID3D12GraphicsCommandList *list, uint32_t slot, Table table,
             uint32_t shader, uint32_t x, uint32_t y, D3D12_RESOURCE_BARRIER *barriers,
             uint32_t barrier_count) const
    {
        auto handle = descriptors->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += offset(slot, table);
        list->ResourceBarrier(barrier_count, barriers);
        list->SetPipelineState(shaders[shader].Get());
        list->SetComputeRootDescriptorTable(1, handle);
        list->Dispatch(x, y, 1);
        for (uint32_t i = 0; i < barrier_count; ++i)
            std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
        list->ResourceBarrier(barrier_count, barriers);
    }
};

HRESULT XeGtaoDx12::Create(ID3D12Device *device, const std::filesystem::path &shaders,
                           uint32_t width, uint32_t height, uint32_t frame_slots,
                           std::unique_ptr<XeGtaoDx12> &result)
{
    if (!device || width == 0 || width > max_extent || height == 0 || height > max_extent ||
        frame_slots == 0 || frame_slots > 8)
        return E_INVALIDARG;
    try {
        auto impl = std::make_unique<Impl>();
        impl->device = device;
        impl->width = padded(width);
        impl->height = padded(height);
        impl->slots.resize(frame_slots);
        const HRESULT hr = impl->initialize(shaders);
        if (FAILED(hr))
            return hr;
        result.reset(new XeGtaoDx12(std::move(impl)));
        return S_OK;
    } catch (const std::bad_alloc &) {
        return E_OUTOFMEMORY;
    }
}

XeGtaoDx12::XeGtaoDx12(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
XeGtaoDx12::~XeGtaoDx12() = default;

HRESULT XeGtaoDx12::Record(ID3D12GraphicsCommandList *list, uint32_t i, const Frame &frame,
                            const Settings &settings)
{
    if (!settings.enabled || settings.intensity == 0.0f)
        return S_FALSE;
    auto &impl = *impl_;
    if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || i >= impl.slots.size() ||
        !frame.depth || frame.width == 0 || frame.width > max_extent ||
        frame.height == 0 || frame.height > max_extent ||
        padded(frame.width) != impl.width || padded(frame.height) != impl.height ||
        !valid_projection(frame.projection) || !valid_settings(settings))
        return E_INVALIDARG;

    const auto depth_desc = frame.depth->GetDesc();
    if (!rect_fits(depth_desc, frame) || !valid_depth_srv(depth_desc.Format, frame.depth_srv_format))
        return E_INVALIDARG;

    DXGI_FORMAT normal_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (frame.view_normals) {
        const auto desc = frame.view_normals->GetDesc();
        if (!rect_fits(desc, frame) || !valid_normal_srv(desc.Format, frame.normal_srv_format))
            return E_INVALIDARG;
        normal_format = frame.normal_srv_format;
    }

    const auto config = constants(frame, settings, impl.width, impl.height);
    impl.srv(i, prefilter, 0, frame.depth, frame.depth_srv_format);
    impl.srv(i, horizon, 1, frame.view_normals, normal_format);
    impl.srv(i, final_a, 2, frame.depth, frame.depth_srv_format);
    impl.srv(i, final_b, 2, frame.depth, frame.depth_srv_format);

    auto &slot = impl.slots[i];
    auto *heap = impl.descriptors.Get();
    list->EndQuery(impl.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2);
    list->SetDescriptorHeaps(1, &heap);
    list->SetComputeRootSignature(impl.root.Get());
    list->SetComputeRoot32BitConstants(0, sizeof(config) / sizeof(uint32_t), &config, 0);

    auto prefilter_barrier = transition(impl.depth.Get(), shader_read, uav);
    impl.run(list, i, prefilter, prefilter_cs, impl.width / 16, impl.height / 16, &prefilter_barrier, 1);

    D3D12_RESOURCE_BARRIER horizon_barriers[]{
        transition(impl.work_a.Get(), shader_read, uav), transition(impl.edges.Get(), shader_read, uav),
    };
    impl.run(list, i, horizon, horizon_low_cs + uint32_t(settings.quality), impl.width / 8,
             impl.height / 8, horizon_barriers, 2);

    if (settings.denoise_passes >= 2) {
        auto blur_barrier = transition(impl.work_b.Get(), shader_read, uav);
        impl.run(list, i, blur_ab, blur_cs, impl.width / 16, impl.height / 8, &blur_barrier, 1);
    }
    if (settings.denoise_passes == 3) {
        auto blur_barrier = transition(impl.work_a.Get(), shader_read, uav);
        impl.run(list, i, blur_ba, blur_cs, impl.width / 16, impl.height / 8, &blur_barrier, 1);
    }

    auto final_barrier = transition(slot.output.Get(), output_state, uav);
    impl.run(list, i, settings.denoise_passes == 2 ? final_b : final_a, final_cs,
             impl.width / 16, impl.height / 8, &final_barrier, 1);

    list->EndQuery(impl.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2 + 1);
    list->ResolveQueryData(impl.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2, 2,
                           impl.timestamps.Get(), i * 2 * sizeof(uint64_t));
    slot.has_timing = true;
    return S_OK;
}

ID3D12Resource *XeGtaoDx12::Output(uint32_t slot) const
{
    return slot < impl_->slots.size() ? impl_->slots[slot].output.Get() : nullptr;
}

HRESULT XeGtaoDx12::ReadMilliseconds(uint32_t slot, uint64_t frequency, double &milliseconds) const
{
    if (slot >= impl_->slots.size() || frequency == 0)
        return E_INVALIDARG;
    if (!impl_->slots[slot].has_timing)
        return S_FALSE;
    const size_t first = slot * 2;
    const D3D12_RANGE read_range{first * sizeof(uint64_t), (first + 2) * sizeof(uint64_t)};
    void *data = nullptr;
    const HRESULT hr = impl_->timestamps->Map(0, &read_range, &data);
    if (FAILED(hr))
        return hr;
    const auto *ticks = static_cast<const uint64_t *>(data);
    const auto start = ticks[first];
    const auto end = ticks[first + 1];
    const D3D12_RANGE written_range{0, 0};
    impl_->timestamps->Unmap(0, &written_range);
    if (end < start)
        return E_FAIL;
    milliseconds = double(end - start) * 1000.0 / double(frequency);
    return S_OK;
}

uint64_t XeGtaoDx12::ResidentTextureBytes() const
{
    return impl_->texture_bytes;
}

} // namespace zzz_ao
