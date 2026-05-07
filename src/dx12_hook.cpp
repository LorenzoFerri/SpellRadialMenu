#include "dx12_hook.h"
#include "asset_reader.h"
#include "common.h"
#include "icon_loader.h"
#include "input_hook.h"
#include "radial_menu.h"
#include "eldenring_font.h"

#include <MinHook.h>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <cstddef>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace radial_spell_menu::dx12_hook {
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

static HWND    g_hwnd        = nullptr;
static WNDPROC g_old_wndproc = nullptr;

// ── hook plumbing ─────────────────────────────────────────────────────────
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT);
using PFN_ECL     = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

static PFN_Present g_orig_present = nullptr;
static PFN_ECL     g_orig_ecl     = nullptr;

static void* g_hook_present = nullptr;
static void* g_hook_ecl     = nullptr;

// ── WndProc ───────────────────────────────────────────────────────────────
static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l);
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

static void TryInitializeIcons()
{
    if (!asset_reader::Install()) return;

    if (!g_icon_srv_allocated) {
        for (std::size_t i = 0; i < icon_loader::kMaxAtlases; ++i) {
            SrvAlloc(nullptr, &g_icon_srv_cpu[i], &g_icon_srv_gpu[i]);
        }
        g_icon_srv_allocated = true;
    }

    if (icon_loader::TryInitialize(g_device, g_queue, g_icon_srv_cpu, g_icon_srv_gpu, icon_loader::kMaxAtlases)) {
        radial_menu::SetIconTextureResolver(&icon_loader::Resolve);
    }
}

// ── ImGui + D3D12 init (called on first Present) ──────────────────────────
static void Init(IDXGISwapChain3* sc)
{
    sc->GetDevice(IID_PPV_ARGS(&g_device));
    if (!g_device) { Log("Init: GetDevice failed"); return; }

    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_buf_count = desc.BufferCount;
    g_hwnd      = desc.OutputWindow;

    Log("Init: buffers=%u hwnd=%p", g_buf_count, (void*)g_hwnd);

    // RTV heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        d.NumDescriptors = g_buf_count;
        if (FAILED(g_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_rtv_heap))))
            { Log("Init: RTV heap failed"); return; }
        g_rtv_stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    // SRV heap (shader-visible, for ImGui font)
    {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = 48;
        d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_srv_heap))))
            { Log("Init: SRV heap failed"); return; }
    }

    // Per-frame allocators + back buffers + RTVs
    DXGI_FORMAT fmt = (desc.BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                    ? DXGI_FORMAT_R8G8B8A8_UNORM : desc.BufferDesc.Format;
    g_frames = new Frame[g_buf_count]();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_buf_count; i++) {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&g_frames[i].allocator))))
            { Log("Init: allocator[%u] failed", i); return; }
        if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].backbuffer))))
            { Log("Init: GetBuffer[%u] failed", i); return; }
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format        = fmt;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_device->CreateRenderTargetView(g_frames[i].backbuffer, &rd, rtv);
        g_frames[i].rtv = rtv;
        rtv.ptr += g_rtv_stride;
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
    info.RTVFormat            = fmt;
    info.SrvDescriptorHeap    = g_srv_heap;
    info.SrvDescriptorAllocFn = SrvAlloc;
    info.SrvDescriptorFreeFn  = SrvFree;
    ImGui_ImplDX12_Init(&info);

    g_old_wndproc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
    g_ready = true;
    Log("ImGui ready");
}

// ── Present hook ──────────────────────────────────────────────────────────
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain3* sc, UINT sync, UINT flags)
{
    if (!g_ready) {
        if (g_queue) Init(sc);
        return g_orig_present(sc, sync, flags);
    }

    if (!radial_menu::IsOpen()) {
        return g_orig_present(sc, sync, flags);
    }

    TryInitializeIcons();

    UINT idx = sc->GetCurrentBackBufferIndex();
    Frame& f = g_frames[idx];

    f.allocator->Reset();
    g_cmdlist->Reset(f.allocator, nullptr);

    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = f.backbuffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_cmdlist->ResourceBarrier(1, &b);

    g_cmdlist->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
    g_cmdlist->SetDescriptorHeaps(1, &g_srv_heap);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    radial_menu::Draw(input_hook::GetOpenSpellSlots());

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmdlist);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    g_cmdlist->ResourceBarrier(1, &b);
    g_cmdlist->Close();

    ID3D12CommandList* cmd = g_cmdlist;
    g_queue->ExecuteCommandLists(1, &cmd);

    return g_orig_present(sc, sync, flags);
}

// ── ExecuteCommandLists hook — captures the real command queue ────────────
static void STDMETHODCALLTYPE HookedECL(ID3D12CommandQueue* q, UINT n,
                                         ID3D12CommandList* const* lists)
{
    if (!g_queue) {
        // Only grab graphics queues (type 0 = DIRECT)
        D3D12_COMMAND_QUEUE_DESC qd = q->GetDesc();
        if (qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_queue = q;
            Log("Command queue captured");
        }
    }
    g_orig_ecl(q, n, lists);
}

} // namespace

// ── public ────────────────────────────────────────────────────────────────
bool Install()
{
    // ── 1. Create a dummy window ──────────────────────────────────────────
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RSM_Dummy";
    RegisterClassExW(&wc);
    HWND dummy = CreateWindowExW(0, L"RSM_Dummy", L"", WS_OVERLAPPED,
                                 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummy) { Log("CreateWindowEx failed"); return false; }

    // ── 2. D3D12 device + command queue ──────────────────────────────────
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        Log("D3D12CreateDevice failed"); DestroyWindow(dummy); return false;
    }
    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));

    // ── 3. DXGI factory + swap chain (just to read vtable) ───────────────
    IDXGIFactory4* fac = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&fac));

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width       = 8;
    scd.Height      = 8;
    scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc  = {1, 0};
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = fac->CreateSwapChainForHwnd(q, dummy, &scd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        Log("Dummy CreateSwapChainForHwnd failed: 0x%08X", (unsigned)hr);
        q->Release(); dev->Release(); fac->Release(); DestroyWindow(dummy); return false;
    }

    // Read vtable pointers before releasing
    void** sc_vt = *reinterpret_cast<void***>(sc1);
    void** q_vt  = *reinterpret_cast<void***>(q);
    g_hook_present = sc_vt[8];   // IDXGISwapChain::Present
    g_hook_ecl     = q_vt[10];   // ID3D12CommandQueue::ExecuteCommandLists

    sc1->Release();
    fac->Release();
    q->Release();
    dev->Release();
    DestroyWindow(dummy);
    UnregisterClassW(L"RSM_Dummy", GetModuleHandleW(nullptr));

    // ── 4. Install hooks ──────────────────────────────────────────────────
    if (MH_CreateHook(g_hook_present, (void*)HookedPresent, (void**)&g_orig_present) != MH_OK
     || MH_EnableHook(g_hook_present) != MH_OK) {
        Log("Present hook failed"); return false;
    }
    if (MH_CreateHook(g_hook_ecl, (void*)HookedECL, (void**)&g_orig_ecl) != MH_OK
     || MH_EnableHook(g_hook_ecl) != MH_OK) {
        Log("ECL hook failed"); return false;
    }

    Log("Hooks installed");
    return true;
}

void Shutdown()
{
    icon_loader::Shutdown();
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

} // namespace radial_spell_menu::dx12_hook
