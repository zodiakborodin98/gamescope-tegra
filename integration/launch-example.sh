#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Foreground example only; no service/global install or hardware prep.
set -eu
: "${GAMESCOPE_BIN:?path to your separately qualified Tegra Gamescope executable}"
: "${STEAM_BIN:?path to your own lawful ARM64 Steam launcher}"
: "${XWAYLAND_WRAPPER:?path to your qualified EGLStream Xwayland wrapper}"
: "${XWAYLAND_ELF:?path to that qualified Xwayland executable}"
: "${XWAYLAND_GBM_DIR:?path to its compatible private GBM closure}"
: "${CLIENT_LIB_PATH:?complete client-private Mesa/LLVM path plus runtime tail}"
: "${NVIDIA_ICD_JSON:?path to your compatible NVIDIA Vulkan ICD manifest}"
[ -x "$GAMESCOPE_BIN" ] && [ -x "$STEAM_BIN" ] && [ -x "$XWAYLAND_WRAPPER" ] && [ -x "$XWAYLAND_ELF" ]
[ -d "$XWAYLAND_GBM_DIR" ] && [ -r "$NVIDIA_ICD_JSON" ]
export WLR_XWAYLAND="$XWAYLAND_WRAPPER" XWL_XWAYLAND_ELF="$XWAYLAND_ELF"
export XWL_PRIVATE_LIBGBM_DIR="$XWAYLAND_GBM_DIR"
export VK_ICD_FILENAMES="$NVIDIA_ICD_JSON" VK_DRIVER_FILES="$NVIDIA_ICD_JSON"
unset DISPLAY WAYLAND_DISPLAY LD_PRELOAD
unset GALLIUM_DRIVER LIBGL_ALWAYS_SOFTWARE __GLX_VENDOR_LIBRARY_NAME
# Software GL variables belong only to the Steam child, not the Gamescope parent.
# supply any additional Steam flags as individually quoted arguments to this script.
exec "$GAMESCOPE_BIN" --backend tegra-l4t --expose-wayland -w 1280 -h 720 -W 720 -H 1280 -r 60 --scaler stretch -- \
    env -u GAMESCOPE_WAYLAND_DISPLAY -u LIBGL_ALWAYS_INDIRECT -u MESA_LOADER_DRIVER_OVERRIDE \
        LD_LIBRARY_PATH="$CLIENT_LIB_PATH" GALLIUM_DRIVER=llvmpipe LIBGL_ALWAYS_SOFTWARE=1 \
        __GLX_VENDOR_LIBRARY_NAME=mesa "$STEAM_BIN" -gamepadui "$@"
