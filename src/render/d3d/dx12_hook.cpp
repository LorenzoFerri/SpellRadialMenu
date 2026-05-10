#include "render/d3d/dx12_hook.h"
#include "core/common.h"
#include "game/state/gameplay_state.h"
#include "input/input_hook.h"
#include "input/radial_input.h"
#include "render/d3d/dx12_vtable.h"
#include "render/icons/icon_loader.h"
#include "render/vfs/asset_reader.h"
#include "render/ui/radial_menu.h"
#include "render/ui/eldenring_font.h"

#include <MinHook.h>
#include <windows.h>
#include <windowsx.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <cstddef>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace radial_menu_mod::dx12_hook {
namespace {

// ── per-frame D3D12 resources ─────────────────────────────────────────────
struct Frame {
    ID3D12CommandAllocator*     allocator  = nullptr;
    ID3D12Resource*             backbuffer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv        = {};
};

static bool    g_ready      = false;
static UINT    g_buf_count  = 0;
static Frame*  g_frames     = nullptr;

static ID3D12Device*               g_device    = nullptr;
static ID3D12CommandQueue*         g_queue     = nullptr;
static ID3D12GraphicsCommandList*  g_cmdlist   = nullptr;
static ID3D12DescriptorHeap*       g_rtv_heap  = nullptr;
static ID3D12DescriptorHeap*       g_srv_heap  = nullptr;
static UINT                        g_rtv_stride = 0;
static D3D12_CPU_DESCRIPTOR_HANDLE g_icon_srv_cpu[icon_loader::kMaxAtlases] = {};
static D3D12_GPU_DESCRIPTOR_HANDLE g_icon_srv_gpu[icon_loader::kMaxAtlases] = {};
static bool                        g_icon_srv_allocated = false;
static bool                        g_icons_ready = false;
static bool                        g_logged_icon_vfs_unavailable = false;

static HWND    g_hwnd        = nullptr;
static WNDPROC g_old_wndproc = nullptr;
static POINT g_last_mouse_pos = {};
static bool g_have_last_mouse_pos = false;

// ── hook plumbing ─────────────────────────────────────────────────────────
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT);
using PFN_ECL     = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

static PFN_Present g_orig_present = nullptr;
static PFN_ECL     g_orig_ecl     = nullptr;

static void* g_hook_present = nullptr;
static void* g_hook_ecl     = nullptr;

// ── WndProc ───────────────────────────────────────────────────────────────
static bool IsKeyboardMouseTrigger(WPARAM key)
{
    return key == VK_TAB || key == VK_CAPITAL;
}

static void HandleMouseMove(LPARAM lparam)
{
    POINT pos{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    if (g_have_last_mouse_pos) {
        radial_input::AddMouseDelta(
            static_cast<float>(pos.x - g_last_mouse_pos.x),
            static_cast<float>(pos.y - g_last_mouse_pos.y));
    }
    g_last_mouse_pos = pos;
    g_have_last_mouse_pos = true;
}

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l);

    if (radial_menu::IsOpen()) {
        if (msg == WM_INPUT) return 0;
        if (msg == WM_MOUSEMOVE) {
            HandleMouseMove(l);
            return 0;
        }
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 0;
        if ((msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) &&
            IsKeyboardMouseTrigger(w)) {
            return 0;
        }
    } else if (msg == WM_MOUSEMOVE) {
        g_last_mouse_pos = POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        g_have_last_mouse_pos = true;
    }

    return CallWindowProcW(g_old_wndproc, hwnd, msg, w, l);
}

// ── SRV bump allocator ────────────────────────────────────────────────────
static UINT g_srv_used = 0;
static void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                     D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    UINT stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    UINT idx    = g_srv_used++;
    cpu->ptr = g_srv_heap->GetCPUDescriptorHandleForHeapStart().ptr + idx * stride;
    gpu->ptr = g_srv_heap->GetGPUDescriptorHandleForHeapStart().ptr + idx * stride;
}
static void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE,
                    D3D12_GPU_DESCRIPTOR_HANDLE) {}

static bool TryInitializeIcons()
{
    if (g_icons_ready) return true;

    if (!g_icon_srv_allocated) {
        for (std::size_t i = 0; i < icon_loader::kMaxAtlases; ++i) {
            SrvAlloc(nullptr, &g_icon_srv_cpu[i], &g_icon_srv_gpu[i]);
        }
        g_icon_srv_allocated = true;
    }

    if (icon_loader::TryInitialize(g_device, g_queue, g_icon_srv_cpu, g_icon_srv_gpu, icon_loader::kMaxAtlases)) {
        radial_menu::SetIconTextureResolver(&icon_loader::Resolve);
        g_icons_ready = true;
        Log("Icon loader initialized.");
        return true;
    }
    return false;
}

// ── ImGui + D3D12 init (called on first Present) ──────────────────────────
static void Init(IDXGISwapChain3* swap_chain)
{
    swap_chain->GetDevice(IID_PPV_ARGS(&g_device));
    if (!g_device) { Log("Init: GetDevice failed"); return; }

    DXGI_SWAP_CHAIN_DESC swap_chain_desc{};
    swap_chain->GetDesc(&swap_chain_desc);
    g_buf_count = swap_chain_desc.BufferCount;
    g_hwnd = swap_chain_desc.OutputWindow;

    // RTV heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = g_buf_count;
        if (FAILED(g_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&g_rtv_heap))))
            { Log("Init: RTV heap failed"); return; }
        g_rtv_stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    // SRV heap (shader-visible, for ImGui font)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 48;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&g_srv_heap))))
            { Log("Init: SRV heap failed"); return; }
    }

    // Per-frame allocators + back buffers + RTVs
    DXGI_FORMAT rtv_format = (swap_chain_desc.BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                    ? DXGI_FORMAT_R8G8B8A8_UNORM : swap_chain_desc.BufferDesc.Format;
    g_frames = new Frame[g_buf_count]();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_buf_count; i++) {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&g_frames[i].allocator))))
            { Log("Init: allocator[%u] failed", i); return; }
        if (FAILED(swap_chain->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].backbuffer))))
            { Log("Init: GetBuffer[%u] failed", i); return; }
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = rtv_format;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_device->CreateRenderTargetView(g_frames[i].backbuffer, &rtv_desc, rtv_handle);
        g_frames[i].rtv = rtv_handle;
        rtv_handle.ptr += g_rtv_stride;
    }

    // Command list
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        g_frames[0].allocator, nullptr, IID_PPV_ARGS(&g_cmdlist))))
        { Log("Init: CreateCommandList failed"); return; }
    g_cmdlist->Close();

    // ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontFromMemoryCompressedTTF(
        EldenRingFont_compressed_data,
        (int)EldenRingFont_compressed_size,
        20.0f);
    ImGui_ImplWin32_Init(g_hwnd);

    ImGui_ImplDX12_InitInfo info{};
    info.Device               = g_device;
    info.CommandQueue         = g_queue;
    info.NumFramesInFlight    = (int)g_buf_count;
    info.RTVFormat            = rtv_format;
    info.SrvDescriptorHeap    = g_srv_heap;
    info.SrvDescriptorAllocFn = SrvAlloc;
    info.SrvDescriptorFreeFn  = SrvFree;
    ImGui_ImplDX12_Init(&info);

    g_old_wndproc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
    g_ready = true;
    Log("D3D12 overlay initialized (buffers=%u hwnd=%p).", g_buf_count, (void*)g_hwnd);
}

static void RenderRadialOverlay(IDXGISwapChain3* swap_chain)
{
    UINT buffer_index = swap_chain->GetCurrentBackBufferIndex();
    Frame& frame = g_frames[buffer_index];

    frame.allocator->Reset();
    g_cmdlist->Reset(frame.allocator, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.backbuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_cmdlist->ResourceBarrier(1, &barrier);

    g_cmdlist->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
    g_cmdlist->SetDescriptorHeaps(1, &g_srv_heap);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    radial_menu::Draw(input_hook::GetOpenSpellSlots(), input_hook::GetOpenMenuTitle(), input_hook::GetOpenMenuControls());

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmdlist);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_cmdlist->ResourceBarrier(1, &barrier);
    g_cmdlist->Close();

    ID3D12CommandList* command_list = g_cmdlist;
    g_queue->ExecuteCommandLists(1, &command_list);
}

static void CaptureDirectQueue(ID3D12CommandQueue* queue)
{
    if (g_queue) return;

    D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
    if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        g_queue = queue;
        Log("Direct command queue captured.");
    }
}

static void PollKeyboardMouseRadial()
{
    const bool spell_held = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
    const bool item_held = (GetAsyncKeyState(VK_CAPITAL) & 0x8000) != 0;
    radial_input::HandleKeyboardMouseState(spell_held, item_held);
}

// ── Present hook ──────────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain3* swap_chain, UINT sync, UINT flags)
{
    if (!g_ready) {
        if (g_queue) Init(swap_chain);
        return g_orig_present(swap_chain, sync, flags);
    }

    if (!asset_reader::Install() && !g_logged_icon_vfs_unavailable) {
        Log("Icon loader: game VFS hook unavailable; icon archives cannot be read from game memory.");
        g_logged_icon_vfs_unavailable = true;
    }

    const bool gameplay_ready = gameplay_state::RefreshNormalGameplayHudState();
    PollKeyboardMouseRadial();

    if (!radial_menu::IsOpen()) {
        if (!g_icons_ready && gameplay_ready) {
            TryInitializeIcons();
        }
        return g_orig_present(swap_chain, sync, flags);
    }

    RenderRadialOverlay(swap_chain);
    return g_orig_present(swap_chain, sync, flags);
}

// ── ExecuteCommandLists hook — captures the real command queue ────────────
static void STDMETHODCALLTYPE HookedECL(ID3D12CommandQueue* queue, UINT command_list_count,
                                          ID3D12CommandList* const* lists)
{
    CaptureDirectQueue(queue);
    g_orig_ecl(queue, command_list_count, lists);
}

} // namespace

// ── public ────────────────────────────────────────────────────────────────
bool Install()
{
    dx12_vtable::HookTargets targets{};
    if (!dx12_vtable::DiscoverHookTargets(targets)) return false;
    g_hook_present = targets.present;
    g_hook_ecl = targets.execute_command_lists;

    if (MH_CreateHook(g_hook_present, (void*)HookedPresent, (void**)&g_orig_present) != MH_OK
     || MH_EnableHook(g_hook_present) != MH_OK) {
        Log("Present hook failed"); return false;
    }
    if (MH_CreateHook(g_hook_ecl, (void*)HookedECL, (void**)&g_orig_ecl) != MH_OK
     || MH_EnableHook(g_hook_ecl) != MH_OK) {
        Log("ECL hook failed"); return false;
    }
    Log("D3D12 hooks installed.");
    return true;
}

void Shutdown()
{
    icon_loader::Shutdown();
    g_icons_ready = false;
    if (g_hook_present) { MH_DisableHook(g_hook_present); MH_RemoveHook(g_hook_present); }
    if (g_hook_ecl)     { MH_DisableHook(g_hook_ecl);     MH_RemoveHook(g_hook_ecl); }

    if (g_ready) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (g_hwnd && g_old_wndproc)
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_old_wndproc);
    }

    if (g_cmdlist)  g_cmdlist->Release();
    if (g_rtv_heap) g_rtv_heap->Release();
    if (g_srv_heap) g_srv_heap->Release();
    if (g_device)   g_device->Release();
    if (g_frames) {
        for (UINT i = 0; i < g_buf_count; i++) {
            if (g_frames[i].allocator)  g_frames[i].allocator->Release();
            if (g_frames[i].backbuffer) g_frames[i].backbuffer->Release();
        }
        delete[] g_frames;
    }
}

} // namespace radial_menu_mod::dx12_hook
