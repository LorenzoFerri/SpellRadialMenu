#pragma once

#include <cstdint>
#include <string>

namespace radial_menu_mod::message_repository {

bool Initialize();
std::string LookupMagicName(std::uint32_t msg_id);
std::string LookupGoodsName(std::uint32_t msg_id);

}  // namespace radial_menu_mod::message_repository
