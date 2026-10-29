# mtlcheck - standalone verification of build-time `.metallib` files

**English** | [中文](../../../docs/zh-CN/metal/tools/mtlcheck-README.md)

Without launching the game, this tool loads every `.metallib` under
`metal/generated/metallibs/**` in a separate process and verifies:

1. `newLibraryWithData` can load it (the path bgfx takes when the blob starts with the
   `MTLB` magic);
2. the library contains the `xlatMtlMain` entry point;
3. **corruption fallback**: truncated/garbage metallibs must fail to load. This only tests
   Metal's behaviour; the actual fallback logic lives in `MetalBindings.cpp`
   (failure -> fall back to the AOT MSL source + `metallibLoadFailures` counter).

```sh
clang -fobjc-arc -O1 -framework Foundation -framework Metal -lc++ -o /tmp/mtlcheck mtlcheck.mm
/tmp/mtlcheck ../generated/metallibs
```

Measured (Apple M3 / macOS 26.6.2 25G83):

```text
device=Apple M3
RESULT total=45 loadOk=45 loadFail=0 fnOk=45
CORRUPT truncated(64B) load=FAIL_AS_EXPECTED err=Invalid library file
CORRUPT garbage        load=FAIL_AS_EXPECTED err=Invalid library file (unexpected end of file)
```
