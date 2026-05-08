#include "render/d3d/dx12_vtable.h"

#include "core/common.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

namespace radial_spell_menu::dx12_vtable {

bool DiscoverHookTargets(HookTargets& targets)
{
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"RSM_Dummy";
    RegisterClassExW(&window_class);

    HWND dummy_window = CreateWindowExW(0, L"RSM_Dummy", L"", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr,
        window_class.hInstance, nullptr);
    if (!dummy_window) {
        Log("CreateWindowEx failed");
        return false;
    }

    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        Log("D3D12CreateDevice failed");
        DestroyWindow(dummy_window);
        return false;
    }

    ID3D12CommandQueue* command_queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue));

    IDXGIFactory4* factory = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{};
    swap_chain_desc.Width = 8;
    swap_chain_desc.Height = 8;
    swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.SampleDesc = {1, 0};
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = 2;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swap_chain = nullptr;
    HRESULT result = factory->CreateSwapChainForHwnd(command_queue, dummy_window, &swap_chain_desc, nullptr, nullptr,
        &swap_chain);
    if (FAILED(result)) {
        Log("Dummy CreateSwapChainForHwnd failed: 0x%08X", static_cast<unsigned>(result));
        command_queue->Release();
        device->Release();
        factory->Release();
        DestroyWindow(dummy_window);
        return false;
    }

    void** swap_chain_vtable = *reinterpret_cast<void***>(swap_chain);
    void** queue_vtable = *reinterpret_cast<void***>(command_queue);
    targets.present = swap_chain_vtable[8];
    targets.execute_command_lists = queue_vtable[10];

    swap_chain->Release();
    factory->Release();
    command_queue->Release();
    device->Release();
    DestroyWindow(dummy_window);
    UnregisterClassW(L"RSM_Dummy", GetModuleHandleW(nullptr));
    return targets.present != nullptr && targets.execute_command_lists != nullptr;
}

}  // namespace radial_spell_menu::dx12_vtable
