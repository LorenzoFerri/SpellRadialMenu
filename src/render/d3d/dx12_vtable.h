#pragma once

namespace radial_spell_menu::dx12_vtable {

struct HookTargets {
    void* present = nullptr;
    void* execute_command_lists = nullptr;
};

bool DiscoverHookTargets(HookTargets& targets);

}  // namespace radial_spell_menu::dx12_vtable
