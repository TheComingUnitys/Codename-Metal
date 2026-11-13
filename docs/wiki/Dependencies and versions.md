# Dependencies and versions

| Component | Version / commit |
|---|---|
| Codename Engine | 1.0.1 |
| Lime | 8.3.2 (development checkout) |
| OpenFL | 9.5.x (`openfl-metal` fork) |
| HaxeFlixel | 5.3.0 (`flixel-metal` fork) |
| Haxe | 4.3.x |
| hxcpp | 4.3.2 |
| bgfx | API v168 (`eebf5686a8c5f1c77e1af2dad17b5746ba17da8f`) |
| bx | `57548da89a07771c5b0f8a6711149be70e08a329` |
| bimg | `22177183239e18d7af603e6915156371cc38d18d` |
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
