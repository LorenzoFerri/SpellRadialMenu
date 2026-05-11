#pragma once

#include <cstdint>

namespace radial_menu_mod::in_game_pad {

bool PollInput(std::int32_t input);
void InvalidateCaches();

}  // namespace radial_menu_mod::in_game_pad
