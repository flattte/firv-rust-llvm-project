#include "llvm/CodeGen/FirvHarden.h"

#include "llvm/InitializePasses.h"
#include "llvm/ADT/Twine.h"
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
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <vector>
#include <map>

using namespace llvm;

#define DEBUG_TYPE "firv-harden"

char FirvHarden::ID = 0;

INITIALIZE_PASS_BEGIN(FirvHarden, DEBUG_TYPE,
                    "Insert FIRV hardening", false, true)
INITIALIZE_PASS_DEPENDENCY(TargetPassConfig)
INITIALIZE_PASS_END(FirvHarden, DEBUG_TYPE,
                    "Insert FIRV hardening", false, true)

void FirvHarden::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetPassConfig>();
}

FirvHarden::FirvHarden() : FunctionPass(ID) {
  initializeFirvHardenPass(*PassRegistry::getPassRegistry());
}

FunctionPass *llvm::createFirvHardenPass() { return new FirvHarden(); }

BasicBlock *AddEntryBlock(Function &Fn, Twine name) {
    return Fn.getEntryBlock()
        .splitBasicBlockBefore(&Fn.getEntryBlock().front(), name);
}

BasicBlock *CreateFirvPrologue(
    Function &Fn,
    AllocaInst *&FirvAI1,
    AllocaInst *&FirvAI2
) {
    BasicBlock *NewBB = AddEntryBlock(Fn, "FirvPrologue");
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

    B.CreateUnreachable();

    return FailBB;
}

bool isHardeningSupportedForType(Type *Ty) {
    switch (Ty->getTypeID()) {
        case Type::IntegerTyID:
        case Type::FloatTyID:
        case Type::DoubleTyID:
            return true;
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
    Value* Slot1,
    Value* Slot2
) {
    LLVMContext &Context = Fn.getContext();
    Type *RetType = Fn.getReturnType();
    BasicBlock *ReturnBB = BasicBlock::Create(Context, "ReturnBB", &Fn);
    IRBuilder<> B(ReturnBB);

    auto V1 = B.CreateLoad(RetType, Slot1, true,"RetVal1");
    auto V2 = B.CreateLoad(RetType, Slot2, true,"RetVal1");
    
    auto Cmp = AddSlotComparison(B, RetType, Slot1, Slot2);
    auto V = B.CreateSelect(Cmp, V1, V2);
    B.CreateRet(V);

    return ReturnBB;
}

BasicBlock* CreateFirvEpilogue(
    Function &Fn,
    AllocaInst *&FirvAI1,
    AllocaInst *&FirvAI2,
    BasicBlock* FailBB,
    BasicBlock* ReturnBB
) {
    LLVMContext &Context = Fn.getContext();
    auto RetType = Fn.getReturnType();
    BasicBlock *EpilogueBB = BasicBlock::Create(Context, "FirvEpilogue", &Fn);
    IRBuilder<> B(EpilogueBB);

    auto V1 = B.CreateLoad(RetType, dyn_cast<Value>(FirvAI1), true, "ai1");
    auto V2 = B.CreateLoad(RetType, dyn_cast<Value>(FirvAI2), true, "ai2");

    auto Cmp = AddSlotComparison(B, RetType, V1, V2);

    if (!Cmp) {
        errs() << "Cannot create comparison for the " << *RetType << " type.\n";

        return nullptr;
    }

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

void CloneBasicBlocks(
    Function &Fn,
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

BasicBlock* CreateFirvInterlude(
    Function &Fn,
    std::vector<BasicBlock*> &ClonedBBs
) {
    LLVMContext &Context = Fn.getContext();
    BasicBlock *InterludeBB = BasicBlock::Create(Context, "FirvInterlude", &Fn);
    IRBuilder<> B(InterludeBB);

    BasicBlock* NextBB = ClonedBBs[0];

    B.CreateBr(NextBB);

    return InterludeBB;
}

void ReplaceReturns(
    std::vector<BasicBlock*> &Blocks,
    Value* Slot,
    BasicBlock* Next
) {
    for (BasicBlock* BB: Blocks) {
        auto I = BB->getTerminator();

        if (isa<ReturnInst>(I)) {
            auto RI = dyn_cast<ReturnInst>(I);
            auto val = RI->getReturnValue();
            auto BR = BranchInst::Create(Next);

            IRBuilder<> B(I);
            B.CreateStore(val, Slot, true);
            ReplaceInstWithInst(I, BR);
        }
    }
}

void StoreArgsAndLoad(Function &Fn) {
    BasicBlock *StoreLoadBB = AddEntryBlock(Fn, "StoreLoad");
    IRBuilder<> B(&StoreLoadBB->front());

    for (auto &arg : Fn.args()) {
        auto argTy = arg.getType();
        auto Slot = B.CreateAlloca(
            argTy,
            nullptr,
            Twine(arg.getName()).concat(".st")
        );
        
        B.CreateStore(&arg, Slot, true);
        auto V = B.CreateLoad(argTy, Slot, true);

        arg.replaceUsesOutsideBlock(V, StoreLoadBB);
    }
}

bool FirvHarden::runOnFunction(Function &Fn) {
    if (!Fn.hasFnAttribute(Attribute::FirvHarden)) {
        return false;
    }

    Type *RetType = Fn.getReturnType();
    if (!isHardeningSupportedForType(RetType)) {
        errs() << "Firv Hardening is not supported for type " << *RetType << "\n"; 

        return false;
    }

    StoreArgsAndLoad(Fn);

    std::vector<BasicBlock*> OriginalBBs;
    std::vector<BasicBlock*> ClonedBBs;
    CloneBasicBlocks(Fn, OriginalBBs, ClonedBBs);

    AllocaInst *FirvAI1 = nullptr;
    AllocaInst *FirvAI2 = nullptr;
    CreateFirvPrologue(Fn, FirvAI1, FirvAI2);
    
    BasicBlock *FailBB = CreateFailBB(Fn);
    BasicBlock *ReturnBB = CreateReturnBB(Fn, FirvAI1, FirvAI2);

    auto EpilogueBB = CreateFirvEpilogue(Fn, FirvAI1, FirvAI2, FailBB, ReturnBB);

    if (!EpilogueBB) {
        return false;
    }

    auto InterludeBB = CreateFirvInterlude(Fn, ClonedBBs);

    ReplaceReturns(OriginalBBs, FirvAI1, InterludeBB);
    ReplaceReturns(ClonedBBs, FirvAI2, EpilogueBB);

    return true;
}
