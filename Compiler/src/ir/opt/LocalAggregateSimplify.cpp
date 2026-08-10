#include "ir/opt/LocalAggregateSimplify.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/GepCombine.hpp"
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
//步骤 1.1：沿全局对象派生出的 GEP/形状保持 bitcast 遍历完整 use 图；仅允许
//load 或作为非 volatile memcpy 源，任一写入、逃逸或未知调用都会否定只读性。
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
            //证明必须覆盖整个指针闭包，不能因只检查当前 load 而忽略别处写入。
            return false;
        }
    }
    return true;
}
//步骤 1.2：从 load 指针逆向剥离形状保持 bitcast 和 GEP，再按正向顺序解析
//全常量零基路径；结果必须落到全局对象的标量元素。
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
//步骤 1.3：提取用于表达式综合的一维 i32 常量表；元素数、数组形状、位宽
//和每个初始化值均须完全匹配，不能以缺失元素参与枚举。
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
//步骤 1.4：按 IR 的 i32 语义解释综合候选。算术使用宽类型检查有符号溢出，
//位运算按位模式执行，移位显式排除非法位数及当前 IR 不允许的溢出情形。
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
    //候选必须在完整定义域的每一点都有定义，不能借未定义行为凑出表值。
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
//线性扫描完整定义域并识别索引、常量和一次加减表
std::shared_ptr<LocalAggregateSynthExpression>
LocalAggregateSimplifyPass::synthesizeFastInt32TableExpression(
    const std::vector<int32_t> &Values) {
    if (Values.empty() ||
        Values.size() > static_cast<size_t>(MaxFastSynthArrayElements)) {
        return nullptr;
    }
    auto MakeIndex = []() {
        auto Result =
            std::make_shared<LocalAggregateSynthExpression>();
        Result->Kind = LocalAggregateSynthExpressionKind::Index;
        Result->Cost = 0;
        return Result;
    };
    auto MakeConstant = [](int32_t Value) {
        auto Result =
            std::make_shared<LocalAggregateSynthExpression>();
        Result->Kind = LocalAggregateSynthExpressionKind::Constant;
        Result->Constant = Value;
        Result->Cost = 0;
        return Result;
    };
    auto MakeBinary = [](
        BinaryInst::Operation Op,
        const std::shared_ptr<LocalAggregateSynthExpression> &Left,
        const std::shared_ptr<LocalAggregateSynthExpression> &Right) {
        auto Result =
            std::make_shared<LocalAggregateSynthExpression>();
        Result->Kind = LocalAggregateSynthExpressionKind::Binary;
        Result->Op = Op;
        Result->Cost = 1 + (Left ? Left->Cost : 0) +
                       (Right ? Right->Cost : 0);
        Result->Left = Left;
        Result->Right = Right;
        return Result;
    };
    //1.5.1 大表只做 O(N) 完整验证：索引表与常量表不产生
    //新二元指令，不会根据局部样本或运行时输入猜测表形状。
    bool IsIndex = true;
    bool IsUniform = true;
    for (size_t I = 0; I < Values.size(); ++I) {
        IsIndex &= Values[I] == static_cast<int32_t>(I);
        IsUniform &= Values[I] == Values.front();
    }
    if (IsIndex) return MakeIndex();
    if (IsUniform) return MakeConstant(Values.front());

    //1.5.2 对 index+C 和 C-index 逐点使用宽位计算，任一点超出
    //i32定义域就拒绝，不借有符号溢出完成表达式拟合。
    const int64_t Delta = Values.front();
    bool IsIndexPlusConstant = true;
    bool IsConstantMinusIndex = true;
    for (size_t I = 0; I < Values.size(); ++I) {
        const int64_t Index = static_cast<int64_t>(I);
        const int64_t Added = Index + Delta;
        const int64_t Subtracted = Delta - Index;
        IsIndexPlusConstant &=
            Added >= std::numeric_limits<int32_t>::min() &&
            Added <= std::numeric_limits<int32_t>::max() &&
            Values[I] == static_cast<int32_t>(Added);
        IsConstantMinusIndex &=
            Subtracted >= std::numeric_limits<int32_t>::min() &&
            Subtracted <= std::numeric_limits<int32_t>::max() &&
            Values[I] == static_cast<int32_t>(Subtracted);
    }
    if (IsIndexPlusConstant) {
        return MakeBinary(BinaryInst::ADD, MakeIndex(),
                          MakeConstant(static_cast<int32_t>(Delta)));
    }
    if (IsConstantMinusIndex) {
        return MakeBinary(BinaryInst::SUB,
                          MakeConstant(static_cast<int32_t>(Delta)),
                          MakeIndex());
    }
    return nullptr;
}
//步骤 1.5：以“在完整表定义域上的结果向量”为状态做有界枚举；语义相同的
//树只保留首个低成本代表，命中目标向量才接受表达式。
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
    //1.5.1 以索引自身和固定来源的少量常量为零成本种子；常量集合来自表的
    //端点、一阶/二阶差分和表值，并受统一数量预算限制。
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
        //1.5.2 按总指令成本逐层组合已知状态。相同成本下优先平衡的子树成本
        //划分，使固定状态预算覆盖更多不同形状。
        std::set<SemanticVector> PendingSemantics;
        std::vector<std::pair<SemanticVector, ExpressionPtr>> Pending;
        bool EnumerationBudgetReached = false;
        std::vector<int> LeftCostOrder;
        LeftCostOrder.reserve(static_cast<size_t>(Cost));
        for (int LeftCost = 0; LeftCost < Cost; ++LeftCost)
            LeftCostOrder.push_back(LeftCost);
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
                        //每个候选已在整个数组定义域逐点求值；完整相等时立即
                        //接受，而非根据局部样本或运行时输入猜测。
                        if (Semantics == Values) return Candidate;
                        if (KnownStates.count(Semantics) ||
                            PendingSemantics.count(Semantics))
                            continue;
                        PendingSemantics.insert(Semantics);
                        Pending.push_back(
                            {std::move(Semantics), std::move(Candidate)});
                        if (Pending.size() >=
                            static_cast<size_t>(MaxSynthPendingStates)) {
                            //达到枚举预算只停止优化搜索，不放宽候选正确性。
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
        //1.5.3 按确定枚举顺序截断本成本层状态，不依据部分匹配程度调整搜索。
        if (Pending.size() >
            static_cast<size_t>(MaxSynthStatesPerCost))
            Pending.resize(static_cast<size_t>(MaxSynthStatesPerCost));
        for (const auto &Entry : Pending)
            AddKnownState(Entry.first, Entry.second);
    }
    return nullptr;
}
//步骤 1.6：递归在原 load 前物化已验证表达式；Created 记录全部新节点，供
//调用者在后续类型检查失败时逆序回滚。
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
//收益验证只比较表达式自身与一次标量 load，严格更便宜才允许替换；旧 GEP
//是否还能删除不计入收益，避免高估。
bool LocalAggregateSimplifyPass::isSynthExpressionProfitable(
    const std::shared_ptr<LocalAggregateSynthExpression> &Expression) {
    return Expression && Expression->Cost >= 0 &&
           Expression->Cost < GenericScalarLoadCost;
}
//步骤 1 清理辅助：load 删除后，从其指针向根逆向删除无 use 的 GEP/bitcast，
//遇到仍被使用或非透明指令即停止。
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
//步骤 1 主流程同时处理两类只读全局 load：常量路径直接取初始化值；受范围
//证明约束的一维动态 i32 查表尝试用完整定义域等价表达式替换。
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
        //1.7 常量路径：缓存整程序只读证明，读取精确初始化元素并核对 load 类型。
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
        //1.8 动态路径候选只接受全局一维数组的 [0, index] GEP，常量 index 已
        //由前一分支处理；每个源表最多执行一次有界表达式综合。
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
        //1.9 RangeAnalysis 必须证明运行时索引完整落在表定义域，且索引与
        //load 都是同形 i32，综合式才具有与原 load 相同的定义范围和类型。
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
    //1.10 所有候选验证完后统一提交：替换 load，删除其定义，再清理失去使用
    //的透明指针链。综合失败产生的节点已在候选局部逆序回滚。
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
//步骤 2—5 的通用路径追踪：沿形状保持 bitcast 和零基 GEP 递归到 alloca；
//环检测防止异常 use 图无限递归，动态索引只把 IsConstant 置否而保留根信息。
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
//创建递归访问集合并追踪指针相对根alloca的常量路径
LocalAggregatePointerPath LocalAggregateSimplifyPass::tracePointer(Value *V) {
    std::unordered_set<Value *> Visiting;
    return tracePointer(V, Visiting);
}
//步骤 4.1：解析以 alloca 为直接基址的非空全常量聚合路径，并返回路径末端
//类型；首索引必须为零，保持访问在同一个分配对象内。
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
//标量路径辅助在通用常量路径基础上额外要求末端类型可独立建槽。
bool LocalAggregateSimplifyPass::getDirectConstantScalarPath(
    AllocaInst *AI, GetElementPtrInst *GEP, std::vector<int> &Path,
    std::shared_ptr<IRType> &ElementTy) {
    if (!getDirectConstantPath(AI, GEP, Path, ElementTy))
        return false;
    if (!IRType::isScalar(ElementTy)) return false;
    return true;
}
//步骤 4.2：从已知派生地址继续追加零基全常量 GEP 路径，并同步解析新的
//pointee 类型；NewPath 始终相对根 alloca。
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
//步骤 3.1：识别一维标量数组的直接 [0, index] 元素地址；index 可为动态
//整数，后续阶段再验证其值域或选择表达能力。
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
//步骤 4.3：识别标量指针上的单常量偏移 GEP，要求偏移前后标量类型一致，
//供展平路径后映射到相邻标量槽。
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
//步骤 2/3 的 memcpy 源证明：GlobalConstant 天然只读；保留下来的合成
//GlobalVariable 仅在每个 use 都是非 volatile memcpy 源时才可视为不可变。
bool LocalAggregateSimplifyPass::isProvablyReadOnlySource(Variable *Source) {
    if (!Source) return false;
    if (Source->vartag == Variable::GlobalConstant) return true;
    if (Source->vartag != Variable::GlobalVariable) return false;
    for (Use *U : Source->getUses()) {
        if (!U || U->getValue() != Source) continue;
        auto *CI = dynamic_cast<CallInst *>(U->getUser());
        if (!CI || !CI->isNonVolatileMemcpy() || CI->getArgIndex(U) != 1)
            return false;
    }
    return true;
}
//步骤 2/3 的初始化匹配：memcpy 必须从已证只读的同形全局源完整覆盖目标
//alloca，字节数为非负精确常量，不接受部分初始化。
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
//步骤 2.1：在转发前遍历 alloca 的完整透明指针图，要求所有派生地址、
//load/store 和 memcpy 都位于同一基本块；任何逃逸或未知用途都拒绝整项。
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
//步骤 2 主流程在单块程序序上维护“最近完整常量初始化 + 各元素覆盖”状态，
//只替换能由该精确状态回答的常量路径 load。
bool LocalAggregateSimplifyPass::forwardConstantLoads(AllocaInst *AI) {
    BasicBlock *BB = AI ? AI->getParent() : nullptr;
    if (!BB || !hasSupportedSameBlockUses(AI, BB)) return false;
    bool Changed = false;
    bool StateValid = false;
    Variable *InitialValue = nullptr;
    std::map<std::vector<int>, Value *> Overwrites;
    std::vector<LoadInst *> DeadLoads;
    auto Instructions = collectInstructions(*BB);
    //2.2 顺序解释同块指令：timer 不修改聚合对象，状态可以跨越；完整常量
    //memcpy 重置初值，常量路径 store 更新覆盖表，load 从覆盖或初值中转发。
    for (Instruction *I : Instructions) {
        if (auto *CI = dynamic_cast<CallInst *>(I)) {
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
    //2.4 扫描结束后统一删除已被替换的 load，避免遍历过程中链表失效。
    for (LoadInst *LI : DeadLoads)
        delete LI;
    return Changed;
}
//步骤 3 的形状辅助只接受一维标量数组，并返回固定元素数。
int LocalAggregateSimplifyPass::getOneDimScalarArrayCount(AllocaInst *AI) {
    auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(AllocaInst::getAllocatedType(AI));
    if (!ArrTy || !IRType::isScalar(ArrTy->getElementType()))
        return -1;
    return ArrTy->getElementCount();
}
//bool index 仅能覆盖至多两个元素；普通整数可为后续表达式或 select 比较
//构造任意合法元素编号。
bool LocalAggregateSimplifyPass::indexTypeCanSelectAll(
    Value *Index, int Count) {
    if (!Index || !Index->getIRType() ||
        !Index->getIRType()->isInt())
        return false;
    return !Index->getIRType()->isBool() || Count <= 2;
}
//步骤 3.2：构造只读局部数组展开计划。alloca 只能有一次完整常量 memcpy
//初始化和若干直接元素 GEP；每个 GEP 只能被有界数量的load使用。跨块初始化
//必须支配load，并由MemorySSA证明初始化状态到load之间没有对该元素的其他写入。
bool LocalAggregateSimplifyPass::collectReadOnlyConstantArrayUses(
    Function &F, AllocaInst *AI,
    LocalAggregateReadOnlyConstantArrayPlan &Plan, int Count,
    const DominatorTree &DT, const MemorySSAResult &MSSA,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) {
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
                if (Plan.Loads.size() >= 256) return false;
                Plan.Loads.push_back(LI);
                continue;
            }
            return false;
        }
    }
    if (Plan.Loads.empty()) return false;
    BasicBlock *InitBB = Plan.InitCopy->getParent();
    if (!InitBB || InitBB != AI->getParent()) return false;
    MemoryAccess *InitAccess = MSSA.getMemoryAccess(Plan.InitCopy);
    if (InitBB->getParent() != &F ||
        !InitAccess || !InitAccess->isDef()) {
        return false;
    }
    for (LoadInst *Load : Plan.Loads) {
        BasicBlock *LoadBB = Load ? Load->getParent() : nullptr;
        if (!LoadBB || LoadBB->getParent() != &F) return false;
        if (LoadBB == InitBB) {
            if (!Instruction::comesBefore(Plan.InitCopy, Load))
                return false;
            continue;
        }
        MemoryAccess *LoadAccess = MSSA.getMemoryAccess(Load);
        if (!LoadAccess || !DT.dominates(InitBB, LoadBB) ||
            MSSA.hasClobberBetween(
                Plan.InitCopy, LoadAccess, MemoryLocation::get(Load),
                AA, LI, FAM, 256)) {
            return false;
        }
    }
    return true;
}
//步骤 3.3：常量索引直接取元素；动态索引若所有元素相同直接取公共值，否则
//在原 load 前构造逐元素比较/select 链，并记录每个新节点供事务式回滚。
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
//步骤 3 主流程先完整验证数组、索引、类型与代码增长收益，再为
//全部 load 构造替换值；任一失败都回滚所有新指令，成功后统一删除旧对象。
bool LocalAggregateSimplifyPass::expandReadOnlyConstantArrayLoads(
    Function &F, AllocaInst *AI, const DominatorTree &DT,
    const MemorySSAResult &MSSA, BasicAAResult &AA,
    const LoopInfo &LI, FunctionAnalysisPassManager &FAM) {
    constexpr int kMaxDynamicSelectArrayElements = 2;
    int Count = getOneDimScalarArrayCount(AI);
    if (Count <= 0) return false;
    LocalAggregateReadOnlyConstantArrayPlan Plan;
    if (!collectReadOnlyConstantArrayUses(
            F, AI, Plan, Count, DT, MSSA, AA, LI, FAM))
        return false;
    for (LoadInst *LI : Plan.Loads) {
        if (!LI) return false;
    }
    Value *FirstElement = Variable::getConstantElement(Plan.Source, {0});
    if (!FirstElement) return false;
    std::vector<int32_t> TableValues;
    std::shared_ptr<LocalAggregateSynthExpression> Expression;
    if (collectInt32ConstantTable(Plan.Source, Count, TableValues))
        Expression = synthesizeInt32TableExpression(TableValues);
    bool CanUseExpression = isSynthExpressionProfitable(Expression);
    //3.5 统一表无需 select；非统一动态表优先采用有收益的综合表达式，若未
    //综合成功则只允许至多两个元素的极小 select 链。
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
    //3.6 预构造每个 load 的替换。综合表达式、常量元素和 select 链必须与
    //load 类型同形；任一项失败时逆序删除本计划创建的全部节点。
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
            for (auto It = Created.rbegin(); It != Created.rend(); ++It)
                delete *It;
            return false;
        }
        Replacements.push_back({LI, Replacement});
    }
    //3.7 所有构造均成功后提交：替换并删除 load、初始化 memcpy、无用元素
    //GEP，最后仅在 alloca 已无 use 时删除根对象；若仍有保守保留的派生use，
    //前面的替换仍然已经提交，必须返回changed以触发分析失效。
    for (auto &Replacement : Replacements) {
        Replacement.first->replaceAllUsesWith(Replacement.second);
        delete Replacement.first;
    }
    delete Plan.InitCopy;
    for (auto It = Plan.ElementPtrs.rbegin();
         It != Plan.ElementPtrs.rend(); ++It) {
        if (!(*It)->hasUses()) delete *It;
    }
    if (AI->hasUses()) return true;
    delete AI;
    return true;
}
//步骤 3.8：只依据已注册内建函数的 memset 语义识别非 volatile 调用；
//同名普通函数不能作为内存初始化证明。
bool LocalAggregateSimplifyPass::isNonVolatileMemset(CallInst *CI) {
    if (!CI || CI->getArgCount() < 4 || !CI->getBuiltinCallee() ||
        !BuildInFunction::isMemsetName(CI->getCalleeName())) {
        return false;
    }
    int32_t IsVolatile = 0;
    return CI->getArg(3) &&
           CI->getArg(3)->tryGetIntConst(IsVolatile) &&
           IsVolatile == 0;
}
//步骤 3.9：完整清零必须从对象起点开始，并以精确字节数覆盖整个数组；
//指针路径只允许保持起点的 bitcast 和全零 GEP。
bool LocalAggregateSimplifyPass::isWholeZeroMemset(
    CallInst *CI, AllocaInst *AI) {
    if (!CI || !AI || !isNonVolatileMemset(CI)) return false;
    int32_t FillValue = 0;
    int64_t ByteCount = 0;
    if (!CI->getArg(1) ||
        !CI->getArg(1)->tryGetIntConst(FillValue) ||
        FillValue != 0 || !CI->getArg(2) ||
        !CI->getArg(2)->tryGetInt64Const(ByteCount) || ByteCount < 0) {
        return false;
    }
    auto ObjectType = AllocaInst::getAllocatedType(AI);
    if (!ObjectType ||
        static_cast<uint64_t>(ByteCount) !=
            static_cast<uint64_t>(ObjectType->getSize())) {
        return false;
    }
    Value *Pointer = CI->getArg(0);
    std::unordered_set<Value *> Seen;
    while (Pointer && Seen.insert(Pointer).second) {
        if (auto *BC = dynamic_cast<BitCastInst *>(Pointer)) {
            Pointer = BC->getUnaryOperand();
            continue;
        }
        auto *GEP = dynamic_cast<GetElementPtrInst *>(Pointer);
        if (!GEP) break;
        for (Value *Index : GEP->getIndices()) {
            int ConstantIndex = 0;
            if (!Value::tryGetIntIndex(Index, ConstantIndex) ||
                ConstantIndex != 0) {
                return false;
            }
        }
        Pointer = GEP->getBasePointer();
    }
    return Pointer == AI;
}
//步骤 3.10：对“整对象清零后，以常量写补齐，再进行动态读取”的一维
//i32 局部表重建完整状态。触发条件只来自 use 闭包、支配、对象边界与表语义，
//不依赖函数名、测试规模或运行时输入。
bool LocalAggregateSimplifyPass::foldPatchedLocalArrayLoads(
    AllocaInst *AI, const DominatorTree &DT, RangeInfo &RI,
    const LoopInfo &LI) {
    if (!AI || !AI->getParent()) return false;
    Function *Owner = AI->getParent()->getParent();
    if (!Owner || Owner->front() != AI->getParent()) return false;
    auto ArrayType = std::dynamic_pointer_cast<ARRAYType>(
        AllocaInst::getAllocatedType(AI));
    if (!ArrayType || ArrayType->getElementCount() <= 0 ||
        ArrayType->getElementCount() > MaxFastSynthArrayElements) {
        return false;
    }
    auto ElementType = ArrayType->getElementType();
    if (!ElementType || !ElementType->isInt() || ElementType->isBool() ||
        ElementType->getIntegerBitWidth() != 32) {
        return false;
    }
    const int Count = ArrayType->getElementCount();
    CallInst *Initializer = nullptr;
    std::vector<StoreInst *> PatchStores;
    std::vector<LoadInst *> Loads;
    std::map<int, int32_t> PatchValues;
    size_t PointerCount = 0;
    //3.10.1 根对象只允许派生直接 [0,index] 元素 GEP；每个元素地址只可
    //用于读取、常量覆盖或本计划唯一的整对象清零。任一逃逸或未知 use 即拒绝。
    for (Use *RootUse : AI->getUses()) {
        if (!RootUse || RootUse->getValue() != AI) continue;
        if (++PointerCount >
            static_cast<size_t>(MaxFastSynthArrayElements * 2)) {
            return false;
        }
        auto *GEP = dynamic_cast<GetElementPtrInst *>(RootUse->getUser());
        Value *Index = nullptr;
        if (!GEP || GEP->getUserIndex(RootUse) != 0 ||
            !getDirectOneDimElementIndex(AI, GEP, Index) ||
            !gepIsProvablyWithinKnownObject(GEP, RI, DT)) {
            return false;
        }
        for (Use *ElementUse : GEP->getUses()) {
            if (!ElementUse || ElementUse->getValue() != GEP) continue;
            auto *I = dynamic_cast<Instruction *>(ElementUse->getUser());
            if (!I) return false;
            int OperandIndex = I->getUserIndex(ElementUse);
            if (auto *Load = dynamic_cast<LoadInst *>(I)) {
                if (OperandIndex != 0 ||
                    !IRType::hasSameShape(
                        Load->getIRType(), ElementType) ||
                    Loads.size() >=
                        static_cast<size_t>(MaxFastSynthArrayElements)) {
                    return false;
                }
                Loads.push_back(Load);
                continue;
            }
            if (auto *Store = dynamic_cast<StoreInst *>(I)) {
                int ConstantIndex = 0;
                int64_t ConstantValue = 0;
                Value *StoredValue = Store->getValueOperand();
                if (OperandIndex != 1 || !Store->fullyCoversItsPointer() ||
                    !Value::tryGetIntIndex(Index, ConstantIndex) ||
                    ConstantIndex < 0 || ConstantIndex >= Count ||
                    !StoredValue ||
                    !IRType::hasSameShape(
                        StoredValue->getIRType(), ElementType) ||
                    !StoredValue->tryGetInt64ConstAtWidth(
                        32, ConstantValue) ||
                    ConstantValue < std::numeric_limits<int32_t>::min() ||
                    ConstantValue > std::numeric_limits<int32_t>::max() ||
                    !PatchValues.emplace(
                        ConstantIndex,
                        static_cast<int32_t>(ConstantValue)).second) {
                    return false;
                }
                PatchStores.push_back(Store);
                continue;
            }
            auto *CI = dynamic_cast<CallInst *>(I);
            if (!CI || CI->getArgIndex(ElementUse) != 0 || Initializer ||
                !isWholeZeroMemset(CI, AI)) {
                return false;
            }
            Initializer = CI;
        }
    }
    if (!Initializer || Loads.empty() ||
        Initializer->getParent() != AI->getParent() ||
        LI.getLoopFor(Initializer->getParent())) {
        return false;
    }
    //3.10.2 清零先于全部访问；每个补丁写又必须支配每个读取。因此任一
    //动态 load 到达时，整表状态都精确等于“零表+全部常量补丁”。
    for (StoreInst *Store : PatchStores) {
        if (!DT.dominatesValue(Initializer, Store)) return false;
        for (LoadInst *Load : Loads)
            if (!DT.dominatesValue(Store, Load)) return false;
    }
    for (LoadInst *Load : Loads)
        if (!DT.dominatesValue(Initializer, Load)) return false;

    std::vector<int32_t> Values(static_cast<size_t>(Count), 0);
    for (const auto &Patch : PatchValues)
        Values[static_cast<size_t>(Patch.first)] = Patch.second;
    auto Expression = synthesizeFastInt32TableExpression(Values);
    if (!isSynthExpressionProfitable(Expression)) return false;

    //3.10.3 全部下标必须是定义良好的 i32，且其完整 GEP 已证明落在对象内。
    //先为所有 load 物化替换，任一失败就逆序回滚，不留下部分变换。
    std::vector<std::pair<LoadInst *, Value *>> Replacements;
    std::vector<Instruction *> Created;
    Replacements.reserve(Loads.size());
    for (LoadInst *Load : Loads) {
        auto *GEP = dynamic_cast<GetElementPtrInst *>(
            Load->getPointerOperand());
        Value *Index = nullptr;
        if (!getDirectOneDimElementIndex(AI, GEP, Index) ||
            !Index || !Index->getIRType() ||
            !Index->getIRType()->isInt() ||
            Index->getIRType()->isBool() ||
            Index->getIRType()->getIntegerBitWidth() != 32 ||
            valueMayDependOnUndef(Index) ||
            !gepIsProvablyWithinKnownObject(GEP, RI, DT)) {
            for (auto It = Created.rbegin(); It != Created.rend(); ++It)
                delete *It;
            return false;
        }
        int ConstantIndex = 0;
        Value *Replacement = nullptr;
        if (Value::tryGetIntIndex(Index, ConstantIndex)) {
            if (ConstantIndex < 0 || ConstantIndex >= Count) {
                for (auto It = Created.rbegin(); It != Created.rend(); ++It)
                    delete *It;
                return false;
            }
            Replacement = Value::makeIntegerLike(
                Load, Values[static_cast<size_t>(ConstantIndex)]);
        } else {
            Replacement = materializeSynthExpression(
                Expression, Index, Load, Created);
        }
        if (!Replacement ||
            !IRType::hasSameShape(
                Load->getIRType(), Replacement->getIRType())) {
            for (auto It = Created.rbegin(); It != Created.rend(); ++It)
                delete *It;
            return false;
        }
        Replacements.push_back({Load, Replacement});
    }
    //3.10.4 所有证明和构造都完成后统一提交；写入与根对象由阶段5在确认
    //已无任何读取后删除，避免在本阶段制造半删除状态。
    for (auto &Replacement : Replacements) {
        Replacement.first->replaceAllUsesWith(Replacement.second);
        delete Replacement.first;
    }
    return true;
}
//步骤 4.4：为一个精确标量路径登记槽位类型；重复路径必须保持同一标量
//类型，新路径使总槽数超出预算时立即拒绝标量化。
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
//步骤 4.5：递归验证某个派生指针的完整 use 闭包。标量末端只允许 load/store；
//其余路径只能继续经过可展平的标量常量偏移或零基常量聚合 GEP。
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
        //4.5.1 标量偏移先把当前路径展平成线性编号，再按偏移反解回根聚合
        //路径；越界、类型不匹配或同一 GEP 映射冲突均拒绝。
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
        //4.5.2 聚合派生 GEP 追加全常量相对路径；末端只能是标量或可继续
        //递归的数组，其他复合类型不在本 pass 的标量化范围内。
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
//步骤 4 主流程先建立完整标量化计划，再创建槽位并替换地址；计划要求对象
//地址不逃逸、所有访问路径恒定，且可选初始化 memcpy 的顺序完全可证明。
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
    //4.1 收集根 alloca 的直接用途：至多一次完整只读 memcpy 初始化，其余
    //必须是全常量 GEP；遇到标量末端时登记对应槽位。
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
    //4.2 从每个直接 GEP 递归验证透明派生指针与最终内存访问，补全槽位表。
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
    //4.3 有初始化 memcpy 时，同块访问必须位于 memcpy 之后，跨块访问只在
    //初始化位于 alloca 块时接受；timer 不访问对象，不参与内存次序判定。
    if (InitCopy) {
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
    //4.4 在修改 IR 前提取每个实际槽位的初始化常量，并按真实标量类型核验，
    //防止相同存储大小的不同整数位宽被混用。
    std::map<std::vector<int>, Value *> InitialValues;
    if (InitSource) {
        for (auto &Entry : SlotTypes) {
            Value *Init = Variable::getConstantElement(InitSource, Entry.first);
            if (!Init ||
                !IRType::hasSameScalarType(Init->getIRType(),
                                           Entry.second))
                return false;
            InitialValues[Entry.first] = Init;
        }
    }
    //4.5 在原聚合 alloca 前为每条被访问路径创建标量 alloca。
    std::map<std::vector<int>, AllocaInst *> Slots;
    for (auto &Entry : SlotTypes) {
        auto *Slot = new AllocaInst(Entry.second);
        if (!AI->insertBefore(Slot)) {
            delete Slot;
            return false;
        }
        Slots[Entry.first] = Slot;
    }
    //4.6 以标量 store 替代整对象初始化；store 插在原 memcpy 之前，或在无
    //memcpy 时插在原 alloca 之前，保持后续访问可见的初值。
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
    //4.7 先逆序改写更深的透明派生 GEP，再处理根的直接 GEP，保证先叶子后
    //父节点删除；只有精确落到已建槽位的地址才替换。
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
    //4.8 删除已被标量 store 取代的 memcpy，并在根 alloca 确认无 use 后删除。
    if (InitCopy) delete InitCopy;
    if (AI->hasUses()) return false;
    delete AI;
    return true;
}
//步骤 5.1：遍历聚合指针闭包，收集纯写入和透明派生地址。load、作为 memcpy
//源、保存指针本身或传给未知调用均构成可观察用途，使整个对象不可删除。
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
                CI && ((CallInst::isNonVolatileMemcpy(CI) &&
                        CI->getArgIndex(U) == 0) ||
                       (isWholeZeroMemset(CI, AI) &&
                        CI->getArgIndex(U) == 0))) {
                if (SeenWrites.insert(I).second) Result.Writes.push_back(I);
                continue;
            }
            Result.IsDead = false;
            break;
        }
    }
    return Result;
}
//步骤 5.2：先删除全部写入，再按发现顺序的逆序删除派生指针，最后仅在根
//alloca 已无 use 时删除对象；逆序保证叶子先于父地址消失。
bool LocalAggregateSimplifyPass::deleteDeadAggregate(AllocaInst *AI) {
    LocalAggregateDeadUses Uses = collectDeadAggregateUses(AI);
    if (!Uses.IsDead) return false;
    for (Instruction *I : Uses.Writes)
        delete I;
    for (auto It = Uses.DerivedPointers.rbegin();
         It != Uses.DerivedPointers.rend(); ++It) {
        Instruction *I = *It;
        if (!I->hasUses()) delete I;
    }
    if (AI->hasUses()) return false;
    delete AI;
    return true;
}
//每个阶段重新快照仍存活的数组 alloca，避免前一阶段删除对象后留下悬空项。
std::vector<AllocaInst *>
LocalAggregateSimplifyPass::collectArrayAllocas(Function &F) {
    std::vector<AllocaInst *> Aggregates;
    for (AllocaInst *AI : collectInstructions<AllocaInst>(F))
        if (AllocaInst::isArrayAllocation(AI)) Aggregates.push_back(AI);
    return Aggregates;
}
//按既定阶段运行局部聚合简化并返回分析保留状态
PreservationStatus
LocalAggregateSimplifyPass::run(Function &F,
                                FunctionAnalysisPassManager &FAM) {
    bool Changed = false;
    //1. 先折叠只读全局 load 与可综合动态查表。
    Changed |= foldReadOnlyGlobalLoads(F, FAM);
    //2. 转发同块内由完整常量拷贝和常量覆盖确定的元素 load。
    for (AllocaInst *AI : collectArrayAllocas(F))
        Changed |= forwardConstantLoads(AI);
    //3. 在数组查询预算内整体展开只读常量初始化的小数组。前两阶段或前一个
    //数组一旦改写IR，就先精确失效旧AA/MemorySSA，再为下一个数组重建快照。
    DominatorTree *DT = nullptr;
    BasicAAResult *AA = nullptr;
    LoopInfo *LI = nullptr;
    MemorySSAResult *MSSA = nullptr;
    RangeInfo *RI = nullptr;
    auto InvalidateMemoryAnalyses = [&]() {
        PreservationStatus PA;
        PA.keep<DominantAnalysis>();
        PA.keep<PostDominantAnalysis>();
        PA.keep<LoopAnalysis>();
        PA.discard<BasicAliasAnalysis>();
        PA.discard<MemorySSAAnalysis>();
        PA.discard<RangeAnalysis>();
        PA.discard<SideEffectAnalysis>();
        FAM.invalidate(F, std::move(PA));
        DT = nullptr;
        AA = nullptr;
        LI = nullptr;
        MSSA = nullptr;
        RI = nullptr;
    };
    std::vector<AllocaInst *> ReadOnlyArrays = collectArrayAllocas(F);
    if (!ReadOnlyArrays.empty() && Changed)
        InvalidateMemoryAnalyses();
    constexpr std::size_t MaxMemorySSAArrayQueries = 64;
    std::size_t QueriedArrays = 0;
    for (AllocaInst *AI : ReadOnlyArrays) {
        if (QueriedArrays++ >= MaxMemorySSAArrayQueries) break;
        if (!DT) {
            DT = &FAM.getResult<DominantAnalysis>(F);
            AA = &FAM.getResult<BasicAliasAnalysis>(F);
            LI = &FAM.getResult<LoopAnalysis>(F);
            MSSA = &FAM.getResult<MemorySSAAnalysis>(F);
            RI = &FAM.getResult<RangeAnalysis>(F);
        }
        if (foldPatchedLocalArrayLoads(AI, *DT, *RI, *LI)) {
            Changed = true;
            InvalidateMemoryAnalyses();
            continue;
        }
        if (!expandReadOnlyConstantArrayLoads(
                F, AI, *DT, *MSSA, *AA, *LI, FAM)) {
            continue;
        }
        Changed = true;
        InvalidateMemoryAnalyses();
    }
    //4. 把剩余仅有常量路径访问的聚合拆成标量槽。
    for (AllocaInst *AI : collectArrayAllocas(F))
        Changed |= scalarizeConstantIndexedAggregate(AI);
    //5. 删除最后仍只被写入、从未被观察的聚合对象。
    for (AllocaInst *AI : collectArrayAllocas(F))
        Changed |= deleteDeadAggregate(AI);
    //6. 任一阶段都可能删除内存操作、指针表达式或 alloca，保守失效全部函数
    //分析；完全未改写时保留所有分析结果。
    return Changed ? PreservationStatus::none()
                   : PreservationStatus::all();
}
