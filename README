# SMOKE stands for "Scripting Meta Object Kompiler Engine" - for instance ;)

smokegen is the binding-generator component of the SMOKE toolkit (version 3).
It parses Qt C++ headers using LLVM/Clang and emits the SMOKE data tables
consumed by smokeqt and language bindings (e.g. CommonQt).

Qt 6 is required; Qt 5 is not supported.

## Prerequisites

- CMake 3.22 or later
- LLVM/Clang (matching versions; tested with LLVM 19)
- Qt 6.11
- A C++17-capable compiler (GCC, Clang, or MSVC)

## Building

```
mkdir build && cd build
cmake .. -GNinja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/path/to/install -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DSMOKE_QT_VERSION=6 -DQt6_DIR=/path/to/qt6 -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm ..\smokegen
ninja -j 6
ninja install
```

On Windows, run the cmake commands from an x64 Native Tools Command Prompt.
The Visual Studio generator can be used instead of Ninja if preferred, but Ninja is recommended for faster incremental builds.

## Regenerating SmokeQt bindings

After installing smokegen, the smokeqt CMake build will invoke the smokegen
executable automatically when generating the SmokeQt binding tables from the
Qt headers.
