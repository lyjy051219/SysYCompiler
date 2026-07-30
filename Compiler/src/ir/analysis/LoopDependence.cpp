#include "ir/analysis/LoopDependence.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <utility>
//构造不绑定任何函数分析结果的空查询器
LoopDependenceInfo::LoopDependenceInfo() = default;
//使用函数及其依赖分析结果构造循环依赖查询器
LoopDependenceInfo::LoopDependenceInfo(Function *F, LoopInfo *LoopInfoPtr,
                                       SCEVResult *SCEV,
                                       BasicAAResult *AliasAnalysis,
                                       FunctionAnalysisPassManager *AnalysisManager)
    : Func(F), LI(LoopInfoPtr), SE(SCEV), AA(AliasAnalysis),
      FAM(AnalysisManager) {}
//销毁循环依赖查询器及其惰性缓存
LoopDependenceInfo::~LoopDependenceInfo() = default;
//移动构造查询器并接管全部循环摘要
LoopDependenceInfo::LoopDependenceInfo(LoopDependenceInfo &&) noexcept =
    default;
//移动赋值查询器并接管全部循环摘要
LoopDependenceInfo &
LoopDependenceInfo::operator=(LoopDependenceInfo &&) noexcept = default;
//返回指定循环的缓存摘要，尚未构建时立即构建并缓存
LoopDependenceInfo::LoopSummary &
LoopDependenceInfo::getOrBuildSummary(Loop *L) {
    auto It = Cache.find(L);
    if (It != Cache.end()) return *It->second;
    auto Summary = buildSummary(L);
    LoopSummary *Raw = Summary.get();
    Cache.emplace(L, std::move(Summary));
    return *Raw;
}
//返回指定循环及其子循环中的全部可建模访存
const std::vector<LoopMemoryAccess> &
LoopDependenceInfo::getMemoryAccesses(Loop *L) {
    return L ? getOrBuildSummary(L).Accesses : EmptyMemoryAccesses;
}
//返回指定循环及其子循环中的全部保守依赖记录
const std::vector<LoopDependenceRecord> &
LoopDependenceInfo::getDependences(Loop *L) {
    return L ? getOrBuildSummary(L).Dependences : EmptyDependenceRecords;
}
//判断循环摘要是否包含未知内存或不能跨越的外部效果
bool LoopDependenceInfo::hasUnknownMemory(Loop *L) {
    if (!L) return true;
    const LoopSummary &Summary = getOrBuildSummary(L);
    //兼容只查询 UnknownMemory 的既有消费者：可观察调用和严格顺序屏障
    //虽不等于未知程序内存，也都不能被循环重排或并行化跨越。
    return Summary.UnknownMemory || Summary.ObservableEffect ||
           Summary.OrderingBarrier;
}
//判断循环中是否存在程序内存之外的可观察效果
bool LoopDependenceInfo::hasObservableEffect(Loop *L) {
    return L && getOrBuildSummary(L).ObservableEffect;
}
//判断循环中是否存在不可跨越的严格顺序屏障
bool LoopDependenceInfo::hasOrderingBarrier(Loop *L) {
    return L && getOrBuildSummary(L).OrderingBarrier;
}
//返回指定访存沿目标循环轴变化的总字节步长
std::optional<int64_t>
LoopDependenceInfo::getByteStride(Instruction *I, Loop *L) {
    if (!I || !L) return std::nullopt;
    LoopSummary &Summary = getOrBuildSummary(L);
    std::optional<int64_t> CommonStride;
    bool Found = false;
    for (const LoopMemoryAccess &Access : Summary.Accesses) {
        if (Access.Inst != I) continue;
        Found = true;
        if (!Access.AddressExact ||
            Access.Subscripts.size() != Access.ByteStrides.size()) {
            return std::nullopt;
        }
        int64_t Total = 0;
        for (std::size_t Index = 0; Index < Access.Subscripts.size();
             ++Index) {
            const LoopAffineExpr &Expression = Access.Subscripts[Index];
            auto It = Expression.LoopCoefficients.find(L);
            if (It == Expression.LoopCoefficients.end()) continue;
            int64_t Contribution = 0;
            int64_t Updated = 0;
            if (!checkedMul(It->second, Access.ByteStrides[Index],
                            Contribution) ||
                !checkedAdd(Total, Contribution, Updated)) {
                return std::nullopt;
            }
            Total = Updated;
        }
        if (CommonStride && *CommonStride != Total) return std::nullopt;
        CommonStride = Total;
    }
    return Found ? CommonStride : std::nullopt;
}
//根据相关分析的保留状态判断当前结果是否失效
bool LoopDependenceInfo::invalidate(Function &,
                                    const PreservationStatus &PA) {
    return !PA.isKept(LoopDependenceAnalysis::ID()) ||
           !PA.isKept(LoopAnalysis::ID()) ||
           !PA.isKept(SCEVAnalysis::ID()) ||
           !PA.isKept(BasicAliasAnalysis::ID()) ||
           !PA.isKept(SideEffectAnalysis::ID());
}
//执行带溢出检查的64位有符号加法
bool LoopDependenceInfo::checkedAdd(int64_t A, int64_t B, int64_t &Result) {
    const __int128 Wide = static_cast<__int128>(A) +
                          static_cast<__int128>(B);
    if (Wide < std::numeric_limits<int64_t>::min() ||
        Wide > std::numeric_limits<int64_t>::max()) {
        return false;
    }
    Result = static_cast<int64_t>(Wide);
    return true;
}
//执行带溢出检查的64位有符号乘法
bool LoopDependenceInfo::checkedMul(int64_t A, int64_t B, int64_t &Result) {
    const __int128 Wide = static_cast<__int128>(A) *
                          static_cast<__int128>(B);
    if (Wide < std::numeric_limits<int64_t>::min() ||
        Wide > std::numeric_limits<int64_t>::max()) {
        return false;
    }
    Result = static_cast<int64_t>(Wide);
    return true;
}
//移除仿射表达式中系数为零的循环轴和不变量项
void LoopDependenceInfo::eraseZeroTerms(LoopAffineExpr &Expression) {
    for (auto It = Expression.LoopCoefficients.begin();
         It != Expression.LoopCoefficients.end();) {
        if (It->second == 0)
            It = Expression.LoopCoefficients.erase(It);
        else
            ++It;
    }
    for (auto It = Expression.InvariantTerms.begin();
         It != Expression.InvariantTerms.end();) {
        if (It->second == 0)
            It = Expression.InvariantTerms.erase(It);
        else
            ++It;
    }
}
//将一条仿射表达式按比例安全地累加到目标表达式
bool LoopDependenceInfo::addScaledAffine(LoopAffineExpr &Destination,
                                         const LoopAffineExpr &Source,
                                         int64_t Scale) {
    if (!Destination.Exact || !Source.Exact) {
        Destination.Exact = false;
        return false;
    }
    int64_t Scaled = 0;
    int64_t Updated = 0;
    if (!checkedMul(Source.Constant, Scale, Scaled) ||
        !checkedAdd(Destination.Constant, Scaled, Updated)) {
        Destination.Exact = false;
        return false;
    }
    Destination.Constant = Updated;
    for (const auto &Entry : Source.LoopCoefficients) {
        if (!checkedMul(Entry.second, Scale, Scaled) ||
            !checkedAdd(Destination.LoopCoefficients[Entry.first], Scaled,
                        Updated)) {
            Destination.Exact = false;
            return false;
        }
        Destination.LoopCoefficients[Entry.first] = Updated;
    }
    for (const auto &Entry : Source.InvariantTerms) {
        if (!checkedMul(Entry.second, Scale, Scaled) ||
            !checkedAdd(Destination.InvariantTerms[Entry.first], Scaled,
                        Updated)) {
            Destination.Exact = false;
            return false;
        }
        Destination.InvariantTerms[Entry.first] = Updated;
    }
    eraseZeroTerms(Destination);
    return true;
}
//判断候选循环是否等于或嵌套在给定分析范围中
bool LoopDependenceInfo::loopIsInScope(const Loop *Candidate,
                                       const Loop *Scope) {
    if (!Candidate || !Scope) return false;
    return Candidate == Scope ||
           (Candidate->getHeader() && Scope->contains(Candidate->getHeader()));
}
//按照前序遍历收集当前循环及其全部子循环
void LoopDependenceInfo::collectLoopsPreorder(
    Loop *L, std::vector<Loop *> &Loops) {
    if (!L) return;
    Loops.push_back(L);
    for (Loop *SubLoop : L->getSubLoops())
        collectLoopsPreorder(SubLoop, Loops);
}
//判断表达式是否只包含目标循环轴的非零系数
bool LoopDependenceInfo::isPureAxisCoordinate(
    const LoopAffineExpr &Expression, Loop *Axis, int64_t &Coefficient) {
    if (!Expression.Exact || !Axis || !Expression.InvariantTerms.empty() ||
        Expression.LoopCoefficients.size() != 1) {
        return false;
    }
    auto It = Expression.LoopCoefficients.find(Axis);
    if (It == Expression.LoopCoefficients.end() || It->second == 0)
        return false;
    Coefficient = It->second;
    return true;
}
//交换查询要求每个潜在冲突至少在两个交换轴之一被证明局限于同次迭代。
bool LoopDependenceInfo::isInterchangeLegal(Loop *Outer, Loop *Inner) {
    if (!Outer || !Inner || Outer == Inner || !Inner->getHeader() ||
        !Outer->contains(Inner->getHeader())) {
        return false;
    }
    LoopSummary &Summary = getOrBuildSummary(Outer);
    if (Summary.UnknownMemory || Summary.ObservableEffect ||
        Summary.OrderingBarrier)
        return false;
    for (const LoopDependenceRecord &Record : Summary.Dependences) {
        bool OuterEqual = false;
        bool InnerEqual = false;
        for (std::size_t I = 0;
             I < Record.Loops.size() && I < Record.DirectionMasks.size();
             ++I) {
            if (Record.Loops[I] == Outer)
                OuterEqual =
                    Record.DirectionMasks[I] == LoopDepSameIteration;
            if (Record.Loops[I] == Inner)
                InnerEqual =
                    Record.DirectionMasks[I] == LoopDepSameIteration;
        }
        if (!OuterEqual && !InnerEqual) return false;
    }
    return true;
}
//分布查询验证原块内顺序，并要求反向冲突在被复制循环树的每个轴上均为同次迭代。
bool LoopDependenceInfo::isDistributionLegal(
    Loop *L, const InstructionSet &First, const InstructionSet &Second) {
    if (!L) return false;
    LoopSummary &Summary = getOrBuildSummary(L);
    if (Summary.UnknownMemory || Summary.ObservableEffect ||
        Summary.OrderingBarrier)
        return false;
    for (const LoopMemoryAccess &SecondAccess : Summary.Accesses) {
        if (!Second.count(SecondAccess.Inst)) continue;
        for (const LoopMemoryAccess &FirstAccess : Summary.Accesses) {
            if (!First.count(FirstAccess.Inst) ||
                !isWriteConflict(SecondAccess, FirstAccess)) {
                continue;
            }
            if (areNoAlias(SecondAccess, FirstAccess)) continue;
            if (!FirstAccess.Inst || !SecondAccess.Inst ||
                FirstAccess.Inst->getParent() !=
                    SecondAccess.Inst->getParent() ||
                FirstAccess.Order >= SecondAccess.Order) {
                return false;
            }
            for (Loop *Axis : Summary.Loops) {
                if (!areForcedEqual(SecondAccess, FirstAccess, Axis))
                    return false;
            }
        }
    }
    return true;
}
//判断两条指令之间的潜在冲突是否局限在同次循环迭代
bool LoopDependenceInfo::isConflictLocalToIteration(
    Instruction *A, Instruction *B, Loop *L) {
    if (!A || !B || !L) return false;
    LoopSummary &Summary = getOrBuildSummary(L);
    if (Summary.UnknownMemory || Summary.ObservableEffect ||
        Summary.OrderingBarrier)
        return false;
    std::vector<const LoopMemoryAccess *> FirstAccesses;
    std::vector<const LoopMemoryAccess *> SecondAccesses;
    for (const LoopMemoryAccess &Access : Summary.Accesses) {
        if (Access.Inst == A) FirstAccesses.push_back(&Access);
        if (Access.Inst == B) SecondAccesses.push_back(&Access);
    }
    if (FirstAccesses.empty() || SecondAccesses.empty()) return false;
    //一个调用可映射到多个实际对象；只有每一对潜在写冲突都被证明局限在
    //同次迭代，才能把整条指令对视为局部冲突。
    for (const LoopMemoryAccess *First : FirstAccesses) {
        for (const LoopMemoryAccess *Second : SecondAccesses) {
            if (!isWriteConflict(*First, *Second) ||
                areNoAlias(*First, *Second)) {
                continue;
            }
            if (!areForcedEqual(*First, *Second, L)) return false;
        }
    }
    return true;
}
//调度专用查询只处理外层不变读取与有限内层写，并由范围别名证明覆盖完整内层域。
bool LoopDependenceInfo::isInvariantAccessDisjointFromLoopWrite(
    Instruction *Invariant, Instruction *Write,
    Loop *Scope, Loop *VaryingLoop) {
    if (!Invariant || !Write || !Scope || !VaryingLoop || !SE || !LI ||
        !AA || VaryingLoop->getParentLoop() != Scope ||
        !Invariant->getParent() || !Write->getParent() ||
        !Scope->contains(Invariant->getParent()) ||
        VaryingLoop->contains(Invariant->getParent()) ||
        !VaryingLoop->contains(Write->getParent()) ||
        !SE->isKnownFiniteLoop(VaryingLoop)) {
        return false;
    }
    auto Induction = SE->getCanonicalInduction(VaryingLoop);
    if (!Induction || !Induction->Valid || !Induction->Phi ||
        !Induction->UpdateValue ||
        !SE->isKnownNoWrapInductionValue(VaryingLoop, Induction->Phi) ||
        !SE->isKnownNoWrapInductionValue(
            VaryingLoop, Induction->UpdateValue)) {
        return false;
    }
    LoopSummary &Summary = getOrBuildSummary(Scope);
    if (Summary.UnknownMemory || Summary.ObservableEffect ||
        Summary.OrderingBarrier)
        return false;
    std::vector<const LoopMemoryAccess *> FirstAccesses;
    std::vector<const LoopMemoryAccess *> SecondAccesses;
    for (const LoopMemoryAccess &Access : Summary.Accesses) {
        if (Access.Inst == Invariant) FirstAccesses.push_back(&Access);
        if (Access.Inst == Write) SecondAccesses.push_back(&Access);
    }
    if (FirstAccesses.empty() || SecondAccesses.empty()) {
        return false;
    }
    for (const LoopMemoryAccess *First : FirstAccesses) {
        if (!First->Reads || First->Writes) return false;
        for (const LoopAffineExpr &Subscript : First->Subscripts) {
            auto It = Subscript.LoopCoefficients.find(VaryingLoop);
            if (It != Subscript.LoopCoefficients.end() &&
                It->second != 0) {
                return false;
            }
        }
    }
    for (const LoopMemoryAccess *Second : SecondAccesses)
        if (!Second->Writes) return false;
    for (const LoopMemoryAccess *First : FirstAccesses) {
        for (const LoopMemoryAccess *Second : SecondAccesses) {
            if (AA->aliasWithIndexRange(
                    First->Location, Second->Location, *LI) !=
                AliasInfo::NoAlias) {
                return false;
            }
        }
    }
    return true;
}
//仿射化递归展开 SCEV；只接受可精确相加缩放的表达式，非线性、截断或溢出均返回非 Exact。
LoopAffineExpr LoopDependenceInfo::buildAffine(const SCEVExpr *E,
                                               Loop *Scope) const {
    LoopAffineExpr Result;
    if (!E || !Scope) return Result;
    switch (E->Kind) {
    case SCEVExprKind::Constant:
        Result.Exact = true;
        Result.Constant = E->ConstValue;
        return Result;
    case SCEVExprKind::Unknown: {
        Value *Raw = E->RawValue;
        if (!Raw) return Result;
        auto *I = dynamic_cast<Instruction *>(Raw);
        BasicBlock *BB = I ? I->getParent() : nullptr;
        if (BB && Scope->contains(BB)) return Result;
        Result.Exact = true;
        Result.InvariantTerms[E] = 1;
        return Result;
    }
    case SCEVExprKind::Add:
    case SCEVExprKind::Sub: {
        if (E->Operands.size() != 2) return Result;
        Result = buildAffine(E->Operands[0], Scope);
        LoopAffineExpr RHS = buildAffine(E->Operands[1], Scope);
        if (!addScaledAffine(Result, RHS,
                             E->Kind == SCEVExprKind::Add ? 1 : -1)) {
            Result.Exact = false;
        }
        return Result;
    }
    case SCEVExprKind::Mul: {
        if (E->Operands.size() != 2) return Result;
        const SCEVExpr *Constant = nullptr;
        const SCEVExpr *Other = nullptr;
        if (E->Operands[0] &&
            E->Operands[0]->Kind == SCEVExprKind::Constant) {
            Constant = E->Operands[0];
            Other = E->Operands[1];
        } else if (E->Operands[1] &&
                   E->Operands[1]->Kind == SCEVExprKind::Constant) {
            Constant = E->Operands[1];
            Other = E->Operands[0];
        }
        if (!Constant || !Other) return Result;
        Result.Exact = true;
        LoopAffineExpr Operand = buildAffine(Other, Scope);
        if (!addScaledAffine(Result, Operand, Constant->ConstValue))
            Result.Exact = false;
        return Result;
    }
    case SCEVExprKind::Cast:
        if (E->CastKind == SCEVCastKind::Trunc || E->Operands.size() != 1)
            return Result;
        return buildAffine(E->Operands[0], Scope);
    case SCEVExprKind::AddRec: {
        if (E->Operands.size() != 2 || !E->RecLoop) return Result;
        if (!loopIsInScope(E->RecLoop, Scope)) {
            Result.Exact = true;
            Result.InvariantTerms[E] = 1;
            return Result;
        }
        Result = buildAffine(E->Operands[0], Scope);
        LoopAffineExpr Step = buildAffine(E->Operands[1], Scope);
        if (!Result.Exact || !Step.Exact ||
            !Step.LoopCoefficients.empty() ||
            !Step.InvariantTerms.empty()) {
            Result.Exact = false;
            return Result;
        }
        int64_t Updated = 0;
        if (!checkedAdd(Result.LoopCoefficients[E->RecLoop], Step.Constant,
                        Updated)) {
            Result.Exact = false;
            return Result;
        }
        Result.LoopCoefficients[E->RecLoop] = Updated;
        eraseZeroTerms(Result);
        return Result;
    }
    case SCEVExprKind::Div:
    case SCEVExprKind::Mod:
    case SCEVExprKind::Shl:
    case SCEVExprKind::LShr:
    case SCEVExprKind::AShr:
    case SCEVExprKind::BitAnd:
    case SCEVExprKind::BitOr:
    case SCEVExprKind::BitXor:
    case SCEVExprKind::Binomial:
    case SCEVExprKind::Min:
    case SCEVExprKind::Max:
    case SCEVExprKind::Select:
    case SCEVExprKind::Compare:
    case SCEVExprKind::MulRec:
    case SCEVExprKind::DivRec:
        return Result;
    }
    return Result;
}
//指针访问建模沿类型化 GEP 链收集同字节步长的下标带，并验证根对象在循环内稳定。
bool LoopDependenceInfo::buildPointerAccess(
    Instruction *I, Value *Pointer, const MemoryLocation &Location,
    bool Reads, bool Writes, Loop *Scope, std::size_t Order,
    LoopMemoryAccess &Access) const {
    if (!I || !Pointer || !Scope || !SE || (!Reads && !Writes))
        return false;
    Access.Inst = I;
    Access.Pointer = Pointer;
    Access.Location = Location;
    Access.Reads = Reads;
    Access.Writes = Writes;
    Access.Order = Order;
    //1.逆向穿过 GEP/BitCast，把相同步长的分段 GEP 下标合并为一个坐标带。
    std::map<int64_t, LoopAffineExpr, std::greater<int64_t>> Bands;
    Value *Current = Pointer;
    std::unordered_set<Value *> Visited;
    unsigned Depth = 0;
    unsigned IndexTerms = 0;
    bool Exact = true;
    while (Current) {
        if (Depth++ >= MaxPointerChainDepth ||
            !Visited.insert(Current).second) {
            Exact = false;
            break;
        }
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(Current)) {
            Value *Base = nullptr;
            std::vector<Value *> Indices;
            if (!GEP->getBaseAndIndices(Base, Indices)) {
                Exact = false;
                break;
            }
            std::vector<int64_t> Strides;
            int64_t FinalPointeeStride = 0;
            if (!GetElementPtrInst::getIndexStrides(
                    Base, Indices, Strides, FinalPointeeStride) ||
                Strides.size() != Indices.size()) {
                Exact = false;
                break;
            }
            (void)FinalPointeeStride;
            for (std::size_t Index = 0; Index < Indices.size(); ++Index) {
                if (++IndexTerms > MaxGEPIndexTerms || Strides[Index] <= 0) {
                    Exact = false;
                    break;
                }
                LoopAffineExpr Component =
                    buildAffine(SE->getSCEV(Indices[Index]), Scope);
                auto It = Bands.find(Strides[Index]);
                if (It == Bands.end()) {
                    LoopAffineExpr Zero;
                    Zero.Exact = true;
                    It = Bands.emplace(Strides[Index], std::move(Zero)).first;
                }
                if (!addScaledAffine(It->second, Component, 1)) {
                    Exact = false;
                    break;
                }
            }
            if (!Exact) break;
            Current = Base;
            continue;
        }
        if (auto *BitCast = dynamic_cast<BitCastInst *>(Current)) {
            Current = BitCast->getUnaryOperand();
            continue;
        }
        break;
    }
    if (!Current) Exact = false;
    Access.Root = Current;
    if (auto *RootI = dynamic_cast<Instruction *>(Current)) {
        BasicBlock *RootBB = RootI->getParent();
        if (RootBB && Scope->contains(RootBB)) Exact = false;
    }
    //2.发布根、下标和步长；环、深度预算、未知类型或循环内动态根都会使地址不精确。
    Access.Subscripts.reserve(Bands.size());
    Access.ByteStrides.reserve(Bands.size());
    for (auto &Band : Bands) {
        Access.ByteStrides.push_back(Band.first);
        Access.Subscripts.push_back(std::move(Band.second));
        if (!Access.Subscripts.back().Exact) Exact = false;
    }
    Access.AddressExact = Exact && Access.Root;
    return Access.AddressExact;
}
//标量 Load/Store 只负责提取真实指针、访问宽度和读写角色，地址求解与调用复用同一路径。
bool LoopDependenceInfo::buildMemoryAccess(Instruction *I, Loop *Scope,
                                           std::size_t Order,
                                           LoopMemoryAccess &Access) const {
    if (!I || !Scope) return false;
    if (auto *Load = dynamic_cast<LoadInst *>(I)) {
        Value *Pointer = Load->getPointerOperand();
        return buildPointerAccess(
            I, Pointer, MemoryLocation::get(Load), true, false,
            Scope, Order, Access);
    }
    if (auto *Store = dynamic_cast<StoreInst *>(I)) {
        Value *Pointer = Store->getPointerOperand();
        return buildPointerAccess(
            I, Pointer, MemoryLocation::get(Store), false, true,
            Scope, Order, Access);
    }
    return false;
}
//判断两次访存之间是否存在至少一侧写入的潜在冲突
bool LoopDependenceInfo::isWriteConflict(const LoopMemoryAccess &A,
                                         const LoopMemoryAccess &B) {
    return (A.Writes || B.Writes) &&
           (A.Reads || A.Writes) && (B.Reads || B.Writes);
}
//判断两次访存的完整内存区间是否确定互不别名
bool LoopDependenceInfo::areNoAlias(const LoopMemoryAccess &A,
                                    const LoopMemoryAccess &B) const {
    if (!AA || !LI || !A.Pointer || !B.Pointer) return false;
    if (!A.Root || !B.Root || A.Root == B.Root) return false;
    return AA->alias(A.Location, B.Location, *LI) == AliasInfo::NoAlias;
}
//轴相等证明要求同根、同布局和不回绕归纳轴，并找到唯一的单位系数坐标带。
bool LoopDependenceInfo::areForcedEqual(const LoopMemoryAccess &A,
                                        const LoopMemoryAccess &B,
                                        Loop *Axis) const {
    if (!Axis || !SE || !A.AddressExact || !B.AddressExact || !A.Root ||
        A.Root != B.Root || !A.Location.Size || !B.Location.Size ||
        *A.Location.Size == 0 || *A.Location.Size != *B.Location.Size ||
        A.ByteStrides.empty() || A.ByteStrides != B.ByteStrides ||
        A.Subscripts.size() != A.ByteStrides.size() ||
        B.Subscripts.size() != B.ByteStrides.size()) {
        return false;
    }
    auto Induction = SE->getCanonicalInduction(Axis);
    if (!Induction || !Induction->Valid || !Induction->Phi ||
        !Induction->UpdateValue ||
        !SE->isKnownNoWrapInductionValue(Axis, Induction->Phi) ||
        !SE->isKnownNoWrapInductionValue(
            Axis, Induction->UpdateValue)) {
        return false;
    }
    if (*A.Location.Size >
        static_cast<std::size_t>(A.ByteStrides.back())) {
        return false;
    }
    for (std::size_t I = 1; I < A.ByteStrides.size(); ++I) {
        const int64_t OuterStride = A.ByteStrides[I - 1];
        const int64_t InnerStride = A.ByteStrides[I];
        if (OuterStride <= InnerStride || InnerStride <= 0 ||
            OuterStride % InnerStride != 0) {
            return false;
        }
    }
    for (std::size_t AI = 0; AI < A.ByteStrides.size(); ++AI) {
        const LoopAffineExpr &AExpr = A.Subscripts[AI];
        const LoopAffineExpr &BExpr = B.Subscripts[AI];
        int64_t ACoefficient = 0;
        int64_t BCoefficient = 0;
        if (!isPureAxisCoordinate(AExpr, Axis, ACoefficient) ||
            !isPureAxisCoordinate(BExpr, Axis, BCoefficient)) {
            continue;
        }
        if (ACoefficient != BCoefficient ||
            (ACoefficient != 1 && ACoefficient != -1) ||
            AExpr.Constant != BExpr.Constant) {
            continue;
        }
        bool IsolatedAxis = true;
        for (std::size_t Other = 0; Other < A.Subscripts.size(); ++Other) {
            if (Other == AI) continue;
            auto ACoeff = A.Subscripts[Other].LoopCoefficients.find(Axis);
            auto BCoeff = B.Subscripts[Other].LoopCoefficients.find(Axis);
            if ((ACoeff != A.Subscripts[Other].LoopCoefficients.end() &&
                 ACoeff->second != 0) ||
                (BCoeff != B.Subscripts[Other].LoopCoefficients.end() &&
                 BCoeff->second != 0)) {
                IsolatedAxis = false;
                break;
            }
        }
        if (IsolatedAxis) return true;
    }
    return false;
}
//循环依赖主流程：收集可建模访存，枚举潜在写冲突，再生成逐轴方向向量。
std::unique_ptr<LoopDependenceInfo::LoopSummary>
LoopDependenceInfo::buildSummary(Loop *L) {
    //1.验证依赖分析齐备，并按外到内顺序收集当前循环树的轴。
    auto Summary = std::make_unique<LoopSummary>();
    if (!L || !Func || !LI || !SE || !AA || !FAM) {
        Summary->UnknownMemory = true;
        return Summary;
    }
    collectLoopsPreorder(L, Summary->Loops);
    //2.按函数稳定顺序扫描循环体；调用先映射为实际对象访问，只有未知
    //程序内存才设置 UnknownMemory，可观察调用另外记录顺序屏障。
    std::size_t Order = 0;
    for (BasicBlock *BB : L->getBlocksInFunctionOrder()) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            const std::size_t ThisOrder = Order++;
            if (auto *Call = dynamic_cast<CallInst *>(I)) {
                CallEffectSummary Effects =
                    getCallEffectSummary(Call, *FAM);
                Summary->UnknownMemory =
                    Summary->UnknownMemory || Effects.HasUnknownMemory ||
                    Effects.MayCrossObjectBounds;
                Summary->ObservableEffect =
                    Summary->ObservableEffect ||
                    Effects.HasObservableEffect;
                Summary->OrderingBarrier =
                    Summary->OrderingBarrier ||
                    Effects.HasOrderingBarrier;
                for (const CallMemoryEffect &Effect :
                     Effects.ProgramMemory) {
                    if (isNoModRef(Effect.ModRef)) continue;
                    if (Summary->Accesses.size() >=
                        MaxMemoryAccesses) {
                        Summary->UnknownMemory = true;
                        break;
                    }
                    LoopMemoryAccess Access;
                    const bool Reads = isRefSet(Effect.ModRef);
                    const bool Writes = isModSet(Effect.ModRef);
                    if (!buildPointerAccess(
                            Call, Effect.Location.Pointer,
                            Effect.Location, Reads, Writes, L,
                            ThisOrder, Access)) {
                        Summary->UnknownMemory = true;
                        continue;
                    }
                    Summary->Accesses.push_back(std::move(Access));
                }
                continue;
            }
            if (!dynamic_cast<LoadInst *>(I) &&
                !dynamic_cast<StoreInst *>(I)) {
                continue;
            }
            if (Summary->Accesses.size() >= MaxMemoryAccesses) {
                Summary->UnknownMemory = true;
                continue;
            }
            LoopMemoryAccess Access;
            if (!buildMemoryAccess(I, L, ThisOrder, Access)) {
                Summary->UnknownMemory = true;
                continue;
            }
            Summary->Accesses.push_back(std::move(Access));
        }
    }
    //3.枚举访问对：先排除无写冲突和不同对象，再分类依赖类型与每个轴的方向。
    const std::size_t Count = Summary->Accesses.size();
    std::size_t ExaminedPairs = 0;
    for (std::size_t I = 0; I < Count; ++I) {
        for (std::size_t J = I; J < Count; ++J) {
            LoopMemoryAccess &Source = Summary->Accesses[I];
            LoopMemoryAccess &Sink = Summary->Accesses[J];
            if (!isWriteConflict(Source, Sink)) continue;
            if (++ExaminedPairs > MaxDependencePairs) {
                Summary->UnknownMemory = true;
                break;
            }
            if (areNoAlias(Source, Sink)) continue;
            LoopDependenceRecord Record;
            Record.Source = &Source;
            Record.Sink = &Sink;
            if (Source.Writes && Sink.Writes)
                Record.Kind = LoopDependenceKind::Output;
            else if (Source.Writes && Sink.Reads)
                Record.Kind = LoopDependenceKind::Flow;
            else if (Source.Reads && Sink.Writes)
                Record.Kind = LoopDependenceKind::Anti;
            else
                Record.Kind = LoopDependenceKind::Unknown;
            Record.Loops = Summary->Loops;
            Record.DirectionMasks.reserve(Record.Loops.size());
            Record.Exact = Source.AddressExact && Sink.AddressExact;
            for (Loop *Axis : Record.Loops) {
                const bool Equal = areForcedEqual(Source, Sink, Axis);
                Record.DirectionMasks.push_back(
                    Equal ? static_cast<uint8_t>(LoopDepSameIteration)
                          : static_cast<uint8_t>(LoopDepAnyDirection));
                Record.Exact = Record.Exact && Equal;
            }
            Summary->Dependences.push_back(std::move(Record));
        }
        if (ExaminedPairs > MaxDependencePairs) break;
    }
    return Summary;
}
//Pass 入口依次取得循环、标量演化、别名和调用效果结果，再返回按需构建的依赖查询器。
LoopDependenceInfo LoopDependenceAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const {
    //1.构造期显式请求 SideEffect，确保分析管理器记录依赖和跨函数失效关系。
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    (void)FAM.getResult<SideEffectAnalysis>(F);
    //2.返回函数级惰性查询器，每个 LoopSummary 在首次请求时构造并缓存。
    return LoopDependenceInfo(&F, &LI, &SE, &AA, &FAM);
}
