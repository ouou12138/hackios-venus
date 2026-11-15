#!/usr/bin/env bash
# Only builds component artifacts; does not install them or change boot profiles.
set -euo pipefail
module=$(cd "$(dirname "$0")/.." && pwd)
xnu_tree=${1:?usage: build-userland.sh /path/to/full-hackios-tree}
xnu_tree=$(cd "$xnu_tree" && pwd)
source "$xnu_tree/xnu-env.sh"
mesa="$module/out/work/xnu-deps/build/mesa/out"
test -f "$mesa/libredwoodair.dylib"
expected=$(cat "$module/backend/mesa/air/"* | sha256sum | cut -d' ' -f1)
test "$(cut -d' ' -f1 "$mesa/built-from-air")" = "$expected"
out="$module/out/runtime"
mkdir -p "$out/lib/RedwoodMetal.bundle" "$out/bin"
cc=(xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1)
source_dir="$module/src/userland"
"${cc[@]}" -dynamiclib -install_name /redwood/lib/libredwoodmetal.dylib \
    -framework Foundation -framework IOKit -framework Metal -lobjc \
    -o "$out/lib/libredwoodmetal.dylib" "$source_dir/redwoodmetal.m" "$source_dir/redwooddrawable.m"
"${cc[@]}" -bundle -framework Foundation -framework IOKit -framework Metal -lobjc \
    -o "$out/lib/RedwoodMetal.bundle/RedwoodMetal" "$source_dir/redwoodmetal.m" "$source_dir/redwooddrawable.m"
cp "$source_dir/metal_plugin.plist" "$out/lib/RedwoodMetal.bundle/Info.plist"
cp "$mesa/libredwoodair.dylib" "$out/lib/"
"${cc[@]}" -framework CoreFoundation -framework IOKit \
    -o "$out/bin/metal-enable" "$source_dir/metal-enable.c"
"${cc[@]}" -framework Foundation -framework IOKit -framework Metal -lobjc \
    -o "$out/bin/venus-metal-desktop-probe" "$module/tests/desktop-probe.m"
"$THEOS/toolchain/linux/iphone/bin/ldid" -S"$module/tests/probe.entitlements" "$out/bin/venus-metal-desktop-probe"
"${cc[@]}" -framework Foundation -framework Metal -framework QuartzCore -framework CoreGraphics -lobjc \
    -o "$out/bin/lemonade_metal_ca" "$module/tests/lemonade_metal_ca.m"
printf 'Built runtime under %s; no installation or default activation performed.\n' "$out"
