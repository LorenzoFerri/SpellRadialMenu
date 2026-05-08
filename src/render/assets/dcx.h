#pragma once

#include <cstdint>
#include <vector>

namespace radial_spell_menu::dcx {

bool StartsWithDcx(const std::vector<std::uint8_t>& bytes);
bool Decompress(const std::vector<std::uint8_t>& dcx, std::vector<std::uint8_t>& out, const char** dflt_error);

}  // namespace radial_spell_menu::dcx
