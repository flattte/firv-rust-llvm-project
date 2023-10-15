FIRV CHANGELOG
===

14.05.2023
---

1. Added attribute `firv_harden` to `llvm/include/llvm/IR/Attributes.td`
2. Added bitcode code value for `ATTR_KIND_FIRV_HARDEN` in `llvm/include/llvm/Bitcode/LLVMBitCodes.h`
3. Added case for reading bitcode (`getAttrFromCode`) in `llvm/lib/Bitcode/BitcodeReader.cpp`
4. Added case for writing bitcode (`getAttrKindEncoding`) in `llvm/lib/Bitcode/BitcodeWriter.cpp`
5. Added handling for `CodeExtractor::constructFunction` (extracting block to a function -- inheriting the function attribute) in `llvm/lib/Transform/Utils/CodeExtractor.cpp`

19.05.2023
---

1. Added FirvHardenPass class and required initializations.
2. Added dummy implementation.
