#include "llvm/CodeGen/FirvHarden.h"

#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Type.h"
#include "llvm/InitializePasses.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <map>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "firv-harden"

char FirvHarden::ID = 0;
static const int MaxDepth = 10;

INITIALIZE_PASS_BEGIN(FirvHarden, DEBUG_TYPE, "Insert FIRV hardening", false,
                      true)
INITIALIZE_PASS_DEPENDENCY(TargetPassConfig)
INITIALIZE_PASS_END(FirvHarden, DEBUG_TYPE, "Insert FIRV hardening", false,
                    true)

void FirvHarden::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetPassConfig>();
}

FirvHarden::FirvHarden() : FunctionPass(ID) {
  initializeFirvHardenPass(*PassRegistry::getPassRegistry());
}

FunctionPass *llvm::createFirvHardenPass() { return new FirvHarden(); }

BasicBlock *AddEntryBlock(Function &Fn, Twine name) {
  return Fn.getEntryBlock().splitBasicBlockBefore(&Fn.getEntryBlock().front(),
                                                  name);
}

BasicBlock *CreateFirvPrologue(Function &Fn, AllocaInst *&FirvAI1,
                               AllocaInst *&FirvAI2,
                               Argument *SRetArg = nullptr) {
  BasicBlock *NewBB = AddEntryBlock(Fn, "FirvPrologue");
  IRBuilder<> B(&NewBB->front());
  Type *RetType = Fn.getReturnType();

  if (SRetArg) {
    FirvAI1 =
        B.CreateAlloca(SRetArg->getParamStructRetType(), nullptr, "FirvSlot1");
    FirvAI2 =
        B.CreateAlloca(SRetArg->getParamStructRetType(), nullptr, "FirvSlot2");
  } else {
    FirvAI1 = B.CreateAlloca(RetType, nullptr, "FirvSlot1");
    FirvAI2 = B.CreateAlloca(RetType, nullptr, "FirvSlot2");
  }

  return NewBB;
}

BasicBlock *CreateFailBB(Function &Fn) {
  LLVMContext &Context = Fn.getContext();
  BasicBlock *FailBB = BasicBlock::Create(Context, "FailBB", &Fn);
  IRBuilder<> B(FailBB);

  B.CreateCall(Intrinsic::getDeclaration(Fn.getParent(), Intrinsic::trap));
  B.CreateUnreachable();

  return FailBB;
}

bool isHardeningSupportedForType(Type *Ty) {
  return Ty->isFloatingPointTy() || Ty->isIntegerTy() || Ty->isStructTy() ||
         Ty->isArrayTy();
}

Value *CompareArrayElements(Function &Fn, IRBuilder<> &B, ArrayType *ArrayTy,
                            Value *V1, Value *V2);
Value *CompareStructFields(Function &Fn, IRBuilder<> &B, StructType *StructTy,
                           Value *V1, Value *V2);

Value *CompareStructFields(Function &Fn, IRBuilder<> &B, StructType *StructTy,
                           Value *V1, Value *V2) {
  LLVMContext &Context = Fn.getContext();
  Value *AllFieldsEqual = ConstantInt::getTrue(Context);

  for (unsigned int i = 0; i < StructTy->getNumElements(); ++i) {
    Value *FieldV1 = B.CreateExtractValue(
        V1, {i}, V1->getName().str() + ".field" + std::to_string(i));
    Value *FieldV2 = B.CreateExtractValue(
        V2, {i}, V2->getName().str() + ".field" + std::to_string(i));
    Value *FieldEqual;

    Type *FieldType = StructTy->getElementType(i);
    if (FieldType->isIntegerTy()) {
      FieldEqual =
          B.CreateICmpEQ(FieldV1, FieldV2, "cmpField" + std::to_string(i));
    } else if (FieldType->isStructTy()) {
      FieldEqual = CompareStructFields(Fn, B, cast<StructType>(FieldType),
                                       FieldV1, FieldV2);
    } else if (FieldType->isFloatingPointTy()) {
      FieldEqual = B.CreateFCmpOEQ(FieldV1, FieldV2, ".cmpFloatLike");
    } else if (FieldType->isArrayTy()) {
      FieldEqual = CompareArrayElements(Fn, B, cast<ArrayType>(FieldType),
                                        FieldV1, FieldV2);
    } else {
      return nullptr;
    }
    AllFieldsEqual =
        B.CreateAnd(AllFieldsEqual, FieldEqual, "andTmp" + std::to_string(i));
  }

  return AllFieldsEqual;
}

Value *CompareArrayElements(Function &Fn, IRBuilder<> &B, ArrayType *ArrayTy,
                            Value *V1, Value *V2) {
  dbgs() << "comparing " << *ArrayTy << " in firv\n";
  LLVMContext &Context = Fn.getContext();
  Value *AllElementsEqual = ConstantInt::getTrue(Context);
  unsigned int NumElements = ArrayTy->getNumElements();
  Type *ElementType = ArrayTy->getElementType();
  dbgs() << "comparing " << *ElementType << " in firv\n";

  for (unsigned int i = 0; i < NumElements; ++i) {
    dbgs() << "comparing step " << i << " " << *V1 << "\n";

    // Use GEP to get the pointer to the array element
    Value *ElementPtrV1 =
        B.CreateGEP(ArrayTy, V1, {B.getInt32(0), B.getInt32(i)},
                    V1->getName().str() + ".elementPtr" + std::to_string(i));
    Value *ElementPtrV2 =
        B.CreateGEP(ArrayTy, V2, {B.getInt32(0), B.getInt32(i)},
                    V2->getName().str() + ".elementPtr" + std::to_string(i));

    // Load the array element
    Value *ElementV1 =
        B.CreateLoad(ElementType, ElementPtrV1,
                     V1->getName().str() + ".element" + std::to_string(i));
    Value *ElementV2 =
        B.CreateLoad(ElementType, ElementPtrV2,
                     V2->getName().str() + ".element" + std::to_string(i));

    Value *ElementEqual;
    dbgs() << "comparing step further\n";

    if (ElementType->isIntegerTy()) {
      ElementEqual = B.CreateICmpEQ(ElementV1, ElementV2,
                                    "cmpElement" + std::to_string(i));
    } else if (ElementType->isStructTy()) {
      ElementEqual = CompareStructFields(Fn, B, cast<StructType>(ElementType),
                                         ElementV1, ElementV2);
    } else if (ElementType->isFloatingPointTy()) {
      ElementEqual = B.CreateFCmpOEQ(ElementV1, ElementV2, ".cmpFloatLike");
    } else if (ElementType->isArrayTy()) {
      ElementEqual = CompareArrayElements(Fn, B, cast<ArrayType>(ElementType),
                                          ElementV1, ElementV2);
    } else {
      return nullptr;
    }

    AllElementsEqual = B.CreateAnd(AllElementsEqual, ElementEqual,
                                   "andTmp" + std::to_string(i));
  }

  return AllElementsEqual;
}

Value *AddSlotComparison(Function &Fn, IRBuilder<> &B, Type *RetType, Value *V1,
                         Value *V2) {
  if (RetType->isIntegerTy()) {
    dbgs() << "comparing " << *RetType << " in firv\n";
    return B.CreateICmpEQ(V1, V2, "cmpInt");
  } else if (RetType->isFloatingPointTy()) {
    return B.CreateFCmpOEQ(V1, V2, "cmpFloat");
  } else if (RetType->isStructTy()) {
    std::map<Type *, int> RecursionDepth;
    return CompareStructFields(Fn, B, cast<StructType>(RetType), V1, V2);
  } else if (RetType->isArrayTy()) {
    return CompareArrayElements(Fn, B, cast<ArrayType>(RetType), V1, V2);
  } else {
    errs() << "Unsupported type for comparison.\n";
    return nullptr;
  }
}

BasicBlock *CreateReturnBB(Function &Fn, Value *Slot1, Value *Slot2,
                           Argument *SRetArg = nullptr) {
  LLVMContext &Context = Fn.getContext();
  Type *RetType = Fn.getReturnType();
  BasicBlock *ReturnBB = BasicBlock::Create(Context, "ReturnBB", &Fn);
  IRBuilder<> B(ReturnBB);

  if (SRetArg) {
    auto V1 =
        B.CreateLoad(SRetArg->getParamStructRetType(), Slot1, true, "RetVal1");
    B.CreateLoad(SRetArg->getParamStructRetType(), Slot2, true, "RetVal2");
    B.CreateRetVoid();
  } else {
    auto V1 = B.CreateLoad(RetType, Slot1, true, "RetVal1");
    B.CreateLoad(RetType, Slot2, true, "RetVal2");
    B.CreateRet(V1);
  }

  return ReturnBB;
}

BasicBlock *CreateSlotCheck(Function &Fn, AllocaInst *&FirvAI1,
                            AllocaInst *&FirvAI2, BasicBlock *ThisBB,
                            BasicBlock *NextBB, Argument *SRetArg = nullptr) {
  LLVMContext &Context = Fn.getContext();
  auto RetType = Fn.getReturnType();

  IRBuilder<> B(ThisBB);

  MDBuilder MDB(Context);

  Value *V1;
  Value *V2;
  if (SRetArg) {
    V1 = FirvAI1;
    V2 = FirvAI2;
  } else {
    V1 = B.CreateLoad(RetType, dyn_cast<Value>(FirvAI1), true, "ai1");
    V2 = B.CreateLoad(RetType, dyn_cast<Value>(FirvAI2), true, "ai2");
  }

  auto Cmp = AddSlotComparison(
      Fn, B, SRetArg ? SRetArg->getParamStructRetType() : RetType, V1, V2);

  if (!Cmp) {
    errs() << "Cannot create comparison for the " << *RetType << " type.\n";
    return nullptr;
  }

  BasicBlock *FailBB = CreateFailBB(Fn);

  B.CreateCondBr(Cmp, NextBB, FailBB, MDB.createBranchWeights(1, 99999));

  return ThisBB;
}

BasicBlock *CreateFirvEpilogue(Function &Fn, AllocaInst *&FirvAI1,
                               AllocaInst *&FirvAI2, BasicBlock *ReturnBB,
                               Argument *SRetArg = nullptr) {
  LLVMContext &Context = Fn.getContext();

  BasicBlock *EpilogueBB = BasicBlock::Create(Context, "FirvEpilogue.1", &Fn);
  BasicBlock *Epilogue2BB = BasicBlock::Create(Context, "FirvEpilogue.2", &Fn);

  CreateSlotCheck(Fn, FirvAI1, FirvAI2, EpilogueBB, Epilogue2BB, SRetArg);
  CreateSlotCheck(Fn, FirvAI1, FirvAI2, Epilogue2BB, ReturnBB, SRetArg);

  return EpilogueBB;
}

using BlockMapping = std::map<const BasicBlock *, BasicBlock *>;

void ReplaceSuccesor(BranchInst *Br, BlockMapping CloneMapping,
                     unsigned int id) {
  const auto succ = Br->getSuccessor(id);
  const auto map = CloneMapping.find(succ);

  if (map == CloneMapping.end()) {
    errs() << "Missing mapping for BasicBlock" << succ->getName() << "\n";
    return;
  }

  const auto newSucc = map->second;
  Br->setSuccessor(id, newSucc);
}

void ReplaceSuccessors(BranchInst *Br, BlockMapping CloneMapping) {
  ReplaceSuccesor(Br, CloneMapping, 0);

  if (Br->isUnconditional())
    return;

  ReplaceSuccesor(Br, CloneMapping, 1);
}

void CloneBasicBlocks(Function &Fn, std::vector<BasicBlock *> &OriginalBBs,
                      std::vector<BasicBlock *> &ClonedBBs) {
  for (BasicBlock &BB : Fn) {
    OriginalBBs.push_back(&BB);
  }

  ValueToValueMapTy VMap;
  BlockMapping CloneMapping;

  for (const BasicBlock *BB : OriginalBBs) {
    auto Clone = CloneBasicBlock(BB, VMap, ".cl", &Fn);
    ClonedBBs.push_back(Clone);
    CloneMapping[BB] = Clone;
  }

  for (BasicBlock *ClonedBlock : ClonedBBs) {
    for (Instruction &I : *ClonedBlock) {
      RemapInstruction(&I, VMap, RF_IgnoreMissingLocals);

      if (isa<BranchInst>(I)) {
        BranchInst *Br = dyn_cast<BranchInst>(&I);
        ReplaceSuccessors(Br, CloneMapping);
      }

      if (DILocation *Loc = I.getDebugLoc()) {
        I.setDebugLoc(Loc);
      }
    }
  }
}

BasicBlock *CreateFirvInterlude(Function &Fn,
                                std::vector<BasicBlock *> &ClonedBBs) {
  LLVMContext &Context = Fn.getContext();
  BasicBlock *InterludeBB = BasicBlock::Create(Context, "FirvInterlude", &Fn);
  IRBuilder<> B(InterludeBB);

  BasicBlock *NextBB = ClonedBBs[0];
  B.CreateBr(NextBB);

  return InterludeBB;
}

void ReplaceReturns(std::vector<BasicBlock *> &Blocks, Value *Slot,
                    BasicBlock *Next, Argument *SRetArg = nullptr) {
  const DataLayout &DL = Blocks[0]->getModule()->getDataLayout();
  for (BasicBlock *BB : Blocks) {
    auto I = BB->getTerminator();

    if (isa<ReturnInst>(I)) {
      auto RI = dyn_cast<ReturnInst>(I);
      auto val = RI->getReturnValue();
      auto BR = BranchInst::Create(Next);

      IRBuilder<> B(I);
      if (SRetArg) {
        auto DestPtr =
            B.CreateBitCast(Slot, Type::getInt8PtrTy(B.getContext()));
        auto SrcPtr =
            B.CreateBitCast(SRetArg, Type::getInt8PtrTy(B.getContext()));
        B.CreateMemCpy(DestPtr, MaybeAlign(), SrcPtr, MaybeAlign(),
                       DL.getTypeStoreSize(SRetArg->getParamStructRetType()));
      } else {
        B.CreateStore(val, Slot, true);
      }
      BR->setDebugLoc(I->getDebugLoc());
      ReplaceInstWithInst(I, BR);
    }
  }
}
void StoreArgsAndLoad(Function &Fn) {
  BasicBlock *StoreLoadBB = AddEntryBlock(Fn, "StoreLoad");
  IRBuilder<> B(&StoreLoadBB->front());

  for (auto &arg : Fn.args()) {
    auto argTy = arg.getType();
    dbgs() << "arg:" << arg.getName() << *argTy << "\n";
    auto Slot =
        B.CreateAlloca(argTy, nullptr, Twine(arg.getName()).concat(".st"));
    B.CreateStore(&arg, Slot, true);
    auto V = B.CreateLoad(argTy, Slot, true);
    arg.replaceUsesOutsideBlock(V, StoreLoadBB);
  }
}

Argument *findSRetArgument(Function &Fn) {
  for (auto &arg : Fn.args()) {
    if (arg.hasAttribute(Attribute::StructRet)) {
      return &arg;
    }
  }
  return nullptr;
}

bool FirvHarden::runOnFunction(Function &Fn) {
  if (!Fn.hasFnAttribute(Attribute::FirvHarden)) {
    return false;
  }

  Type *RetType = Fn.getReturnType();
  Argument *SRetArg = findSRetArgument(Fn);
  if (SRetArg) {
    RetType = SRetArg->getParamStructRetType();
    dbgs() << "SRetArg:" << *SRetArg << " type:" << *RetType << "\n";
  }

  if (!isHardeningSupportedForType(RetType)) {
    errs() << "Firv Hardening on type " << *RetType << "\n";
    return false;
  }

  errs() << "Firv Hardening on type " << *RetType << "\n";

  StoreArgsAndLoad(Fn);

  std::vector<BasicBlock *> OriginalBBs;
  std::vector<BasicBlock *> ClonedBBs;
  CloneBasicBlocks(Fn, OriginalBBs, ClonedBBs);

  AllocaInst *FirvAI1 = nullptr;
  AllocaInst *FirvAI2 = nullptr;
  CreateFirvPrologue(Fn, FirvAI1, FirvAI2, SRetArg);

  BasicBlock *ReturnBB = CreateReturnBB(Fn, FirvAI1, FirvAI2, SRetArg);

  auto InterludeBB = CreateFirvInterlude(Fn, ClonedBBs);

  auto EpilogueBB = CreateFirvEpilogue(Fn, FirvAI1, FirvAI2, ReturnBB, SRetArg);

  if (!EpilogueBB) {
    return false;
  }

  ReplaceReturns(OriginalBBs, FirvAI1, InterludeBB, SRetArg);
  ReplaceReturns(ClonedBBs, FirvAI2, EpilogueBB, SRetArg);

  for (BasicBlock *BB : OriginalBBs) {
    for (Instruction &I : *BB) {
      if (DILocation *Loc = I.getDebugLoc()) {
        I.setDebugLoc(Loc);
      }
    }
  }

  for (BasicBlock *BB : ClonedBBs) {
    for (Instruction &I : *BB) {
      if (DILocation *Loc = I.getDebugLoc()) {
        I.setDebugLoc(Loc);
      }
    }
  }

  return true;
}
