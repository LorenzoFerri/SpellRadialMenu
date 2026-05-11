#include "game/input/native_input.h"

#include "game/input/radial_camera.h"
#include "game/input/radial_switch.h"

namespace radial_menu_mod::native_input {

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

}  // namespace radial_menu_mod::native_input
