#pragma once

namespace radial_menu_mod::dx12_vtable {

struct HookTargets {
    void* present = nullptr;
    void* resize_buffers = nullptr;
    void* resize_buffers1 = nullptr;
    void* set_fullscreen_state = nullptr;
    void* execute_command_lists = nullptr;
};

bool DiscoverHookTargets(HookTargets& targets);

}  // namespace radial_menu_mod::dx12_vtable
