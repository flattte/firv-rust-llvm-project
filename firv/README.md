# Firv LLVM Readme

## Requirements

For building the LLVM version we don't require any additional tools over the upstream version of the project. The most significant required tools are:
* CMake
* ninja
* C++ toolchain (e.g. Clang, GCC, MSVC)

Consult the [LLVM requirements](https://llvm.org/docs/GettingStarted.html#software) for full set of requirements and their versions.

## Configure build

```
cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLLVM_TARGETS_TO_BUILD="RISCV;X86"
```

The `-DLLVM_TARGETS_TO_BUILD="RISCV;X86"` flag reduces the number of targets that are built, for shorter compilation time.

## Build (ninja):

```
ninja -C build
```
