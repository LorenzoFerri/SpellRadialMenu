#pragma once

#include <cstdint>

namespace radial_spell_menu::singleton_resolver {

std::uintptr_t ResolveSingletonStaticAddress(const char* singleton_name);

}  // namespace radial_spell_menu::singleton_resolver
