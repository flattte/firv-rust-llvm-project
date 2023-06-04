#ifndef LLVM_CODEGEN_FIRVHARDEN_H
#define LLVM_CODEGEN_FIRVHARDEN_H

#include "llvm/Pass.h"

namespace llvm {

class Function;

class FirvHarden : public FunctionPass {
public:
  static char ID; // Pass identification, replacement for typeid.

  FirvHarden();

  bool runOnFunction(Function &Fn) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;
};


} // end namespace llvm

#endif // LLVM_CODEGEN_FIRVHARDEN_H
