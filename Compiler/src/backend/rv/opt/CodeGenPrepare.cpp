#include "riscv/opt/CodeGenPrepare.hpp"

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/NewPassManager.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>

namespace riscv {
namespace {

struct PrepareCandidate {
  const Loop *loop = nullptr;
  BasicBlock *preheader = nullptr;
  BasicBlock *latch = nullptr;
  GetElementPtrInst *gep = nullptr;
  unsigned varyingIndex = 0;
  int64_t start = 0;
  int64_t step = 0;
  std::size_t indexSize = 4;
  std::size_t score = 0;
};

bool fitsInt32(int64_t value) {
  return value >= std::numeric_limits<int>::min() &&
         value <= std::numeric_limits<int>::max();
}

std::optional<int64_t>
estimateTripCount(const LoopInductionInfo &Info) {
  if (!Info.hasUpperBound || Info.step == 0) return std::nullopt;

  const int64_t span = Info.upperBound - Info.initial;
  if (span == 0) return int64_t(1);
  if ((span > 0) != (Info.step > 0)) return std::nullopt;
  if (span % Info.step != 0) return std::nullopt;

  int64_t steps = span / Info.step;
  if (steps < 0) steps = -steps;
  return steps + 1;
}

bool isParamOrGlobal(Value *V) {
  auto *Var = dynamic_cast<Variable *>(V);
  return Var && (Var->isParam() || Var->isGlobal());
}

bool dominatesPreheader(Value *V, BasicBlock *PreHeader,
                        const DominatorTree &DT) {
  if (!V) return false;
  if (V->isConst() || isParamOrGlobal(V)) return true;
  auto *Inst = dynamic_cast<Instruction *>(V);
  if (!Inst || !PreHeader || !PreHeader->back()) return false;
  return DT.dominatesValue(V, PreHeader->back());
}

bool collectLoopOrder(const Loop *L, std::vector<const Loop *> &Loops) {
  if (!L) return false;
  Loops.push_back(L);
  for (const Loop *Sub : L->getSubLoops()) {
    collectLoopOrder(Sub, Loops);
  }
  return true;
}

bool insertBeforeTerminator(BasicBlock *BB, Instruction *Inst) {
  if (!BB || !Inst || BB->empty()) return false;
  auto It = BB->rbegin();
  if (It == BB->rend()) return false;
  It.insertBefore(Inst);
  return true;
}

bool insertBeforeFirstNonPhi(BasicBlock *BB, Instruction *Inst) {
  if (!BB || !Inst) return false;
  for (auto It = BB->begin(); It != BB->end(); ++It) {
    Instruction *Cur = *It;
    if (!dynamic_cast<PhiInst *>(Cur)) {
      It.insertBefore(Inst);
      return true;
    }
  }
  if (BB->empty()) {
    BB->push_back(Inst);
    return true;
  }
  auto It = BB->rbegin();
  if (It == BB->rend()) return false;
  It.insertBefore(Inst);
  return true;
}

std::optional<PrepareCandidate>
tryBuildCandidate(GetElementPtrInst &GEP, const Loop *L, BasicBlock *PreHeader,
                  BasicBlock *Latch, const DominatorTree &DT,
                  const std::unordered_map<Value *, LoopInductionInfo>
                      &LoopInductions) {
  auto &Ops = GEP.getUserList();
  if (Ops.size() <= 1) return std::nullopt;
  if (!PreHeader || !Latch || !PreHeader->back() || !Latch->back())
    return std::nullopt;

  unsigned VaryingIndex = 0;
  int64_t Start = 0;
  int64_t Step = 0;
  std::size_t IndexSize = 4;
  bool FoundVarying = false;

  for (unsigned I = 0; I < Ops.size(); ++I) {
    Value *Op = Ops[I] ? Ops[I]->getValue() : nullptr;
    if (!Op) return std::nullopt;

    if (I == 0) {
      if (!dominatesPreheader(Op, PreHeader, DT)) return std::nullopt;
      continue;
    }

    if (!FoundVarying) {
      auto It = LoopInductions.find(Op);
      if (It != LoopInductions.end() && It->second.RecLoop == L &&
          fitsInt32(It->second.initial) &&
          fitsInt32(It->second.step)) {
        const auto TripCount = estimateTripCount(It->second);
        if (TripCount && *TripCount <= 1) return std::nullopt;
        // 首版只处理末尾归纳索引；前缀索引必须全是循环不变量。
        if (I + 1 != Ops.size()) return std::nullopt;
        FoundVarying = true;
        VaryingIndex = I;
        Start = It->second.initial;
        Step = It->second.step;
        IndexSize = Op->getIRType() ? Op->getIRType()->getSize() : 4;
        if (Step == 0) return std::nullopt;
        continue;
      }
    }

    if (!dominatesPreheader(Op, PreHeader, DT)) return std::nullopt;
  }

  if (!FoundVarying) return std::nullopt;

  for (auto It = GEP.getUseList().begin(); It != GEP.getUseList().end(); ++It) {
    Use *UseNode = *It;
    if (!UseNode) return std::nullopt;
    auto *UserInst = dynamic_cast<Instruction *>(UseNode->getUser());
    if (!UserInst) return std::nullopt;
    BasicBlock *UserBB = UserInst->getParent();
    if (!UserBB || !L->contains(UserBB)) return std::nullopt;
    if (dynamic_cast<PhiInst *>(UserInst)) return std::nullopt;
  }

  PrepareCandidate Candidate;
  Candidate.loop = L;
  Candidate.preheader = PreHeader;
  Candidate.latch = Latch;
  Candidate.gep = &GEP;
  Candidate.varyingIndex = VaryingIndex;
  Candidate.start = Start;
  Candidate.step = Step;
  Candidate.indexSize = IndexSize;
  Candidate.score = GEP.getUseList().size;
  return Candidate;
}

std::vector<PrepareCandidate>
collectCandidatesForLoop(const Loop *L, const DominatorTree &DT,
                         const std::unordered_map<Value *, LoopInductionInfo>
                             &LoopInductions) {
  BasicBlock *PreHeader = nullptr;
  BasicBlock *Latch = nullptr;
  if (!L->getLoopSimplifyForm(PreHeader, Latch)) return {};

  std::vector<PrepareCandidate> Candidates;
  for (BasicBlock *BB : L->getBlocks()) {
    if (!BB) continue;
    for (Instruction *Inst : *BB) {
      auto *GEP = dynamic_cast<GetElementPtrInst *>(Inst);
      if (!GEP) continue;
      auto Candidate =
          tryBuildCandidate(*GEP, L, PreHeader, Latch, DT, LoopInductions);
      if (!Candidate) continue;
      Candidates.push_back(std::move(*Candidate));
    }
  }

  std::stable_sort(Candidates.begin(), Candidates.end(),
                   [](const PrepareCandidate &A, const PrepareCandidate &B) {
                     return A.score > B.score;
                   });
  return Candidates;
}

std::size_t maxTransformsForLoop(std::size_t candidateCount) {
  if (candidateCount <= 2) {
    return candidateCount;
  }

  // 宽环里同时升格太多 pointer IV 会放大 live range 风险；
  // 先保留前两个最值钱的候选，避免把所有地址链都展开。
  return 2;
}

GetElementPtrInst *cloneGEPWithReplacement(GetElementPtrInst &Orig,
                                           unsigned ReplacedIndex,
                                           Value *Replacement) {
  auto &Ops = Orig.getUserList();
  if (Ops.empty() || !Ops[0]) return nullptr;

  auto *Clone = new GetElementPtrInst(Ops[0]->getValue());
  for (unsigned I = 1; I < Ops.size(); ++I) {
    Value *Op = (I == ReplacedIndex) ? Replacement : Ops[I]->getValue();
    if (!Op) {
      delete Clone;
      return nullptr;
    }
    Clone->addUse(Op);
  }
  Clone->updateType();
  return Clone;
}

bool rewriteCandidate(const PrepareCandidate &Candidate) {
  GetElementPtrInst *Orig = Candidate.gep;
  if (!Orig || !Candidate.loop || !Candidate.preheader || !Candidate.latch)
    return false;
  auto OrigType = Orig->getIRType();
  if (!OrigType) return false;

  auto *StartConst =
      ConstInt::newConstInt(static_cast<int>(Candidate.start),
                            Candidate.indexSize);
  auto *StepConst =
      ConstInt::newConstInt(static_cast<int>(Candidate.step),
                            Candidate.indexSize);

  GetElementPtrInst *InitGEP = cloneGEPWithReplacement(
      *Orig, Candidate.varyingIndex, StartConst);
  if (!InitGEP) return false;

  auto *PtrPhi = new PhiInst(OrigType);
  // latch 只做一步推进；前缀索引已体现在 PtrPhi 里。
  auto *LatchGEP = new GetElementPtrInst(PtrPhi);
  LatchGEP->addUse(StepConst);
  LatchGEP->updateType();

  PtrPhi->addIncoming(InitGEP, Candidate.preheader);
  PtrPhi->addIncoming(LatchGEP, Candidate.latch);

  if (!insertBeforeTerminator(Candidate.preheader, InitGEP)) {
    return false;
  }
  if (!insertBeforeFirstNonPhi(Candidate.loop->getHeader(), PtrPhi)) {
    return false;
  }
  if (!insertBeforeTerminator(Candidate.latch, LatchGEP)) {
    return false;
  }

  Orig->replaceAllUsesWith(PtrPhi);
  if (!Orig->getUseList().is_empty()) return false;
  delete Orig;
  return true;
}

} // namespace

bool runCodeGenPrepare(Module &module) {
  if (std::getenv("BACKEND_DISABLE_CODEGEN_PREPARE") != nullptr) {
    return false;
  }

  FunctionAnalysisPassManager FAM;
  FAM.registerPass([] { return DominantAnalysis(); });
  FAM.registerPass([] { return LoopAnalysis(); });
  FAM.registerPass([] { return SCEVAnalysis(); });

  bool Changed = false;
  for (auto &FuncPtr : module.getFunctions()) {
    if (!FuncPtr) continue;
    Function &F = *FuncPtr;
    if (F.Size() == 0) continue;

    const auto &DT = FAM.getResult<DominantAnalysis>(F);
    const auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SCEV = FAM.getResult<SCEVAnalysis>(F);
    const auto &LoopInductions = SCEV.getLoopInductions();

    std::vector<const Loop *> Loops;
    for (Loop *Top : LI.getTopLevelLoops()) {
      collectLoopOrder(Top, Loops);
    }
    std::stable_sort(Loops.begin(), Loops.end(),
                     [](const Loop *A, const Loop *B) {
                       if (A->getDepth() != B->getDepth()) {
                         return A->getDepth() > B->getDepth();
                       }
                       return A->getHeader() < B->getHeader();
                     });

    for (const Loop *L : Loops) {
      if (!L) continue;
      auto Candidates = collectCandidatesForLoop(L, DT, LoopInductions);
      const std::size_t Limit = maxTransformsForLoop(Candidates.size());
      for (std::size_t I = 0; I < Limit; ++I) {
        const auto &Candidate = Candidates[I];
        Changed |= rewriteCandidate(Candidate);
      }
    }
  }

  return Changed;
}

} // namespace riscv
