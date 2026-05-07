#include "input_hook.h"

#include "common.h"
#include "radial_menu.h"
#include "spell_manager.h"

#include <MinHook.h>
#include <windows.h>
#include <xinput.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace radial_spell_menu::input_hook {

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

constexpr ULONGLONG kHoldThresholdMs = 180;
constexpr ULONGLONG kQuickSelectHoldMs = 260;
constexpr ULONGLONG kQuickSelectReleaseGapMs = 100;
constexpr ULONGLONG kTapPressMs = 60;
constexpr ULONGLONG kTapReleaseMs = 90;
constexpr WORD kInterceptedButton = XINPUT_GAMEPAD_DPAD_UP;
constexpr DWORD kControllerCount = 4;

enum class SyntheticInputPhase {
    idle,
    quick_select_hold,
    quick_select_release_gap,
    tap_press,
    tap_release,
};

struct ControllerContext {
    bool dpad_up_down = false;
    bool radial_opened = false;
    ULONGLONG pressed_at = 0;
    SyntheticInputPhase synthetic_phase = SyntheticInputPhase::idle;
    ULONGLONG synthetic_phase_until = 0;
    std::uint32_t remaining_taps = 0;
};

std::array<ControllerContext, kControllerCount> g_controllers = {};
std::vector<SpellSlot> g_open_spell_slots;
XInputGetStateFn g_original_xinput_get_state = nullptr;
LPVOID g_xinput_target = nullptr;
std::uintptr_t g_cs_fe_man_static_address = 0;
bool g_searched_cs_fe_man = false;
bool g_logged_missing_cs_fe_man = false;

struct SectionInfo {
    std::uintptr_t address = 0;
    std::uint32_t virtual_address = 0;
    std::uint32_t size = 0;
};

bool GetSection(const char* name, SectionInfo& section)
{
    const auto base = GetModuleBase();
    if (!base) return false;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char section_name[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
        std::memcpy(section_name, sections[i].Name, IMAGE_SIZEOF_SHORT_NAME);
        if (std::strcmp(section_name, name) != 0) continue;

        section.address = base + sections[i].VirtualAddress;
        section.virtual_address = sections[i].VirtualAddress;
        section.size = sections[i].Misc.VirtualSize ? sections[i].Misc.VirtualSize : sections[i].SizeOfRawData;
        return section.size != 0;
    }

    return false;
}

bool MatchBytes(const std::uint8_t* bytes, std::size_t size, std::size_t offset, const std::uint8_t* pattern,
    std::size_t pattern_size)
{
    return offset <= size && pattern_size <= size - offset && std::memcmp(bytes + offset, pattern, pattern_size) == 0;
}

bool ResolveRipRva(std::uint32_t text_rva, std::uint32_t disp_offset, const std::uint8_t* disp_base, std::uint32_t& rva)
{
    const auto disp = *reinterpret_cast<const std::int32_t*>(disp_base);
    rva = text_rva + disp_offset + sizeof(std::int32_t) + static_cast<std::uint32_t>(disp);
    return true;
}

bool FindCondJumpBefore(const std::uint8_t* text, std::size_t text_size, std::uint32_t pos, std::uint32_t& jump_pos)
{
    if (pos >= 2 && text[pos - 2] == 0x75) {
        jump_pos = pos - 2;
        return true;
    }

    static constexpr std::uint8_t kJneNear[] = {0x0F, 0x85};
    if (pos >= 6 && MatchBytes(text, text_size, pos - 6, kJneNear, sizeof(kJneNear))) {
        jump_pos = pos - 6;
        return true;
    }

    return false;
}

bool TryResolveDerivedSingletonCandidate(const SectionInfo& text_section, const SectionInfo& data_section,
    std::uint32_t candidate_pos, std::uint32_t name_disp_offset, const char* singleton_name, std::uintptr_t& static_address)
{
    const auto base = GetModuleBase();
    const auto* text = reinterpret_cast<const std::uint8_t*>(text_section.address);
    std::uint32_t jump_pos = 0;
    if (!FindCondJumpBefore(text, text_section.size, candidate_pos, jump_pos) || jump_pos < 3) return false;

    const std::uint32_t test_pos = jump_pos - 3;
    if (test_pos + 3 > text_section.size) return false;

    const std::uint8_t test_rex = text[test_pos];
    const std::uint8_t test_modrm = text[test_pos + 2];
    const std::uint8_t test_rexb = test_rex & 1;
    const std::uint8_t test_mod = test_modrm & 0xC0;
    const std::uint8_t test_reg1 = test_modrm & 0x07;
    const std::uint8_t test_reg2 = (test_modrm >> 3) & 0x07;
    if ((test_rex & 0xF8) != 0x48 || test_mod != 0xC0 || test_reg1 != test_reg2) return false;

    std::uint32_t singleton_rva = 0;
    bool found_singleton_static = false;
    for (std::uint32_t pad = 0; pad <= 3; ++pad) {
        if (test_pos < 7 + pad) continue;
        const std::uint32_t mov_pos = test_pos - 7 - pad;
        if (mov_pos + 7 > text_section.size) continue;

        const std::uint8_t mov_rex = text[mov_pos];
        const std::uint8_t mov_modrm = text[mov_pos + 2];
        const std::uint8_t mov_rexw = (mov_rex >> 2) & 1;
        const std::uint8_t mov_mod = mov_modrm & 0xC0;
        const std::uint8_t mov_mem = mov_modrm & 0x07;
        const std::uint8_t mov_reg = (mov_modrm >> 3) & 0x07;

        if ((mov_rex & 0xF8) == 0x48 && mov_mod == 0 && mov_mem == 5 && mov_rexw == test_rexb &&
            mov_reg == test_reg1) {
            ResolveRipRva(text_section.virtual_address, mov_pos + 3, text + mov_pos + 3, singleton_rva);
            found_singleton_static = true;
            break;
        }
    }
    if (!found_singleton_static) return false;
    if (singleton_rva < data_section.virtual_address ||
        singleton_rva >= data_section.virtual_address + data_section.size) {
        return false;
    }

    std::uint32_t name_rva = 0;
    ResolveRipRva(text_section.virtual_address, name_disp_offset, text + name_disp_offset, name_rva);
    const auto* name = reinterpret_cast<const char*>(base + name_rva);
    if (std::strcmp(name, singleton_name) != 0) return false;

    static_address = base + singleton_rva;
    return true;
}

bool TryResolveFd4SingletonCandidate(const SectionInfo& text_section, const SectionInfo& data_section,
    std::uint32_t candidate_pos, const char* singleton_name, std::uintptr_t& static_address)
{
    using GetNameFn = const char* (*)(const std::uint8_t*);

    const auto base = GetModuleBase();
    const auto* text = reinterpret_cast<const std::uint8_t*>(text_section.address);
    std::uint32_t call_pos = 0;
    bool found_call = false;
    for (std::uint32_t pad = 0; pad <= 1; ++pad) {
        if (candidate_pos < 5 + pad) continue;
        const std::uint32_t pos = candidate_pos - 5 - pad;
        if (pos < text_section.size && text[pos] == 0xE8) {
            call_pos = pos;
            found_call = true;
            break;
        }
    }
    if (!found_call || call_pos < 7) return false;

    static constexpr std::uint8_t kLeaRcx[] = {0x48, 0x8D, 0x0D};
    const std::uint32_t reflection_pos = call_pos - 7;
    if (!MatchBytes(text, text_section.size, reflection_pos, kLeaRcx, sizeof(kLeaRcx))) return false;

    std::uint32_t jump_pos = 0;
    if (!FindCondJumpBefore(text, text_section.size, reflection_pos, jump_pos) || jump_pos < 3) return false;

    const std::uint32_t test_pos = jump_pos - 3;
    if (test_pos + 3 > text_section.size) return false;

    const std::uint8_t test_rex = text[test_pos];
    const std::uint8_t test_modrm = text[test_pos + 2];
    const std::uint8_t test_rexb = test_rex & 1;
    const std::uint8_t test_mod = test_modrm & 0xC0;
    const std::uint8_t test_reg1 = test_modrm & 0x07;
    const std::uint8_t test_reg2 = (test_modrm >> 3) & 0x07;
    if ((test_rex & 0xF8) != 0x48 || test_mod != 0xC0 || test_reg1 != test_reg2) return false;

    std::uint32_t singleton_rva = 0;
    bool found_singleton_static = false;
    for (std::uint32_t pad = 0; pad <= 3; ++pad) {
        if (test_pos < 7 + pad) continue;
        const std::uint32_t mov_pos = test_pos - 7 - pad;
        if (mov_pos + 7 > text_section.size) continue;

        const std::uint8_t mov_rex = text[mov_pos];
        const std::uint8_t mov_modrm = text[mov_pos + 2];
        const std::uint8_t mov_rexw = (mov_rex >> 2) & 1;
        const std::uint8_t mov_mod = mov_modrm & 0xC0;
        const std::uint8_t mov_mem = mov_modrm & 0x07;
        const std::uint8_t mov_reg = (mov_modrm >> 3) & 0x07;

        if ((mov_rex & 0xF8) == 0x48 && mov_mod == 0 && mov_mem == 5 && mov_rexw == test_rexb &&
            mov_reg == test_reg1) {
            ResolveRipRva(text_section.virtual_address, mov_pos + 3, text + mov_pos + 3, singleton_rva);
            found_singleton_static = true;
            break;
        }
    }
    if (!found_singleton_static) return false;
    if (singleton_rva < data_section.virtual_address ||
        singleton_rva >= data_section.virtual_address + data_section.size) {
        return false;
    }

    std::uint32_t reflection_rva = 0;
    ResolveRipRva(text_section.virtual_address, reflection_pos + 3, text + reflection_pos + 3, reflection_rva);

    std::uint32_t get_name_rva = 0;
    ResolveRipRva(text_section.virtual_address, call_pos + 1, text + call_pos + 1, get_name_rva);
    if (get_name_rva < text_section.virtual_address || get_name_rva >= text_section.virtual_address + text_section.size) {
        return false;
    }

    const auto get_name = reinterpret_cast<GetNameFn>(base + get_name_rva);
    const auto* name = get_name(reinterpret_cast<const std::uint8_t*>(base + reflection_rva));
    if (name == nullptr || std::strcmp(name, singleton_name) != 0) return false;

    static_address = base + singleton_rva;
    return true;
}

std::uintptr_t ResolveSingletonStaticAddress(const char* singleton_name)
{
    SectionInfo text_section;
    SectionInfo data_section;
    if (!GetSection(".text", text_section) || !GetSection(".data", data_section)) return 0;

    static constexpr std::uint8_t kLeaRcx[] = {0x48, 0x8D, 0x0D};
    static constexpr std::uint8_t kLeaR8[] = {0x4C, 0x8D, 0x05};
    static constexpr std::uint8_t kLeaR9[] = {0x4C, 0x8D, 0x0D};
    static constexpr std::uint8_t kMovR9[] = {0x4C, 0x8B, 0xC8};

    const auto* text = reinterpret_cast<const std::uint8_t*>(text_section.address);
    for (std::uint32_t pos = 0; pos < text_section.size; ++pos) {
        if (text[pos] != 0xBA) continue;

        std::uint32_t instruction_positions[4] = {pos, 0, 0, 0};
        std::uint8_t instruction_types[4] = {5, 0, 0, 0};
        std::uint32_t count = 1;

        while (count < 4) {
            const std::uint32_t next_pos = instruction_positions[count - 1] + (instruction_types[count - 1] == 4 ? 3 :
                (instruction_types[count - 1] == 5 ? 5 : 7));
            if (MatchBytes(text, text_section.size, next_pos, kLeaRcx, sizeof(kLeaRcx))) {
                instruction_positions[count] = next_pos;
                instruction_types[count++] = 1;
            } else if (MatchBytes(text, text_section.size, next_pos, kLeaR8, sizeof(kLeaR8))) {
                instruction_positions[count] = next_pos;
                instruction_types[count++] = 2;
            } else if (MatchBytes(text, text_section.size, next_pos, kLeaR9, sizeof(kLeaR9))) {
                instruction_positions[count] = next_pos;
                instruction_types[count++] = 3;
            } else if (MatchBytes(text, text_section.size, next_pos, kMovR9, sizeof(kMovR9))) {
                instruction_positions[count] = next_pos;
                instruction_types[count++] = 4;
            } else if (next_pos < text_section.size && text[next_pos] == 0xE8) {
                break;
            } else {
                break;
            }
        }

        while (count < 4) {
            const std::uint32_t first_pos = instruction_positions[0];
            if (first_pos < 7) break;
            const std::uint32_t prev_pos = first_pos - 7;
            std::uint8_t type = 0;
            std::uint32_t actual_pos = prev_pos;
            if (MatchBytes(text, text_section.size, prev_pos, kLeaRcx, sizeof(kLeaRcx))) type = 1;
            else if (MatchBytes(text, text_section.size, prev_pos, kLeaR8, sizeof(kLeaR8))) type = 2;
            else if (MatchBytes(text, text_section.size, prev_pos, kLeaR9, sizeof(kLeaR9))) type = 3;
            else if (MatchBytes(text, text_section.size, prev_pos + 4, kMovR9, sizeof(kMovR9))) {
                type = 4;
                actual_pos = prev_pos + 4;
            } else {
                break;
            }

            for (std::uint32_t i = count; i > 0; --i) {
                instruction_positions[i] = instruction_positions[i - 1];
                instruction_types[i] = instruction_types[i - 1];
            }
            instruction_positions[0] = actual_pos;
            instruction_types[0] = type;
            ++count;
        }

        if (count != 4) continue;

        std::uint8_t mask = 0;
        std::uint32_t name_disp_offset = 0;
        for (std::uint32_t i = 0; i < 4; ++i) {
            if (instruction_types[i] == 1) mask |= 1;
            else if (instruction_types[i] == 2) mask |= 2;
            else if (instruction_types[i] == 3) {
                mask |= 4;
                name_disp_offset = instruction_positions[i] + 3;
            } else if (instruction_types[i] == 4) {
                mask |= 8;
            }
        }
        std::uintptr_t static_address = 0;
        if (mask == 7) {
            if (TryResolveDerivedSingletonCandidate(text_section, data_section, instruction_positions[0], name_disp_offset,
                singleton_name, static_address)) {
                return static_address;
            }
        } else if (mask == 11) {
            if (TryResolveFd4SingletonCandidate(text_section, data_section, instruction_positions[0], singleton_name,
                static_address)) {
                return static_address;
            }
        }
    }

    return 0;
}

std::uintptr_t ResolveCSFeMan()
{
    if (!g_searched_cs_fe_man) {
        g_searched_cs_fe_man = true;
        g_cs_fe_man_static_address = ResolveSingletonStaticAddress("CSFeMan");
        if (g_cs_fe_man_static_address) {
            Log("CSFeMan singleton resolved at 0x%p.", reinterpret_cast<void*>(g_cs_fe_man_static_address));
        }
    }

    if (!g_cs_fe_man_static_address) return 0;
    return *reinterpret_cast<std::uintptr_t*>(g_cs_fe_man_static_address);
}

bool IsNormalGameplayHudState()
{
    constexpr std::uintptr_t kHudStateOffset = 0x78;
    constexpr std::uint8_t kHudStateDefault = 3;

    const auto fe_man = ResolveCSFeMan();
    if (!fe_man) {
        if (!g_logged_missing_cs_fe_man) {
            g_logged_missing_cs_fe_man = true;
            Log("CSFeMan unresolved; radial menu input guard will allow legacy behavior.");
        }
        return true;
    }

    return *reinterpret_cast<const std::uint8_t*>(fe_man + kHudStateOffset) == kHudStateDefault;
}

bool CanStartRadialInput()
{
    return IsNormalGameplayHudState() && !GetMemorizedSpells().empty();
}

float NormalizeThumbAxis(SHORT value, SHORT deadzone)
{
    if (std::abs(static_cast<int>(value)) <= deadzone) {
        return 0.0f;
    }

    const float normalized = static_cast<float>(value) / 32767.0f;
    return (normalized < -1.0f) ? -1.0f : ((normalized > 1.0f) ? 1.0f : normalized);
}

void QueueTapSequence(ControllerContext& controller, ULONGLONG now, std::uint32_t tap_count)
{
    if (tap_count == 0) {
        controller.synthetic_phase = SyntheticInputPhase::idle;
        controller.synthetic_phase_until = 0;
        controller.remaining_taps = 0;
        return;
    }

    controller.synthetic_phase = SyntheticInputPhase::tap_press;
    controller.synthetic_phase_until = now + kTapPressMs;
    controller.remaining_taps = tap_count;
}

void QueueQuickSelectSequence(ControllerContext& controller, ULONGLONG now, std::uint32_t taps_after_first_spell)
{
    controller.synthetic_phase = SyntheticInputPhase::quick_select_hold;
    controller.synthetic_phase_until = now + kQuickSelectHoldMs;
    controller.remaining_taps = taps_after_first_spell;
}

void ApplyVirtualTap(ControllerContext& controller, XINPUT_STATE* state, ULONGLONG now)
{
    state->Gamepad.wButtons &= ~kInterceptedButton;

    switch (controller.synthetic_phase) {
    case SyntheticInputPhase::idle:
        return;

    case SyntheticInputPhase::quick_select_hold:
        if (now < controller.synthetic_phase_until) {
            state->Gamepad.wButtons |= kInterceptedButton;
            return;
        }

        controller.synthetic_phase = SyntheticInputPhase::quick_select_release_gap;
        controller.synthetic_phase_until = now + kQuickSelectReleaseGapMs;
        return;

    case SyntheticInputPhase::quick_select_release_gap:
        if (now < controller.synthetic_phase_until) {
            return;
        }

        if (controller.remaining_taps == 0) {
            controller.synthetic_phase = SyntheticInputPhase::idle;
            controller.synthetic_phase_until = 0;
            return;
        }

        controller.synthetic_phase = SyntheticInputPhase::tap_press;
        controller.synthetic_phase_until = now + kTapPressMs;
        state->Gamepad.wButtons |= kInterceptedButton;
        return;

    case SyntheticInputPhase::tap_press:
        if (controller.remaining_taps == 0) {
            controller.synthetic_phase = SyntheticInputPhase::idle;
            controller.synthetic_phase_until = 0;
            return;
        }

        state->Gamepad.wButtons |= kInterceptedButton;
        if (now >= controller.synthetic_phase_until) {
            --controller.remaining_taps;
            controller.synthetic_phase = SyntheticInputPhase::tap_release;
            controller.synthetic_phase_until = now + kTapReleaseMs;
        }
        return;

    case SyntheticInputPhase::tap_release:
        if (now < controller.synthetic_phase_until) {
            return;
        }

        if (controller.remaining_taps == 0) {
            controller.synthetic_phase = SyntheticInputPhase::idle;
            controller.synthetic_phase_until = 0;
            return;
        }

        controller.synthetic_phase = SyntheticInputPhase::tap_press;
        controller.synthetic_phase_until = now + kTapPressMs;
        state->Gamepad.wButtons |= kInterceptedButton;
        return;
    }
}

DWORD WINAPI HookedXInputGetState(DWORD user_index, XINPUT_STATE* state)
{
    const DWORD result = g_original_xinput_get_state(user_index, state);
    if (result != ERROR_SUCCESS || state == nullptr || user_index >= g_controllers.size()) {
        return result;
    }

    auto& controller = g_controllers[user_index];
    const ULONGLONG now = GetTickCount64();
    const bool physical_dpad_up_pressed = (state->Gamepad.wButtons & kInterceptedButton) != 0;

    if (physical_dpad_up_pressed && !controller.dpad_up_down) {
        if (!CanStartRadialInput()) {
            return result;
        }

        controller.dpad_up_down = true;
        controller.radial_opened = false;
        controller.pressed_at = now;
    }

    if (controller.dpad_up_down && !controller.radial_opened && !IsNormalGameplayHudState()) {
        controller.dpad_up_down = false;
        controller.radial_opened = false;
        return result;
    }

    if (controller.dpad_up_down && physical_dpad_up_pressed && !controller.radial_opened &&
        (now - controller.pressed_at) >= kHoldThresholdMs) {
        controller.radial_opened = true;
        g_open_spell_slots = GetMemorizedSpells();
        int initial_selection = GetCurrentSpellSlot();
        if (initial_selection < 0 && !g_open_spell_slots.empty()) {
            initial_selection = 0;
        }

        radial_spell_menu::radial_menu::Open(initial_selection);
        Log("Opening radial spell menu for controller %lu.", static_cast<unsigned long>(user_index));
    }

    if (controller.radial_opened) {
        const float right_x = NormalizeThumbAxis(state->Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        const float right_y = NormalizeThumbAxis(state->Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);

        radial_spell_menu::radial_menu::UpdateSelectionFromStick(right_x, right_y, g_open_spell_slots.size());

        state->Gamepad.sThumbRX = 0;
        state->Gamepad.sThumbRY = 0;
    }

    if (controller.dpad_up_down && !physical_dpad_up_pressed) {
        if (controller.radial_opened) {
            const int selected_slot = radial_spell_menu::radial_menu::GetSelectedSlot();

            if (selected_slot >= 0 && selected_slot < static_cast<int>(g_open_spell_slots.size())) {
                const std::size_t selected_index = static_cast<std::size_t>(selected_slot);
                if (!SwitchToSpellSlot(g_open_spell_slots[selected_index].slot_index)) {
                    QueueQuickSelectSequence(controller, now, static_cast<std::uint32_t>(selected_index));
                    Log("Queued quick-select plus %u follow-up taps for spell selection.",
                        static_cast<unsigned int>(selected_index));
                }
            }

            radial_spell_menu::radial_menu::Close();
            g_open_spell_slots.clear();
            Log("Closed radial spell menu for controller %lu.", static_cast<unsigned long>(user_index));
        } else {
            QueueTapSequence(controller, now, 1);
        }

        controller.dpad_up_down = false;
        controller.radial_opened = false;
        state->Gamepad.wButtons &= ~kInterceptedButton;
    }

    if (controller.dpad_up_down || controller.radial_opened) {
        state->Gamepad.wButtons &= ~kInterceptedButton;
    }

    ApplyVirtualTap(controller, state, now);

    return result;
}

}  // namespace

bool Install()
{
    if (g_xinput_target != nullptr) {
        return true;
    }

    constexpr const wchar_t* kXinputModules[] = {
        L"xinput1_4.dll",
        L"xinput1_3.dll",
        L"xinput9_1_0.dll",
    };

    for (const auto* module_name : kXinputModules) {
        HMODULE module = LoadLibraryW(module_name);
        if (module == nullptr) {
            continue;
        }

        auto* const target = reinterpret_cast<LPVOID>(GetProcAddress(module, "XInputGetState"));
        if (target == nullptr) {
            continue;
        }

        const MH_STATUS create_status = MH_CreateHook(target, reinterpret_cast<LPVOID>(&HookedXInputGetState),
            reinterpret_cast<LPVOID*>(&g_original_xinput_get_state));
        if (create_status != MH_OK) {
            Log("Failed to create XInput hook: %s", MH_StatusToString(create_status));
            return false;
        }

        const MH_STATUS enable_status = MH_EnableHook(target);
        if (enable_status != MH_OK) {
            Log("Failed to enable XInput hook: %s", MH_StatusToString(enable_status));
            return false;
        }

        g_xinput_target = target;
        Log("Installed XInputGetState hook from %ls.", module_name);
        return true;
    }

    Log("Unable to locate an XInput DLL to hook.");
    return false;
}

void Shutdown()
{
    if (g_xinput_target == nullptr) {
        return;
    }

    MH_DisableHook(g_xinput_target);
    MH_RemoveHook(g_xinput_target);
    radial_spell_menu::radial_menu::Close();
    g_controllers = {};
    g_open_spell_slots.clear();
    g_xinput_target = nullptr;
    g_original_xinput_get_state = nullptr;
}

bool IsMenuOpen()
{
    return radial_spell_menu::radial_menu::IsOpen();
}

const std::vector<SpellSlot>& GetOpenSpellSlots()
{
    return g_open_spell_slots;
}

}  // namespace radial_spell_menu::input_hook
