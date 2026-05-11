#include "game/input/game_input_probe.h"

#include "game/input/radial_camera.h"
#include "game/input/radial_switch.h"

namespace radial_menu_mod::game_input_probe {

bool Initialize()
{
    radial_switch::Initialize();
    radial_camera::Initialize(&radial_switch::IsRadialActive);
    return true;
}

void SampleFrame()
{
    radial_switch::SampleFrame();
    radial_camera::SampleFrame();
}

}  // namespace radial_menu_mod::game_input_probe
