#pragma once

#include <cstdint>

namespace radial_menu_mod::singleton_resolver {

std::uintptr_t ResolveSingletonStaticAddress(const char* singleton_name);

}  // namespace radial_menu_mod::singleton_resolver
