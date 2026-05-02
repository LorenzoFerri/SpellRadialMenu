with open("src/spell_icon_resolve.cpp", "r") as f:
    content = f.read()

import re

new_func = """bool TryLoadSpellIconDynamically(std::uint32_t icon_id, const wchar_t* fallback_path_subdir, const wchar_t* fallback_path_flat, DdsMip0Data& out)
{
    wchar_t base_name[72] = {};
    FormatMenuKnowledgeBaseName(icon_id, base_name, sizeof(base_name) / sizeof(base_name[0]));

    // Check standard ME2 paths
    wchar_t tpf_path[MAX_PATH] = {};
    if (swprintf(tpf_path, sizeof(tpf_path) / sizeof(tpf_path[0]), L"menu\\\\hi\\\\00_Solo\\\\%ls.tpf.dcx", base_name) > 0) {
        if (TryLoadTpfDcxFromFile(tpf_path, out)) {
            return true;
        }
    }

    if (TryLoadTpfDcxFromBhd(L"menu\\\\hi\\\\00_Solo.tpfbhd", L"menu\\\\hi\\\\00_Solo.tpfbdt", icon_id, out)) {
        return true;
    }

    // Attempt to load absolutely from the DLL path (useful for mods like ERR)
    HMODULE hm = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&TryLoadSpellIconDynamically, &hm)) {
        wchar_t dll_path[MAX_PATH] = {};
        GetModuleFileNameW(hm, dll_path, MAX_PATH);
        
        std::wstring dll_dir = dll_path;
        size_t last_slash = dll_dir.find_last_of(L"\\\\/");
        if (last_slash != std::wstring::npos) {
            dll_dir = dll_dir.substr(0, last_slash);
            // Navigate up from dll/offline/ to the root of the mod folder
            // Typically ERRv2.2.4.4/dll/offline -> ERRv2.2.4.4/mod
            size_t dll_slash = dll_dir.find_last_of(L"\\\\/");
            if (dll_slash != std::wstring::npos) {
                std::wstring parent = dll_dir.substr(0, dll_slash);
                size_t dll_slash2 = parent.find_last_of(L"\\\\/");
                if (dll_slash2 != std::wstring::npos) {
                    std::wstring root = parent.substr(0, dll_slash2);
                    
                    // Try ERR's mod folder
                    std::wstring mod_bhd = root + L"\\\\mod\\\\menu\\\\hi\\\\00_Solo.tpfbhd";
                    std::wstring mod_bdt = root + L"\\\\mod\\\\menu\\\\hi\\\\00_Solo.tpfbdt";
                    if (TryLoadTpfDcxFromBhd(mod_bhd.c_str(), mod_bdt.c_str(), icon_id, out)) {
                        return true;
                    }
                    
                    std::wstring mod_tpf = root + L"\\\\mod\\\\menu\\\\hi\\\\00_Solo\\\\" + base_name + L".tpf.dcx";
                    if (TryLoadTpfDcxFromFile(mod_tpf.c_str(), out)) {
                        return true;
                    }
                }
            }
        }
    }

    const wchar_t* try_paths[] = {
        fallback_path_subdir,
        fallback_path_flat,
    };

    for (const wchar_t* path : try_paths) {
        if (path == nullptr) continue;
        if (TryLoadDdsMip0FromFile(path, out)) {
            return true;
        }
    }

    wchar_t dds_path[MAX_PATH] = {};
    if (swprintf(dds_path, sizeof(dds_path) / sizeof(dds_path[0]), L"spell_icons\\\\%ls.dds", base_name) > 0) {
        if (TryLoadDdsMip0FromFile(dds_path, out)) {
            return true;
        }
    }

    return false;
}"""

# Replace the whole function TryLoadSpellIconDynamically
pattern = r'bool TryLoadSpellIconDynamically\(std::uint32_t icon_id, const wchar_t\* fallback_path_subdir, const wchar_t\* fallback_path_flat, DdsMip0Data& out\)[\s\S]*?return false;\s+\}'

content = re.sub(pattern, new_func, content)

with open("src/spell_icon_resolve.cpp", "w") as f:
    f.write(content)

