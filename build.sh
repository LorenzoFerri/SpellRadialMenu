#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
VENDOR_DIR="${PROJECT_ROOT}/vendor"
IMGUI_DIR="${VENDOR_DIR}/imgui"
MINHOOK_DIR="${VENDOR_DIR}/minhook"
TOOLCHAIN_DIR="${PROJECT_ROOT}/toolchains"
LLVM_MINGW_ROOT="${TOOLCHAIN_DIR}/llvm-mingw"
LLVM_MINGW_ARCHIVE="${TOOLCHAIN_DIR}/llvm-mingw.tar.xz"
LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/20260421/llvm-mingw-20260421-ucrt-ubuntu-22.04-x86_64.tar.xz"

IMGUI_REPO="https://github.com/ocornut/imgui.git"
IMGUI_REF="a70b97ee48b5a83fc3aa4dec479d64233b715363"
MINHOOK_REPO="https://github.com/TsudaKageyu/minhook.git"
MINHOOK_REF="05c06c5bbca226b72ffb40fc0caaef33bcaf6f74"

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'Missing required command: %s\n' "$1" >&2
        exit 1
    fi
}

ensure_vendor() {
    local name="$1"
    local repo="$2"
    local ref="$3"
    local dir="$4"
    local check_file="$5"

    if [[ -f "${dir}/${check_file}" ]]; then
        return
    fi

    require_cmd git
    rm -rf "${dir}"
    git clone --filter=blob:none --no-checkout "${repo}" "${dir}"
    git -C "${dir}" fetch --depth 1 origin "${ref}"
    git -C "${dir}" checkout --detach FETCH_HEAD

    if [[ ! -f "${dir}/${check_file}" ]]; then
        printf 'Failed to prepare %s in %s\n' "${name}" "${dir}" >&2
        exit 1
    fi
}

ensure_portable_llvm_mingw() {
    if [[ -x "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++" ]]; then
        return
    fi

    require_cmd curl
    require_cmd tar

    local extract_dir="${TOOLCHAIN_DIR}/llvm-mingw-extract"
    rm -rf "${LLVM_MINGW_ROOT}" "${extract_dir}"
    mkdir -p "${extract_dir}"
    curl -L --fail --output "${LLVM_MINGW_ARCHIVE}" "${LLVM_MINGW_URL}"
    tar -xf "${LLVM_MINGW_ARCHIVE}" -C "${extract_dir}"

    local extracted=("${extract_dir}"/*)
    if [[ ${#extracted[@]} -ne 1 || ! -d "${extracted[0]}" ]]; then
        printf 'Unexpected llvm-mingw archive layout.\n' >&2
        exit 1
    fi

    mv "${extracted[0]}" "${LLVM_MINGW_ROOT}"
    rm -rf "${extract_dir}"
}

require_cmd cmake
require_cmd ninja

mkdir -p "${BUILD_DIR}" "${VENDOR_DIR}" "${PROJECT_ROOT}/natives" "${TOOLCHAIN_DIR}"

ensure_vendor "ImGui" "${IMGUI_REPO}" "${IMGUI_REF}" "${IMGUI_DIR}" "imgui.cpp"
ensure_vendor "MinHook" "${MINHOOK_REPO}" "${MINHOOK_REF}" "${MINHOOK_DIR}" "include/MinHook.h"

TOOLCHAIN_FILE="${PROJECT_ROOT}/cmake/mingw-w64-toolchain.cmake"
if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1 && command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    :
else
    ensure_portable_llvm_mingw
    export LLVM_MINGW_ROOT
    TOOLCHAIN_FILE="${PROJECT_ROOT}/cmake/llvm-mingw-toolchain.cmake"
fi

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}"

cmake --build "${BUILD_DIR}" --config Release

printf '\nBuilt DLL:\n  %s/natives/RadialSpellMenu.dll\n' "${PROJECT_ROOT}"
printf '\nLaunch with me3:\n  me3 launch --profile "%s/RadialSpellMenuProfile.me3"\n' "${PROJECT_ROOT}"
