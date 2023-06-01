#include "llvm/CodeGen/FirvHarden.h"

#include "llvm/InitializePasses.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Type.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <vector>
#include <map>

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

BasicBlock *CreateFirvPrologue(
    Function &Fn,
    AllocaInst *&FirvAI1,
    AllocaInst *&FirvAI2
) {
    BasicBlock *NewBB = Fn.getEntryBlock()
        .splitBasicBlockBefore(&Fn.getEntryBlock().front(), "FirvPrologue");
    IRBuilder<> B(&NewBB->front());
    Type *RetType = Fn.getReturnType();

    FirvAI1 = B.CreateAlloca(RetType, nullptr, "FirvSlot1");
    FirvAI2 = B.CreateAlloca(RetType, nullptr, "FirvSlot2");

    return NewBB;
}

BasicBlock* CreateFailBB(Function &Fn) {
    LLVMContext &Context = Fn.getContext();
    BasicBlock *FailBB = BasicBlock::Create(Context, "FirvFailBB", &Fn);
    IRBuilder<> B(FailBB);

    B.CreateAlloca(Type::getInt32Ty(Context), nullptr, "Lalalala");

    return FailBB;
}

bool isHardeningSupportedForType(Type *Ty) {
    switch (Ty->getTypeID()) {
        case Type::IntegerTyID:
        case Type::FloatTyID:
        case Type::DoubleTyID:
            return true;
        // case Type::StructTyID:
        //     outs() << "Support for struct types (" << *Ty << ") is limited. \n";
        //     return true;
        default:
            return false;
    }
}

Instruction* AddSlotComparison(IRBuilder<> &B, Type *Type, Value *V1, Value *V2) {
    switch (Type->getTypeID()) {
        case Type::IntegerTyID:
            return cast<ICmpInst>(B.CreateICmpNE(V1, V2, "neq.i"));
        case Type::FloatTyID:
        case Type::DoubleTyID:
            return cast<FCmpInst>(B.CreateFCmpONE(V1, V2, "neq.f"));
        default:
            return nullptr;
    }
}

BasicBlock* CreateReturnBB(
    Function &Fn,
    Value* V
) {
    LLVMContext &Context = Fn.getContext();
    BasicBlock *ReturnBB = BasicBlock::Create(Context, "ReturnBB", &Fn);
    IRBuilder<> B(ReturnBB);

    B.CreateRet(V);

    return ReturnBB;
}

BasicBlock* CreateFirvEpilogue(
    Function &Fn,
    AllocaInst *&FirvAI1,
    AllocaInst *&FirvAI2
) {
    LLVMContext &Context = Fn.getContext();
    auto RetType = Fn.getReturnType();
    BasicBlock *EpilogueBB = BasicBlock::Create(Context, "FirvEpilogue", &Fn);
    IRBuilder<> B(EpilogueBB);

    BasicBlock *FailBB = CreateFailBB(Fn);
    auto V1 = B.CreateLoad(RetType, dyn_cast<Value>(FirvAI1), "ai1");
    auto V2 = B.CreateLoad(RetType, dyn_cast<Value>(FirvAI2), "ai2");

    auto Cmp = AddSlotComparison(B, RetType, V1, V2);

    if (!Cmp) {
        errs() << "Cannot create comparison for the " << *RetType << " type.\n";

        return nullptr;
    }

    BasicBlock* ReturnBB = CreateReturnBB(Fn, V1);

    B.CreateCondBr(Cmp, FailBB, ReturnBB);

    return EpilogueBB;
}


using BlockMapping = std::map<const BasicBlock*, BasicBlock*>;

void ReplaceSuccesor(BranchInst* Br, BlockMapping CloneMapping, unsigned int id) {
    const auto succ = Br->getSuccessor(id);
    const auto map = CloneMapping.find(succ);

    if(map == CloneMapping.end()) {
        errs() << "Missing mapping for BasicBlock" <<
            succ->getName() << "\n";
        
        return;
    }

    const auto newSucc = map->second;

    Br->setSuccessor(id, newSucc);
}

void ReplaceSuccessors(BranchInst* Br, BlockMapping CloneMapping) {
    ReplaceSuccesor(Br, CloneMapping, 0);

    if (Br->isUnconditional()) return;

    ReplaceSuccesor(Br, CloneMapping, 1);
}

void CloneBasicBlocks(Function &Fn,
    std::vector<BasicBlock*> &OriginalBBs,
    std::vector<BasicBlock*> &ClonedBBs
) {
    for (BasicBlock &BB : Fn) {
        OriginalBBs.push_back(&BB);
    }

    ValueToValueMapTy VMap;
    BlockMapping CloneMapping;

    for (const BasicBlock* BB : OriginalBBs) {
        auto Clone = CloneBasicBlock(BB, VMap, ".cl", &Fn);
        ClonedBBs.push_back(Clone);

        CloneMapping[BB] = Clone;
    }

    for (BasicBlock* ClonedBlock : ClonedBBs) {
        for (Instruction &I : *ClonedBlock) {
            RemapInstruction(&I, VMap, RF_IgnoreMissingLocals);

            if (isa<BranchInst>(I) ) {
                BranchInst* Br = dyn_cast<BranchInst>(&I);
                
                ReplaceSuccessors(Br, CloneMapping);
            }
        }
    }
}

bool FirvHarden::runOnFunction(Function &Fn) {
    if (!Fn.hasFnAttribute(Attribute::FirvHarden)) {
        return false;
    }

    outs() << "FIRV HARDEN\n";

    std::vector<BasicBlock*> OriginalBBs;
    std::vector<BasicBlock*> ClonedBBs;
    CloneBasicBlocks(Fn, OriginalBBs, ClonedBBs);

    Type *RetType = Fn.getReturnType();

    if (!isHardeningSupportedForType(RetType)) {
        errs() << "Firv Hardening is not supported for type " << *RetType << "\n"; 

        return false;
    }

    AllocaInst *FirvAI1 = nullptr;
    AllocaInst *FirvAI2 = nullptr;

    CreateFirvPrologue(Fn, FirvAI1, FirvAI2);
    auto EpilogueBB = CreateFirvEpilogue(Fn, FirvAI1, FirvAI2);

    if (!EpilogueBB) {
        return false;
    }


    
    Fn.print(outs());

    return true;
}
