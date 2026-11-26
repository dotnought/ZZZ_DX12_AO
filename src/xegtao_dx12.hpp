#pragma once

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace zzz_ao {

enum class Quality : uint32_t { low, medium, high, ultra };

struct Settings {
    bool enabled = true;
    float intensity = 1.35f;
    float radius = 0.55f;
    float falloff = 0.615f;
    float fade_start = 40.0f;
    float fade_end = 80.0f;
    Quality quality = Quality::medium;
    uint32_t denoise_passes = 1; // 0 bypasses filtering, 1–3 control spatial blur.
};

struct Projection {
    // Positive view Z = depth_numerator / (depth_bias - deviceZ).
    // With no jitter a symmetric perspective camera uses {2tanX,-2tanY}
    // and {-tanX,tanY}. Include any off-axis offset and current-frame jitter.
    float depth_numerator;
    float depth_bias;
    std::array<float, 2> uv_to_view_scale;
    std::array<float, 2> uv_to_view_offset;
    float background_depth;
};

struct Frame {
    ID3D12Resource *depth = nullptr;
    DXGI_FORMAT depth_srv_format = DXGI_FORMAT_UNKNOWN;
    // Optional signed unit normals in positive-Z view coordinates, sharing
    // the depth texture's rect. Use a float or SNORM SRV.
    ID3D12Resource *view_normals = nullptr;
    DXGI_FORMAT normal_srv_format = DXGI_FORMAT_UNKNOWN;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    Projection projection{};
    uint32_t temporal_index = 0; // Advance on real frames only, when a temporal consumer is active.
};

class XeGtaoDx12 {
public:
    static HRESULT Create(ID3D12Device *device, const std::filesystem::path &shaders,
                          uint32_t width, uint32_t height, uint32_t frame_slots,
                          std::unique_ptr<XeGtaoDx12> &result);

    ~XeGtaoDx12();
    XeGtaoDx12(const XeGtaoDx12 &) = delete;
    XeGtaoDx12 &operator=(const XeGtaoDx12 &) = delete;

    // Use dedicated DIRECT command lists submitted serially to the same render
    // queue before composition. All slots share scratch textures on that queue.
    // Inputs must already allow NON_PIXEL_SHADER_RESOURCE and remain alive until
    // the submit fence completes. The caller owns fence/slot reuse. Only reuse
    // a slot after its GPU work and consumers complete, and retire all slots
    // before replacing or releasing the backend.
    // S_FALSE means AO is disabled for this frame and no commands were recorded.
    // The active extent can vary within the allocated 16-pixel padded size.
    HRESULT Record(ID3D12GraphicsCommandList *list, uint32_t slot, const Frame &frame,
                   const Settings &settings);

    // Same queue/lifetime contract as Record. Overwrites a single-sample R8
    // UNORM (or typeless R8) target matching the active extent. Record outside
    // a render pass, after native mask production and before its consumers.
    // The target starts and ends in RENDER_TARGET. Keep it alive through
    // completion. GPU timing includes the copy into the game's mask.
    HRESULT RecordToNativeMask(ID3D12GraphicsCommandList *list, uint32_t slot, const Frame &frame,
                               const Settings &settings, ID3D12Resource *native_mask);

    // DXGI_FORMAT_R8_UINT (visibility / 255). The visible rect starts at (0,0)
    // and is frame.width by frame.height. The resource returns to output_state.
    static constexpr auto output_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource *Output(uint32_t slot) const;

    // Read after the slot's submission fence. Pass frequency for that same queue.
    HRESULT ReadMilliseconds(uint32_t slot, uint64_t queue_frequency, double &milliseconds) const;
    uint64_t ResidentTextureBytes() const;

private:
    struct Impl;
    explicit XeGtaoDx12(std::unique_ptr<Impl> impl);
    HRESULT RecordInternal(ID3D12GraphicsCommandList *list, uint32_t slot, const Frame &frame,
                           const Settings &settings, ID3D12Resource *native_mask);
    std::unique_ptr<Impl> impl_;
};

} // namespace zzz_ao
