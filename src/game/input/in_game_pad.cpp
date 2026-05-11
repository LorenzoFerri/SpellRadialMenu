#include "game/input/in_game_pad.h"

#include "core/common.h"
#include "game/state/singleton_resolver.h"

#include <cstddef>

namespace radial_menu_mod::in_game_pad {
namespace {

constexpr std::uintptr_t kLoadedPadEntriesOffset = 0xE0;
constexpr std::size_t kLoadedPadEntryCount = 9;
constexpr std::uintptr_t kPadEntryPadOffset = 0x00;
constexpr std::uintptr_t kPadPadDeviceOffset = 0x10;
constexpr std::uintptr_t kPadAllowPollingOffset = 0x20;
constexpr std::uintptr_t kPadKeyAssignOffset = 0x28;
constexpr std::uintptr_t kPadInputTypeGroupOffset = 0x38;
constexpr std::uintptr_t kPadInputCodeCheckOffset = 0x40;
constexpr std::uintptr_t kKeyAssignVirtualInputIndexMapOffset = 0x38;
constexpr std::uintptr_t kInputDevicesVirtualMultiDeviceOffset = 0x08;
constexpr std::uintptr_t kVirtualMultiDeviceVirtualInputDataOffset = 0x10;
constexpr std::uintptr_t kVirtualInputDataDigitalBitsetOffset = 0x30;
constexpr std::uintptr_t kDynamicBitsetIntegerCountOffset = 0x08;
constexpr std::uintptr_t kDynamicBitsetDataOffset = 0x10;
constexpr std::uint32_t kInGamePadUserInputVtableRva = 0x2BE0488;

struct TreeHeader {
    std::uintptr_t allocator = 0;
    std::uintptr_t head = 0;
    std::uintptr_t size = 0;
};

struct TreeNodeHeader {
    std::uintptr_t left = 0;
    std::uintptr_t parent = 0;
    std::uintptr_t right = 0;
    std::uint8_t black_red = 0;
    std::uint8_t is_nil = 0;
};

struct InputTypeGroup {
    std::int32_t mapped_inputs[4] = {};
    std::uint32_t input_types[4] = {};
};

struct InputCodeState {
    bool state_1 = false;
    bool state_2 = false;
};

template <typename T>
bool ReadGameMemory(std::uintptr_t address, T& value)
{
    if (!IsReadableMemory(reinterpret_cast<const void*>(address), sizeof(T))) return false;
    value = *reinterpret_cast<const T*>(address);
    return true;
}

std::uintptr_t TreeMinNode(std::uintptr_t node)
{
    TreeNodeHeader header = {};
    while (ReadGameMemory(node, header)) {
        TreeNodeHeader left_header = {};
        if (header.is_nil != 0 || !ReadGameMemory(header.left, left_header) || left_header.is_nil != 0) break;
        node = header.left;
    }
    return node;
}

std::uintptr_t TreeNextNode(std::uintptr_t node, std::uintptr_t head)
{
    TreeNodeHeader header = {};
    if (!ReadGameMemory(node, header)) return 0;

    TreeNodeHeader right_header = {};
    if (ReadGameMemory(header.right, right_header) && right_header.is_nil == 0) return TreeMinNode(header.right);

    while (true) {
        const auto parent = header.parent;
        if (parent == head) return 0;

        TreeNodeHeader parent_header = {};
        if (!ReadGameMemory(parent, parent_header)) return 0;
        if (node != parent_header.right) return parent;

        node = parent;
        header = parent_header;
    }
}

template <typename T>
bool FindTreeValue(std::uintptr_t tree, std::int32_t key, T& value)
{
    TreeHeader tree_header = {};
    if (!ReadGameMemory(tree, tree_header) || !tree_header.head || tree_header.size > 1024) return false;

    TreeNodeHeader head_header = {};
    if (!ReadGameMemory(tree_header.head, head_header) || !head_header.parent) return false;

    auto node = TreeMinNode(head_header.parent);
    for (std::uintptr_t i = 0; node && node != tree_header.head && i < tree_header.size; ++i) {
        std::int32_t node_key = 0;
        if (!ReadGameMemory(node + 0x1C, node_key)) return false;
        if (node_key == key) return ReadGameMemory(node + 0x20, value);
        node = TreeNextNode(node, tree_header.head);
    }

    return false;
}

std::uintptr_t ResolveFD4PadManager()
{
    static std::uintptr_t static_address = 0;
    static bool searched = false;
    if (!searched) {
        searched = true;
        static_address = singleton_resolver::ResolveSingletonStaticAddress("FD4PadManager");
        if (static_address) Log("FD4PadManager static resolved at 0x%llX.", static_cast<unsigned long long>(static_address));
    }

    if (!static_address) return 0;
    std::uintptr_t pad_manager = 0;
    return ReadGameMemory(static_address, pad_manager) ? pad_manager : 0;
}

bool ResolveInGamePad(std::uintptr_t& in_game_pad)
{
    in_game_pad = 0;

    const auto pad_manager = ResolveFD4PadManager();
    if (!pad_manager) return false;

    const auto module_base = GetModuleBase();
    for (std::size_t i = 0; i < kLoadedPadEntryCount; ++i) {
        std::uintptr_t entry = 0;
        std::uintptr_t pad = 0;
        std::uintptr_t vtable = 0;
        if (!ReadGameMemory(pad_manager + kLoadedPadEntriesOffset + i * sizeof(entry), entry) || !entry) continue;
        if (!ReadGameMemory(entry + kPadEntryPadOffset, pad) || !pad) continue;
        if (!ReadGameMemory(pad, vtable) || vtable < module_base) continue;

        if (static_cast<std::uint32_t>(vtable - module_base) == kInGamePadUserInputVtableRva) {
            in_game_pad = pad;
            return true;
        }
    }

    return false;
}

bool ReadVirtualDigitalInput(std::uintptr_t input_devices, std::int32_t virtual_input_index)
{
    if (virtual_input_index < 0) return false;

    std::uintptr_t virtual_multi_device = 0;
    if (!ReadGameMemory(input_devices + kInputDevicesVirtualMultiDeviceOffset, virtual_multi_device) ||
        !virtual_multi_device) {
        return false;
    }

    const auto bitset = virtual_multi_device + kVirtualMultiDeviceVirtualInputDataOffset +
        kVirtualInputDataDigitalBitsetOffset;

    std::uintptr_t integer_count = 0;
    std::uintptr_t bitset_data = 0;
    if (!ReadGameMemory(bitset + kDynamicBitsetIntegerCountOffset, integer_count) ||
        !ReadGameMemory(bitset + kDynamicBitsetDataOffset, bitset_data) || !bitset_data) {
        return false;
    }

    const auto row_index = static_cast<std::uintptr_t>(virtual_input_index / 32);
    if (row_index >= integer_count) return false;

    std::uint32_t row = 0;
    if (!ReadGameMemory(bitset_data + row_index * sizeof(row), row)) return false;
    return ((row >> (virtual_input_index & 31)) & 1u) != 0;
}

}  // namespace

bool PollInput(std::int32_t input)
{
    std::uintptr_t pad = 0;
    if (!ResolveInGamePad(pad)) return false;

    bool allow_polling = false;
    if (!ReadGameMemory(pad + kPadAllowPollingOffset, allow_polling) || !allow_polling) return false;

    std::uintptr_t input_devices = 0;
    std::uintptr_t key_assign = 0;
    std::uintptr_t input_type_group_tree = 0;
    std::uintptr_t input_code_check_tree = 0;
    if (!ReadGameMemory(pad + kPadPadDeviceOffset, input_devices) || !input_devices ||
        !ReadGameMemory(pad + kPadKeyAssignOffset, key_assign) || !key_assign ||
        !ReadGameMemory(pad + kPadInputTypeGroupOffset, input_type_group_tree) || !input_type_group_tree ||
        !ReadGameMemory(pad + kPadInputCodeCheckOffset, input_code_check_tree) || !input_code_check_tree) {
        return false;
    }

    InputTypeGroup group = {};
    if (!FindTreeValue(input_type_group_tree, input, group)) return false;

    std::uintptr_t virtual_input_index_tree = 0;
    if (!ReadGameMemory(key_assign + kKeyAssignVirtualInputIndexMapOffset, virtual_input_index_tree) ||
        !virtual_input_index_tree) {
        return false;
    }

    for (std::size_t i = 0; i < 4; ++i) {
        const auto mapped_input = group.mapped_inputs[i];
        if (mapped_input == -1 || group.input_types[i] != 0) continue;

        InputCodeState code_state = {};
        std::int32_t virtual_input_index = -1;
        if (!FindTreeValue(input_code_check_tree, mapped_input, code_state) || !code_state.state_1 ||
            code_state.state_2 || !FindTreeValue(virtual_input_index_tree, mapped_input, virtual_input_index)) {
            continue;
        }

        if (ReadVirtualDigitalInput(input_devices, virtual_input_index)) return true;
    }

    return false;
}

}  // namespace radial_menu_mod::in_game_pad
