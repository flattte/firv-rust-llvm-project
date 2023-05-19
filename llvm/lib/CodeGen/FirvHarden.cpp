#include "llvm/CodeGen/FirvHarden.h"

#include "llvm/InitializePasses.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"

#include <iostream>

using namespace llvm;

#define DEBUG_TYPE "firv-harden"

char FirvHarden::ID = 0;

INITIALIZE_PASS_BEGIN(FirvHarden, DEBUG_TYPE,
                      "Insert FIRV hardening", false, true)
INITIALIZE_PASS_DEPENDENCY(TargetPassConfig)
INITIALIZE_PASS_DEPENDENCY(StackProtector)
INITIALIZE_PASS_END(FirvHarden, DEBUG_TYPE,
                    "Insert FIRV hardening", false, true)

FirvHarden::FirvHarden() : FunctionPass(ID) {
  initializeFirvHardenPass(*PassRegistry::getPassRegistry());
}

FunctionPass *llvm::createFirvHardenPass() { return new FirvHarden(); }

bool FirvHarden::runOnFunction(Function &Fn) {
    if (Fn.hasFnAttribute(Attribute::FirvHarden)) {
        std::cout << "FIRV HARDEN" << std::endl;
        // Fn.print(std::cout);
    }

    return false;
}
