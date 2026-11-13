# Building from source

## Toolchain

- Haxe 4.3.x + hxcpp 4.3.2
- Lime **8.3.2** development checkout
- bgfx Metal static libraries (built from the bundled `bgfx-master`)
- Metal-specific OpenFL/Flixel forks

See [[Dependencies and versions]] for exact versions.

## 1. Clone the repositories

```sh
git clone https://github.com/TheComingUnitys/Codename-Metal
git clone https://github.com/TheComingUnitys/openfl-metal
git clone https://github.com/TheComingUnitys/flixel-metal
```

## 2. Point haxelib at the forks

From `Codename-Metal/CodenameEngine-1.0.1/`:

```sh
haxelib dev openfl  /path/to/openfl-metal
haxelib dev flixel  /path/to/flixel-metal
haxelib path openfl   # must print .../openfl-metal/src/
haxelib path flixel   # must print .../flixel-metal/
```

## 3. Build

```sh
cd Codename-Metal/CodenameEngine-1.0.1
export MACOSX_DEPLOYMENT_TARGET=11.0

# sync backend sources into the Lime dev checkout
./metal/scripts/apply.sh --lime "$HOME/haxelib-dev/lime-832" --skip-aot --skip-metallib

# native ndll
cd "$HOME/haxelib-dev/lime-832"
haxelib run lime rebuild mac -Dmetal -Dmetal_instancing -DMACOSX_DEPLOYMENT_TARGET=11.0

# app
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
haxelib run lime build mac -Dmetal -Dmetal_instancing -DMACOSX_DEPLOYMENT_TARGET=11.0

# copy the ndll into the app bundle
./metal/scripts/apply.sh --lime "$HOME/haxelib-dev/lime-832" --skip-aot --skip-metallib --deploy
```

## 4. Universal build

```sh
./metal/scripts/make-universal.sh --dmg
```

Output: `~/Downloads/CodenameEngine-Metal-universal.dmg`. The script lipo-combines arm64/x86_64, ad-hoc signs and prints the SHA-256.

## AOT shader cache

The shader cache is generated at build time and is **not committed**:

```sh
./metal/scripts/apply.sh --lime "$HOME/haxelib-dev/lime-832"   # full AOT + metallib generation
python3 metal/scripts/check-docs.py
```

Current baseline: **59 / 59** stages have a prebuilt `.metallib`.

## Smoke checks

```sh
LIME_DIR="$HOME/haxelib-dev/lime-832" ./metal/scripts/check-compile.sh
haxe -cp metal/haxe -main lime.graphics.metal.TestTranslate --interp
haxe -cp metal/haxe -main lime.graphics.metal.TestTranslateCompat --interp
```
