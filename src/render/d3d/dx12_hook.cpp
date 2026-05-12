#include "render/d3d/dx12_hook.h"
#include "core/common.h"
#include "game/equipment/radial_slots.h"
#include "game/input/native_input.h"
#include "game/state/gameplay_state.h"
#include "input/radial_input.h"
#include "render/d3d/dx12_vtable.h"
#include "render/icons/icon_loader.h"
#include "render/vfs/asset_reader.h"
#include "render/ui/radial_menu.h"
#include "render/ui/eldenring_font.h"

#include <MinHook.h>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace radial_menu_mod::dx12_hook {
namespace {

constexpr bool kDisableNativeInputForDiagnosticBuild = false;

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
static ULONGLONG                   g_next_icon_init_attempt_ms = 0;
static int                         g_icon_prewarm_phase = 0;
static bool                        g_logged_icon_prewarm_complete = false;
static std::vector<std::uint32_t>  g_icon_prewarm_ids;
static ULONGLONG                   g_last_slow_asset_install_log_ms = 0;
static ULONGLONG                   g_last_slow_gameplay_state_log_ms = 0;
static ULONGLONG                   g_last_slow_native_input_log_ms = 0;
static ULONGLONG                   g_last_slow_icon_init_log_ms = 0;
static ULONGLONG                   g_last_slow_icon_refresh_log_ms = 0;
static ULONGLONG                   g_last_slow_icon_prewarm_log_ms = 0;
static ULONGLONG                   g_last_slow_render_log_ms = 0;

static HWND    g_hwnd        = nullptr;
static WNDPROC g_old_wndproc = nullptr;

// ── hook plumbing ─────────────────────────────────────────────────────────
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT);
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_ResizeBuffers1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
using PFN_SetFullscreenState = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
using PFN_ECL     = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

static PFN_Present g_orig_present = nullptr;
static PFN_ResizeBuffers g_orig_resize_buffers = nullptr;
static PFN_ResizeBuffers1 g_orig_resize_buffers1 = nullptr;
static PFN_SetFullscreenState g_orig_set_fullscreen_state = nullptr;
static PFN_ECL     g_orig_ecl     = nullptr;

static void* g_hook_present = nullptr;
static void* g_hook_resize_buffers = nullptr;
static void* g_hook_resize_buffers1 = nullptr;
static void* g_hook_set_fullscreen_state = nullptr;
static void* g_hook_ecl     = nullptr;

static IDXGISwapChain* g_last_fullscreen_swap_chain = nullptr;
static BOOL g_last_fullscreen_request = FALSE;
static bool g_has_last_fullscreen_request = false;

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l);
    return CallWindowProcW(g_old_wndproc, hwnd, msg, w, l);
}

static void LogSlowDuration(const char* label, ULONGLONG start_ms, ULONGLONG threshold_ms,
                            ULONGLONG& last_log_ms)
{
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed = now - start_ms;
    if (elapsed < threshold_ms) return;
    if (last_log_ms != 0 && now - last_log_ms < 2000) return;

    last_log_ms = now;
    Log("Timing: %s took %llums.", label, static_cast<unsigned long long>(elapsed));
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

    const ULONGLONG now = GetTickCount64();
    if (g_next_icon_init_attempt_ms != 0 && now < g_next_icon_init_attempt_ms) return false;

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
    g_next_icon_init_attempt_ms = now + 1000;
    return false;
}

static void RefreshRequiredIconAtlasesForSlots(const std::vector<RadialSlot>& slots)
{
    if (!g_icons_ready) return;

    const ULONGLONG start = GetTickCount64();

    std::vector<std::uint32_t> icon_ids;
    icon_ids.reserve(slots.size());
    for (const RadialSlot& slot : slots) {
        if (slot.icon_id != 0) icon_ids.push_back(slot.icon_id);
    }
    icon_loader::PreloadIcons(icon_ids, 1);
    LogSlowDuration("RefreshRequiredIconAtlasesForOpenSlots", start, 16, g_last_slow_icon_refresh_log_ms);
}

static void AddPrewarmSlotIcons(const std::vector<RadialSlot>& slots)
{
    for (const RadialSlot& slot : slots) {
        if (slot.icon_id == 0) continue;
        if (std::find(g_icon_prewarm_ids.begin(), g_icon_prewarm_ids.end(), slot.icon_id) == g_icon_prewarm_ids.end()) {
            g_icon_prewarm_ids.push_back(slot.icon_id);
        }
    }
}

static void ResetIconPrewarm()
{
    g_icon_prewarm_phase = 0;
    g_logged_icon_prewarm_complete = false;
    g_icon_prewarm_ids.clear();
}

static void PrewarmRadialIcons()
{
    if (!g_icons_ready || radial_menu::IsOpen() || g_icon_prewarm_phase >= 3) return;

    const ULONGLONG start = GetTickCount64();
    if (g_icon_prewarm_phase == 0) {
        AddPrewarmSlotIcons(GetMemorizedSpells());
        g_icon_prewarm_phase = 1;
    } else if (g_icon_prewarm_phase == 1) {
        AddPrewarmSlotIcons(GetQuickItems());
        g_icon_prewarm_phase = 2;
    } else if (icon_loader::PreloadIcons(g_icon_prewarm_ids, 1)) {
        g_icon_prewarm_phase = 3;
        if (!g_logged_icon_prewarm_complete) {
            Log("Icon loader radial prewarm complete (icons=%zu).", g_icon_prewarm_ids.size());
            g_logged_icon_prewarm_complete = true;
        }
    }
    LogSlowDuration("PrewarmRadialIcons", start, 16, g_last_slow_icon_prewarm_log_ms);
}

static void WaitForQueueIdle()
{
    if (!g_device || !g_queue) return;

    ID3D12Fence* fence = nullptr;
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return;

    HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_handle) {
        fence->Release();
        return;
    }

    constexpr UINT64 fence_value = 1;
    if (SUCCEEDED(g_queue->Signal(fence, fence_value)) && fence->GetCompletedValue() < fence_value) {
        if (SUCCEEDED(fence->SetEventOnCompletion(fence_value, event_handle))) {
            WaitForSingleObject(event_handle, 2000);
        }
    }

    CloseHandle(event_handle);
    fence->Release();
}

static void ReleaseOverlayResources(const char* reason)
{
    if (!g_ready && !g_device && !g_frames && !g_rtv_heap && !g_srv_heap && !g_cmdlist) return;

    WaitForQueueIdle();
    icon_loader::Shutdown();
    g_icons_ready = false;
    g_icon_srv_allocated = false;
    g_srv_used = 0;
    for (std::size_t i = 0; i < icon_loader::kMaxAtlases; ++i) {
        g_icon_srv_cpu[i] = {};
        g_icon_srv_gpu[i] = {};
    }

    if (g_ready) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    if (g_hwnd && g_old_wndproc) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_old_wndproc);
    }

    if (g_cmdlist) { g_cmdlist->Release(); g_cmdlist = nullptr; }
    if (g_frames) {
        for (UINT i = 0; i < g_buf_count; i++) {
            if (g_frames[i].allocator)  g_frames[i].allocator->Release();
            if (g_frames[i].backbuffer) g_frames[i].backbuffer->Release();
        }
        delete[] g_frames;
        g_frames = nullptr;
    }
    if (g_rtv_heap) { g_rtv_heap->Release(); g_rtv_heap = nullptr; }
    if (g_srv_heap) { g_srv_heap->Release(); g_srv_heap = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }

    g_ready = false;
    g_buf_count = 0;
    g_rtv_stride = 0;
    g_next_icon_init_attempt_ms = 0;
    ResetIconPrewarm();
    g_last_slow_asset_install_log_ms = 0;
    g_last_slow_gameplay_state_log_ms = 0;
    g_last_slow_native_input_log_ms = 0;
    g_last_slow_icon_init_log_ms = 0;
    g_last_slow_icon_refresh_log_ms = 0;
    g_last_slow_icon_prewarm_log_ms = 0;
    g_last_slow_render_log_ms = 0;
    g_last_fullscreen_swap_chain = nullptr;
    g_has_last_fullscreen_request = false;
    g_hwnd = nullptr;
    g_old_wndproc = nullptr;
    Log("Overlay resources released for %s.", reason);
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
    if (buffer_index >= g_buf_count || !g_frames || !g_frames[buffer_index].allocator ||
        !g_frames[buffer_index].backbuffer || !g_cmdlist || !g_srv_heap || !g_queue) {
        return;
    }
    Frame& frame = g_frames[buffer_index];

    if (FAILED(frame.allocator->Reset())) return;
    if (FAILED(g_cmdlist->Reset(frame.allocator, nullptr))) return;

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

    radial_menu::Draw(radial_input::GetOpenRadialSlots(), radial_input::GetOpenMenuTitle(),
        radial_input::GetOpenMenuControls());

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

// ── Present hook ──────────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain3* swap_chain, UINT sync, UINT flags)
{
    if (!g_ready) {
        if (g_queue) Init(swap_chain);
        return g_orig_present(swap_chain, sync, flags);
    }

    ULONGLONG section_start = GetTickCount64();
    const bool asset_reader_installed = asset_reader::Install();
    LogSlowDuration("asset_reader::Install", section_start, 4, g_last_slow_asset_install_log_ms);
    if (!asset_reader_installed && !g_logged_icon_vfs_unavailable) {
        Log("Icon loader: game VFS hook unavailable; icon archives cannot be read from game memory.");
        g_logged_icon_vfs_unavailable = true;
    }

    section_start = GetTickCount64();
    const bool gameplay_ready = gameplay_state::RefreshNormalGameplayHudState();
    LogSlowDuration("gameplay_state::RefreshNormalGameplayHudState", section_start, 4, g_last_slow_gameplay_state_log_ms);

    if (!kDisableNativeInputForDiagnosticBuild) {
        section_start = GetTickCount64();
        native_input::SampleFrame();
        LogSlowDuration("native_input::SampleFrame", section_start, 4, g_last_slow_native_input_log_ms);
    }
    const bool radial_open = radial_menu::IsOpen();

    if (gameplay_ready) {
        if (!g_icons_ready) {
            section_start = GetTickCount64();
            TryInitializeIcons();
            LogSlowDuration("TryInitializeIcons", section_start, 16, g_last_slow_icon_init_log_ms);
        }
        if (g_icons_ready && radial_open) {
            RefreshRequiredIconAtlasesForSlots(radial_input::GetOpenRadialSlots());
        }
        if (g_icons_ready && !radial_open) {
            PrewarmRadialIcons();
        }
    } else {
        InvalidateRadialSlotCaches();
        ResetIconPrewarm();
    }

    if (!radial_open) {
        return g_orig_present(swap_chain, sync, flags);
    }

    section_start = GetTickCount64();
    RenderRadialOverlay(swap_chain);
    LogSlowDuration("RenderRadialOverlay", section_start, 4, g_last_slow_render_log_ms);
    return g_orig_present(swap_chain, sync, flags);
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* swap_chain, UINT buffer_count, UINT width,
                                                     UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags)
{
    Log("Swap chain ResizeBuffers detected (%ux%u buffers=%u).", width, height, buffer_count);
    ReleaseOverlayResources("swap-chain reset");
    return g_orig_resize_buffers(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers1(IDXGISwapChain3* swap_chain, UINT buffer_count, UINT width,
                                                      UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags,
                                                      const UINT* creation_node_mask, IUnknown* const* present_queue)
{
    Log("Swap chain ResizeBuffers1 detected (%ux%u buffers=%u).", width, height, buffer_count);
    ReleaseOverlayResources("swap-chain reset");
    return g_orig_resize_buffers1(swap_chain, buffer_count, width, height, new_format, swap_chain_flags,
                                  creation_node_mask, present_queue);
}

static HRESULT STDMETHODCALLTYPE HookedSetFullscreenState(IDXGISwapChain* swap_chain, BOOL fullscreen,
                                                          IDXGIOutput* target)
{
    BOOL current_fullscreen = FALSE;
    const HRESULT state_result = swap_chain->GetFullscreenState(&current_fullscreen, nullptr);
    const bool state_changed = FAILED(state_result) || current_fullscreen != fullscreen;
    const bool repeated_request = g_has_last_fullscreen_request &&
        g_last_fullscreen_swap_chain == swap_chain && g_last_fullscreen_request == fullscreen;

    if (state_changed && !repeated_request) {
        Log("SetFullscreenState detected (fullscreen=%d).", fullscreen ? 1 : 0);
        ReleaseOverlayResources("fullscreen transition");
    }
    g_last_fullscreen_swap_chain = swap_chain;
    g_last_fullscreen_request = fullscreen;
    g_has_last_fullscreen_request = true;
    return g_orig_set_fullscreen_state(swap_chain, fullscreen, target);
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
    g_hook_resize_buffers = targets.resize_buffers;
    g_hook_resize_buffers1 = targets.resize_buffers1;
    g_hook_set_fullscreen_state = targets.set_fullscreen_state;
    g_hook_ecl = targets.execute_command_lists;

    if (MH_CreateHook(g_hook_present, (void*)HookedPresent, (void**)&g_orig_present) != MH_OK
     || MH_EnableHook(g_hook_present) != MH_OK) {
        Log("Present hook failed"); return false;
    }
    if (MH_CreateHook(g_hook_resize_buffers, (void*)HookedResizeBuffers, (void**)&g_orig_resize_buffers) != MH_OK
     || MH_EnableHook(g_hook_resize_buffers) != MH_OK) {
        Log("ResizeBuffers hook failed"); return false;
    }
    if (g_hook_resize_buffers1 &&
        (MH_CreateHook(g_hook_resize_buffers1, (void*)HookedResizeBuffers1, (void**)&g_orig_resize_buffers1) != MH_OK
      || MH_EnableHook(g_hook_resize_buffers1) != MH_OK)) {
        Log("ResizeBuffers1 hook failed"); return false;
    }
    if (MH_CreateHook(g_hook_set_fullscreen_state, (void*)HookedSetFullscreenState, (void**)&g_orig_set_fullscreen_state) != MH_OK
     || MH_EnableHook(g_hook_set_fullscreen_state) != MH_OK) {
        Log("SetFullscreenState hook failed"); return false;
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
    if (g_hook_present) { MH_DisableHook(g_hook_present); MH_RemoveHook(g_hook_present); }
    if (g_hook_resize_buffers) { MH_DisableHook(g_hook_resize_buffers); MH_RemoveHook(g_hook_resize_buffers); }
    if (g_hook_resize_buffers1) { MH_DisableHook(g_hook_resize_buffers1); MH_RemoveHook(g_hook_resize_buffers1); }
    if (g_hook_set_fullscreen_state) { MH_DisableHook(g_hook_set_fullscreen_state); MH_RemoveHook(g_hook_set_fullscreen_state); }
    if (g_hook_ecl)     { MH_DisableHook(g_hook_ecl);     MH_RemoveHook(g_hook_ecl); }

    ReleaseOverlayResources("shutdown");
}

} // namespace radial_menu_mod::dx12_hook
