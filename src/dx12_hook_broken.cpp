#include "dx12_hook.h"

#include "common.h"
#include "input_hook.h"
#include "radial_menu.h"
#include "spell_manager.h"

#include <MinHook.h>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <cstdint>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace radial_spell_menu::dx12_hook {

namespace {

// ── Type aliases ──────────────────────────────────────────────────────────
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

// ── DX12 SwapChain vtable indices ─────────────────────────────────────────
constexpr UINT kSwapChainVtableIndexPresent = 8;

// ── Per-frame bookkeeping ─────────────────────────────────────────────────
struct FrameContext {
    ID3D12CommandAllocator* command_allocator = nullptr;
    ID3D12Resource* render_target = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = {};
    UINT64 fence_value = 0;
};

// ── All mutable D3D12 / ImGui state in one place ──────────────────────────
struct Dx12State {
    bool present_hook_installed = false;
    bool imgui_initialized = false;
    HWND window = nullptr;
    IDXGISwapChain3* swap_chain = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* command_queue = nullptr;
    ID3D12GraphicsCommandList* command_list = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    UINT rtv_descriptor_size = 0;
    DXGI_FORMAT rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT64 next_fence_value = 1;
    std::vector<FrameContext> frames;
} g_dx12;

PresentFn g_original_present = nullptr;
LPVOID g_present_target = nullptr;

WNDPROC g_original_wndproc = nullptr;

// ── Helpers ───────────────────────────────────────────────────────────────

void WaitForLastFrame()
{
    if (g_dx12.fence == nullptr || g_dx12.fence_event == nullptr) {
        return;
    }

    const UINT64 fence_value = g_dx12.next_fence_value++;
    g_dx12.command_queue->Signal(g_dx12.fence, fence_value);
    if (g_dx12.fence->GetCompletedValue() < fence_value) {
        g_dx12.fence->SetEventOnCompletion(fence_value, g_dx12.fence_event);
        WaitForSingleObject(g_dx12.fence_event, INFINITE);
    }
}

LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, w_param, l_param)) {
        return 1;
    }
    return CallWindowProcW(g_original_wndproc, hwnd, msg, w_param, l_param);
}

bool InitializeImGui(IDXGISwapChain* game_swap_chain)
{
    if (g_dx12.imgui_initialized) {
        return true;
    }

    // Obtain the D3D12 device from the swap chain.
    if (FAILED(game_swap_chain->GetDevice(IID_PPV_ARGS(&g_dx12.device)))) {
        Log("Failed to obtain ID3D12Device from swap chain.");
        return false;
    }

    // Query swap chain for IDXGISwapChain3.
    if (FAILED(game_swap_chain->QueryInterface(IID_PPV_ARGS(&g_dx12.swap_chain)))) {
        Log("Failed to query IDXGISwapChain3.");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sc_desc = {};
    g_dx12.swap_chain->GetDesc(&sc_desc);
    g_dx12.window = sc_desc.OutputWindow;
    g_dx12.rtv_format = sc_desc.BufferDesc.Format;

    const UINT buffer_count = sc_desc.BufferCount;
    g_dx12.frames.resize(buffer_count);

    // Create RTV heap.
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = buffer_count;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_dx12.rtv_heap)))) {
            Log("Failed to create RTV descriptor heap.");
            return false;
        }
        g_dx12.rtv_descriptor_size =
            g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    // Create SRV heap (slot 0 reserved for ImGui font).
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 1;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_dx12.srv_heap)))) {
            Log("Failed to create SRV descriptor heap.");
            return false;
        }
    }

    // Create per-frame command allocators and RTVs using game's back buffers.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = g_dx12.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < buffer_count; ++i) {
        if (FAILED(g_dx12.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_dx12.frames[i].command_allocator)))) {
            Log("Failed to create command allocator for frame %u.", i);
            return false;
        }

        g_dx12.frames[i].rtv_handle = rtv_handle;
        rtv_handle.ptr += g_dx12.rtv_descriptor_size;

        // Get the game's back buffer (not our own)
        if (FAILED(g_dx12.swap_chain->GetBuffer(i, IID_PPV_ARGS(&g_dx12.frames[i].render_target)))) {
            Log("Failed to get back buffer %u.", i);
            return false;
        }

        g_dx12.device->CreateRenderTargetView(
            g_dx12.frames[i].render_target, nullptr, g_dx12.frames[i].rtv_handle);
    }

    // Create command queue for rendering our overlay.
    {
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(g_dx12.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&g_dx12.command_queue)))) {
            Log("Failed to create command queue.");
            return false;
        }
    }

    // Create the command list.
    if (FAILED(g_dx12.device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_dx12.frames[0].command_allocator, nullptr,
            IID_PPV_ARGS(&g_dx12.command_list)))) {
        Log("Failed to create command list.");
        return false;
    }
    g_dx12.command_list->Close();

    // Create fence.
    if (FAILED(g_dx12.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_dx12.fence)))) {
        Log("Failed to create fence.");
        return false;
    }
    g_dx12.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Initialize ImGui.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui_ImplWin32_Init(g_dx12.window);
    ImGui_ImplDX12_Init(
        g_dx12.device,
        static_cast<int>(buffer_count),
        g_dx12.rtv_format,
        g_dx12.srv_heap,
        g_dx12.srv_heap->GetCPUDescriptorHandleForHeapStart(),
        g_dx12.srv_heap->GetGPUDescriptorHandleForHeapStart());

    // Subclass the game window so ImGui can receive input events.
    g_original_wndproc =
        reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_dx12.window, GWLP_WNDPROC,
                                                     reinterpret_cast<LONG_PTR>(&HookedWndProc)));

    g_dx12.imgui_initialized = true;
    Log("ImGui initialized successfully (%u back buffers).", buffer_count);
    return true;
}

void RenderImGuiOverlay()
{
    if (!radial_menu::IsOpen()) {
        return;
    }

    const auto spell_slots = GetMemorizedSpells();
    radial_menu::Draw(spell_slots);
}

// ── Hooked functions ──────────────────────────────────────────────────────

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags)
{
    if (!g_dx12.imgui_initialized) {
        if (!InitializeImGui(swap_chain)) {
            Log("ImGui initialization failed.");
            return g_original_present(swap_chain, sync_interval, flags);
        }
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderImGuiOverlay();
    ImGui::EndFrame();
    ImGui::Render();

    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data == nullptr || draw_data->TotalVtxCount == 0) {
        return g_original_present(swap_chain, sync_interval, flags);
    }

    return g_original_present(swap_chain, sync_interval, flags);
}



// ── VTable discovery ──────────────────────────────────────────────────────

bool GetDXGISwapChainVTable(void** out_vtable)
{
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"RadialSpellMenuVTableProbe";

    RegisterClassExW(&window_class);

    HWND window = CreateWindowExW(
        0,
        window_class.lpszClassName,
        L"RadialSpellMenuDummyWindow",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        100,
        100,
        nullptr,
        nullptr,
        window_class.hInstance,
        nullptr);

    if (window == nullptr) {
        return false;
    }

    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain* swap_chain = nullptr;

    bool success = false;

    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        if (SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) {
            DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
            swap_chain_desc.BufferCount = 2;
            swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swap_chain_desc.OutputWindow = window;
            swap_chain_desc.SampleDesc.Count = 1;
            swap_chain_desc.Windowed = TRUE;
            swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

            if (SUCCEEDED(factory->CreateSwapChain(queue, &swap_chain_desc, &swap_chain))) {
                *out_vtable = *reinterpret_cast<void***>(swap_chain);
                success = true;
            }
        }
    }

    SafeRelease(swap_chain);
    SafeRelease(queue);
    SafeRelease(device);
    SafeRelease(factory);
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);

    return success;
}

}  // namespace

// ── Public API ────────────────────────────────────────────────────────────

bool Install()
{
    // Discover vtable addresses via a temporary swap chain.
    void* swap_chain_vtable = nullptr;
    if (!GetDXGISwapChainVTable(&swap_chain_vtable)) {
        Log("Failed to discover DXGI swap chain vtable.");
        return false;
    }

    auto** const vtable = static_cast<void**>(swap_chain_vtable);
    g_present_target = vtable[kSwapChainVtableIndexPresent];

    if (g_present_target == nullptr) {
        Log("Failed to resolve Present target address.");
        return false;
    }

    MH_CreateHook(g_present_target,
        reinterpret_cast<LPVOID>(&hkPresent),
        reinterpret_cast<LPVOID*>(&g_original_present));

    MH_EnableHook(g_present_target);

    g_dx12.present_hook_installed = true;
    Log("DX12 hooks installed (Present).");
    return true;
}

void Shutdown()
{
    if (g_dx12.imgui_initialized) {
        WaitForLastFrame();
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        if (g_original_wndproc != nullptr && g_dx12.window != nullptr) {
            SetWindowLongPtrW(g_dx12.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_wndproc));
        }
    }

    if (g_present_target != nullptr) {
        MH_DisableHook(g_present_target);
        MH_RemoveHook(g_present_target);
    }

    for (auto& frame : g_dx12.frames) {
        SafeRelease(frame.command_allocator);
        SafeRelease(frame.render_target);
    }
    g_dx12.frames.clear();

    SafeRelease(g_dx12.command_list);
    SafeRelease(g_dx12.rtv_heap);
    SafeRelease(g_dx12.srv_heap);
    SafeRelease(g_dx12.fence);
    SafeRelease(g_dx12.swap_chain);
    SafeRelease(g_dx12.device);

    if (g_dx12.fence_event != nullptr) {
        CloseHandle(g_dx12.fence_event);
        g_dx12.fence_event = nullptr;
    }

    g_dx12 = {};
    g_original_present = nullptr;
    g_present_target = nullptr;
    g_original_wndproc = nullptr;

    Log("DX12 hooks shut down.");
}

}  // namespace radial_spell_menu::dx12_hook
