#include "input/input_hook.h"

#include "game/state/gameplay_state.h"
#include "input/radial_input.h"
#include "render/ui/radial_menu.h"

namespace radial_menu_mod::input_hook {

bool Install()
{
    return true;
}

void Shutdown()
{
    radial_input::Reset();
}

bool IsMenuOpen()
{
    return radial_menu::IsOpen();
}

bool IsGameplayReady()
{
    return gameplay_state::IsNormalGameplayHudState();
}

const std::vector<SpellSlot>& GetOpenSpellSlots()
{
    return radial_input::GetOpenSpellSlots();
}

const char* GetOpenMenuTitle()
{
    return radial_input::GetOpenMenuTitle();
}

const char* GetOpenMenuControls()
{
    return radial_input::GetOpenMenuControls();
}

}  // namespace radial_menu_mod::input_hook
