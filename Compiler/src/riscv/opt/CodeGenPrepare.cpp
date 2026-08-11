#include "riscv/opt/CodeGenPrepare.hpp"

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/NewPassManager.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace riscv {
// CodeGenPrepare pass 的 RISC-V 实现。
//
// 作用：在中端 IR 层面为后端做地址计算的强度削减。核心变换是把循环体内、
// 索引为仿射归纳变量的 getelementptr（GEP，数组元素地址）改写成一个指针型
// 归纳变量（pointer IV）：预头块里算出初始地址，header 放一个 PHI，latch 里每
// 次迭代只做一次固定步长的指针推进，从而把每次迭代的乘法+加法地址计算削减为
// 一次加法。此外把 base、可变索引、步长都相同、常量字节偏移在立即数范围内的
// 一簇 GEP 聚类，让它们共享同一个指针 IV（配合常量偏移）。
//
// 正确性依赖：借助 SCEV 与区间分析证明索引在整型范围内不会溢出/回绕
// （no-wrap），否则用指针推进替换重新计算可能改变结果，因此这里做了大量保守
// 的区间与支配性检查，任何无法证明安全的候选都会被放弃。
namespace {

// 一个待改写的候选：一个（或一簇）循环内 GEP，连同其归纳变量、起点、步长、
// 预头/latch 位置以及需要在预头克隆的中间值集合。
struct PrepareCandidate {
  struct ClusterMember {
    GetElementPtrInst *gep = nullptr;
    int64_t elementOffset = 0;
  };

  const Loop *loop = nullptr;
  BasicBlock *preheader = nullptr;
  BasicBlock *latch = nullptr;
  GetElementPtrInst *gep = nullptr;
  Value *originalIndex = nullptr;
  Value *induction = nullptr;
  unsigned varyingIndex = 0;
  int64_t start = 0;
  int64_t step = 0;
  std::size_t indexSize = 4;
  std::size_t score = 0;
  std::unordered_set<Value *> cloneValues;
  std::vector<ClusterMember> clusterMembers;
};

// 一个 GEP 展开成线性地址后的签名：base 指针 + 各原子项的系数 + 常量字节偏移，
// 用于判断两个 GEP 是否只相差一个常量偏移，从而可以聚类共享指针 IV。
struct LinearAddressSignature {
  Value *base = nullptr;
  std::map<Value *, int64_t> terms;
  int64_t constant = 0;
  std::shared_ptr<IRType> pointeeType;
};

// 对某个索引值做仿射分析的结果：它相对哪个归纳变量、系数、以及归纳变量出现次数
// （出现超过一次说明非线性，不可用）。
struct AffineIndexInfo {
  Value *induction = nullptr;
  int coefficient = 0;
  unsigned occurrences = 0;
};

// 一个带符号整数区间 [min, max]，均限制在 i32 范围内。
struct SignedRange {
  int64_t min = 0;
  int64_t max = 0;
};

using InductionRangeMap = std::unordered_map<Value *, SignedRange>;
using ValueRangeCache = std::unordered_map<Value *, std::optional<SignedRange>>;
using SCEVRangeCache =
    std::unordered_map<const SCEVExpr *, std::optional<SignedRange>>;
using KnownSCEVRangeMap = std::unordered_map<const SCEVExpr *, SignedRange>;
using AffineIndexCache =
    std::unordered_map<Value *, std::optional<AffineIndexInfo>>;

// 一个不标记 inbounds 的 GEP 指令类型。指针 IV 推进可能临时越过数组边界（下一
// 迭代才回到范围内），因此新造的 GEP 不能声称 inbounds，以免误导后续优化。
class NonInBoundsGEPInst final : public GetElementPtrInst {
public:
  explicit NonInBoundsGEPInst(Value *Base) : GetElementPtrInst(Base) {
    setInBounds(false);
  }

  void dumpIR() override {
    Value::dumpIR();
    std::cout << " = getelementptr ";
    auto &Ops = getUserList();
    if (Ops.empty() || !Ops[0] || !Ops[0]->getValue()) {
      std::cout << "<invalid>\n";
      return;
    }

    Value *Base = Ops[0]->getValue();
    auto BaseType = Base->getIRType();
    std::shared_ptr<IRType> ElementType = BaseType;
    if (BaseType && BaseType->getTypeSystem() == IR_POINTER) {
      ElementType =
          std::dynamic_pointer_cast<POINTERType>(BaseType)->getPointerType();
    }
    ElementType->toString();
    std::cout << ", ";
    BaseType->toString();
    std::cout << ' ';
    Base->dumpIR();
    for (std::size_t I = 1; I < Ops.size(); ++I) {
      Value *Index = Ops[I]->getValue();
      std::cout << ", ";
      Index->getIRType()->toString();
      std::cout << ' ';
      Index->dumpIR();
    }
    std::cout << '\n';
  }
};

// 值是否能放入 int32。
bool fitsInt32(int64_t value) {
  return value >= std::numeric_limits<int>::min() &&
         value <= std::numeric_limits<int>::max();
}

// 是否是函数参数或全局变量（视为循环不变、在预头之前即可用）。
bool isParamOrGlobal(Value *V) {
  if (dynamic_cast<Param *>(V)) {
    return true;
  }
  auto *Var = dynamic_cast<Variable *>(V);
  return Var && Var->isGlobal();
}

// 值 V 是否在预头之前就已定义（循环不变），使其可安全用于预头处算初始地址。
// 常量、参数、全局都算是；否则要求其定义支配预头末尾。
bool dominatesPreheader(Value *V, BasicBlock *PreHeader,
                        const DominatorTree &DT) {
  if (!V)
    return false;
  if (V->isConst() || isParamOrGlobal(V))
    return true;
  auto *Inst = dynamic_cast<Instruction *>(V);
  if (!Inst || !PreHeader || !PreHeader->back())
    return false;
  return DT.dominatesValue(V, PreHeader->back());
}

// 是否是本 pass 支持作为索引处理的整型值（4 或 8 字节整数）。
bool isSupportedIndexValue(Value *V) {
  return V && V->getIRType() && V->getIRType()->getTypeSystem() == IR_INT &&
         (V->getIRType()->getSize() == 4 || V->getIRType()->getSize() == 8);
}

// 区间 [Min, Max] 是否整体落在 i32 范围内（用 128 位中间量避免溢出）。
bool rangeFitsI32(__int128 Min, __int128 Max) {
  return Min >= std::numeric_limits<int>::min() &&
         Max <= std::numeric_limits<int>::max();
}

// 求比较谓词的逻辑取反（如 E 变 NE、GE 变 L），用于把“退出条件”换算为
// “继续循环条件”。
BinaryInst::Operation invertPredicate(BinaryInst::Operation Op) {
  switch (Op) {
  case BinaryInst::E:
    return BinaryInst::NE;
  case BinaryInst::NE:
    return BinaryInst::E;
  case BinaryInst::GE:
    return BinaryInst::L;
  case BinaryInst::L:
    return BinaryInst::GE;
  case BinaryInst::LE:
    return BinaryInst::G;
  case BinaryInst::G:
    return BinaryInst::LE;
  default:
    return Op;
  }
}

// 交换比较两侧操作数时对应的谓词变换（如 GE 与 LE 互换），用于把归纳变量统一
// 挪到比较左侧。
BinaryInst::Operation swapPredicate(BinaryInst::Operation Op) {
  switch (Op) {
  case BinaryInst::GE:
    return BinaryInst::LE;
  case BinaryInst::L:
    return BinaryInst::G;
  case BinaryInst::LE:
    return BinaryInst::GE;
  case BinaryInst::G:
    return BinaryInst::L;
  default:
    return Op;
  }
}

// 递归收集循环 L 及其所有子循环的基本块。
void collectLoopBlocks(const Loop *L, std::vector<BasicBlock *> &Blocks) {
  if (!L)
    return;
  Blocks.insert(Blocks.end(), L->getBlocks().begin(), L->getBlocks().end());
  for (const Loop *SubLoop : L->getSubLoops()) {
    collectLoopBlocks(SubLoop, Blocks);
  }
}

// SCEV 展开的规模上限，防止病态输入导致指数级递归/内存爆炸。
constexpr std::size_t kMaxSCEVExpansionCost = 16384;
constexpr std::size_t kMaxSCEVRecursionDepth = 512;

// 值是否是可能参与 SCEV 表达式展开的指令类型。
bool isSCEVExpressionValue(Value *V) {
  return dynamic_cast<BinaryInst *>(V) || dynamic_cast<PhiInst *>(V) ||
         dynamic_cast<SelectInst *>(V) ||
         dynamic_cast<GetElementPtrInst *>(V) || dynamic_cast<ZextInst *>(V) ||
         dynamic_cast<SextInst *>(V) || dynamic_cast<TruncInst *>(V) ||
         dynamic_cast<BitCastInst *>(V);
}

// 估算把 V 完全展开为 SCEV 表达式的规模（带缓存、递归深度上限与环检测）。
// 超过 kMaxSCEVExpansionCost 即返回一个越界值，供上层提前放弃该函数。
std::size_t expandedSCEVCost(Value *V,
                             std::unordered_map<Value *, std::size_t> &Cache,
                             std::unordered_set<Value *> &Active,
                             std::size_t Depth = 0) {
  if (!V || !isSCEVExpressionValue(V))
    return 1;
  if (Depth >= kMaxSCEVRecursionDepth)
    return kMaxSCEVExpansionCost + 1;
  auto Cached = Cache.find(V);
  if (Cached != Cache.end())
    return Cached->second;
  if (!Active.insert(V).second)
    return 1;

  std::size_t Cost = 1;
  auto *U = dynamic_cast<User *>(V);
  for (const auto &UseNode : U->getUserList()) {
    const std::size_t OperandCost = expandedSCEVCost(
        UseNode ? UseNode->getValue() : nullptr, Cache, Active, Depth + 1);
    if (OperandCost >
        kMaxSCEVExpansionCost - std::min(Cost, kMaxSCEVExpansionCost)) {
      Cost = kMaxSCEVExpansionCost + 1;
      break;
    }
    Cost += OperandCost;
  }
  Active.erase(V);
  Cache[V] = Cost;
  return Cost;
}

// 预检查：整个函数里所有循环归纳 PHI 与决定循环去留的退出条件的 SCEV 展开规模
// 是否都在可控范围内。任一超限就返回 false，让调用方整体跳过该函数，避免后续
// 分析开销失控。
bool hasManageableSCEVInputs(const LoopInfo &LI) {
  std::unordered_map<Value *, std::size_t> Cache;
  std::vector<const Loop *> Worklist;
  for (const Loop *Top : LI.getTopLevelLoops()) {
    Worklist.push_back(Top);
  }

  while (!Worklist.empty()) {
    const Loop *L = Worklist.back();
    Worklist.pop_back();
    if (!L)
      continue;
    Worklist.insert(Worklist.end(), L->getSubLoops().begin(),
                    L->getSubLoops().end());

    BasicBlock *Header = L->getHeader();
    if (Header) {
      for (Instruction *Inst : *Header) {
        if (!dynamic_cast<PhiInst *>(Inst))
          break;
        std::unordered_set<Value *> Active;
        if (expandedSCEVCost(Inst, Cache, Active) > kMaxSCEVExpansionCost)
          return false;
      }
    }

    std::vector<BasicBlock *> Blocks;
    collectLoopBlocks(L, Blocks);
    for (BasicBlock *BB : Blocks) {
      BrInst *Branch = BB ? BB->getBranchTerminator() : nullptr;
      if (!BasicBlock::isCondBr(Branch))
        continue;
      auto &Ops = Branch->getUserList();
      if (Ops.size() != 3)
        continue;
      BasicBlock *TrueBB = dynamic_cast<BasicBlock *>(Ops[1]->getValue());
      BasicBlock *FalseBB = dynamic_cast<BasicBlock *>(Ops[2]->getValue());
      if ((TrueBB && L->contains(TrueBB)) ==
          (FalseBB && L->contains(FalseBB))) {
        continue;
      }
      std::unordered_set<Value *> Active;
      if (expandedSCEVCost(Ops[0]->getValue(), Cache, Active) >
          kMaxSCEVExpansionCost) {
        return false;
      }
    }
  }
  return true;
}

// 从 Root 出发遍历其计算树，判断是否含有对区间/回绕推理不安全的运算：除法、
// 无符号除/取余，以及各种类型转换/select 等（它们会破坏“线性、无回绕”前提）。
bool containsUnsafeSCEVOperation(Value *Root) {
  std::vector<Value *> Worklist{Root};
  std::unordered_set<Value *> Visited;
  while (!Worklist.empty()) {
    Value *V = Worklist.back();
    Worklist.pop_back();
    if (!V || !Visited.insert(V).second)
      continue;

    if (auto *Bin = dynamic_cast<BinaryInst *>(V)) {
      if (Bin->getOp() == BinaryInst::DIV || Bin->getOp() == BinaryInst::UDIV ||
          Bin->getOp() == BinaryInst::UREM) {
        return true;
      }
    }
    if (dynamic_cast<SelectInst *>(V) || dynamic_cast<ZextInst *>(V) ||
        dynamic_cast<SextInst *>(V) || dynamic_cast<TruncInst *>(V) ||
        dynamic_cast<BitCastInst *>(V) || dynamic_cast<FPTOSIInst *>(V) ||
        dynamic_cast<SITOFPInst *>(V)) {
      return true;
    }

    auto *U = dynamic_cast<User *>(V);
    if (!U)
      continue;
    for (const auto &UseNode : U->getUserList()) {
      if (UseNode)
        Worklist.push_back(UseNode->getValue());
    }
  }
  return false;
}

// SCEV 表达式是否递归提及循环 L（即含以 L 为归纳循环的递推项）。用于判断循环
// 上界是否真的是循环不变量。深度超限时保守返回 true（视为提及，放弃变换）。
bool scevMentionsLoop(const SCEVExpr *Expr, const Loop *L,
                      std::unordered_set<const SCEVExpr *> &Visited,
                      std::size_t Depth = 0) {
  if (!Expr || !Visited.insert(Expr).second)
    return false;
  if (Depth >= kMaxSCEVRecursionDepth)
    return true;
  if ((Expr->Kind == SCEVExprKind::AddRec ||
       Expr->Kind == SCEVExprKind::MulRec ||
       Expr->Kind == SCEVExprKind::DivRec) &&
      Expr->RecLoop == L) {
    return true;
  }
  for (const SCEVExpr *Operand : Expr->Operands) {
    if (scevMentionsLoop(Operand, L, Visited, Depth + 1))
      return true;
  }
  return false;
}

// 一个循环的“可靠 trip count信息”：退出上界表达式、归纳递推的起点
// 与步长、化为“继续循环”后的谓词，以及退出测试在 header 还是 latch 上。
struct ReliableTripCountInfo {
  const SCEVExpr *bound = nullptr;
  int64_t recurrenceStart = 0;
  int64_t recurrenceStep = 0;
  BinaryInst::Operation predicate = BinaryInst::ADD;
  bool headerTested = false;
  bool latchTested = false;
};

// 分析循环 L 的退出条件，尝试提取可靠的迭代次数信息。要求循环只有唯一退出块、
// 退出条件是一个把归纳变量与循环不变上界比较的二元比较，且该比较能被精确判定
// （步长方向与谓词一致，或 NE 情形能整除），否则返回 nullopt。这里刻意保守：
// 只接受“恰好一次到达边界即退出”的规范形态，作为后续区间推导的可信基础。
std::optional<ReliableTripCountInfo> getReliableTripCountInfo(SCEVResult &SCEV,
                                                              const Loop *L) {
  BasicBlock *Exiting = nullptr;
  bool ContinueOnTrue = true;
  std::vector<BasicBlock *> Blocks;
  collectLoopBlocks(L, Blocks);
  for (BasicBlock *BB : Blocks) {
    BrInst *Branch = BB ? BB->getBranchTerminator() : nullptr;
    if (!BasicBlock::isCondBr(Branch))
      continue;
    auto &Ops = Branch->getUserList();
    BasicBlock *TrueBB = dynamic_cast<BasicBlock *>(Ops[1]->getValue());
    BasicBlock *FalseBB = dynamic_cast<BasicBlock *>(Ops[2]->getValue());
    const bool TrueInLoop = TrueBB && L->contains(TrueBB);
    const bool FalseInLoop = FalseBB && L->contains(FalseBB);
    if (TrueInLoop == FalseInLoop)
      continue;
    if (Exiting && Exiting != BB)
      return std::nullopt;
    Exiting = BB;
    ContinueOnTrue = TrueInLoop;
  }
  if (!Exiting)
    return std::nullopt;

  BrInst *Branch = Exiting->getBranchTerminator();
  auto &BranchOps = Branch->getUserList();
  auto *Compare = dynamic_cast<BinaryInst *>(
      BranchOps.empty() ? nullptr : BranchOps[0]->getValue());
  if (!Compare || Compare->getUserList().size() != 2)
    return std::nullopt;

  if (containsUnsafeSCEVOperation(Compare))
    return std::nullopt;

  BinaryInst::Operation Predicate =
      ContinueOnTrue ? Compare->getOp() : invertPredicate(Compare->getOp());
  Value *LHS = Compare->getUserList()[0]->getValue();
  Value *RHS = Compare->getUserList()[1]->getValue();
  auto Affine = SCEV.getAffine(LHS);
  if (!Affine || Affine->RecLoop != L) {
    Affine = SCEV.getAffine(RHS);
    if (!Affine || Affine->RecLoop != L)
      return std::nullopt;
    std::swap(LHS, RHS);
    Predicate = swapPredicate(Predicate);
  }

  const SCEVExpr *Bound = SCEV.getSCEV(RHS);
  std::unordered_set<const SCEVExpr *> SCEVVisited;
  if (!Bound || scevMentionsLoop(Bound, L, SCEVVisited) ||
      !Affine->ConstantStart || !Affine->ConstantStep ||
      *Affine->ConstantStep == 0 || !fitsInt32(*Affine->ConstantStart)) {
    return std::nullopt;
  }

  const int64_t Step = *Affine->ConstantStep;
  bool Exact = false;
  // 判断“步长方向与谓词是否保证恰好一次跨过边界后退出”。< / <= 需步长为正，
  // > / >= 需步长为负；NE 则要求上界为常量且步长能整除起点到上界的距离，
  // 否则可能跳过边界形成事实上的无限循环，不能视为可靠迭代次数。
  switch (Predicate) {
  case BinaryInst::L:
  case BinaryInst::LE:
    Exact = Step > 0;
    break;
  case BinaryInst::G:
  case BinaryInst::GE:
    Exact = Step < 0;
    break;
  case BinaryInst::NE: {
    if (Bound->Kind != SCEVExprKind::Constant)
      return std::nullopt;
    const __int128 Distance =
        Step > 0
            ? static_cast<__int128>(Bound->ConstValue) - *Affine->ConstantStart
            : static_cast<__int128>(*Affine->ConstantStart) - Bound->ConstValue;
    const __int128 Magnitude =
        Step > 0 ? static_cast<__int128>(Step) : -static_cast<__int128>(Step);
    Exact = Distance > 0 && Distance % Magnitude == 0;
    break;
  }
  default:
    return std::nullopt;
  }
  if (!Exact)
    return std::nullopt;
  const auto &Backedges = L->getBackedgeSrcs();
  return ReliableTripCountInfo{Bound,
                               *Affine->ConstantStart,
                               Step,
                               Predicate,
                               Exiting == L->getHeader(),
                               Backedges.size() == 1 &&
                                   Backedges.front() == Exiting};
}

// 当无法从 SCEV 拿到 trip count 上界时的兜底：对“步长为 +1 且谓词为 <”或
// “步长为 -1 且谓词为 >”这两种单位步长严格比较的规范循环，直接给出归纳变量
// 的保守取值区间（从初值到 i32 边界），供上层证明索引不回绕。区分 header 测试
// 与 latch 测试两种约定（旋转前后差一次迭代），并小心边界避免自身溢出。
std::optional<SignedRange>
getDynamicStrictUnitRange(const ReliableTripCountInfo &TripInfo,
                          const LoopInductionInfo &Induction) {
  if (TripInfo.recurrenceStep != Induction.step)
    return std::nullopt;
  if (TripInfo.predicate == BinaryInst::L &&
      TripInfo.recurrenceStep == 1) {
    if (TripInfo.headerTested &&
        TripInfo.recurrenceStart == Induction.initial) {
      return SignedRange{Induction.initial,
                         std::numeric_limits<int>::max()};
    }
    const __int128 Next = static_cast<__int128>(Induction.initial) + 1;
    if (TripInfo.latchTested && Next <= std::numeric_limits<int>::max() &&
        TripInfo.recurrenceStart == Next) {
      return SignedRange{Induction.initial,
                         std::numeric_limits<int>::max() - 1LL};
    }
  }
  if (TripInfo.predicate == BinaryInst::G &&
      TripInfo.recurrenceStep == -1) {
    if (TripInfo.headerTested &&
        TripInfo.recurrenceStart == Induction.initial) {
      return SignedRange{std::numeric_limits<int>::min(),
                         Induction.initial};
    }
    const __int128 Next = static_cast<__int128>(Induction.initial) - 1;
    if (TripInfo.latchTested && Next >= std::numeric_limits<int>::min() &&
        TripInfo.recurrenceStart == Next) {
      return SignedRange{std::numeric_limits<int>::min() + 1LL,
                         Induction.initial};
    }
  }
  return std::nullopt;
}

// 前向声明：按值/按 SCEV 计算保守的“不回绕”区间，两者互相递归。
std::optional<SignedRange>
computeNoWrapRange(Value *V, const InductionRangeMap &InductionRanges,
                   std::unordered_set<Value *> &Active, ValueRangeCache &Cache);

// 递归计算 SCEV 表达式的带符号区间。对常量、未知值（转交按值分析）、加减乘、
// 除（仅当除数区间不含 0）、min/max 分别做区间算术，任何中间结果超出 i32 即
// 放弃（返回 nullopt）。带缓存、活跃集环检测与递归深度上限。KnownRanges 提供
// 已确定的归纳变量区间作为已知事实。
std::optional<SignedRange>
computeSCEVRange(const SCEVExpr *Expr, const InductionRangeMap &InductionRanges,
                 std::unordered_set<const SCEVExpr *> &Active,
                 SCEVRangeCache &Cache, ValueRangeCache &ValueCache,
                 const KnownSCEVRangeMap &KnownRanges) {
  if (!Expr || Active.count(Expr))
    return std::nullopt;
  auto Cached = Cache.find(Expr);
  if (Cached != Cache.end())
    return Cached->second;
  if (Active.size() >= kMaxSCEVRecursionDepth)
    return std::nullopt;
  auto Remember = [&](std::optional<SignedRange> Result) {
    Cache[Expr] = Result;
    return Result;
  };
  auto Known = KnownRanges.find(Expr);
  if (Known != KnownRanges.end())
    return Remember(Known->second);
  if (Expr->Kind == SCEVExprKind::Constant) {
    if (!fitsInt32(Expr->ConstValue))
      return Remember(std::nullopt);
    return Remember(SignedRange{Expr->ConstValue, Expr->ConstValue});
  }
  if (Expr->Kind == SCEVExprKind::Unknown) {
    if (!Expr->RawValue)
      return Remember(std::nullopt);
    std::unordered_set<Value *> ValueActive;
    return Remember(computeNoWrapRange(Expr->RawValue, InductionRanges,
                                       ValueActive, ValueCache));
  }
  if (Expr->Operands.size() != 2)
    return Remember(std::nullopt);

  Active.insert(Expr);
  const auto LHS = computeSCEVRange(Expr->Operands[0], InductionRanges, Active,
                                    Cache, ValueCache, KnownRanges);
  const auto RHS = computeSCEVRange(Expr->Operands[1], InductionRanges, Active,
                                    Cache, ValueCache, KnownRanges);
  Active.erase(Expr);
  if (!LHS || !RHS)
    return Remember(std::nullopt);

  if (Expr->Kind == SCEVExprKind::Max) {
    return Remember(SignedRange{std::max(LHS->min, RHS->min),
                                std::max(LHS->max, RHS->max)});
  }
  if (Expr->Kind == SCEVExprKind::Min) {
    return Remember(SignedRange{std::min(LHS->min, RHS->min),
                                std::min(LHS->max, RHS->max)});
  }

  __int128 Min = 0;
  __int128 Max = 0;
  if (Expr->Kind == SCEVExprKind::Add) {
    Min = static_cast<__int128>(LHS->min) + RHS->min;
    Max = static_cast<__int128>(LHS->max) + RHS->max;
  } else if (Expr->Kind == SCEVExprKind::Sub) {
    Min = static_cast<__int128>(LHS->min) - RHS->max;
    Max = static_cast<__int128>(LHS->max) - RHS->min;
  } else if (Expr->Kind == SCEVExprKind::Mul) {
    const __int128 Products[] = {
        static_cast<__int128>(LHS->min) * RHS->min,
        static_cast<__int128>(LHS->min) * RHS->max,
        static_cast<__int128>(LHS->max) * RHS->min,
        static_cast<__int128>(LHS->max) * RHS->max,
    };
    Min = *std::min_element(std::begin(Products), std::end(Products));
    Max = *std::max_element(std::begin(Products), std::end(Products));
  } else if (Expr->Kind == SCEVExprKind::Div &&
             (RHS->max < 0 || RHS->min > 0)) {
    const __int128 Quotients[] = {
        static_cast<__int128>(LHS->min) / RHS->min,
        static_cast<__int128>(LHS->min) / RHS->max,
        static_cast<__int128>(LHS->max) / RHS->min,
        static_cast<__int128>(LHS->max) / RHS->max,
    };
    Min = *std::min_element(std::begin(Quotients), std::end(Quotients));
    Max = *std::max_element(std::begin(Quotients), std::end(Quotients));
  } else {
    return Remember(std::nullopt);
  }
  if (!rangeFitsI32(Min, Max))
    return Remember(std::nullopt);
  return Remember(
      SignedRange{static_cast<int64_t>(Min), static_cast<int64_t>(Max)});
}

// 为所有归纳变量迭代地建立取值区间表。多轮不动点求解：每轮利用已确定区间作为
// 已知事实，尝试为尚未确定的归纳变量推出区间——先算 trip count 区间，据此从初值
// 与步长外推归纳变量的终值端点，取初值到终值的包络作为区间；trip count 拿不到
// 时退回单位步长兜底。直到某一轮没有新增即停止。
InductionRangeMap buildInductionRanges(
    SCEVResult &SCEV,
    const std::unordered_map<Value *, LoopInductionInfo> &LoopInductions) {
  InductionRangeMap Result;
  for (std::size_t Round = 0; Round <= LoopInductions.size(); ++Round) {
    std::vector<std::pair<Value *, SignedRange>> Pending;
    SCEVRangeCache SCEVCache;
    ValueRangeCache ValueCache;
    KnownSCEVRangeMap KnownRanges;
    for (const auto &[KnownValue, KnownRange] : Result) {
      if (const SCEVExpr *Expr = SCEV.getSCEV(KnownValue))
        KnownRanges.emplace(Expr, KnownRange);
    }
    for (const auto &[V, Info] : LoopInductions) {
      if (!V || Result.count(V) || !Info.RecLoop || !fitsInt32(Info.initial) ||
          Info.step == 0 || containsUnsafeSCEVOperation(V)) {
        continue;
      }
      const auto TripInfo = getReliableTripCountInfo(SCEV, Info.RecLoop);
      if (!TripInfo)
        continue;
      std::unordered_set<const SCEVExpr *> Active;
      const auto TripRange =
          computeSCEVRange(SCEV.getTripCount(Info.RecLoop), Result, Active,
                           SCEVCache, ValueCache, KnownRanges);
      if (!TripRange || TripRange->max < 0) {
        const auto DynamicRange = getDynamicStrictUnitRange(*TripInfo, Info);
        if (DynamicRange) {
          Pending.emplace_back(V, *DynamicRange);
        }
        continue;
      }

      Active.clear();
      if (!computeSCEVRange(TripInfo->bound, Result, Active, SCEVCache,
                            ValueCache, KnownRanges)) {
        continue;
      }

      const __int128 ExitEnd =
          static_cast<__int128>(TripInfo->recurrenceStart) +
          static_cast<__int128>(TripInfo->recurrenceStep) * TripRange->max;
      const __int128 ExitMin =
          std::min<__int128>(TripInfo->recurrenceStart, ExitEnd);
      const __int128 ExitMax =
          std::max<__int128>(TripInfo->recurrenceStart, ExitEnd);
      if (!rangeFitsI32(ExitMin, ExitMax))
        continue;

      // Rotated loops may expose their latch count while header-tested loops
      // expose their iteration count. This endpoint covers both conventions.
      const __int128 End = static_cast<__int128>(Info.initial) +
                           static_cast<__int128>(Info.step) * TripRange->max;
      const __int128 Min = std::min<__int128>(Info.initial, End);
      const __int128 Max = std::max<__int128>(Info.initial, End);
      if (!rangeFitsI32(Min, Max))
        continue;
      Pending.emplace_back(
          V, SignedRange{static_cast<int64_t>(Min), static_cast<int64_t>(Max)});
    }
    if (Pending.empty())
      break;
    for (const auto &[V, Range] : Pending) {
      Result.emplace(V, Range);
    }
  }
  return Result;
}

// 按值（而非 SCEV）计算保守的“不回绕”i32 区间。只处理 4 字节整数：常量、
// 已知归纳变量取表中区间、加/减/乘按区间算术、取模按除数量级给出界。任何中间
// 结果超出 i32 即返回 nullopt，表示无法证明不回绕。带缓存与环检测。
std::optional<SignedRange>
computeNoWrapRange(Value *V, const InductionRangeMap &InductionRanges,
                   std::unordered_set<Value *> &Active,
                   ValueRangeCache &Cache) {
  if (!V || Active.count(V))
    return std::nullopt;
  auto Cached = Cache.find(V);
  if (Cached != Cache.end())
    return Cached->second;
  if (Active.size() >= kMaxSCEVRecursionDepth)
    return std::nullopt;
  auto Remember = [&](std::optional<SignedRange> Result) {
    Cache[V] = Result;
    return Result;
  };
  if (!V || !V->getIRType() || V->getIRType()->getTypeSystem() != IR_INT ||
      V->getIRType()->getSize() != 4) {
    return Remember(std::nullopt);
  }
  if (auto *C = dynamic_cast<ConstInt *>(V)) {
    return Remember(SignedRange{C->getVal(), C->getVal()});
  }
  auto InductionIt = InductionRanges.find(V);
  if (InductionIt != InductionRanges.end())
    return Remember(InductionIt->second);

  auto *Bin = dynamic_cast<BinaryInst *>(V);
  if (!Bin || Bin->getUserList().size() != 2)
    return Remember(std::nullopt);

  const auto Op = Bin->getOp();
  if (Op == BinaryInst::MOD) {
    Active.insert(V);
    const auto RHS = computeNoWrapRange(Bin->getUserList()[1]->getValue(),
                                        InductionRanges, Active, Cache);
    const auto LHS = computeNoWrapRange(Bin->getUserList()[0]->getValue(),
                                        InductionRanges, Active, Cache);
    Active.erase(V);
    if (!RHS || RHS->min != RHS->max || RHS->min == 0)
      return Remember(std::nullopt);
    // 除数为 -1 且被除数可能是 INT_MIN 时，取模/取商会溢出，保守放弃。
    if (RHS->min == -1 && (!LHS || LHS->min == std::numeric_limits<int>::min()))
      return Remember(std::nullopt);

    const uint64_t Magnitude =
        RHS->min < 0 ? static_cast<uint64_t>(-static_cast<__int128>(RHS->min))
                     : static_cast<uint64_t>(RHS->min);
    const int64_t Bound = static_cast<int64_t>(
        std::min<uint64_t>(Magnitude - 1, std::numeric_limits<int>::max()));
    if (LHS && LHS->min >= 0)
      return Remember(SignedRange{0, Bound});
    if (LHS && LHS->max <= 0)
      return Remember(SignedRange{-Bound, 0});
    return Remember(SignedRange{-Bound, Bound});
  }
  if (Op != BinaryInst::ADD && Op != BinaryInst::SUB && Op != BinaryInst::MUL)
    return Remember(std::nullopt);

  Active.insert(V);
  const auto LHS = computeNoWrapRange(Bin->getUserList()[0]->getValue(),
                                      InductionRanges, Active, Cache);
  const auto RHS = computeNoWrapRange(Bin->getUserList()[1]->getValue(),
                                      InductionRanges, Active, Cache);
  Active.erase(V);
  if (!LHS || !RHS)
    return Remember(std::nullopt);

  __int128 Min = 0;
  __int128 Max = 0;
  if (Op == BinaryInst::ADD) {
    Min = static_cast<__int128>(LHS->min) + RHS->min;
    Max = static_cast<__int128>(LHS->max) + RHS->max;
  } else if (Op == BinaryInst::SUB) {
    Min = static_cast<__int128>(LHS->min) - RHS->max;
    Max = static_cast<__int128>(LHS->max) - RHS->min;
  } else {
    const __int128 Products[] = {
        static_cast<__int128>(LHS->min) * RHS->min,
        static_cast<__int128>(LHS->min) * RHS->max,
        static_cast<__int128>(LHS->max) * RHS->min,
        static_cast<__int128>(LHS->max) * RHS->max,
    };
    Min = *std::min_element(std::begin(Products), std::end(Products));
    Max = *std::max_element(std::begin(Products), std::end(Products));
  }
  if (!rangeFitsI32(Min, Max))
    return Remember(std::nullopt);
  return Remember(
      SignedRange{static_cast<int64_t>(Min), static_cast<int64_t>(Max)});
}

// 索引 V 是否可证明在 i32 内不回绕（即 computeNoWrapRange 能给出区间）。这是把
// 重复地址计算安全替换为指针 IV 推进的关键前提。
bool indexDoesNotWrap(Value *V, const InductionRangeMap &InductionRanges) {
  std::unordered_set<Value *> Active;
  ValueRangeCache Cache;
  return computeNoWrapRange(V, InductionRanges, Active, Cache).has_value();
}

// 计算某类型在代码生成时的字节大小（int/bool/float/指针/数组），供 GEP 各级
// 索引换算字节步长。无法确定或溢出时返回 nullopt。
std::optional<int64_t> codegenTypeSize(const std::shared_ptr<IRType> &Type) {
  if (!Type)
    return std::nullopt;
  switch (Type->getTypeSystem()) {
  case IR_INT: {
    auto IntTy = std::dynamic_pointer_cast<INTType>(Type);
    return IntTy && IntTy->getIsBool() ? 1 : 4;
  }
  case IR_FLOAT:
    return 4;
  case IR_POINTER:
    return 8;
  case IR_ARRAY: {
    auto ArrayTy = std::dynamic_pointer_cast<ARRAYType>(Type);
    if (!ArrayTy || ArrayTy->getElementCount() < 0)
      return std::nullopt;
    const auto ElementSize = codegenTypeSize(ArrayTy->getElementType());
    if (!ElementSize)
      return std::nullopt;
    const __int128 Size = static_cast<__int128>(*ElementSize) *
                          ArrayTy->getElementCount();
    if (Size <= 0 || Size > std::numeric_limits<int64_t>::max())
      return std::nullopt;
    return static_cast<int64_t>(Size);
  }
  default:
    return std::nullopt;
  }
}

// 把原子项 Atom 的系数累加 Coefficient 进线性签名的 terms（系数归零则删项）。
// 溢出 int64 则返回 false。
bool addLinearCoefficient(std::map<Value *, int64_t> &Terms, Value *Atom,
                          int64_t Coefficient) {
  const __int128 Sum = static_cast<__int128>(Terms[Atom]) + Coefficient;
  if (Sum < std::numeric_limits<int64_t>::min() ||
      Sum > std::numeric_limits<int64_t>::max()) {
    return false;
  }
  if (Sum == 0)
    Terms.erase(Atom);
  else
    Terms[Atom] = static_cast<int64_t>(Sum);
  return true;
}

// 把索引值 V（乘以字节比例 Scale）展开进线性地址签名。常量并入常量偏移；对可
// 证明不回绕的加/减，递归分解左右子式（减法翻转右侧比例）；否则把 V 整体作为
// 一个原子项计入。Budget/Depth 限制展开规模，防止病态深链。
bool addLinearIndex(Value *V, int64_t Scale,
                    const InductionRangeMap &InductionRanges,
                    LinearAddressSignature &Signature,
                    ValueRangeCache &RangeCache, std::size_t &Budget,
                    std::size_t Depth = 0) {
  if (!V || Depth >= kMaxSCEVRecursionDepth || Budget == 0)
    return false;
  --Budget;
  if (auto *Constant = dynamic_cast<ConstInt *>(V)) {
    const __int128 Sum = static_cast<__int128>(Signature.constant) +
                         static_cast<__int128>(Constant->getVal()) * Scale;
    if (Sum < std::numeric_limits<int64_t>::min() ||
        Sum > std::numeric_limits<int64_t>::max()) {
      return false;
    }
    Signature.constant = static_cast<int64_t>(Sum);
    return true;
  }

  auto *Bin = dynamic_cast<BinaryInst *>(V);
  std::unordered_set<Value *> RangeActive;
  if (Bin && Bin->getUserList().size() == 2 &&
      (Bin->getOp() == BinaryInst::ADD ||
       Bin->getOp() == BinaryInst::SUB) &&
      computeNoWrapRange(V, InductionRanges, RangeActive, RangeCache)) {
    Value *LHS = Bin->getUserList()[0]->getValue();
    Value *RHS = Bin->getUserList()[1]->getValue();
    if (!addLinearIndex(LHS, Scale, InductionRanges, Signature, RangeCache,
                        Budget, Depth + 1))
      return false;
    const int64_t RHSScale =
        Bin->getOp() == BinaryInst::SUB ? -Scale : Scale;
    return addLinearIndex(RHS, RHSScale, InductionRanges, Signature,
                          RangeCache, Budget, Depth + 1);
  }

  return addLinearCoefficient(Signature.terms, V, Scale);
}

// 把整条 GEP 展开成线性地址签名（base + 各项系数 + 常量字节偏移）。逐级索引按
// 其被索引类型的字节大小换算比例，任何一步无法确定或展开失败即返回 nullopt。
// 用于后续判断两条 GEP 是否仅相差常量偏移而可聚类共享指针 IV。
std::optional<LinearAddressSignature>
buildLinearAddressSignature(GetElementPtrInst &GEP,
                            const InductionRangeMap &InductionRanges) {
  auto &Ops = GEP.getUserList();
  if (Ops.size() <= 1 || !Ops[0] || !Ops[0]->getValue())
    return std::nullopt;
  Value *Base = Ops[0]->getValue();
  auto BaseTy = std::dynamic_pointer_cast<POINTERType>(Base->getIRType());
  if (!BaseTy)
    return std::nullopt;

  LinearAddressSignature Signature;
  Signature.base = Base;
  ValueRangeCache RangeCache;
  std::size_t Budget = kMaxSCEVExpansionCost;
  auto CurrentTy = BaseTy->getPointerType();
  for (std::size_t I = 1; I < Ops.size(); ++I) {
    std::shared_ptr<IRType> IndexedTy = CurrentTy;
    if (I != 1) {
      if (auto ArrayTy = std::dynamic_pointer_cast<ARRAYType>(CurrentTy)) {
        IndexedTy = ArrayTy->getElementType();
      } else if (auto PointerTy =
                     std::dynamic_pointer_cast<POINTERType>(CurrentTy)) {
        IndexedTy = PointerTy->getPointerType();
      }
    }
    const auto Scale = codegenTypeSize(IndexedTy);
    if (!Scale || *Scale <= 0 || !Ops[I] ||
        !addLinearIndex(Ops[I]->getValue(), *Scale, InductionRanges, Signature,
                        RangeCache, Budget)) {
      return std::nullopt;
    }
    CurrentTy = IndexedTy;
  }
  Signature.pointeeType = CurrentTy;
  return Signature;
}

// 分析索引值 V 是否是关于循环 L 的仿射表达式（coefficient*induction + 不变量）。
// 归纳变量本身系数为 1、出现 1 次；循环不变量出现 0 次；加/减线性组合系数相加减；
// 乘法仅当整棵子树都是循环不变量（出现 0 次）时可克隆，系数记 0。要求整条表达式
// 里归纳变量最多出现一次且唯一，否则拒绝。可克隆的中间值记入 CloneValues，稍后
// 会在预头以初值重建。带缓存与活跃集环检测。
bool analyzeAffineIndex(
    Value *V, const Loop *L, BasicBlock *PreHeader, const DominatorTree &DT,
    const std::unordered_map<Value *, LoopInductionInfo> &LoopInductions,
    bool AllowExpressions, std::unordered_set<Value *> &CloneValues,
    std::unordered_set<Value *> &Active, AffineIndexCache &Cache,
    AffineIndexInfo &Result) {
  if (!isSupportedIndexValue(V))
    return false;

  auto Cached = Cache.find(V);
  if (Cached != Cache.end()) {
    if (!Cached->second)
      return false;
    Result = *Cached->second;
    return true;
  }
  if (Active.size() >= kMaxSCEVRecursionDepth)
    return false;
  auto Reject = [&]() {
    Cache[V] = std::nullopt;
    return false;
  };

  auto InductionIt = LoopInductions.find(V);
  if (InductionIt != LoopInductions.end() && InductionIt->second.RecLoop == L) {
    Result.induction = V;
    Result.coefficient = 1;
    Result.occurrences = 1;
    Cache[V] = Result;
    return true;
  }

  if (dominatesPreheader(V, PreHeader, DT)) {
    Cache[V] = Result;
    return true;
  }
  if (!AllowExpressions)
    return Reject();

  auto *Bin = dynamic_cast<BinaryInst *>(V);
  if (!Bin || Bin->getUserList().size() != 2)
    return Reject();
  if (Active.count(V))
    return false;
  const auto Op = Bin->getOp();
  if (Op != BinaryInst::ADD && Op != BinaryInst::SUB && Op != BinaryInst::MUL) {
    return Reject();
  }

  Active.insert(V);
  AffineIndexInfo LHS;
  AffineIndexInfo RHS;
  Value *LHSValue = Bin->getUserList()[0]->getValue();
  Value *RHSValue = Bin->getUserList()[1]->getValue();
  const bool Valid =
      analyzeAffineIndex(LHSValue, L, PreHeader, DT, LoopInductions,
                         AllowExpressions, CloneValues, Active, Cache, LHS) &&
      analyzeAffineIndex(RHSValue, L, PreHeader, DT, LoopInductions,
                         AllowExpressions, CloneValues, Active, Cache, RHS);
  Active.erase(V);
  if (!Valid || LHS.occurrences + RHS.occurrences > 1)
    return Reject();
  if (LHS.induction && RHS.induction && LHS.induction != RHS.induction)
    return Reject();

  Result.induction = LHS.induction ? LHS.induction : RHS.induction;
  Result.occurrences = LHS.occurrences + RHS.occurrences;
  if (Op == BinaryInst::MUL) {
    // Multiplication is cloneable only in a loop-invariant subtree.
    if (Result.occurrences != 0)
      return Reject();
    Result.coefficient = 0;
  } else if (Op == BinaryInst::ADD) {
    Result.coefficient = LHS.coefficient + RHS.coefficient;
  } else {
    Result.coefficient = LHS.coefficient - RHS.coefficient;
  }

  CloneValues.insert(V);
  Cache[V] = Result;
  return true;
}

// 按“先父后子”的先序收集循环 L 及其子循环。
bool collectLoopOrder(const Loop *L, std::vector<const Loop *> &Loops) {
  if (!L)
    return false;
  Loops.push_back(L);
  for (const Loop *Sub : L->getSubLoops()) {
    collectLoopOrder(Sub, Loops);
  }
  return true;
}

// 扫描各循环 header 里的 PHI，借助 SCEV 识别出常量起点与常量步长的归纳变量，
// 建立“PHI 值 -> 归纳信息（初值、步长、所属循环）”映射。
std::unordered_map<Value *, LoopInductionInfo>
collectLoopInductions(SCEVResult &SCEV,
                      const std::vector<const Loop *> &Loops) {
  std::unordered_map<Value *, LoopInductionInfo> Result;
  for (const Loop *L : Loops) {
    BasicBlock *Header = L ? L->getHeader() : nullptr;
    if (!Header)
      continue;
    for (Instruction *Inst : *Header) {
      auto *Phi = dynamic_cast<PhiInst *>(Inst);
      if (!Phi)
        break;
      const auto Affine = SCEV.getAffine(Phi);
      if (!Affine || Affine->RecLoop != L || !Affine->ConstantStart ||
          !Affine->ConstantStep) {
        continue;
      }
      LoopInductionInfo Info;
      Info.RecLoop = L;
      Info.initial = *Affine->ConstantStart;
      Info.step = *Affine->ConstantStep;
      Result.emplace(Phi, Info);
    }
  }
  return Result;
}

// 把指令插到块的终结指令之前（用于在预头/latch 末尾插入地址计算）。
bool insertBeforeTerminator(BasicBlock *BB, Instruction *Inst) {
  if (!BB || !Inst || BB->empty())
    return false;
  auto It = BB->rbegin();
  if (It == BB->rend())
    return false;
  It.insertBefore(Inst);
  return true;
}

// 把指令插到块里第一条非 PHI 指令之前（PHI 必须保持在块首，指针 IV 的 PHI
// 需插在它们之后、其余指令之前）。
bool insertBeforeFirstNonPhi(BasicBlock *BB, Instruction *Inst) {
  if (!BB || !Inst)
    return false;
  for (auto It = BB->begin(); It != BB->end(); ++It) {
    Instruction *Cur = *It;
    if (!dynamic_cast<PhiInst *>(Cur)) {
      It.insertBefore(Inst);
      return true;
    }
  }
  if (BB->empty()) {
    BB->pushBack(Inst);
    return true;
  }
  auto It = BB->rbegin();
  if (It == BB->rend())
    return false;
  It.insertBefore(Inst);
  return true;
}

// 把 Inst 插到块中指定的 Before 指令之前。
bool insertBeforeInstruction(BasicBlock *BB, Instruction *Before,
                             Instruction *Inst) {
  if (!BB || !Before || !Inst)
    return false;
  for (auto It = BB->begin(); It != BB->end(); ++It) {
    if (*It == Before) {
      It.insertBefore(Inst);
      return true;
    }
  }
  return false;
}

// 尝试为单个 GEP 构造改写候选。要求：base 与所有非可变索引都循环不变；恰有
// 最后一个索引是关于某归纳变量的仿射式，系数为 +1/-1，类型 4 字节且可证明不
// 回绕；归纳变量初值/步长在 i32 内、区间跨度大于步长量级（否则收益不足）。
// 还检查 GEP 的所有使用者要么在循环内、要么被原 GEP 所在块支配（允许展开后
// 残余循环复用末尾地址），且不能是 PHI。满足则返回候选。
std::optional<PrepareCandidate> tryBuildCandidate(
    GetElementPtrInst &GEP, const Loop *L, BasicBlock *PreHeader,
    BasicBlock *Latch, const DominatorTree &DT,
    const InductionRangeMap &InductionRanges,
    const std::unordered_map<Value *, LoopInductionInfo> &LoopInductions) {
  auto &Ops = GEP.getUserList();
  if (Ops.size() <= 1)
    return std::nullopt;
  if (!PreHeader || !Latch || !PreHeader->back() || !Latch->back())
    return std::nullopt;

  unsigned VaryingIndex = 0;
  int64_t Start = 0;
  int64_t Step = 0;
  std::size_t IndexSize = 4;
  bool FoundVarying = false;
  Value *OriginalIndex = nullptr;
  Value *Induction = nullptr;
  std::unordered_set<Value *> CloneValues;
  const bool AllowAffineExpressions =
      std::getenv("BACKEND_DISABLE_AFFINE_POINTER_IV") == nullptr;

  for (unsigned I = 0; I < Ops.size(); ++I) {
    Value *Op = Ops[I] ? Ops[I]->getValue() : nullptr;
    if (!Op)
      return std::nullopt;

    if (I == 0) {
      if (!dominatesPreheader(Op, PreHeader, DT))
        return std::nullopt;
      continue;
    }

    if (I + 1 == Ops.size()) {
      AffineIndexInfo IndexInfo;
      std::unordered_set<Value *> Active;
      std::unordered_set<Value *> IndexCloneValues;
      AffineIndexCache IndexCache;
      if (analyzeAffineIndex(Op, L, PreHeader, DT, LoopInductions,
                             AllowAffineExpressions, IndexCloneValues, Active,
                             IndexCache, IndexInfo) &&
          IndexInfo.occurrences == 1 && IndexInfo.induction &&
          (IndexInfo.coefficient == 1 || IndexInfo.coefficient == -1)) {
        if (Op->getIRType()->getSize() != 4 ||
            !indexDoesNotWrap(Op, InductionRanges)) {
          return std::nullopt;
        }
        auto It = LoopInductions.find(IndexInfo.induction);
        if (It == LoopInductions.end() || It->second.RecLoop != L ||
            !fitsInt32(It->second.initial) || !fitsInt32(It->second.step)) {
          return std::nullopt;
        }
        auto InductionRange = InductionRanges.find(IndexInfo.induction);
        const __int128 RangeSpan =
            InductionRange == InductionRanges.end()
                ? 0
                : static_cast<__int128>(InductionRange->second.max) -
                      InductionRange->second.min;
        const __int128 StepMagnitude =
            It->second.step > 0 ? static_cast<__int128>(It->second.step)
                                : -static_cast<__int128>(It->second.step);
        if (InductionRange == InductionRanges.end() ||
            RangeSpan <= StepMagnitude) {
          return std::nullopt;
        }

        int64_t EffectiveStep = It->second.step;
        if (IndexInfo.coefficient == -1) {
          if (EffectiveStep == std::numeric_limits<int>::min())
            return std::nullopt;
          EffectiveStep = -EffectiveStep;
        }
        if (EffectiveStep == 0 || !fitsInt32(EffectiveStep))
          return std::nullopt;

        FoundVarying = true;
        VaryingIndex = I;
        OriginalIndex = Op;
        Induction = IndexInfo.induction;
        Start = It->second.initial;
        Step = EffectiveStep;
        IndexSize = Op->getIRType()->getSize();
        CloneValues = std::move(IndexCloneValues);
        continue;
      }
    }

    if (!dominatesPreheader(Op, PreHeader, DT))
      return std::nullopt;
  }

  if (!FoundVarying)
    return std::nullopt;

  for (auto It = GEP.getUseList().begin(); It != GEP.getUseList().end(); ++It) {
    Use *UseNode = *It;
    if (!UseNode)
      return std::nullopt;
    auto *UserInst = dynamic_cast<Instruction *>(UseNode->getUser());
    if (!UserInst)
      return std::nullopt;
    BasicBlock *UserBB = UserInst->getParent();
    if (!UserBB)
      return std::nullopt;
    if (dynamic_cast<PhiInst *>(UserInst))
      return std::nullopt;

    // A loop-produced address may remain live into a dominated exit block.
    // This is common after unrolling: the final adjacent address is reused by
    // a scalar remainder, so rejecting every out-of-loop use prevents the
    // whole constant-offset GEP cluster from sharing one pointer IV.  The
    // replacement pointer is defined in the loop header and therefore also
    // dominates any non-PHI use already dominated by the original GEP block.
    // Keep PHIs excluded because their edge-specific dominance rule needs
    // separate handling.
    if (!L->contains(UserBB) &&
        !DT.dominates(GEP.getParent(), UserBB)) {
      return std::nullopt;
    }
  }

  PrepareCandidate Candidate;
  Candidate.loop = L;
  Candidate.preheader = PreHeader;
  Candidate.latch = Latch;
  Candidate.gep = &GEP;
  Candidate.originalIndex = OriginalIndex;
  Candidate.induction = Induction;
  Candidate.varyingIndex = VaryingIndex;
  Candidate.start = Start;
  Candidate.step = Step;
  Candidate.indexSize = IndexSize;
  Candidate.score = GEP.getUseList().size;
  Candidate.cloneValues = std::move(CloneValues);
  return Candidate;
}

// 收集循环 L 的所有候选并做聚类。先要求循环是 loop-simplify 规范形（有唯一预头
// 与 latch），逐块扫描 GEP 生成候选；再计算每个候选的线性地址签名，把 base、
// terms、指针元素类型、归纳变量、步长都相同、且常量字节偏移落在立即数范围内、
// 能被元素大小整除的候选聚为一簇（贪心选取能带最多成员的锚点）。最终按分数
// （使用次数，聚类会累加成员分）从高到低稳定排序。可用环境变量关闭聚类。
std::vector<PrepareCandidate> collectCandidatesForLoop(
    const Loop *L, const DominatorTree &DT,
    const InductionRangeMap &InductionRanges,
    const std::unordered_map<Value *, LoopInductionInfo> &LoopInductions) {
  BasicBlock *PreHeader = nullptr;
  BasicBlock *Latch = nullptr;
  if (!L->getLoopSimplifyForm(PreHeader, Latch))
    return {};

  std::vector<PrepareCandidate> Candidates;
  // 使用 RPO 序（只含当前循环层、由 CFG 确定性遍历生成）而不是
  // L->getBlocks()（其填充顺序来自 LoopInfo 内部 unordered_map 的哈希遍历，
  // 随进程 ASLR 变化），保证候选收集顺序可复现：同输入多次编译 .s 一致。
  for (BasicBlock *BB : L->getBlocksInRPO()) {
    if (!BB)
      continue;
    for (Instruction *Inst : *BB) {
      auto *GEP = dynamic_cast<GetElementPtrInst *>(Inst);
      if (!GEP)
        continue;
      auto Candidate = tryBuildCandidate(*GEP, L, PreHeader, Latch, DT,
                                         InductionRanges, LoopInductions);
      if (!Candidate)
        continue;
      Candidates.push_back(std::move(*Candidate));
    }
  }

  std::vector<std::optional<LinearAddressSignature>> Signatures;
  Signatures.reserve(Candidates.size());
  for (PrepareCandidate &Candidate : Candidates) {
    Signatures.push_back(
        buildLinearAddressSignature(*Candidate.gep, InductionRanges));
  }
  if (std::getenv("BACKEND_DISABLE_GEP_CLUSTERING") != nullptr) {
    std::stable_sort(Candidates.begin(), Candidates.end(),
                     [](const PrepareCandidate &A,
                        const PrepareCandidate &B) {
                       return A.score > B.score;
                     });
    return Candidates;
  }

  // 计算成员 GEP 相对锚点 GEP 的元素偏移：仅当二者线性签名的 base/terms/元素
  // 类型/归纳变量/步长完全一致，且字节偏移在 12 位立即数范围（-2048..2047）内、
  // 能整除元素大小时才可聚类，否则无法用“基址+常量偏移”共享同一指针 IV。
  auto ClusterOffset = [&](std::size_t Anchor,
                           std::size_t Member) -> std::optional<int64_t> {
    if (!Signatures[Anchor] || !Signatures[Member])
      return std::nullopt;
    const auto &A = *Signatures[Anchor];
    const auto &M = *Signatures[Member];
    if (A.base != M.base || A.terms != M.terms || !A.pointeeType ||
        A.pointeeType != M.pointeeType ||
        Candidates[Anchor].induction != Candidates[Member].induction ||
        Candidates[Anchor].step != Candidates[Member].step) {
      return std::nullopt;
    }
    const __int128 ByteOffset =
        static_cast<__int128>(M.constant) - A.constant;
    const auto ElementSize = codegenTypeSize(A.pointeeType);
    if (!ElementSize || *ElementSize <= 0 || ByteOffset < -2048 ||
        ByteOffset > 2047 || ByteOffset % *ElementSize != 0) {
      return std::nullopt;
    }
    const __int128 ElementOffset = ByteOffset / *ElementSize;
    if (!rangeFitsI32(ElementOffset, ElementOffset))
      return std::nullopt;
    return static_cast<int64_t>(ElementOffset);
  };

  std::vector<PrepareCandidate> Clustered;
  std::vector<bool> Consumed(Candidates.size(), false);
  while (Clustered.size() < Candidates.size()) {
    std::size_t Best = Candidates.size();
    std::vector<std::pair<std::size_t, int64_t>> BestMembers;
    for (std::size_t I = 0; I < Candidates.size(); ++I) {
      if (Consumed[I])
        continue;
      std::vector<std::pair<std::size_t, int64_t>> Members;
      for (std::size_t J = 0; J < Candidates.size(); ++J) {
        if (I == J || Consumed[J])
          continue;
        const auto Offset = ClusterOffset(I, J);
        if (Offset)
          Members.emplace_back(J, *Offset);
      }
      if (Best == Candidates.size() || Members.size() > BestMembers.size() ||
          (Members.size() == BestMembers.size() &&
           Candidates[I].score > Candidates[Best].score)) {
        Best = I;
        BestMembers = std::move(Members);
      }
    }
    if (Best == Candidates.size())
      break;

    Consumed[Best] = true;
    PrepareCandidate Anchor = std::move(Candidates[Best]);
    for (const auto &[MemberIndex, Offset] : BestMembers) {
      if (Consumed[MemberIndex])
        continue;
      Consumed[MemberIndex] = true;
      Anchor.score += Candidates[MemberIndex].score + 1;
      Anchor.clusterMembers.push_back(
          PrepareCandidate::ClusterMember{Candidates[MemberIndex].gep,
                                          Offset});
    }
    Clustered.push_back(std::move(Anchor));
  }

  Candidates = std::move(Clustered);

  std::stable_sort(Candidates.begin(), Candidates.end(),
                   [](const PrepareCandidate &A, const PrepareCandidate &B) {
                     return A.score > B.score;
                   });
  return Candidates;
}

// 决定单个循环最多做几个变换。候选很少时全做；否则限制新增指针 IV 的数量以
// 控制寄存器压力：聚类过则允许 3 个锚点（每个替代多条临时地址，活跃区间增量
// 可控），无聚类的宽循环仍维持 2 个的上限。
std::size_t maxTransformsForLoop(
    const std::vector<PrepareCandidate> &Candidates) {
  if (Candidates.size() <= 2) {
    return Candidates.size();
  }

  const bool HasCluster =
      std::any_of(Candidates.begin(), Candidates.end(),
                  [](const PrepareCandidate &Candidate) {
                    return !Candidate.clusterMembers.empty();
                  });
  // 聚类后的三个 anchor 通常替代更多临时地址，live range 增量可控；
  // 无聚类的宽循环仍维持原来的两个 pointer IV 上限。
  return HasCluster ? 3 : 2;
}

// 克隆一个 GEP，同时把第 ReplacedIndex 个索引替换为 Replacement。用于在预头
// 生成初始地址（把可变索引替换为初值表达式）。产出的是 non-inbounds GEP。
GetElementPtrInst *cloneGEPWithReplacement(GetElementPtrInst &Orig,
                                           unsigned ReplacedIndex,
                                           Value *Replacement) {
  auto &Ops = Orig.getUserList();
  if (Ops.empty() || !Ops[0])
    return nullptr;

  auto *Clone = new NonInBoundsGEPInst(Ops[0]->getValue());
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

// 在预头把可变索引表达式按“归纳变量取初值 Start”重建出初始索引值。归纳变量
// 本身替换为 Start；循环不变量原样保留；仿射分析标记为可克隆的中间二元运算
// 递归重建并插到预头末尾。返回重建后的初始索引值（失败返回 nullptr）。
Value *
materializeInitialIndex(Value *V, const PrepareCandidate &Candidate,
                        Value *Start,
                        std::unordered_map<Value *, Value *> &Materialized) {
  if (V == Candidate.induction)
    return Start;
  if (!Candidate.cloneValues.count(V))
    return V;
  auto It = Materialized.find(V);
  if (It != Materialized.end())
    return It->second;

  auto *Bin = dynamic_cast<BinaryInst *>(V);
  if (!Bin || Bin->getUserList().size() != 2)
    return nullptr;
  Value *LHS = materializeInitialIndex(Bin->getUserList()[0]->getValue(),
                                       Candidate, Start, Materialized);
  Value *RHS = materializeInitialIndex(Bin->getUserList()[1]->getValue(),
                                       Candidate, Start, Materialized);
  if (!LHS || !RHS)
    return nullptr;

  auto *Clone = new BinaryInst(LHS, Bin->getOp(), RHS);
  if (!insertBeforeTerminator(Candidate.preheader, Clone)) {
    delete Clone;
    return nullptr;
  }
  Materialized[V] = Clone;
  return Clone;
}

// 真正落地一个候选的强度削减改写：
// 1. 预头算出初始地址 InitGEP（可变索引取初值）；
// 2. header 插入指针型 PHI，两个入边分别是预头的 InitGEP 与 latch 的推进结果；
// 3. latch 用一条固定步长的 GEP 对 PHI 做一次推进（步长以元素为单位）；
// 4. 对聚类成员，用 PHI（或 PHI 加常量元素偏移的 GEP）替换其全部使用后删除；
// 5. 最后用 PHI 替换原 GEP 的所有使用并删除原 GEP。
// 任何一步失败即返回 false。
bool rewriteCandidate(const PrepareCandidate &Candidate) {
  GetElementPtrInst *Orig = Candidate.gep;
  if (!Orig || !Candidate.loop || !Candidate.preheader || !Candidate.latch)
    return false;
  auto OrigType = Orig->getIRType();
  if (!OrigType)
    return false;

  auto *StartConst = ConstInt::newConstInt(static_cast<int>(Candidate.start),
                                           Candidate.indexSize);
  auto *StepConst = ConstInt::newConstInt(static_cast<int>(Candidate.step),
                                          Candidate.indexSize);

  std::unordered_map<Value *, Value *> Materialized;
  Value *InitialIndex = materializeInitialIndex(
      Candidate.originalIndex, Candidate, StartConst, Materialized);
  if (!InitialIndex)
    return false;

  GetElementPtrInst *InitGEP =
      cloneGEPWithReplacement(*Orig, Candidate.varyingIndex, InitialIndex);
  if (!InitGEP)
    return false;

  auto *PtrPhi = new PhiInst(OrigType);
  // latch 只做一步推进；前缀索引已体现在 PtrPhi 里。
  auto *LatchGEP = new NonInBoundsGEPInst(PtrPhi);
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

  for (const PrepareCandidate::ClusterMember &Member :
       Candidate.clusterMembers) {
    GetElementPtrInst *MemberGEP = Member.gep;
    if (!MemberGEP || MemberGEP == Orig)
      return false;
    Value *Replacement = PtrPhi;
    if (Member.elementOffset != 0) {
      auto *OffsetGEP = new NonInBoundsGEPInst(PtrPhi);
      OffsetGEP->addUse(ConstInt::newConstInt(
          static_cast<int>(Member.elementOffset), Candidate.indexSize));
      OffsetGEP->updateType();
      if (!insertBeforeInstruction(MemberGEP->getParent(), MemberGEP,
                                   OffsetGEP)) {
        delete OffsetGEP;
        return false;
      }
      Replacement = OffsetGEP;
    }
    MemberGEP->replaceAllUsesWith(Replacement);
    if (!MemberGEP->getUseList().isEmpty())
      return false;
    delete MemberGEP;
  }

  Orig->replaceAllUsesWith(PtrPhi);
  if (!Orig->getUseList().isEmpty())
    return false;
  delete Orig;
  return true;
}

} // namespace

// pass 入口：遍历模块每个函数，注册并取用支配树/循环/SCEV 分析，预检查 SCEV
// 规模，收集归纳变量与其区间，按循环深度从内到外处理每个循环，收集候选、限制
// 数量后逐个改写。返回是否修改了模块。可用环境变量整体关闭本 pass。
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
    if (!FuncPtr)
      continue;
    Function &F = *FuncPtr;
    if (F.size() == 0)
      continue;

    const auto &DT = FAM.getResult<DominantAnalysis>(F);
    const auto &LI = FAM.getResult<LoopAnalysis>(F);
    if (LI.getTopLevelLoops().empty() || !hasManageableSCEVInputs(LI))
      continue;
    auto &SCEV = FAM.getResult<SCEVAnalysis>(F);

    std::vector<const Loop *> Loops;
    for (Loop *Top : LI.getTopLevelLoops()) {
      collectLoopOrder(Top, Loops);
    }
    std::stable_sort(Loops.begin(), Loops.end(),
                     [](const Loop *A, const Loop *B) {
                       // 仅按深度从内到外排序；同深度循环保持 collectLoopOrder
                       // 的确定输入序（stable_sort 不交换相等元素）。不要用
                       // 指针地址做平局比较，否则同深度兄弟循环的相对顺序随
                       // ASLR 变化，导致改写结果抖动。
                       return A->getDepth() > B->getDepth();
                     });
    const auto LoopInductions = collectLoopInductions(SCEV, Loops);
    const auto InductionRanges = buildInductionRanges(SCEV, LoopInductions);

    for (const Loop *L : Loops) {
      if (!L)
        continue;
      auto Candidates =
          collectCandidatesForLoop(L, DT, InductionRanges, LoopInductions);
      const std::size_t Limit = maxTransformsForLoop(Candidates);
      for (std::size_t I = 0; I < Limit; ++I) {
        const auto &Candidate = Candidates[I];
        Changed |= rewriteCandidate(Candidate);
      }
    }
  }

  return Changed;
}

} // namespace riscv
