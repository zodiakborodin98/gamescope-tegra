#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# M7g-xwl - isolated library wrapper for the PINNED Xwayland ELF.
#
# Loader audit (evidence: reports/M7g-run20-result.md, logs/artifacts/staging/
# m7g-diag20.stdout.txt; official libglvnd doc src/EGL/icd_enumeration.md):
#  1) EGL loader = GLVND. The pinned Xwayland links the dispatch libEGL.so.1
#     (run #20 backtrace: libEGL.so.1 -> libEGL_mesa.so.0, SIGILL); the stub in
#     the SAME run reported "EGL 1.5 vendor=NVIDIA".
#  2) GLVND loads EVERY vendor ICD it discovers. Default discovery =
#     __EGL_VENDOR_LIBRARY_DIRS=${sysconfdir}/glvnd/egl_vendor.d:
#     ${datadir}/glvnd/egl_vendor.d (normally /etc then /usr/share), each dir
#     scanned for *.json in strcmp() filename order (10_nvidia before 50_mesa).
#     Setting __EGL_VENDOR_LIBRARY_FILENAMES replaces that with an explicit,
#     ordered JSON list and loads NO other ICD.
#  3) In run #20 a Mesa EGLDisplay was created + eglInitialize()d, so Mesa's
#     gpuvis/LSE code ran and SIGILL'd on Cortex-A57 (no LSE atomics). GLVND
#     dispatched Xwayland's EGL bootstrap to Mesa; the stub's device-only
#     bootstrap stayed on NVIDIA. Restricting this child to the NVIDIA ICD
#     keeps Mesa's vendor library from ever being loaded.
#  4) Child-only: the vendor env below is exported just before exec. Revert =
#     unset XWL_EGL_VENDOR_JSON / a private libgbm dir without it.
#
# Also unchanged: the installed tegra libnvgbm.so LACKS
# gbm_bo_get_fd_for_plane / gbm_bo_create_with_modifiers2, so ONLY this
# Xwayland process gets a private dir holding Mesa's libgbm.so.1 (hash-pinned)
# prepended to LD_LIBRARY_PATH.
#
# NO LD_PRELOAD. NO global/system change. Nothing is exported beyond the child.
#
# WLR_XWAYLAND-style: a compositor may point WLR_XWAYLAND at this script; it
# exec's the real pinned ELF and forwards all arguments unchanged.
set -u
: "${XWL_XWAYLAND_ELF:?set XWL_XWAYLAND_ELF to the pinned Xwayland ELF path}"
DIR="${XWL_PRIVATE_LIBGBM_DIR:-$(dirname -- "$0")/private-libgbm}"
if [ -d "$DIR" ]; then
  LD_LIBRARY_PATH="$DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  export LD_LIBRARY_PATH
fi

# --- child-only NVIDIA EGL vendor selection --------------------------------
# Never silently fall back to Mesa: if no NVIDIA ICD JSON resolves, fail loudly.
json_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum -- "$1" | cut -d' ' -f1
  elif command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$1" | sed 's/.*= *//'
  else
    echo unavailable
  fi
}

EGL_JSON=""
if [ -n "${XWL_EGL_VENDOR_JSON:-}" ]; then
  [ -f "$XWL_EGL_VENDOR_JSON" ] || {
    echo "XWL_EGL_VENDOR_FAIL: XWL_EGL_VENDOR_JSON='$XWL_EGL_VENDOR_JSON' not found" >&2
    exit 70
  }
  EGL_JSON="$XWL_EGL_VENDOR_JSON"
else
  for c in \
    /usr/share/glvnd/egl_vendor.d/10_nvidia.json \
    /etc/glvnd/egl_vendor.d/10_nvidia.json \
    /usr/lib/aarch64-linux-gnu/tegra-egl/nvidia.json \
    /usr/lib/tegra-egl/nvidia.json
  do
    if [ -f "$c" ]; then EGL_JSON="$c"; break; fi
  done
fi
if [ -z "$EGL_JSON" ]; then
  echo "XWL_EGL_VENDOR_FAIL: no NVIDIA EGL vendor JSON found (env XWL_EGL_VENDOR_JSON and default candidates); refusing to start Xwayland where GLVND could pick Mesa" >&2
  exit 70
fi
__EGL_VENDOR_LIBRARY_FILENAMES="$EGL_JSON"
export __EGL_VENDOR_LIBRARY_FILENAMES

# Diagnostics of the selected environment (stderr -> xwayland.log).
echo "XWL_EGL_VENDOR_JSON=$EGL_JSON sha256=$(json_sha256 "$EGL_JSON")" >&2
echo "XWL_PRIVATE_LIBGBM_DIR=$DIR" >&2

exec "$XWL_XWAYLAND_ELF" "$@"
