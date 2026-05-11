#pragma once

namespace radial_menu_mod::radial_camera {

using RadialActiveFn = bool (*)();

bool Initialize(RadialActiveFn is_radial_active);
void SampleFrame();
float ConsumeSelectionX();
float ConsumeSelectionY();

}  // namespace radial_menu_mod::radial_camera
