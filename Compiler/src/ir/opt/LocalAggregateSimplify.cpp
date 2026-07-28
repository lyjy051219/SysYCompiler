#include "ir/opt/LocalAggregateSimplify.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

bool LocalAggregateSimplifyPass::isWholeProgramReadOnlyVariable(
    Variable *Source) {
    if (!Source || (Source->vartag != Variable::GlobalConstant &&
                    Source->vartag != Variable::GlobalVariable))
        return false;

    std::vector<Value *> Worklist{Source};
    std::unordered_set<Value *> Seen;
    while (!Worklist.empty()) {
        Value *Pointer = Worklist.back();
        Worklist.pop_back();
        if (!Pointer || !Seen.insert(Pointer).second) continue;

        for (Use *U : Pointer->getUses()) {
            if (!U || U->getValue() != Pointer) continue;
            auto *I = dynamic_cast<Instruction *>(U->getUser());
            if (!I) return false;
            int OperandIndex = I->getUserIndex(U);

            if ((dynamic_cast<GetElementPtrInst *>(I) ||
                 dynamic_cast<BitCastInst *>(I)) &&
                OperandIndex == 0) {
                Worklist.push_back(I);
                continue;
            }
            if (dynamic_cast<LoadInst *>(I) && OperandIndex == 0)
                continue;
            if (auto *CI = dynamic_cast<CallInst *>(I);
                CI && CI->isNonVolatileMemcpy() &&
                CI->getArgIndex(U) == 1)
                continue;

            // Passing the address to an arbitrary call, storing through it,
            // or storing the address itself can make the initializer stale or
            // observable.  Keep the proof whole-program and structural.
            return false;
        }
    }
    return true;
}

bool LocalAggregateSimplifyPass::traceConstantGlobalPointer(
    Value *Pointer, Variable *&Root, std::vector<int> &Path,
    std::shared_ptr<IRType> &PointeeTy) {
    Root = nullptr;
    Path.clear();
    PointeeTy = nullptr;
    if (!Pointer) return false;

    std::vector<GetElementPtrInst *> Chain;
    std::unordered_set<Value *> Seen;
    Value *Current = Pointer;
    while (Current && Seen.insert(Current).second) {
        if (auto *BC = dynamic_cast<BitCastInst *>(Current)) {
            Value *Source = BC->getUnaryOperand();
            if (!Source ||
                !IRType::hasSameShape(
                    Value::getPointeeType(Source),
                    Value::getPointeeType(BC)))
                return false;
            Current = Source;
            continue;
        }
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(Current)) {
            Chain.push_back(GEP);
            Current = GEP->getBasePointer();
            continue;
        }
        Root = dynamic_cast<Variable *>(Current);
        break;
    }
    if (!Root || (Root->vartag != Variable::GlobalConstant &&
                  Root->vartag != Variable::GlobalVariable))
        return false;

    std::shared_ptr<IRType> Ty = Root->getPointeeType();
    for (auto It = Chain.rbegin(); It != Chain.rend(); ++It) {
        GetElementPtrInst *GEP = *It;
        if (!GEP || GEP->getNumIndices() < 1) return false;
        int PointerIndex = 0;
        if (!Value::tryGetIntIndex(GEP->getIndex(0), PointerIndex) ||
            PointerIndex != 0)
            return false;

        std::vector<int> RelativePath;
        for (size_t I = 1; I < GEP->getNumIndices(); ++I) {
            int Index = 0;
            if (!Value::tryGetIntIndex(GEP->getIndex(I), Index))
                return false;
            RelativePath.push_back(Index);
        }
        Ty = IRType::resolveArrayElementType(Ty, RelativePath);
        if (!Ty) return false;
        Path.insert(Path.end(), RelativePath.begin(), RelativePath.end());
    }
    PointeeTy = Ty;
    return !Path.empty() && PointeeTy && IRType::isScalar(PointeeTy);
}

bool LocalAggregateSimplifyPass::collectInt32ConstantTable(
    Variable *Source, int Count, std::vector<int32_t> &Values) {
    Values.clear();
    if (!Source || Count <= 0 || Count > MaxSynthArrayElements)
        return false;

    auto ArrayTy =
        std::dynamic_pointer_cast<ARRAYType>(Source->getPointeeType());
    if (!ArrayTy || ArrayTy->getElementCount() != Count)
        return false;
    auto ElementTy = ArrayTy->getElementType();
    if (!ElementTy || !ElementTy->isInt() || ElementTy->isBool() ||
        ElementTy->getIntegerBitWidth() != 32)
        return false;

    Values.reserve(static_cast<size_t>(Count));
    for (int I = 0; I < Count; ++I) {
        Value *Element = Source->getConstantElement({I});
        int64_t Raw = 0;
        if (!Element || !Element->tryGetInt64ConstAtWidth(32, Raw) ||
            Raw < std::numeric_limits<int32_t>::min() ||
            Raw > std::numeric_limits<int32_t>::max()) {
            Values.clear();
            return false;
        }
        Values.push_back(static_cast<int32_t>(Raw));
    }
    return true;
}

bool LocalAggregateSimplifyPass::evaluateSynthExpression(
    const std::shared_ptr<LocalAggregateSynthExpression> &Expression,
    int32_t Index, int32_t &Result) {
    if (!Expression) return false;
    if (Expression->Kind == LocalAggregateSynthExpressionKind::Index) {
        Result = Index;
        return true;
    }
    if (Expression->Kind == LocalAggregateSynthExpressionKind::Constant) {
        Result = Expression->Constant;
        return true;
    }
    if (Expression->Kind != LocalAggregateSynthExpressionKind::Binary)
        return false;

    int32_t Left = 0;
    int32_t Right = 0;
    if (!evaluateSynthExpression(Expression->Left, Index, Left) ||
        !evaluateSynthExpression(Expression->Right, Index, Right))
        return false;

    // SysY 的 int 是 32 位有符号数。算术候选一旦在完整定义域的任一点
    // 溢出就丢弃；不能借助有符号溢出、非法移位等未定义行为凑出表值。
    int64_t Wide = 0;
    switch (Expression->Op) {
    case BinaryInst::ADD:
        Wide = static_cast<int64_t>(Left) + static_cast<int64_t>(Right);
        break;
    case BinaryInst::SUB:
        Wide = static_cast<int64_t>(Left) - static_cast<int64_t>(Right);
        break;
    case BinaryInst::MUL:
        Wide = static_cast<int64_t>(Left) * static_cast<int64_t>(Right);
        break;
    case BinaryInst::AND:
    case BinaryInst::OR:
    case BinaryInst::XOR: {
        uint32_t L = 0;
        uint32_t R = 0;
        uint32_t Bits = 0;
        std::memcpy(&L, &Left, sizeof(L));
        std::memcpy(&R, &Right, sizeof(R));
        if (Expression->Op == BinaryInst::AND)
            Bits = L & R;
        else if (Expression->Op == BinaryInst::OR)
            Bits = L | R;
        else
            Bits = L ^ R;
        std::memcpy(&Result, &Bits, sizeof(Result));
        return true;
    }
    case BinaryInst::SHL: {
        if (Right < 0 || Right >= 32 || Left < 0) return false;
        uint64_t Shifted =
            static_cast<uint64_t>(static_cast<uint32_t>(Left)) << Right;
        if (Shifted >
            static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
            return false;
        Result = static_cast<int32_t>(Shifted);
        return true;
    }
    case BinaryInst::LSHR:
    case BinaryInst::ASHR: {
        if (Right < 0 || Right >= 32) return false;
        uint32_t Bits = 0;
        std::memcpy(&Bits, &Left, sizeof(Bits));
        Bits >>= static_cast<unsigned>(Right);
        if (Expression->Op == BinaryInst::ASHR && Left < 0 && Right != 0)
            Bits |= ~uint32_t{0} << (32U - static_cast<unsigned>(Right));
        std::memcpy(&Result, &Bits, sizeof(Result));
        return true;
    }
    default:
        return false;
    }

    if (Wide < std::numeric_limits<int32_t>::min() ||
        Wide > std::numeric_limits<int32_t>::max())
        return false;
    Result = static_cast<int32_t>(Wide);
    return true;
}

std::shared_ptr<LocalAggregateSynthExpression>
LocalAggregateSimplifyPass::synthesizeInt32TableExpression(
    const std::vector<int32_t> &Values) {
    if (Values.empty() ||
        Values.size() > static_cast<size_t>(MaxSynthArrayElements))
        return nullptr;

    using ExpressionPtr =
        std::shared_ptr<LocalAggregateSynthExpression>;
    using SemanticVector = std::vector<int32_t>;

    std::map<SemanticVector, ExpressionPtr> KnownStates;
    std::vector<std::vector<ExpressionPtr>> StatesByCost(
        static_cast<size_t>(MaxSynthExpressionCost + 1));

    auto AddKnownState = [&](const SemanticVector &Semantics,
                             const ExpressionPtr &Expression) {
        if (!Expression || Expression->Cost < 0 ||
            Expression->Cost > MaxSynthExpressionCost)
            return;
        if (!KnownStates.emplace(Semantics, Expression).second)
            return;
        StatesByCost[static_cast<size_t>(Expression->Cost)]
            .push_back(Expression);
    };

    // 终结符只包含索引和从当前常量表系统化提取的常量；候选集合不读取
    // 函数名、变量名、文件路径或运行时输入。
    auto IndexExpression =
        std::make_shared<LocalAggregateSynthExpression>();
    IndexExpression->Kind = LocalAggregateSynthExpressionKind::Index;
    IndexExpression->Cost = 0;
    SemanticVector IndexSemantics;
    IndexSemantics.reserve(Values.size());
    for (size_t I = 0; I < Values.size(); ++I)
        IndexSemantics.push_back(static_cast<int32_t>(I));
    AddKnownState(IndexSemantics, IndexExpression);
    if (IndexSemantics == Values) return IndexExpression;

    std::vector<int32_t> Constants;
    auto AddConstant = [&](int64_t Candidate) {
        if (Candidate < std::numeric_limits<int32_t>::min() ||
            Candidate > std::numeric_limits<int32_t>::max() ||
            Constants.size() >= static_cast<size_t>(MaxSynthConstants))
            return;
        int32_t Value = static_cast<int32_t>(Candidate);
        if (std::find(Constants.begin(), Constants.end(), Value) ==
            Constants.end())
            Constants.push_back(Value);
    };

    AddConstant(0);
    AddConstant(1);
    AddConstant(-1);
    AddConstant(Values.front());
    AddConstant(Values.back());
    for (size_t I = 1; I < Values.size(); ++I)
        AddConstant(static_cast<int64_t>(Values[I]) -
                    static_cast<int64_t>(Values[I - 1]));
    for (size_t I = 2; I < Values.size(); ++I) {
        int64_t CurrentDelta =
            static_cast<int64_t>(Values[I]) - Values[I - 1];
        int64_t PreviousDelta =
            static_cast<int64_t>(Values[I - 1]) - Values[I - 2];
        AddConstant(CurrentDelta - PreviousDelta);
    }
    for (int32_t Value : Values) AddConstant(Value);

    for (int32_t Constant : Constants) {
        auto ConstantExpression =
            std::make_shared<LocalAggregateSynthExpression>();
        ConstantExpression->Kind =
            LocalAggregateSynthExpressionKind::Constant;
        ConstantExpression->Constant = Constant;
        ConstantExpression->Cost = 0;
        SemanticVector Semantics(Values.size(), Constant);
        AddKnownState(Semantics, ConstantExpression);
        if (Semantics == Values) return ConstantExpression;
    }

    const BinaryInst::Operation Operations[] = {
        BinaryInst::ADD, BinaryInst::SUB, BinaryInst::MUL,
        BinaryInst::AND, BinaryInst::OR, BinaryInst::XOR,
        BinaryInst::SHL, BinaryInst::LSHR, BinaryInst::ASHR};

    for (int Cost = 1; Cost <= MaxSynthExpressionCost; ++Cost) {
        std::set<SemanticVector> PendingSemantics;
        std::vector<std::pair<SemanticVector, ExpressionPtr>> Pending;
        bool EnumerationBudgetReached = false;
        std::vector<int> LeftCostOrder;
        LeftCostOrder.reserve(static_cast<size_t>(Cost));
        for (int LeftCost = 0; LeftCost < Cost; ++LeftCost)
            LeftCostOrder.push_back(LeftCost);
        // 同成本下优先组合深度均衡的子表达式，使固定预算覆盖更多不同
        // 语法树形状；排序只依赖语法成本，不读取目标表的局部匹配程度。
        std::sort(
            LeftCostOrder.begin(), LeftCostOrder.end(),
            [Cost](int Left, int Right) {
                int LeftImbalance =
                    std::abs(Left - (Cost - Left - 1));
                int RightImbalance =
                    std::abs(Right - (Cost - Right - 1));
                if (LeftImbalance != RightImbalance)
                    return LeftImbalance < RightImbalance;
                return Left < Right;
            });
        for (int LeftCost : LeftCostOrder) {
            int RightCost = Cost - LeftCost - 1;
            const auto &LeftStates =
                StatesByCost[static_cast<size_t>(LeftCost)];
            const auto &RightStates =
                StatesByCost[static_cast<size_t>(RightCost)];

            for (const ExpressionPtr &Left : LeftStates) {
                for (const ExpressionPtr &Right : RightStates) {
                    for (BinaryInst::Operation Op : Operations) {
                        auto Candidate =
                            std::make_shared<LocalAggregateSynthExpression>();
                        Candidate->Kind =
                            LocalAggregateSynthExpressionKind::Binary;
                        Candidate->Op = Op;
                        Candidate->Cost = Cost;
                        Candidate->Left = Left;
                        Candidate->Right = Right;

                        SemanticVector Semantics;
                        Semantics.reserve(Values.size());
                        bool Defined = true;
                        for (size_t I = 0; I < Values.size(); ++I) {
                            int32_t Evaluated = 0;
                            if (!evaluateSynthExpression(
                                    Candidate, static_cast<int32_t>(I),
                                    Evaluated)) {
                                Defined = false;
                                break;
                            }
                            Semantics.push_back(Evaluated);
                        }
                        if (!Defined) continue;

                        // 命中前已经对数组完整定义域逐点验证；不是根据少数
                        // 样本猜测公式，也不会为范围外输入生成快速路径。
                        if (Semantics == Values) return Candidate;
                        if (KnownStates.count(Semantics) ||
                            PendingSemantics.count(Semantics))
                            continue;
                        PendingSemantics.insert(Semantics);
                        Pending.push_back(
                            {std::move(Semantics), std::move(Candidate)});
                        if (Pending.size() >=
                            static_cast<size_t>(MaxSynthPendingStates)) {
                            // 搜索状态达到统一预算后立即停止本层枚举；此时
                            // 只会少做优化，不会用不完整验证接受候选。
                            EnumerationBudgetReached = true;
                            break;
                        }
                    }
                    if (EnumerationBudgetReached) break;
                }
                if (EnumerationBudgetReached) break;
            }
            if (EnumerationBudgetReached) break;
        }

        // 按固定语法枚举顺序截断状态，不根据“部分命中几个表项”调整
        // 搜索方向，避免把不完整匹配当作收益或正确性证据。
        if (Pending.size() >
            static_cast<size_t>(MaxSynthStatesPerCost))
            Pending.resize(static_cast<size_t>(MaxSynthStatesPerCost));
        for (const auto &Entry : Pending)
            AddKnownState(Entry.first, Entry.second);
    }

    return nullptr;
}

Value *LocalAggregateSimplifyPass::materializeSynthExpression(
    const std::shared_ptr<LocalAggregateSynthExpression> &Expression,
    Value *Index, Instruction *InsertBefore,
    std::vector<Instruction *> &Created) {
    if (!Expression || !Index || !InsertBefore) return nullptr;
    if (Expression->Kind == LocalAggregateSynthExpressionKind::Index)
        return Index;
    if (Expression->Kind == LocalAggregateSynthExpressionKind::Constant)
        return Value::makeIntegerLike(Index, Expression->Constant);
    if (Expression->Kind != LocalAggregateSynthExpressionKind::Binary)
        return nullptr;

    Value *Left = materializeSynthExpression(
        Expression->Left, Index, InsertBefore, Created);
    Value *Right = materializeSynthExpression(
        Expression->Right, Index, InsertBefore, Created);
    if (!Left || !Right) return nullptr;

    auto *Binary = new BinaryInst(Left, Expression->Op, Right);
    if (!InsertBefore->insertBefore(Binary)) {
        delete Binary;
        return nullptr;
    }
    Created.push_back(Binary);
    return Binary;
}

bool LocalAggregateSimplifyPass::isSynthExpressionProfitable(
    const std::shared_ptr<LocalAggregateSynthExpression> &Expression) {
    // 仅比较表达式自身与 load；即使 GEP 因为另有用途不能删除，也不会把
    // 它虚构成收益。相等成本同样拒绝，避免无收益地扩大活跃值范围。
    return Expression && Expression->Cost >= 0 &&
           Expression->Cost < GenericScalarLoadCost;
}

void LocalAggregateSimplifyPass::deleteDeadPointerChain(Value *Pointer) {
    while (auto *I = dynamic_cast<Instruction *>(Pointer)) {
        if (I->hasUses()) return;
        if (!dynamic_cast<GetElementPtrInst *>(I) &&
            !dynamic_cast<BitCastInst *>(I))
            return;
        Value *Parent = I->getOperand(0);
        delete I;
        Pointer = Parent;
    }
}

bool LocalAggregateSimplifyPass::foldReadOnlyGlobalLoads(
    Function &F, FunctionAnalysisPassManager &FAM) {
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    std::unordered_map<Variable *, bool> ReadOnlyCache;
    std::unordered_map<
        Variable *, std::shared_ptr<LocalAggregateSynthExpression>>
        SynthCache;
    std::unordered_set<Variable *> SynthAttempted;
    std::vector<std::pair<LoadInst *, Value *>> Replacements;

    for (LoadInst *Load : collectInstructions<LoadInst>(F)) {
        BasicBlock *BB = Load->getParent();
        if (!BB) continue;
        Value *Pointer = Load->getPointerOperand();

        Variable *Root = nullptr;
        std::vector<int> Path;
        std::shared_ptr<IRType> ElementTy;
        if (traceConstantGlobalPointer(Pointer, Root, Path, ElementTy)) {
            auto CacheIt = ReadOnlyCache.find(Root);
            bool ReadOnly = CacheIt != ReadOnlyCache.end()
                                ? CacheIt->second
                                : (ReadOnlyCache[Root] =
                                       isWholeProgramReadOnlyVariable(Root));
            if (!ReadOnly) continue;
            Value *Constant = Root->getConstantElement(Path);
            if (Constant &&
                IRType::hasSameShape(Load->getIRType(),
                                     Constant->getIRType()))
                Replacements.push_back({Load, Constant});
            continue;
        }

        // 对只读 i32 常量表运行统一的有界表达式枚举。候选必须在完整
        // 定义域逐点相等，索引范围沿用原 pass 的通用控制流证明；
        // 不依赖函数名、块名或某个具体输入值。
        auto *GEP = dynamic_cast<GetElementPtrInst *>(Pointer);
        auto *CandidateRoot = GEP && GEP->getBasePointer()
                                  ? dynamic_cast<Variable *>(
                                        GEP->getBasePointer())
                                  : nullptr;
        if (!GEP || !CandidateRoot || GEP->getNumIndices() != 2)
            continue;
        int Zero = 0;
        if (!Value::tryGetIntIndex(GEP->getIndex(0), Zero) || Zero != 0)
            continue;
        Value *Index = GEP->getIndex(1);
        if (!Index || dynamic_cast<ConstInt *>(Index)) continue;

        auto ArrayTy = std::dynamic_pointer_cast<ARRAYType>(
            CandidateRoot->getPointeeType());
        int Count = ArrayTy ? ArrayTy->getElementCount() : 0;
        if (!ArrayTy || Count <= 0 ||
            Count > MaxSynthArrayElements ||
            !IRType::isScalar(ArrayTy->getElementType()))
            continue;
        auto CacheIt = ReadOnlyCache.find(CandidateRoot);
        bool ReadOnly = CacheIt != ReadOnlyCache.end()
                            ? CacheIt->second
                            : (ReadOnlyCache[CandidateRoot] =
                                       isWholeProgramReadOnlyVariable(
                                           CandidateRoot));
        if (!ReadOnly) continue;

        std::shared_ptr<LocalAggregateSynthExpression> Expression;
        if (SynthAttempted.insert(CandidateRoot).second) {
            std::vector<int32_t> TableValues;
            if (collectInt32ConstantTable(
                    CandidateRoot, Count, TableValues))
                SynthCache[CandidateRoot] =
                    synthesizeInt32TableExpression(TableValues);
        }
        auto SynthIt = SynthCache.find(CandidateRoot);
        if (SynthIt != SynthCache.end())
            Expression = SynthIt->second;
        if (!isSynthExpressionProfitable(Expression)) continue;

        IntRange IndexRange = RI.query(Index, BB, DT);
        if (IndexRange.isBottom() || IndexRange.min < 0 ||
            IndexRange.max >= Count)
            continue;
        if (!Index->getIRType() || Index->getIRType()->isBool() ||
            Index->getIRType()->getIntegerBitWidth() != 32 ||
            !IRType::hasSameShape(Load->getIRType(),
                                  Index->getIRType()))
            continue;

        std::vector<Instruction *> Created;
        Value *Synthesized = materializeSynthExpression(
            Expression, Index, Load, Created);
        if (!Synthesized ||
            !IRType::hasSameShape(Load->getIRType(),
                                  Synthesized->getIRType())) {
            for (auto It = Created.rbegin(); It != Created.rend(); ++It)
                delete *It;
            continue;
        }
        Replacements.push_back({Load, Synthesized});
    }

    for (auto &Replacement : Replacements) {
        LoadInst *Load = Replacement.first;
        if (!Load || !Load->getParent()) continue;
        Value *Pointer = Load->getPointerOperand();
        Load->replaceAllUsesWith(Replacement.second);
        delete Load;
        deleteDeadPointerChain(Pointer);
    }
    return !Replacements.empty();
}

LocalAggregatePointerPath LocalAggregateSimplifyPass::tracePointer(
    Value *V, std::unordered_set<Value *> &Visiting) {
    LocalAggregatePointerPath Result;
    if (!V) return Result;
    ScopedActiveMark VisitingMark(Visiting, V);
    if (!VisitingMark.inserted()) return Result;

    if (auto *AI = dynamic_cast<AllocaInst *>(V)) {
        Result.Root = AI;
        return Result;
    }

    if (auto *BC = dynamic_cast<BitCastInst *>(V)) {
        Value *Operand = BC->getUnaryOperand();
        if (!Operand ||
            !IRType::hasSameShape(
                Value::getPointeeType(Operand),
                Value::getPointeeType(BC)))
            return Result;
        Result = tracePointer(Operand, Visiting);
        return Result;
    }

    auto *GEP = dynamic_cast<GetElementPtrInst *>(V);
    if (!GEP) return Result;

    if (GEP->getNumIndices() < 1 || !GEP->getBasePointer()) return Result;

    Result = tracePointer(GEP->getBasePointer(), Visiting);
    if (!Result.Root) return Result;

    int FirstIndex = 0;
    if (!Value::tryGetIntIndex(GEP->getIndex(0), FirstIndex) ||
        FirstIndex != 0) {
        Result.IsConstant = false;
        return Result;
    }

    for (size_t I = 1; I < GEP->getNumIndices(); ++I) {
        int Index = 0;
        if (!Value::tryGetIntIndex(GEP->getIndex(I), Index)) {
            Result.IsConstant = false;
            break;
        }
        Result.Indices.push_back(Index);
    }

    return Result;
}

LocalAggregatePointerPath LocalAggregateSimplifyPass::tracePointer(Value *V) {
    std::unordered_set<Value *> Visiting;
    return tracePointer(V, Visiting);
}

bool LocalAggregateSimplifyPass::getDirectConstantPath(
    AllocaInst *AI, GetElementPtrInst *GEP, std::vector<int> &Path,
    std::shared_ptr<IRType> &PointeeTy) {
    Path.clear();
    PointeeTy = nullptr;
    if (!AI || !GEP) return false;

    if (GEP->getNumIndices() < 1 || GEP->getBasePointer() != AI)
        return false;

    int FirstIndex = 0;
    if (!Value::tryGetIntIndex(GEP->getIndex(0), FirstIndex) ||
        FirstIndex != 0)
        return false;

    std::shared_ptr<IRType> Ty = AllocaInst::getAllocatedType(AI);
    for (size_t I = 1; I < GEP->getNumIndices(); ++I) {
        int Index = 0;
        if (!Value::tryGetIntIndex(GEP->getIndex(I), Index))
            return false;
        Path.push_back(Index);
    }

    if (Path.empty()) return false;
    PointeeTy = IRType::resolveArrayElementType(Ty, Path);
    return PointeeTy != nullptr;
}

bool LocalAggregateSimplifyPass::getDirectConstantScalarPath(
    AllocaInst *AI, GetElementPtrInst *GEP, std::vector<int> &Path,
    std::shared_ptr<IRType> &ElementTy) {
    if (!getDirectConstantPath(AI, GEP, Path, ElementTy))
        return false;
    if (!IRType::isScalar(ElementTy)) return false;
    return true;
}

bool LocalAggregateSimplifyPass::appendConstantAggregateGEPPath(
    Value *Base, GetElementPtrInst *GEP, const std::vector<int> &BasePath,
    std::vector<int> &NewPath, std::shared_ptr<IRType> &PointeeTy) {
    NewPath.clear();
    PointeeTy = nullptr;
    if (!Base || !GEP) return false;

    if (GEP->getNumIndices() < 2 || GEP->getBasePointer() != Base)
        return false;

    int FirstIndex = 0;
    if (!Value::tryGetIntIndex(GEP->getIndex(0), FirstIndex) ||
        FirstIndex != 0)
        return false;

    std::vector<int> RelativePath;
    for (size_t I = 1; I < GEP->getNumIndices(); ++I) {
        int Index = 0;
        if (!Value::tryGetIntIndex(GEP->getIndex(I), Index))
            return false;
        RelativePath.push_back(Index);
    }

    PointeeTy = IRType::resolveArrayElementType(
        Value::getPointeeType(Base), RelativePath);
    if (!PointeeTy) return false;
    NewPath = BasePath;
    NewPath.insert(NewPath.end(), RelativePath.begin(), RelativePath.end());
    return true;
}

bool LocalAggregateSimplifyPass::getDirectOneDimElementIndex(
    AllocaInst *AI, GetElementPtrInst *GEP, Value *&Index) {
    Index = nullptr;
    if (!AI || !GEP) return false;
    auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(AllocaInst::getAllocatedType(AI));
    if (!ArrTy || !IRType::isScalar(ArrTy->getElementType())) return false;

    if (GEP->getNumIndices() != 2 || GEP->getBasePointer() != AI)
        return false;

    int FirstIndex = 0;
    if (!Value::tryGetIntIndex(GEP->getIndex(0), FirstIndex) ||
        FirstIndex != 0)
        return false;

    Index = GEP->getIndex(1);
    return Index && Index->getIRType() &&
           Index->getIRType()->isInt();
}

bool LocalAggregateSimplifyPass::getConstantOffsetScalarGEP(
    Value *Base, GetElementPtrInst *GEP, int &Offset) {
    Offset = 0;
    if (!Base || !GEP) return false;
    if (GEP->getNumIndices() != 1 || GEP->getBasePointer() != Base)
        return false;

    if (!Value::tryGetIntIndex(GEP->getIndex(0), Offset))
        return false;

    auto BaseTy = IRType::getPointeeType(Base->getIRType());
    auto GEPTy = IRType::getPointeeType(GEP->getIRType());
    return IRType::isScalar(BaseTy) && IRType::isScalar(GEPTy) &&
           IRType::hasSameScalarType(BaseTy, GEPTy);
}

bool LocalAggregateSimplifyPass::isProvablyReadOnlySource(Variable *Source) {
    if (!Source) return false;
    if (Source->vartag == Variable::GlobalConstant) return true;
    if (Source->vartag != Variable::GlobalVariable) return false;

    // Global2Local may retain the original synthesized global as the memcpy
    // source. Treat it as immutable only when every remaining use is a
    // non-volatile memcpy source; any load, store, GEP, or unknown call makes
    // this proof fail conservatively.
    for (Use *U : Source->getUses()) {
        if (!U || U->getValue() != Source) continue;
        auto *CI = dynamic_cast<CallInst *>(U->getUser());
        if (!CI || !CI->isNonVolatileMemcpy() || CI->getArgIndex(U) != 1)
            return false;
    }
    return true;
}

bool LocalAggregateSimplifyPass::isWholeConstantCopy(
    CallInst *CI, AllocaInst *Destination, Variable *Source) {
    if (!CI || !Destination || !Source ||
        !isProvablyReadOnlySource(Source) ||
        !CI->isNonVolatileMemcpy())
        return false;

    auto *ByteCount = dynamic_cast<ConstInt *>(CI->getArg(2));
    auto DestTy = IRType::getPointeeType(Destination->getIRType());
    auto SourceTy = IRType::getPointeeType(Source->getIRType());
    if (!ByteCount || !DestTy || !SourceTy) return false;
    return ByteCount->getVal() >= 0 &&
           static_cast<size_t>(ByteCount->getVal()) == DestTy->getSize() &&
           IRType::hasSameShape(DestTy, SourceTy);
}

// Constant forwarding is intentionally restricted to one block. Validate the
// whole pointer-use graph first so an unmodelled use cannot silently alias it.
bool LocalAggregateSimplifyPass::hasSupportedSameBlockUses(
    AllocaInst *AI, BasicBlock *BB) {
    std::vector<Value *> Worklist{AI};
    std::unordered_set<Value *> Seen;

    while (!Worklist.empty()) {
        Value *Ptr = Worklist.back();
        Worklist.pop_back();
        if (!Seen.insert(Ptr).second) continue;

        for (Use *U : Ptr->getUses()) {
            if (!U || U->getValue() != Ptr) continue;
            auto *I = dynamic_cast<Instruction *>(U->getUser());
            if (!I || I->getParent() != BB) return false;

            int OperandIndex = I->getUserIndex(U);
            if ((dynamic_cast<GetElementPtrInst *>(I) ||
                 dynamic_cast<BitCastInst *>(I)) &&
                OperandIndex == 0) {
                Worklist.push_back(I);
                continue;
            }
            if (dynamic_cast<LoadInst *>(I) && OperandIndex == 0)
                continue;
            if (dynamic_cast<StoreInst *>(I) && OperandIndex == 1)
                continue;
            if (auto *CI = dynamic_cast<CallInst *>(I);
                CI && CallInst::isNonVolatileMemcpy(CI) && OperandIndex == 1)
                continue;
            return false;
        }
    }
    return true;
}

bool LocalAggregateSimplifyPass::forwardConstantLoads(AllocaInst *AI) {
    BasicBlock *BB = AI ? AI->getParent() : nullptr;
    if (!BB || !hasSupportedSameBlockUses(AI, BB)) return false;

    bool Changed = false;
    bool StateValid = false;
    Variable *InitialValue = nullptr;
    std::map<std::vector<int>, Value *> Overwrites;
    std::vector<LoadInst *> DeadLoads;

    auto Instructions = collectInstructions(*BB);
    Function *ParentF = BB->getParent();
    if (ParentF && ParentF->mayReachTimingCall()) {
        std::vector<Instruction *> Accesses;
        for (Instruction *I : Instructions) {
            Value *Pointer = nullptr;
            if (auto *LI = dynamic_cast<LoadInst *>(I)) {
                Pointer = LI->getPointerOperand();
            } else if (auto *SI = dynamic_cast<StoreInst *>(I)) {
                Pointer = SI->getPointerOperand();
            } else if (auto *CI = dynamic_cast<CallInst *>(I);
                       CI && CallInst::isMemcpyLike(CI) &&
                       CI->getArgCount() >= 1) {
                Pointer = CI->getArg(0);
            }
            if (Pointer && tracePointer(Pointer).Root == AI)
                Accesses.push_back(I);
        }

        if (Accesses.size() > 128) return false;
        for (Instruction *From : Accesses) {
            for (Instruction *To : Accesses) {
                // 自身配对同时覆盖经过计时调用后回到该访问的循环。
                if (ParentF->hasTimingBarrierOnPath(From, To))
                    return false;
            }
        }
    }

    for (Instruction *I : Instructions) {
        if (auto *CI = dynamic_cast<CallInst *>(I)) {
            if (CI->mayReachTimingCall()) {
                StateValid = false;
                InitialValue = nullptr;
                Overwrites.clear();
                continue;
            }
            if (!CallInst::isMemcpyLike(CI) || CI->getArgCount() < 2) continue;

            LocalAggregatePointerPath Dest = tracePointer(CI->getArg(0));
            if (Dest.Root != AI) continue;

            auto *Source = dynamic_cast<Variable *>(CI->getArg(1));
            if (!Dest.IsConstant || !Dest.Indices.empty() ||
                !isWholeConstantCopy(CI, AI, Source)) {
                StateValid = false;
                InitialValue = nullptr;
                Overwrites.clear();
                continue;
            }
            StateValid = true;
            InitialValue = Source;
            Overwrites.clear();
            continue;
        }

        if (auto *SI = dynamic_cast<StoreInst *>(I)) {
            LocalAggregatePointerPath Dest = tracePointer(SI->getPointerOperand());
            if (Dest.Root != AI) continue;

            if (!StateValid || !Dest.IsConstant ||
                Dest.Indices.empty()) {
                StateValid = false;
                InitialValue = nullptr;
                Overwrites.clear();
                continue;
            }
            Overwrites[Dest.Indices] = SI->getValueOperand();
            continue;
        }

        auto *LI = dynamic_cast<LoadInst *>(I);
        if (!LI) continue;

        LocalAggregatePointerPath SourcePath = tracePointer(LI->getPointerOperand());
        if (SourcePath.Root != AI || !StateValid ||
            !SourcePath.IsConstant || SourcePath.Indices.empty())
            continue;

        Value *Replacement = nullptr;
        auto OverwriteIt = Overwrites.find(SourcePath.Indices);
        if (OverwriteIt != Overwrites.end())
            Replacement = OverwriteIt->second;
        else
            Replacement = Variable::getConstantElement(InitialValue,
                                             SourcePath.Indices);

        if (!Replacement ||
            !IRType::hasSameShape(LI->getIRType(),
                                  Replacement->getIRType()))
            continue;
        LI->replaceAllUsesWith(Replacement);
        DeadLoads.push_back(LI);
        Changed = true;
    }

    for (LoadInst *LI : DeadLoads)
        delete LI;
    return Changed;
}

int LocalAggregateSimplifyPass::getOneDimScalarArrayCount(AllocaInst *AI) {
    auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(AllocaInst::getAllocatedType(AI));
    if (!ArrTy || !IRType::isScalar(ArrTy->getElementType()))
        return -1;
    return ArrTy->getElementCount();
}

bool LocalAggregateSimplifyPass::indexTypeCanSelectAll(
    Value *Index, int Count) {
    if (!Index || !Index->getIRType() ||
        !Index->getIRType()->isInt())
        return false;
    return !Index->getIRType()->isBool() || Count <= 2;
}

bool LocalAggregateSimplifyPass::collectReadOnlyConstantArrayUses(
    Function &F, AllocaInst *AI,
    LocalAggregateReadOnlyConstantArrayPlan &Plan, int Count) {
    if (!AI || Count <= 0) return false;

    std::set<GetElementPtrInst *> SeenGEPs;
    for (Use *U : AI->getUses()) {
        if (!U || U->getValue() != AI) continue;

        auto *I = dynamic_cast<Instruction *>(U->getUser());
        if (!I) return false;
        int OperandIndex = I->getUserIndex(U);

        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I);
            GEP && OperandIndex == 0) {
            Value *Index = nullptr;
            if (!getDirectOneDimElementIndex(AI, GEP, Index) ||
                !indexTypeCanSelectAll(Index, Count))
                return false;
            if (SeenGEPs.insert(GEP).second)
                Plan.ElementPtrs.push_back(GEP);
            continue;
        }

        if (auto *CI = dynamic_cast<CallInst *>(I);
            CI && OperandIndex == 1 && CallInst::isNonVolatileMemcpy(CI)) {
            if (CI->getArgCount() < 2 || CI->getArg(0) != AI ||
                Plan.InitCopy)
                return false;

            auto *Source = dynamic_cast<Variable *>(CI->getArg(1));
            if (!isWholeConstantCopy(CI, AI, Source)) return false;
            Plan.InitCopy = CI;
            Plan.Source = Source;
            continue;
        }

        return false;
    }

    if (!Plan.InitCopy || !Plan.Source || Plan.ElementPtrs.empty())
        return false;

    for (GetElementPtrInst *GEP : Plan.ElementPtrs) {
        for (Use *U : GEP->getUses()) {
            if (!U || U->getValue() != GEP) continue;
            auto *I = dynamic_cast<Instruction *>(U->getUser());
            if (!I) return false;
            int OperandIndex = I->getUserIndex(U);
            if (auto *LI = dynamic_cast<LoadInst *>(I);
                LI && OperandIndex == 0) {
                Plan.Loads.push_back(LI);
                continue;
            }
            return false;
        }
    }

    if (Plan.Loads.empty()) return false;

    BasicBlock *InitBB = Plan.InitCopy->getParent();
    if (!InitBB || InitBB != AI->getParent()) return false;
    BasicBlock *Entry = F.front();
    for (LoadInst *LI : Plan.Loads) {
        if (LI->getParent() == InitBB) {
            if (!Instruction::comesBefore(Plan.InitCopy, LI)) return false;
        } else if (InitBB != Entry) {
            return false;
        }
    }

    return true;
}

Value *LocalAggregateSimplifyPass::buildConstantArrayLookup(
    Variable *Source, int Count, Value *Index, Instruction *InsertBefore,
    std::vector<Instruction *> &Created) {
    int ConstIndex = 0;
    if (Value::tryGetIntIndex(Index, ConstIndex)) {
        if (ConstIndex < 0 || ConstIndex >= Count) return nullptr;
        return Variable::getConstantElement(Source, {ConstIndex});
    }

    std::vector<Value *> Elements;
    Elements.reserve(static_cast<size_t>(Count));
    for (int I = 0; I < Count; ++I) {
        Value *Elem = Variable::getConstantElement(Source, {I});
        if (!Elem) return nullptr;
        Elements.push_back(Elem);
    }

    bool AllSame = true;
    for (size_t I = 1; I < Elements.size(); ++I) {
        if (!Value::areEqualConstants(Elements[0], Elements[I])) {
            AllSame = false;
            break;
        }
    }
    if (AllSame) return Elements[0];

    Value *Result = Elements[0];
    for (int I = 1; I < Count; ++I) {
        Value *Needle = Value::makeIntegerLike(Index, I);
        if (!Needle) return nullptr;
        auto *Cmp = new BinaryInst(Index, BinaryInst::E, Needle);
        if (!InsertBefore->insertBefore(Cmp)) {
            delete Cmp;
            return nullptr;
        }
        Created.push_back(Cmp);
        auto *Sel = new SelectInst(Cmp, Elements[static_cast<size_t>(I)],
                                   Result);
        if (!InsertBefore->insertBefore(Sel)) {
            delete Sel;
            return nullptr;
        }
        Created.push_back(Sel);
        Result = Sel;
    }
    return Result;
}

bool LocalAggregateSimplifyPass::expandReadOnlyConstantArrayLoads(
    Function &F, AllocaInst *AI) {
    constexpr int kMaxDynamicSelectArrayElements = 2;

    int Count = getOneDimScalarArrayCount(AI);
    if (Count <= 0) return false;

    LocalAggregateReadOnlyConstantArrayPlan Plan;
    if (!collectReadOnlyConstantArrayUses(F, AI, Plan, Count))
        return false;
    std::vector<Instruction *> TimingAccesses{Plan.InitCopy};
    for (LoadInst *LI : Plan.Loads) {
        if (!LI) return false;
        TimingAccesses.push_back(LI);
    }
    for (Instruction *From : TimingAccesses) {
        for (Instruction *To : TimingAccesses) {
            if (F.hasTimingBarrierOnPath(From, To)) return false;
        }
    }

    Value *FirstElement = Variable::getConstantElement(Plan.Source, {0});
    if (!FirstElement) return false;

    std::vector<int32_t> TableValues;
    std::shared_ptr<LocalAggregateSynthExpression> Expression;
    if (collectInt32ConstantTable(Plan.Source, Count, TableValues))
        Expression = synthesizeInt32TableExpression(TableValues);
    bool CanUseExpression = isSynthExpressionProfitable(Expression);

    Value *First = Variable::getConstantElement(Plan.Source, {0});
    bool Uniform = First != nullptr;
    for (int I = 1; Uniform && I < Count; ++I)
        Uniform = Value::areEqualConstants(
            First, Variable::getConstantElement(Plan.Source, {I}));

    bool HasDynamicIndex = false;
    for (LoadInst *LI : Plan.Loads) {
        if (!IRType::hasSameShape(LI->getIRType(),
                                  FirstElement->getIRType()))
            return false;
        auto *GEP = dynamic_cast<GetElementPtrInst *>(LI->getPointerOperand());
        Value *Index = nullptr;
        if (!getDirectOneDimElementIndex(AI, GEP, Index) ||
            !indexTypeCanSelectAll(Index, Count))
            return false;

        int ConstIndex = 0;
        if (Value::tryGetIntIndex(Index, ConstIndex)) {
            if (ConstIndex < 0 || ConstIndex >= Count) return false;
        } else {
            HasDynamicIndex = true;
            if (CanUseExpression &&
                (!Index->getIRType() || Index->getIRType()->isBool() ||
                 Index->getIRType()->getIntegerBitWidth() != 32 ||
                 !IRType::hasSameShape(LI->getIRType(),
                                       Index->getIRType())))
                CanUseExpression = false;
        }
    }
    if (HasDynamicIndex && !CanUseExpression &&
        Count > kMaxDynamicSelectArrayElements && !Uniform)
        return false;

    std::vector<std::pair<LoadInst *, Value *>> Replacements;
    std::vector<Instruction *> Created;
    Replacements.reserve(Plan.Loads.size());
    for (LoadInst *LI : Plan.Loads) {
        auto *GEP = dynamic_cast<GetElementPtrInst *>(LI->getPointerOperand());
        Value *Index = nullptr;
        if (!getDirectOneDimElementIndex(AI, GEP, Index)) {
            for (auto It = Created.rbegin(); It != Created.rend(); ++It)
                delete *It;
            return false;
        }

        int ConstIndex = 0;
        bool IsConstantIndex =
            Value::tryGetIntIndex(Index, ConstIndex);
        Value *Replacement = nullptr;
        if (!IsConstantIndex && CanUseExpression)
            Replacement = materializeSynthExpression(
                Expression, Index, LI, Created);
        else
            Replacement = buildConstantArrayLookup(
                Plan.Source, Count, Index, LI, Created);
        if (!Replacement ||
            !IRType::hasSameShape(LI->getIRType(),
                                  Replacement->getIRType())) {
            // 所有替换都在验证结束后统一提交；失败时按逆序删除新建节点，
            // 保证 pass 不会留下半完成的表达式或 select 链。
            for (auto It = Created.rbegin(); It != Created.rend(); ++It)
                delete *It;
            return false;
        }
        Replacements.push_back({LI, Replacement});
    }

    for (auto &Replacement : Replacements) {
        Replacement.first->replaceAllUsesWith(Replacement.second);
        delete Replacement.first;
    }

    delete Plan.InitCopy;
    for (auto It = Plan.ElementPtrs.rbegin();
         It != Plan.ElementPtrs.rend(); ++It) {
        if (!(*It)->hasUses()) delete *It;
    }

    if (AI->hasUses()) return false;
    delete AI;
    return true;
}

bool LocalAggregateSimplifyPass::addScalarSlot(
    std::map<std::vector<int>, std::shared_ptr<IRType>> &SlotTypes,
    const std::vector<int> &Path,
    const std::shared_ptr<IRType> &ElementTy,
    int MaxScalarizedElements) {
    auto SlotIt = SlotTypes.find(Path);
    if (SlotIt != SlotTypes.end())
        return IRType::hasSameScalarType(SlotIt->second, ElementTy);

    SlotTypes.emplace(Path, ElementTy);
    return static_cast<int>(SlotTypes.size()) <= MaxScalarizedElements;
}

bool LocalAggregateSimplifyPass::collectScalarizedPointerUses(
    AllocaInst *AI, Value *Ptr, const std::vector<int> &PtrPath,
    std::vector<GetElementPtrInst *> &TransparentPtrs,
    std::map<GetElementPtrInst *, std::vector<int>> &TransparentPath,
    std::set<GetElementPtrInst *> &SeenTransparent,
    std::vector<Instruction *> &MemoryAccesses,
    std::map<std::vector<int>, std::shared_ptr<IRType>> &SlotTypes,
    int MaxScalarizedElements) {
    auto PtrPointeeTy = Value::getPointeeType(Ptr);
    bool PtrIsScalar = IRType::isScalar(PtrPointeeTy);

    for (Use *U : Ptr->getUses()) {
        if (!U || U->getValue() != Ptr) continue;
        auto *I = dynamic_cast<Instruction *>(U->getUser());
        if (!I) return false;
        int OperandIndex = I->getUserIndex(U);

        if (dynamic_cast<LoadInst *>(I) && OperandIndex == 0 &&
            PtrIsScalar) {
            MemoryAccesses.push_back(I);
            continue;
        }
        if (dynamic_cast<StoreInst *>(I) && OperandIndex == 1 &&
            PtrIsScalar) {
            MemoryAccesses.push_back(I);
            continue;
        }

        int Offset = 0;
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I);
            GEP && OperandIndex == 0 &&
            getConstantOffsetScalarGEP(Ptr, GEP, Offset)) {
            int FlatIndex = 0;
            if (!IRType::flattenScalarPath(AllocaInst::getAllocatedType(AI), PtrPath,
                                   FlatIndex))
                return false;

            std::vector<int> NewPath;
            std::shared_ptr<IRType> NewElementTy;
            if (!IRType::unflattenScalarPath(AllocaInst::getAllocatedType(AI),
                                     FlatIndex + Offset, NewPath,
                                     NewElementTy))
                return false;
            if (!addScalarSlot(SlotTypes, NewPath, NewElementTy,
                               MaxScalarizedElements))
                return false;

            auto ExistingPath = TransparentPath.find(GEP);
            if (ExistingPath != TransparentPath.end() &&
                ExistingPath->second != NewPath)
                return false;

            if (SeenTransparent.insert(GEP).second) {
                TransparentPtrs.push_back(GEP);
                TransparentPath[GEP] = NewPath;
                if (!collectScalarizedPointerUses(
                        AI, GEP, NewPath, TransparentPtrs, TransparentPath,
                        SeenTransparent, MemoryAccesses, SlotTypes,
                        MaxScalarizedElements))
                    return false;
            }
            continue;
        }

        std::vector<int> NewPath;
        std::shared_ptr<IRType> NewPointeeTy;
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I);
            GEP && OperandIndex == 0 &&
            appendConstantAggregateGEPPath(Ptr, GEP, PtrPath, NewPath,
                                           NewPointeeTy)) {
            if (IRType::isScalar(NewPointeeTy) &&
                !addScalarSlot(SlotTypes, NewPath, NewPointeeTy,
                               MaxScalarizedElements))
                return false;
            if (!IRType::isScalar(NewPointeeTy) &&
                !NewPointeeTy->isArray())
                return false;

            auto ExistingPath = TransparentPath.find(GEP);
            if (ExistingPath != TransparentPath.end() &&
                ExistingPath->second != NewPath)
                return false;

            if (SeenTransparent.insert(GEP).second) {
                TransparentPtrs.push_back(GEP);
                TransparentPath[GEP] = NewPath;
                if (!collectScalarizedPointerUses(
                        AI, GEP, NewPath, TransparentPtrs, TransparentPath,
                        SeenTransparent, MemoryAccesses, SlotTypes,
                        MaxScalarizedElements))
                    return false;
            }
            continue;
        }

        return false;
    }
    return true;
}

bool LocalAggregateSimplifyPass::scalarizeConstantIndexedAggregate(
    AllocaInst *AI) {
    constexpr int kMaxScalarizedElements = 32;
    if (!AI || !AllocaInst::isArrayAllocation(AI)) return false;

    CallInst *InitCopy = nullptr;
    Variable *InitSource = nullptr;
    std::vector<GetElementPtrInst *> ElementPtrs;
    std::vector<GetElementPtrInst *> TransparentPtrs;
    std::map<std::vector<int>, std::shared_ptr<IRType>> SlotTypes;
    std::set<GetElementPtrInst *> SeenGEPs;
    std::set<GetElementPtrInst *> SeenTransparent;
    std::vector<Instruction *> MemoryAccesses;
    std::map<GetElementPtrInst *, std::vector<int>> DirectPath;
    std::map<GetElementPtrInst *, std::vector<int>> TransparentPath;

    for (Use *U : AI->getUses()) {
        if (!U || U->getValue() != AI) continue;
        auto *UserI = dynamic_cast<Instruction *>(U->getUser());
        if (!UserI) return false;

        int OperandIndex = UserI->getUserIndex(U);
        if (auto *CI = dynamic_cast<CallInst *>(UserI);
            CI && OperandIndex == 1 && CallInst::isNonVolatileMemcpy(CI)) {
            if (CI->getArgCount() < 2 || CI->getArg(0) != AI || InitCopy)
                return false;
            auto *Source = dynamic_cast<Variable *>(CI->getArg(1));
            if (!isWholeConstantCopy(CI, AI, Source))
                return false;
            InitCopy = CI;
            InitSource = Source;
            continue;
        }

        auto *GEP = dynamic_cast<GetElementPtrInst *>(UserI);
        if (!GEP || OperandIndex != 0) return false;

        std::vector<int> Path;
        std::shared_ptr<IRType> PointeeTy;
        if (!getDirectConstantPath(AI, GEP, Path, PointeeTy))
            return false;

        if (!IRType::isScalar(PointeeTy) && !PointeeTy->isArray())
            return false;
        if (IRType::isScalar(PointeeTy) &&
            !addScalarSlot(SlotTypes, Path, PointeeTy,
                           kMaxScalarizedElements))
            return false;

        if (SeenGEPs.insert(GEP).second) {
            ElementPtrs.push_back(GEP);
            DirectPath[GEP] = Path;
        }
    }

    if (ElementPtrs.empty()) return false;

    for (GetElementPtrInst *GEP : ElementPtrs) {
        auto PathIt = DirectPath.find(GEP);
        if (PathIt == DirectPath.end()) return false;
        if (!collectScalarizedPointerUses(
                AI, GEP, PathIt->second, TransparentPtrs, TransparentPath,
                SeenTransparent, MemoryAccesses, SlotTypes,
                kMaxScalarizedElements))
            return false;
    }

    if (SlotTypes.empty()) return false;

    if (InitCopy) {
        Function *ParentF = AI->getParent()
                                ? AI->getParent()->getParent()
                                : nullptr;
        if (!ParentF) return false;
        std::vector<Instruction *> TimingAccesses{InitCopy};
        TimingAccesses.insert(TimingAccesses.end(), MemoryAccesses.begin(),
                              MemoryAccesses.end());
        for (Instruction *From : TimingAccesses) {
            for (Instruction *To : TimingAccesses) {
                if (!From || !To ||
                    ParentF->hasTimingBarrierOnPath(From, To)) {
                    return false;
                }
            }
        }
        for (Instruction *Access : MemoryAccesses) {
            if (!Access) return false;
            if (Access->getParent() == InitCopy->getParent()) {
                if (!Instruction::comesBefore(InitCopy, Access))
                    return false;
                continue;
            }
            if (InitCopy->getParent() != AI->getParent())
                return false;
        }
    }

    std::map<std::vector<int>, Value *> InitialValues;
    if (InitSource) {
        for (auto &Entry : SlotTypes) {
            Value *Init = Variable::getConstantElement(InitSource, Entry.first);
            // 标量槽位按真实标量类型匹配，区分存储大小相同的不同整数位宽。
            if (!Init ||
                !IRType::hasSameScalarType(Init->getIRType(),
                                           Entry.second))
                return false;
            InitialValues[Entry.first] = Init;
        }
    }

    std::map<std::vector<int>, AllocaInst *> Slots;
    for (auto &Entry : SlotTypes) {
        auto *Slot = new AllocaInst(Entry.second);
        if (!AI->insertBefore(Slot)) {
            delete Slot;
            return false;
        }
        Slots[Entry.first] = Slot;
    }

    if (!InitialValues.empty()) {
        Instruction *InitInsertPoint = InitCopy ? static_cast<Instruction *>(InitCopy)
                                                : static_cast<Instruction *>(AI);
        for (auto &Entry : Slots) {
            Value *Init = InitialValues[Entry.first];
            auto *Store = new StoreInst(Init, Entry.second);
            if (!InitInsertPoint->insertBefore(Store)) {
                delete Store;
                return false;
            }
        }
    }

    for (auto It = TransparentPtrs.rbegin(); It != TransparentPtrs.rend();
         ++It) {
        GetElementPtrInst *GEP = *It;
        auto PathIt = TransparentPath.find(GEP);
        if (PathIt == TransparentPath.end()) return false;
        auto SlotIt = Slots.find(PathIt->second);
        if (SlotIt != Slots.end())
            GEP->replaceAllUsesWith(SlotIt->second);
        if (GEP->hasUses()) return false;
        delete GEP;
    }

    for (GetElementPtrInst *GEP : ElementPtrs) {
        auto PathIt = DirectPath.find(GEP);
        if (PathIt == DirectPath.end()) return false;
        auto SlotIt = Slots.find(PathIt->second);
        if (SlotIt != Slots.end())
            GEP->replaceAllUsesWith(SlotIt->second);
        if (GEP->hasUses()) return false;
        delete GEP;
    }

    if (InitCopy) delete InitCopy;

    if (AI->hasUses()) return false;
    delete AI;
    return true;
}

LocalAggregateDeadUses LocalAggregateSimplifyPass::collectDeadAggregateUses(
    AllocaInst *AI) {
    LocalAggregateDeadUses Result;
    std::vector<Value *> Worklist{AI};
    std::unordered_set<Value *> SeenValues;
    std::unordered_set<Instruction *> SeenWrites;

    while (!Worklist.empty() && Result.IsDead) {
        Value *Ptr = Worklist.back();
        Worklist.pop_back();
        if (!SeenValues.insert(Ptr).second) continue;

        for (Use *U : Ptr->getUses()) {
            if (!U || U->getValue() != Ptr) continue;
            auto *I = dynamic_cast<Instruction *>(U->getUser());
            if (!I) {
                Result.IsDead = false;
                break;
            }

            int OperandIndex = I->getUserIndex(U);
            if ((dynamic_cast<GetElementPtrInst *>(I) ||
                 dynamic_cast<BitCastInst *>(I)) &&
                OperandIndex == 0) {
                if (!SeenValues.count(I)) {
                    Worklist.push_back(I);
                    Result.DerivedPointers.push_back(I);
                }
                continue;
            }

            if (dynamic_cast<StoreInst *>(I) && OperandIndex == 1) {
                if (SeenWrites.insert(I).second) Result.Writes.push_back(I);
                continue;
            }

            if (auto *CI = dynamic_cast<CallInst *>(I);
                CI && CallInst::isNonVolatileMemcpy(CI) &&
                OperandIndex == 1) {
                if (SeenWrites.insert(I).second) Result.Writes.push_back(I);
                continue;
            }

            // load、memcpy 源、保存指针本身或传入未知调用都可能产生
            // 可观察行为，必须保留整个对象。
            Result.IsDead = false;
            break;
        }
    }
    return Result;
}

bool LocalAggregateSimplifyPass::deleteDeadAggregate(AllocaInst *AI) {
    LocalAggregateDeadUses Uses = collectDeadAggregateUses(AI);
    if (!Uses.IsDead) return false;

    for (Instruction *I : Uses.Writes)
        delete I;

    // 父地址先于派生地址被发现，因此逆序删除可保证先叶子、后根节点。
    for (auto It = Uses.DerivedPointers.rbegin();
         It != Uses.DerivedPointers.rend(); ++It) {
        Instruction *I = *It;
        if (!I->hasUses()) delete I;
    }

    if (AI->hasUses()) return false;
    delete AI;
    return true;
}

std::vector<AllocaInst *>
LocalAggregateSimplifyPass::collectArrayAllocas(Function &F) {
    std::vector<AllocaInst *> Aggregates;
    for (AllocaInst *AI : collectInstructions<AllocaInst>(F))
        if (AllocaInst::isArrayAllocation(AI)) Aggregates.push_back(AI);
    return Aggregates;
}

PreservationStatus
LocalAggregateSimplifyPass::run(Function &F,
                                FunctionAnalysisPassManager &FAM) {
    bool Changed = false;

    Changed |= foldReadOnlyGlobalLoads(F, FAM);
    for (AllocaInst *AI : collectArrayAllocas(F))
        Changed |= forwardConstantLoads(AI);
    for (AllocaInst *AI : collectArrayAllocas(F))
        Changed |= expandReadOnlyConstantArrayLoads(F, AI);
    for (AllocaInst *AI : collectArrayAllocas(F))
        Changed |= scalarizeConstantIndexedAggregate(AI);
    for (AllocaInst *AI : collectArrayAllocas(F))
        Changed |= deleteDeadAggregate(AI);

    return Changed ? PreservationStatus::none()
                   : PreservationStatus::all();
}
