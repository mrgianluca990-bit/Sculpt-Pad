#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "Questo script compila il VST3 macOS. Eseguilo su un Mac con Xcode Command Line Tools e CMake."
  exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "È richiesto CMake 3.22 o successivo. Installalo e avvia nuovamente questo script."
  exit 1
fi

if ! xcode-select -p >/dev/null 2>&1; then
  echo "Sono richiesti Xcode Command Line Tools. Esegui prima: xcode-select --install"
  exit 1
fi

sculpt_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sculpt_build="${SCULPT_PAD_BUILD_DIR:-${sculpt_root}/build-macos}"
sculpt_dist="${SCULPT_PAD_DIST_DIR:-${sculpt_root}/dist}"
sculpt_jobs="${SCULPT_PAD_BUILD_JOBS:-4}"

configure_args=(
  -S "$sculpt_root"
  -B "$sculpt_build"
  -G "Unix Makefiles"
  -DCMAKE_BUILD_TYPE=Release
  '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
)
if [[ -n "${SCULPT_JUCE_SOURCE_DIR:-}" ]]; then
  configure_args+=("-DSCULPT_JUCE_SOURCE_DIR=${SCULPT_JUCE_SOURCE_DIR}")
fi

cmake "${configure_args[@]}"
cmake --build "$sculpt_build" --config Release --target SculptPad_VST3 --parallel "$sculpt_jobs"

sculpt_bundle="${sculpt_build}/SculptPad_artefacts/Release/VST3/Sculpt Pad.vst3"
sculpt_binary="${sculpt_bundle}/Contents/MacOS/Sculpt Pad"
if [[ ! -d "$sculpt_bundle" || ! -f "$sculpt_binary" ]]; then
  echo "La build non ha prodotto il bundle VST3 atteso: $sculpt_bundle"
  exit 1
fi

lipo "$sculpt_binary" -verify_arch arm64 x86_64
plutil -lint "${sculpt_bundle}/Contents/Info.plist"

# Keep the sample attribution and licences with the self-contained instrument.
sculpt_notices="${sculpt_bundle}/Contents/Resources/ThirdParty"
mkdir -p "$sculpt_notices"
cp -R "${sculpt_root}/ThirdParty/." "$sculpt_notices/"

codesign --force --deep --sign - "$sculpt_bundle"
codesign --verify --deep --strict --verbose=2 "$sculpt_bundle"

mkdir -p "$sculpt_dist"
sculpt_archive="${sculpt_dist}/Sculpt-Pad-macOS-VST3.zip"
ditto -c -k --sequesterRsrc --keepParent "$sculpt_bundle" "$sculpt_archive"

echo "VST3 universale compilato (Apple Silicon e Intel): $sculpt_archive"
echo "Il bundle ha una firma locale ad hoc e non è notarizzato. Non sono stati installati file nelle cartelle dei plugin."
