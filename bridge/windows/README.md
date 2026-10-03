# Windows Proxy Build

This project builds a `dinput8.dll` proxy from macOS using `mingw-w64`.

## Required package

```bash
brew install mingw-w64
```

## Build from macOS

```bash
cmake -S . -B build-windows \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WINDOWS_PROXY=ON \
  -DCMAKE_TOOLCHAIN_FILE=bridge/windows/toolchains/x86_64-w64-mingw32.cmake

cmake --build build-windows --target wheelio_dinput8
```

The output DLL is renamed to `dinput8.dll`.

## Usage with CrossOver / Wine

1. Copy the built `dinput8.dll` next to the game executable inside the bottle.
2. Run the native macOS `Wheelio.app`.
3. Start the game through CrossOver/Wine so the DLL can connect to the local bridge on `127.0.0.1:18423`.
