#include "depth_capture.hpp"

#include <array>
#include <sstream>

namespace zzz_ao {
namespace {

using Microsoft::WRL::ComPtr;
constexpr GUID depth_capture_guid{
    0x0e12afc2, 0xdbd4, 0x4947, {0xbf, 0xcc, 0x2a, 0xe3, 0xe7, 0x7a, 0x46, 0x51},
};
constexpr uint32_t sample_spacing = 2;

void CopyDepth(ID3D12GraphicsCommandList *list, ID3D12Resource *depth, ID3D12Resource *readback,
               const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint)
{
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = depth;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0; // Depth plane of R32G8X24.
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
}

} // namespace

std::shared_ptr<DepthCapture> DepthCapture::Record(ID3D12GraphicsCommandList *list,
                                                   ID3D12Resource *early, ID3D12Resource *late)
{
    if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        !early || !late || early == late)
        return {};
    const auto desc = early->GetDesc();
    const auto late_desc = late->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS ||
        late_desc.Format != desc.Format || late_desc.Dimension != desc.Dimension ||
        late_desc.Width != desc.Width || late_desc.Height != desc.Height ||
        desc.Width == 0 || desc.Width > 4096 || desc.Height == 0 || desc.Height > 2160 ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
        late_desc.DepthOrArraySize != 1 || late_desc.MipLevels != 1 || late_desc.SampleDesc.Count != 1)
        return {};

    ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
        return {};
    auto job = std::make_shared<DepthCapture>();
    uint32_t rows = 0;
    uint64_t row_bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &job->first_, &rows, &row_bytes, nullptr);
    if (rows != desc.Height || row_bytes % desc.Width != 0 || job->first_.Footprint.RowPitch < row_bytes)
        return {};
    const auto format = job->first_.Footprint.Format;
    if (format != DXGI_FORMAT_R32_FLOAT && format != DXGI_FORMAT_R32_TYPELESS &&
        format != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)
        return {};
    job->bytes_per_pixel_ = static_cast<uint32_t>(row_bytes / desc.Width);
    if (job->bytes_per_pixel_ != 4 && job->bytes_per_pixel_ != 8)
        return {};
    const uint64_t footprint_bytes = uint64_t(job->first_.Footprint.RowPitch) * rows;
    constexpr uint64_t alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    job->stride_ = (footprint_bytes + alignment - 1) & ~(alignment - 1);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = job->stride_ * 2;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&job->readback_))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&job->fence_))) ||
        FAILED(list->SetPrivateDataInterface(depth_capture_guid, job->readback_.Get())))
        return {};

    job->list_ = list;
    CopyDepth(list, early, job->readback_.Get(), job->first_);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {late, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    auto second = job->first_;
    second.Offset += job->stride_;
    CopyDepth(list, late, job->readback_.Get(), second);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    list->ResourceBarrier(1, &barrier);
    return job;
}

void DepthCapture::Submitted(ID3D12CommandQueue *queue)
{
    if (queue_.Get() != nullptr && queue_.Get() != queue) {
        mismatched_queue_ = true;
        return;
    }
    queue_ = queue;
    submission_thread_ = GetCurrentThreadId();
    ++submission_;
}

void DepthCapture::FinishPresent()
{
    // The execute event fires before the native submission. A later present
    // callback on that same CPU thread is after the submission call returned.
    if (!mismatched_queue_ && signalled_ < submission_ && submission_thread_ == GetCurrentThreadId() &&
        SUCCEEDED(queue_->Signal(fence_.Get(), submission_)))
        signalled_ = submission_;
}

bool DepthCapture::Ready() const
{
    if (mismatched_queue_ || submission_ == 0 || signalled_ != submission_)
        return false;
    return fence_->GetCompletedValue() == submission_;
}

std::string DepthCapture::ReadRows() const
{
    if (!Ready())
        return "character_depth_status\treadback_pending\n";

    const auto &footprint = first_.Footprint;
    const uint32_t width = (footprint.Width + sample_spacing - 1) / sample_spacing;
    const uint32_t height = (footprint.Height + sample_spacing - 1) / sample_spacing;
    std::array<std::string, 2> samples;
    for (auto &sample : samples)
        sample.resize(size_t(width) * height * 8);

    const D3D12_RANGE read{0, static_cast<SIZE_T>(stride_ * 2)};
    void *mapped = nullptr;
    if (FAILED(readback_->Map(0, &read, &mapped)))
        return "character_depth_status\tmap_failed\n";

    const auto *data = static_cast<const uint8_t *>(mapped);
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < samples.size(); ++i) {
        auto *output = samples[i].data();
        for (uint32_t y = 0; y < footprint.Height; y += sample_spacing) {
            const auto *row = data + first_.Offset + i * stride_ + uint64_t(y) * footprint.RowPitch;
            for (uint32_t x = 0; x < footprint.Width; x += sample_spacing) {
                const auto *pixel = row + size_t(x) * bytes_per_pixel_;
                for (size_t b = 0; b < sizeof(float); ++b) {
                    *output++ = hex[pixel[b] >> 4];
                    *output++ = hex[pixel[b] & 15];
                }
            }
        }
    }
    const D3D12_RANGE written{0, 0};
    readback_->Unmap(0, &written);

    std::ostringstream result;
    result << "character_depth\tbefore_ao\t" << width << '\t' << height << '\t' << samples[0] << '\n'
           << "character_depth\tbefore_upscale\t" << width << '\t' << height << '\t' << samples[1] << '\n';
    return std::move(result).str();
}

} // namespace zzz_ao
