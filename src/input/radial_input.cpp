#include "input/radial_input.h"

#include "core/common.h"
#include "game/state/gameplay_state.h"
#include "render/ui/radial_menu.h"

namespace radial_menu_mod::radial_input {
namespace {

enum class RadialKind {
    none,
    spells,
    items,
};

struct RadialHoldState {
    bool hold_down = false;
    bool radial_opened = false;
    RadialKind active_kind = RadialKind::none;
};

RadialHoldState g_hold = {};
std::vector<RadialSlot> g_open_radial_slots;
RadialKind g_open_kind = RadialKind::none;

bool CanStartRadialInput(RadialKind kind)
{
    if (!gameplay_state::GetCachedNormalGameplayHudState()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("Radial input blocked because cached gameplay HUD state is not ready.");
        }
        return false;
    }
    return kind != RadialKind::none;
}

void ResetHoldState(RadialHoldState& hold)
{
    hold.hold_down = false;
    hold.radial_opened = false;
    hold.active_kind = RadialKind::none;
}

void BeginRadialHold(RadialHoldState& hold, RadialKind kind)
{
    hold.hold_down = true;
    hold.radial_opened = false;
    hold.active_kind = kind;
    Log("Radial hold started (kind=%s).", kind == RadialKind::items ? "items" : "spells");
}

void LoadOpenRadialSlots(RadialKind kind)
{
    g_open_radial_slots = kind == RadialKind::items ? GetQuickItems() : GetMemorizedSpells();
}

int CurrentSelectionFor(RadialKind kind)
{
    return kind == RadialKind::items ? GetCurrentQuickItemSlot() : GetCurrentSpellSlot();
}

bool OpenRadial(RadialHoldState& hold)
{
    LoadOpenRadialSlots(hold.active_kind);
    if (g_open_radial_slots.empty()) {
        Log("Radial open failed because no slots were available (kind=%s).",
            hold.active_kind == RadialKind::items ? "items" : "spells");
        g_open_kind = RadialKind::none;
        ResetHoldState(hold);
        return false;
    }

    hold.radial_opened = true;
    g_open_kind = hold.active_kind;

    int initial_selection = CurrentSelectionFor(hold.active_kind);
    if (initial_selection < 0) initial_selection = 0;

    radial_menu::Open(initial_selection);
    Log("Radial opened (kind=%s slots=%zu initial=%d).",
        hold.active_kind == RadialKind::items ? "items" : "spells",
        g_open_radial_slots.size(),
        initial_selection);
    return true;
}

void UpdateRadialSelection(const RadialHoldState& hold, float selection_x, float selection_y)
{
    if (!hold.radial_opened) return;
    radial_menu::UpdateSelectionFromDirection(selection_x, selection_y, g_open_radial_slots.size());
}

void ConfirmRadialSelection(RadialKind active_kind)
{
    const int selected_slot = radial_menu::GetSelectedSlot();
    if (selected_slot < 0 || selected_slot >= static_cast<int>(g_open_radial_slots.size())) return;

    const auto selected_index = static_cast<std::size_t>(selected_slot);
    if (active_kind == RadialKind::items) {
        (void)SwitchToQuickItemSlot(g_open_radial_slots[selected_index].slot_index);
    } else {
        (void)SwitchToSpellSlot(g_open_radial_slots[selected_index].slot_index);
    }
}

void CloseRadial()
{
    radial_menu::Close();
    g_open_radial_slots.clear();
    g_open_kind = RadialKind::none;
}

void HandleHoldRelease(RadialHoldState& hold)
{
    if (hold.radial_opened) {
        ConfirmRadialSelection(hold.active_kind);
        CloseRadial();
        Log("Radial closed by release.");
    }

    ResetHoldState(hold);
}

}  // namespace

void Reset()
{
    radial_menu::Close();
    g_hold = {};
    g_open_radial_slots.clear();
    g_open_kind = RadialKind::none;
}

void UpdateRadialHoldState(bool spell_hold_active, bool item_hold_active, float selection_x, float selection_y)
{
    auto& hold = g_hold;
    const RadialKind pressed_kind = spell_hold_active ? RadialKind::spells :
        (item_hold_active ? RadialKind::items : RadialKind::none);
    const bool hold_continues = hold.active_kind != RadialKind::none
        ? pressed_kind == hold.active_kind
        : pressed_kind != RadialKind::none;
    bool checked_gameplay_state = false;

    if (pressed_kind != RadialKind::none && !hold.hold_down) {
        if (!CanStartRadialInput(pressed_kind)) return;

        BeginRadialHold(hold, pressed_kind);
        checked_gameplay_state = true;
    }

    if (hold.hold_down && !hold.radial_opened && !checked_gameplay_state) {
        if (!gameplay_state::GetCachedNormalGameplayHudState()) {
            ResetHoldState(hold);
            return;
        }
    }

    if (hold.hold_down && hold_continues && !hold.radial_opened) {
        if (!OpenRadial(hold)) return;
    }

    UpdateRadialSelection(hold, selection_x, selection_y);

    if (hold.hold_down && !hold_continues) {
        HandleHoldRelease(hold);
    }
}

const std::vector<RadialSlot>& GetOpenRadialSlots()
{
    return g_open_radial_slots;
}

const char* GetOpenMenuTitle()
{
    return g_open_kind == RadialKind::items ? "Quick Item" : "Quick Spell";
}

const char* GetOpenMenuControls()
{
    return g_open_kind == RadialKind::items
        ? "Right Stick   Release D-pad Down Confirm"
        : "Right Stick   Release D-pad Up Confirm";
}

}  // namespace radial_menu_mod::radial_input
