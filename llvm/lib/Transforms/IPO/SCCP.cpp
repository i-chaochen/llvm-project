//===-- SCCP.cpp ----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements Interprocedural Sparse Conditional Constant Propagation.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/SCCP.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueLattice.h"
#include "llvm/Analysis/ValueLatticeUtils.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/InitializePasses.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/FunctionSpecialization.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/SCCPSolver.h"

using namespace llvm;

#define DEBUG_TYPE "sccp"

STATISTIC(NumInstRemoved, "Number of instructions removed");
STATISTIC(NumArgsElimed ,"Number of arguments constant propagated");
STATISTIC(NumGlobalConst, "Number of globals found to be constant");
STATISTIC(NumDeadBlocks , "Number of basic blocks unreachable");
STATISTIC(NumInstReplaced,
          "Number of instructions replaced with (simpler) instruction");

static cl::opt<bool> SpecializeFunctions("specialize-functions",
    cl::init(false), cl::Hidden, cl::desc("Enable function specialization"));

static cl::opt<unsigned> FuncSpecializationMaxIters(
    "func-specialization-max-iters", cl::init(1), cl::Hidden, cl::desc(
    "The maximum number of iterations function specialization is run"));

static void findReturnsToZap(Function &F,
                             SmallVector<ReturnInst *, 8> &ReturnsToZap,
                             SCCPSolver &Solver) {
  // We can only do this if we know that nothing else can call the function.
  if (!Solver.isArgumentTrackedFunction(&F))
    return;

  if (Solver.mustPreserveReturn(&F)) {
    LLVM_DEBUG(
        dbgs()
        << "Can't zap returns of the function : " << F.getName()
        << " due to present musttail or \"clang.arc.attachedcall\" call of "
           "it\n");
    return;
  }

  assert(
      all_of(F.users(),
             [&Solver](User *U) {
               if (isa<Instruction>(U) &&
                   !Solver.isBlockExecutable(cast<Instruction>(U)->getParent()))
                 return true;
               // Non-callsite uses are not impacted by zapping. Also, constant
               // uses (like blockaddresses) could stuck around, without being
               // used in the underlying IR, meaning we do not have lattice
               // values for them.
               if (!isa<CallBase>(U))
                 return true;
               if (U->getType()->isStructTy()) {
                 return all_of(Solver.getStructLatticeValueFor(U),
                               [](const ValueLatticeElement &LV) {
                                 return !isOverdefined(LV);
                               });
               }

               // We don't consider assume-like intrinsics to be actual address
               // captures.
               if (auto *II = dyn_cast<IntrinsicInst>(U)) {
                 if (II->isAssumeLikeIntrinsic())
                   return true;
               }

               return !isOverdefined(Solver.getLatticeValueFor(U));
             }) &&
      "We can only zap functions where all live users have a concrete value");

  for (BasicBlock &BB : F) {
    if (CallInst *CI = BB.getTerminatingMustTailCall()) {
      LLVM_DEBUG(dbgs() << "Can't zap return of the block due to present "
                        << "musttail call : " << *CI << "\n");
      (void)CI;
      return;
    }

    if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator()))
      if (!isa<UndefValue>(RI->getOperand(0)))
        ReturnsToZap.push_back(RI);
  }
}

static bool runIPSCCP(
    Module &M, const DataLayout &DL, FunctionAnalysisManager *FAM,
    std::function<const TargetLibraryInfo &(Function &)> GetTLI,
    std::function<TargetTransformInfo &(Function &)> GetTTI,
    std::function<AssumptionCache &(Function &)> GetAC,
    function_ref<AnalysisResultsForFn(Function &)> getAnalysis) {
  SCCPSolver Solver(DL, GetTLI, M.getContext());
  FunctionSpecializer Specializer(Solver, M, FAM, GetTLI, GetTTI, GetAC);

  // Loop over all functions, marking arguments to those with their addresses
  // taken or that are external as overdefined.
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    Solver.addAnalysis(F, getAnalysis(F));

    // Determine if we can track the function's return values. If so, add the
    // function to the solver's set of return-tracked functions.
    if (canTrackReturnsInterprocedurally(&F))
      Solver.addTrackedFunction(&F);

    // Determine if we can track the function's arguments. If so, add the
    // function to the solver's set of argument-tracked functions.
    if (canTrackArgumentsInterprocedurally(&F)) {
      Solver.addArgumentTrackedFunction(&F);
      continue;
    }

    // Assume the function is called.
    Solver.markBlockExecutable(&F.front());

    // Assume nothing about the incoming arguments.
    for (Argument &AI : F.args())
      Solver.markOverdefined(&AI);
  }

  // Determine if we can track any of the module's global variables. If so, add
  // the global variables we can track to the solver's set of tracked global
  // variables.
  for (GlobalVariable &G : M.globals()) {
    G.removeDeadConstantUsers();
    if (canTrackGlobalVariableInterprocedurally(&G))
      Solver.trackValueOfGlobalVariable(&G);
  }

  // Solve for constants.
  Solver.solveWhileResolvedUndefsIn(M);

  if (SpecializeFunctions) {
    unsigned Iters = 0;
    while (Iters++ < FuncSpecializationMaxIters && Specializer.run());
  }

  // Iterate over all of the instructions in the module, replacing them with
  // constants if we have found them to be of constant values.
  bool MadeChanges = false;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    SmallVector<BasicBlock *, 512> BlocksToErase;

    if (Solver.isBlockExecutable(&F.front())) {
      bool ReplacedPointerArg = false;
      for (Argument &Arg : F.args()) {
        if (!Arg.use_empty() && tryToReplaceWithConstant(Solver, &Arg)) {
          ReplacedPointerArg |= Arg.getType()->isPointerTy();
          ++NumArgsElimed;
        }
      }

      // If we replaced an argument, we may now also access a global (currently
      // classified as "other" memory). Update memory attribute to reflect this.
      if (ReplacedPointerArg) {
        auto UpdateAttrs = [&](AttributeList AL) {
          MemoryEffects ME = AL.getMemoryEffects();
          if (ME == MemoryEffects::unknown())
            return AL;

          ME |= MemoryEffects(MemoryEffects::Other,
                              ME.getModRef(MemoryEffects::ArgMem));
          return AL.addFnAttribute(
              F.getContext(),
              Attribute::getWithMemoryEffects(F.getContext(), ME));
        };

        F.setAttributes(UpdateAttrs(F.getAttributes()));
        for (User *U : F.users()) {
          auto *CB = dyn_cast<CallBase>(U);
          if (!CB || CB->getCalledFunction() != &F)
            continue;

          CB->setAttributes(UpdateAttrs(CB->getAttributes()));
        }
      }
      MadeChanges |= ReplacedPointerArg;
    }

    SmallPtrSet<Value *, 32> InsertedValues;
    for (BasicBlock &BB : F) {
      if (!Solver.isBlockExecutable(&BB)) {
        LLVM_DEBUG(dbgs() << "  BasicBlock Dead:" << BB);
        ++NumDeadBlocks;

        MadeChanges = true;

        if (&BB != &F.front())
          BlocksToErase.push_back(&BB);
        continue;
      }

      MadeChanges |= simplifyInstsInBlock(Solver, BB, InsertedValues,
                                          NumInstRemoved, NumInstReplaced);
    }

    DomTreeUpdater DTU = SpecializeFunctions && Specializer.isClonedFunction(&F)
        ? DomTreeUpdater(DomTreeUpdater::UpdateStrategy::Lazy)
        : Solver.getDTU(F);

    // Change dead blocks to unreachable. We do it after replacing constants
    // in all executable blocks, because changeToUnreachable may remove PHI
    // nodes in executable blocks we found values for. The function's entry
    // block is not part of BlocksToErase, so we have to handle it separately.
    for (BasicBlock *BB : BlocksToErase) {
      NumInstRemoved += changeToUnreachable(BB->getFirstNonPHI(),
                                            /*PreserveLCSSA=*/false, &DTU);
    }
    if (!Solver.isBlockExecutable(&F.front()))
      NumInstRemoved += changeToUnreachable(F.front().getFirstNonPHI(),
                                            /*PreserveLCSSA=*/false, &DTU);

    BasicBlock *NewUnreachableBB = nullptr;
    for (BasicBlock &BB : F)
      MadeChanges |= removeNonFeasibleEdges(Solver, &BB, DTU, NewUnreachableBB);

    for (BasicBlock *DeadBB : BlocksToErase)
      if (!DeadBB->hasAddressTaken())
        DTU.deleteBB(DeadBB);

    for (BasicBlock &BB : F) {
      for (Instruction &Inst : llvm::make_early_inc_range(BB)) {
        if (Solver.getPredicateInfoFor(&Inst)) {
          if (auto *II = dyn_cast<IntrinsicInst>(&Inst)) {
            if (II->getIntrinsicID() == Intrinsic::ssa_copy) {
              Value *Op = II->getOperand(0);
              Inst.replaceAllUsesWith(Op);
              Inst.eraseFromParent();
            }
          }
        }
      }
    }
  }

  // If we inferred constant or undef return values for a function, we replaced
  // all call uses with the inferred value.  This means we don't need to bother
  // actually returning anything from the function.  Replace all return
  // instructions with return undef.
  //
  // Do this in two stages: first identify the functions we should process, then
  // actually zap their returns.  This is important because we can only do this
  // if the address of the function isn't taken.  In cases where a return is the
  // last use of a function, the order of processing functions would affect
  // whether other functions are optimizable.
  SmallVector<ReturnInst*, 8> ReturnsToZap;

  for (const auto &I : Solver.getTrackedRetVals()) {
    Function *F = I.first;
    const ValueLatticeElement &ReturnValue = I.second;

    // If there is a known constant range for the return value, add !range
    // metadata to the function's call sites.
    if (ReturnValue.isConstantRange() &&
        !ReturnValue.getConstantRange().isSingleElement()) {
      // Do not add range metadata if the return value may include undef.
      if (ReturnValue.isConstantRangeIncludingUndef())
        continue;

      auto &CR = ReturnValue.getConstantRange();
      for (User *User : F->users()) {
        auto *CB = dyn_cast<CallBase>(User);
        if (!CB || CB->getCalledFunction() != F)
          continue;

        // Limit to cases where the return value is guaranteed to be neither
        // poison nor undef. Poison will be outside any range and currently
        // values outside of the specified range cause immediate undefined
        // behavior.
        if (!isGuaranteedNotToBeUndefOrPoison(CB, nullptr, CB))
          continue;

        // Do not touch existing metadata for now.
        // TODO: We should be able to take the intersection of the existing
        // metadata and the inferred range.
        if (CB->getMetadata(LLVMContext::MD_range))
          continue;

        LLVMContext &Context = CB->getParent()->getContext();
        Metadata *RangeMD[] = {
            ConstantAsMetadata::get(ConstantInt::get(Context, CR.getLower())),
            ConstantAsMetadata::get(ConstantInt::get(Context, CR.getUpper()))};
        CB->setMetadata(LLVMContext::MD_range, MDNode::get(Context, RangeMD));
      }
      continue;
    }
    if (F->getReturnType()->isVoidTy())
      continue;
    if (isConstant(ReturnValue) || ReturnValue.isUnknownOrUndef())
      findReturnsToZap(*F, ReturnsToZap, Solver);
  }

  for (auto *F : Solver.getMRVFunctionsTracked()) {
    assert(F->getReturnType()->isStructTy() &&
           "The return type should be a struct");
    StructType *STy = cast<StructType>(F->getReturnType());
    if (Solver.isStructLatticeConstant(F, STy))
      findReturnsToZap(*F, ReturnsToZap, Solver);
  }

  // Zap all returns which we've identified as zap to change.
  SmallSetVector<Function *, 8> FuncZappedReturn;
  for (ReturnInst *RI : ReturnsToZap) {
    Function *F = RI->getParent()->getParent();
    RI->setOperand(0, UndefValue::get(F->getReturnType()));
    // Record all functions that are zapped.
    FuncZappedReturn.insert(F);
  }

  // Remove the returned attribute for zapped functions and the
  // corresponding call sites.
  for (Function *F : FuncZappedReturn) {
    for (Argument &A : F->args())
      F->removeParamAttr(A.getArgNo(), Attribute::Returned);
    for (Use &U : F->uses()) {
      CallBase *CB = dyn_cast<CallBase>(U.getUser());
      if (!CB) {
        assert(isa<BlockAddress>(U.getUser()) ||
               (isa<Constant>(U.getUser()) &&
                all_of(U.getUser()->users(), [](const User *UserUser) {
                  return cast<IntrinsicInst>(UserUser)->isAssumeLikeIntrinsic();
                })));
        continue;
      }

      for (Use &Arg : CB->args())
        CB->removeParamAttr(CB->getArgOperandNo(&Arg), Attribute::Returned);
    }
  }

  // If we inferred constant or undef values for globals variables, we can
  // delete the global and any stores that remain to it.
  for (const auto &I : make_early_inc_range(Solver.getTrackedGlobals())) {
    GlobalVariable *GV = I.first;
    if (isOverdefined(I.second))
      continue;
    LLVM_DEBUG(dbgs() << "Found that GV '" << GV->getName()
                      << "' is constant!\n");
    while (!GV->use_empty()) {
      StoreInst *SI = cast<StoreInst>(GV->user_back());
      SI->eraseFromParent();
      MadeChanges = true;
    }
    M.getGlobalList().erase(GV);
    ++NumGlobalConst;
  }

  return MadeChanges;
}

PreservedAnalyses IPSCCPPass::run(Module &M, ModuleAnalysisManager &AM) {
  const DataLayout &DL = M.getDataLayout();
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  auto GetTLI = [&FAM](Function &F) -> const TargetLibraryInfo & {
    return FAM.getResult<TargetLibraryAnalysis>(F);
  };
  auto GetTTI = [&FAM](Function &F) -> TargetTransformInfo & {
    return FAM.getResult<TargetIRAnalysis>(F);
  };
  auto GetAC = [&FAM](Function &F) -> AssumptionCache & {
    return FAM.getResult<AssumptionAnalysis>(F);
  };
  auto getAnalysis = [&FAM](Function &F) -> AnalysisResultsForFn {
    DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    return {
        std::make_unique<PredicateInfo>(F, DT, FAM.getResult<AssumptionAnalysis>(F)),
        &DT, FAM.getCachedResult<PostDominatorTreeAnalysis>(F),
        SpecializeFunctions ? &FAM.getResult<LoopAnalysis>(F) : nullptr };
  };

  if (!runIPSCCP(M, DL, &FAM, GetTLI, GetTTI, GetAC, getAnalysis))
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserve<DominatorTreeAnalysis>();
  PA.preserve<PostDominatorTreeAnalysis>();
  PA.preserve<FunctionAnalysisManagerModuleProxy>();
  return PA;
}

namespace {

//===--------------------------------------------------------------------===//
//
/// IPSCCP Class - This class implements interprocedural Sparse Conditional
/// Constant Propagation.
///
class IPSCCPLegacyPass : public ModulePass {
public:
  static char ID;

  IPSCCPLegacyPass() : ModulePass(ID) {
    initializeIPSCCPLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override {
    if (skipModule(M))
      return false;
    const DataLayout &DL = M.getDataLayout();
    auto GetTLI = [this](Function &F) -> const TargetLibraryInfo & {
      return this->getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
    };
    auto GetTTI = [this](Function &F) -> TargetTransformInfo & {
      return this->getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);
    };
    auto GetAC = [this](Function &F) -> AssumptionCache & {
      return this->getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
    };
    auto getAnalysis = [this](Function &F) -> AnalysisResultsForFn {
      DominatorTree &DT =
          this->getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
      return {
          std::make_unique<PredicateInfo>(
              F, DT,
              this->getAnalysis<AssumptionCacheTracker>().getAssumptionCache(
                  F)),
          nullptr,  // We cannot preserve the LI, DT or PDT with the legacy pass
          nullptr,  // manager, so set them to nullptr.
          nullptr};
    };

    return runIPSCCP(M, DL, nullptr, GetTLI, GetTTI, GetAC, getAnalysis);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AssumptionCacheTracker>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<TargetTransformInfoWrapperPass>();
  }
};

} // end anonymous namespace

char IPSCCPLegacyPass::ID = 0;

INITIALIZE_PASS_BEGIN(IPSCCPLegacyPass, "ipsccp",
                      "Interprocedural Sparse Conditional Constant Propagation",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(IPSCCPLegacyPass, "ipsccp",
                    "Interprocedural Sparse Conditional Constant Propagation",
                    false, false)

// createIPSCCPPass - This is the public interface to this file.
ModulePass *llvm::createIPSCCPPass() { return new IPSCCPLegacyPass(); }

