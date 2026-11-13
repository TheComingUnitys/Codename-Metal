# Dependencies and versions

| Component | Version / commit |
|---|---|
| Codename Engine | 1.0.1 |
| Lime | 8.3.2 (development checkout) |
| OpenFL | 9.5.x (`openfl-metal` fork) |
| HaxeFlixel | 5.3.0 (`flixel-metal` fork) |
| Haxe | 4.3.x |
| hxcpp | 4.3.2 |
| bgfx | API v168 (`f5ce45e39d6381093e577e80adb1abb2c611302c`) |
| bx | `57548da89a07771c5b0f8a6711149be70e08a329` |
| bimg | `77bebb28e12956bfced82bb2cf7c19ac5735b6c8` |
| Deployment target | macOS 11.0 |

## Metal forks

| Fork | Base | Purpose |
|---|---|---|
| [openfl-metal](https://github.com/TheComingUnitys/openfl-metal) | OpenFL 9.5.x | Metal native-path shims, instanced bitmap batch, `Context3D` instancing API |
| [flixel-metal](https://github.com/TheComingUnitys/flixel-metal) | Flixel 5.3.0 | `FlxG.metalInstancing` toggle |

## Using the forks

```sh
haxelib dev openfl /path/to/openfl-metal
haxelib dev flixel /path/to/flixel-metal
```

To restore upstream versions:

```sh
haxelib dev openfl
haxelib dev flixel
```

then select the versions vendored by the project.
