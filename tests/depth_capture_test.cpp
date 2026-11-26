#include "depth_capture.hpp"

#include <dxgi1_4.h>
#include <d3d12sdklayers.h>

#include <iostream>
#include <string>
#include <string_view>

using Microsoft::WRL::ComPtr;

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
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::cerr << "D3D12 WARP adapter unavailable.\n";
        return 77;
    }

    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    const D3D12_COMMAND_QUEUE_DESC queue_desc{};
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list))))
        return 1;

    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                    D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16, 8, 1, 1,
                                   DXGI_FORMAT_R32G8X24_TYPELESS, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    ComPtr<ID3D12Resource> early, late;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&early))) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&late))))
        return 1;

    ComPtr<ID3D12DescriptorHeap> descriptors;
    const D3D12_DESCRIPTOR_HEAP_DESC descriptor_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2,
                                                     D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device->CreateDescriptorHeap(&descriptor_desc, IID_PPV_ARGS(&descriptors))))
        return 1;
    auto early_view = descriptors->GetCPUDescriptorHandleForHeapStart();
    auto late_view = early_view;
    late_view.ptr += device->GetDescriptorHandleIncrementSize(descriptor_desc.Type);
    D3D12_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(early.Get(), &view, early_view);
    device->CreateDepthStencilView(late.Get(), &view, late_view);
    list->ClearDepthStencilView(early_view, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, nullptr);
    list->ClearDepthStencilView(late_view, D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, 0, nullptr);

    D3D12_RESOURCE_BARRIER transition{};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {early.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &transition);
    auto job = zzz_ao::DepthCapture::Record(list.Get(), early.Get(), late.Get());
    if (!job) {
        std::cerr << "Could not record depth captures.\n";
        return 1;
    }
    if (FAILED(list->Close()) || job->Ready())
        return 1;
    job->Submitted(queue.Get());
    ID3D12CommandList *lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    job->FinishPresent();
    for (int n = 0; !job->Ready() && n < 2000; ++n)
        Sleep(5);

    const auto actual = job->ReadRows();
    const auto expected_row = [](std::string_view stage, std::string_view sample) {
        std::string row = "character_depth\t" + std::string(stage) + "\t8\t4\t";
        for (int i = 0; i < 32; ++i)
            row += sample;
        return row + "\n";
    };
    if (actual != expected_row("before_ao", "0000803e") + expected_row("before_upscale", "0000403f")) {
        std::cerr << "Unexpected encoded depth samples.\n" << actual;
        return 1;
    }

    ComPtr<ID3D12InfoQueue> info;
    if (SUCCEEDED(device.As(&info))) {
        const auto count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0;
            if (FAILED(info->GetMessage(i, nullptr, &size)))
                return 1;
            const auto data = std::make_unique<char[]>(size);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(data.get());
            if (FAILED(info->GetMessage(i, message, &size)))
                return 1;
            if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                std::cerr << message->pDescription << '\n';
                return 1;
            }
        }
    }
    std::cout << "Read back both native depth planes through D3D12 WARP.\n";
    return 0;
}
