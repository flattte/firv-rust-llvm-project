Adding CodeGen pass
===

1. Create the Pass class header in `llvm/include/CodeGen/X.h`
2. Add `initializeXPass` declaration to `llvm/include/llvm/InitializePasses.h`
3. Add `createXPass` declaration to `llvm/include/llvm/CodeGen/Passes.h`
3. Create implementation in `llvm/lib/Codegen/X.cpp`
4. Add the above file to CMakeLists in `llvm/lib/CodeGen/CMakeLists.txt`
5. Add the call to the `createXPass` in desired spot in `llvm/lib/CodeGen/TargetPassConfig.cpp`
