#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="${HOME}/RadialSpellMenu"
BUILD_DIR="${PROJECT_ROOT}/build"
VENDOR_DIR="${PROJECT_ROOT}/vendor"
IMGUI_DIR="${VENDOR_DIR}/imgui"
MINHOOK_DIR="${VENDOR_DIR}/minhook"
TOOLCHAIN_DIR="${PROJECT_ROOT}/toolchains"
LLVM_MINGW_ROOT="${TOOLCHAIN_DIR}/llvm-mingw"
LLVM_MINGW_ARCHIVE="${TOOLCHAIN_DIR}/llvm-mingw.tar.xz"
LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/20260421/llvm-mingw-20260421-ucrt-ubuntu-22.04-x86_64.tar.xz"

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing required command: $1" >&2
        exit 1
    fi
}

ensure_portable_llvm_mingw() {
    require_cmd curl
    require_cmd tar

    mkdir -p "${TOOLCHAIN_DIR}"

    if [[ ! -x "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++" ]]; then
        rm -rf "${LLVM_MINGW_ROOT}" "${TOOLCHAIN_DIR}/llvm-mingw-extract"
        curl -L --fail --output "${LLVM_MINGW_ARCHIVE}" "${LLVM_MINGW_URL}"
        mkdir -p "${TOOLCHAIN_DIR}/llvm-mingw-extract"
        tar -xf "${LLVM_MINGW_ARCHIVE}" -C "${TOOLCHAIN_DIR}/llvm-mingw-extract"
        local extracted_dir
        extracted_dir="$(find "${TOOLCHAIN_DIR}/llvm-mingw-extract" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
        mv "${extracted_dir}" "${LLVM_MINGW_ROOT}"
        rm -rf "${TOOLCHAIN_DIR}/llvm-mingw-extract"
    fi
}

clone_or_update() {
    local repo_url="$1"
    local target_dir="$2"
    local branch="${3:-}"

    if [[ ! -d "${target_dir}/.git" ]]; then
        if [[ -n "${branch}" ]]; then
            git clone --depth 1 --branch "${branch}" "${repo_url}" "${target_dir}"
        else
            git clone --depth 1 "${repo_url}" "${target_dir}"
        fi
        return
    fi

    git -C "${target_dir}" fetch --depth 1 origin
    if [[ -n "${branch}" ]]; then
        git -C "${target_dir}" checkout "${branch}"
        git -C "${target_dir}" pull --ff-only --depth 1 origin "${branch}"
    else
        local current_branch
        current_branch="$(git -C "${target_dir}" rev-parse --abbrev-ref HEAD)"
        git -C "${target_dir}" pull --ff-only --depth 1 origin "${current_branch}"
    fi
}

require_cmd sudo
require_cmd pacman
require_cmd git
require_cmd cmake
require_cmd ninja

mkdir -p "${PROJECT_ROOT}" "${BUILD_DIR}" "${VENDOR_DIR}" "${PROJECT_ROOT}/natives" "${TOOLCHAIN_DIR}"

clone_or_update https://github.com/ocornut/imgui.git "${IMGUI_DIR}" master
clone_or_update https://github.com/TsudaKageyu/minhook.git "${MINHOOK_DIR}" master

TOOLCHAIN_FILE="${PROJECT_ROOT}/cmake/mingw-w64-toolchain.cmake"

if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1 && command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    :
elif sudo -n true >/dev/null 2>&1; then
    sudo pacman -S --needed --noconfirm \
        base-devel \
        cmake \
        git \
        mingw-w64-crt \
        mingw-w64-gcc \
        mingw-w64-headers \
        mingw-w64-winpthreads \
        ninja
else
    ensure_portable_llvm_mingw
    export LLVM_MINGW_ROOT
    TOOLCHAIN_FILE="${PROJECT_ROOT}/cmake/llvm-mingw-toolchain.cmake"
fi

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}"

cmake --build "${BUILD_DIR}" --config Release

ERR_DLL_DIR="/home/faith/ERRv2.2.4.4/dll/offline"
if [[ -d "${ERR_DLL_DIR}" ]]; then
    cp -f "${PROJECT_ROOT}/natives/RadialSpellMenu.dll" "${ERR_DLL_DIR}/RadialSpellMenu.dll"
fi

echo
echo "Built DLL:"
echo "  ${PROJECT_ROOT}/natives/RadialSpellMenu.dll"
echo
echo "Launch with me3:"
echo "  me3 launch --profile \"${PROJECT_ROOT}/myprofile.me3\""
