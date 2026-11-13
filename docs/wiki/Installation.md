# Installation

## Requirements

- macOS 11 (Big Sur) or newer.
- Apple Silicon or Intel Mac (the release DMG is universal).
- No additional runtime libraries are required; Lime and the Metal backend are inside the app bundle.

## Installing

1. Download `CodenameEngine-Metal-<version>-universal.dmg` from [Releases](https://github.com/TheComingUnitys/Codename-Metal/releases).
2. Open the DMG and copy `CodenameEngine.app` wherever you want (normally `/Applications`).
3. The build is ad-hoc signed, not notarized. If macOS refuses to launch it:
   - Right-click `CodenameEngine.app` → **Open** → Open, or
   - remove the quarantine flag: `xattr -dr com.apple.quarantine /Applications/CodenameEngine.app`

## Mods and saves

Mods are loaded from the `mods/` directory shipped inside the app bundle's resources. Save data is stored in the standard Codename Engine save directory.

## Running

Double-click the app, or run the binary directly to see logs:

```sh
/Applications/CodenameEngine.app/Contents/MacOS/CodenameEngine
```

To collect diagnostics, see [[Troubleshooting and diagnostics]].
