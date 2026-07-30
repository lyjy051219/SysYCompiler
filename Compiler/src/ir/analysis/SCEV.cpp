#include "ir/analysis/SCEV.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <unordered_set>
//返回SCEV整数类型支持的有效位宽
static std::optional<unsigned>
getSCEVIntegerBitWidth(const std::shared_ptr<IRType> &Ty) {
    if (!Ty || !Ty->isInt()) return std::nullopt;
    const int Bits = Ty->getIntegerBitWidth(0);
    if (Bits <= 0 || Bits > 64) return std::nullopt;
    return static_cast<unsigned>(Bits);
}
//判断给定数学整数是否能够由目标IR整数类型精确表示
static bool isSCEVSignedValueRepresentable(
    __int128 Value, const std::shared_ptr<IRType> &Ty) {
    auto Bits = getSCEVIntegerBitWidth(Ty);
    if (!Bits) return false;
    if (*Bits == 1)
        return Value == 0 || Value == 1;
    const __int128 Minimum =
        -static_cast<__int128>(uint64_t{1} << (*Bits - 1));
    const __int128 Maximum =
        static_cast<__int128>(uint64_t{1} << (*Bits - 1)) - 1;
    return Value >= Minimum && Value <= Maximum;
}
//返回给定有符号IR整数类型的最小值
static std::optional<int64_t>
getSCEVSignedMinimum(const std::shared_ptr<IRType> &Ty) {
    auto Bits = getSCEVIntegerBitWidth(Ty);
    if (!Bits || *Bits == 1) return std::nullopt;
    if (*Bits == 64) return std::numeric_limits<int64_t>::min();
    return -static_cast<int64_t>(uint64_t{1} << (*Bits - 1));
}
//判断当前节点是否为给定整数常量
bool SCEVExpr::isConstant(int64_t V) const {
    return Kind == SCEVExprKind::Constant && ConstValue == V;
}
//返回当前加法递推多项式的次数
unsigned SCEVAddRecInfo::getDegree() const {
    return Coefficients.empty()
        ? 0
        : static_cast<unsigned>(Coefficients.size() - 1);
}
//判断当前加法递推是否是一阶仿射递推
bool SCEVAddRecInfo::isAffine() const {
    return Coefficients.size() == 2;
}
//构造一个不绑定函数和依赖分析的空SCEV结果
SCEVResult::SCEVResult() = default;
//构造绑定给定函数、循环信息和支配树的SCEV结果
SCEVResult::SCEVResult(Function *F, LoopInfo *LoopInfoPtr, DominatorTree *DTPtr)
    : Func(F), LI(LoopInfoPtr), DT(DTPtr) {}
//移动构造SCEV结果并接管表达式与缓存存储
SCEVResult::SCEVResult(SCEVResult &&) = default;
//移动赋值SCEV结果并接管表达式与缓存存储
SCEVResult &SCEVResult::operator=(SCEVResult &&) = default;
//判断当前节点是否为循环递推表达式
bool SCEVExpr::isRecurrence() const {
    SCEVExprKind K = Kind;
    return K == SCEVExprKind::AddRec ||
           K == SCEVExprKind::MulRec ||
           K == SCEVExprKind::DivRec;
}
//判断当前表达式及其子表达式是否引用给定循环
bool SCEVExpr::mentionsLoop(const Loop *L) const {
    if (!L) return false;
    if (isRecurrence() && RecLoop == L) return true;
    for (const SCEVExpr *Op : Operands)
        if (Op && Op->mentionsLoop(L)) return true;
    return false;
}
//返回给定表达式种类的可读名称
const char *SCEVExpr::kindName(SCEVExprKind K) {
    switch (K) {
    case SCEVExprKind::Constant: return "const";
    case SCEVExprKind::Unknown: return "unknown";
    case SCEVExprKind::Add: return "add";
    case SCEVExprKind::Sub: return "sub";
    case SCEVExprKind::Mul: return "mul";
    case SCEVExprKind::Div: return "div";
    case SCEVExprKind::Mod: return "mod";
    case SCEVExprKind::Shl: return "shl";
    case SCEVExprKind::LShr: return "lshr";
    case SCEVExprKind::AShr: return "ashr";
    case SCEVExprKind::BitAnd: return "and";
    case SCEVExprKind::BitOr: return "or";
    case SCEVExprKind::BitXor: return "xor";
    case SCEVExprKind::Binomial: return "binomial";
    case SCEVExprKind::Min: return "min";
    case SCEVExprKind::Max: return "max";
    case SCEVExprKind::Select: return "select";
    case SCEVExprKind::Cast: return "cast";
    case SCEVExprKind::Compare: return "cmp";
    case SCEVExprKind::AddRec: return "addrec";
    case SCEVExprKind::MulRec: return "mulrec";
    case SCEVExprKind::DivRec: return "divrec";
    }
    return "bad";
}
//返回给定类型转换种类的可读名称
const char *SCEVExpr::castName(SCEVCastKind K) {
    switch (K) {
    case SCEVCastKind::Trunc: return "trunc";
    case SCEVCastKind::ZExt: return "zext";
    case SCEVCastKind::SExt: return "sext";
    case SCEVCastKind::BitCast: return "bitcast";
    }
    return "badcast";
}
//返回给定比较谓词的可读名称
const char *SCEVExpr::predicateName(BinaryInst::Operation Op) {
    switch (Op) {
    case BinaryInst::E: return "eq";
    case BinaryInst::NE: return "ne";
    case BinaryInst::GE: return "ge";
    case BinaryInst::L: return "lt";
    case BinaryInst::LE: return "le";
    case BinaryInst::G: return "gt";
    default: return "badpred";
    }
}
//根据PreservationStatus判断当前SCEV分析结果是否失效
bool SCEVResult::invalidate(Function &, const PreservationStatus &PA) {
    return !PA.isKept(SCEVAnalysis::ID());
}
//返回给定IR值对应的SCEV表达式并按需创建缓存
const SCEVExpr *SCEVResult::getSCEV(Value *V) {
    //1.命中缓存直接返回，正在递归构造的值作为Unknown打断循环依赖
    if (!V) return nullptr;
    auto It = ValueCache.find(V);
    if (It != ValueCache.end()) return It->second;
    if (ActiveValues[V]) return makeUnknown(V);
    ActiveValues[V] = true;
    //2.按照IR指令类型构造表达式并缓存，空结果统一降级为Unknown
    const SCEVExpr *E = create(V);
    ActiveValues[V] = false;
    if (!E) E = makeUnknown(V);
    ValueCache[V] = E;
    return E;
}
//返回给定IR值能够表示的一阶仿射递推信息
std::optional<SCEVAffineInfo> SCEVResult::getAffine(Value *V) {
    return getAffine(getSCEV(V));
}
//返回给定SCEV表达式能够表示的一阶仿射递推信息
std::optional<SCEVAffineInfo> SCEVResult::getAffine(const SCEVExpr *E) const {
    if (!E || E->Kind != SCEVExprKind::AddRec || E->Operands.size() != 2)
        return std::nullopt;
    SCEVAffineInfo Info;
    Info.RecLoop = E->RecLoop;
    Info.Start = E->Operands[0];
    Info.Step = E->Operands[1];
    Info.ConstantStart = asConstant(Info.Start);
    Info.ConstantStep = asConstant(Info.Step);
    return Info;
}
//返回给定IR值能够表示的加法递推信息
std::optional<SCEVAddRecInfo>
SCEVResult::getAddRecurrence(Value *V) {
    return getAddRecurrence(getSCEV(V));
}
//返回给定SCEV表达式能够表示的加法递推信息
std::optional<SCEVAddRecInfo>
SCEVResult::getAddRecurrence(const SCEVExpr *E) const {
    if (!E || E->Kind != SCEVExprKind::AddRec || !E->RecLoop ||
        E->Operands.size() < 2 ||
        E->Operands.size() - 1 > MaxAddRecDegree) {
        return std::nullopt;
    }
    SCEVAddRecInfo Info;
    Info.RecLoop = E->RecLoop;
    Info.Coefficients = E->Operands;
    return Info;
}
//返回给定循环的总迭代次数表达式
const SCEVExpr *SCEVResult::getTripCount(const Loop *L) {
    if (!L) return nullptr;
    auto It = TripCountCache.find(L);
    if (It != TripCountCache.end()) return It->second;
    auto Info = analyzeLoopTripCount(L);
    if (!Info || !Info->TripCount) return nullptr;
    TripCountCache[L] = Info->TripCount;
    if (Info->BackedgeTakenCount)
        BackedgeCountCache[L] = Info->BackedgeTakenCount;
    return Info->TripCount;
}
//返回给定循环的回边执行次数表达式
const SCEVExpr *SCEVResult::getBackedgeTakenCount(const Loop *L) {
    if (!L) return nullptr;
    auto It = BackedgeCountCache.find(L);
    if (It != BackedgeCountCache.end()) return It->second;
    auto Info = analyzeLoopTripCount(L);
    if (!Info || !Info->BackedgeTakenCount) return nullptr;
    if (Info->TripCount) TripCountCache[L] = Info->TripCount;
    BackedgeCountCache[L] = Info->BackedgeTakenCount;
    return Info->BackedgeTakenCount;
}
//返回给定循环能够证明的较小常量总迭代次数
std::optional<int64_t>
SCEVResult::getSmallConstantTripCount(const Loop *L) {
    auto Info = analyzeLoopTripCount(L);
    if (!Info || !Info->ProvenNoWrap) return std::nullopt;
    return Info->ConstantTripCount;
}
//返回给定循环能够证明的较小常量回边执行次数
std::optional<int64_t>
SCEVResult::getSmallConstantBackedgeTakenCount(const Loop *L) {
    auto Info = analyzeLoopTripCount(L);
    if (!Info || !Info->ProvenNoWrap) return std::nullopt;
    return Info->ConstantBackedgeTakenCount;
}
//返回给定循环总迭代次数能够证明的常量倍数
std::optional<int64_t>
SCEVResult::getSmallConstantTripMultiple(const Loop *L) {
    auto Info = analyzeLoopTripCount(L);
    if (!Info || !Info->ProvenNoWrap) return std::nullopt;
    if (!Info->ConstantTripCount) return 1;
    int64_t C = *Info->ConstantTripCount;
    if (C == 0) return 1;
    if (C == std::numeric_limits<int64_t>::min()) return std::nullopt;
    return C < 0 ? -C : C;
}
//判断给定循环总迭代次数是否必为指定因子的倍数
bool SCEVResult::isTripCountMultipleOf(const Loop *L, int64_t Factor) {
    if (Factor <= 0) return false;
    auto Info = analyzeLoopTripCount(L);
    if (!Info || !Info->ProvenNoWrap || !Info->ConstantTripCount) return false;
    return (*Info->ConstantTripCount % Factor) == 0;
}
//按照结构化键驻留表达式并复用已有节点
const SCEVExpr *SCEVResult::intern(std::unique_ptr<SCEVExpr> E) {
    auto It = Interned.find(E->Key);
    if (It != Interned.end()) return It->second;
    const SCEVExpr *Raw = E.get();
    ExprStorage.push_back(std::move(E));
    Interned[Raw->Key] = Raw;
    return Raw;
}
//返回给定IR类型用于表达式驻留的结构化键
std::string SCEVResult::typeKey(std::shared_ptr<IRType> Ty) const {
    return "ty" + IRKey::type(Ty);
}
//创建或复用给定整数值和类型对应的常量表达式
const SCEVExpr *SCEVResult::makeConstant(int64_t V, std::shared_ptr<IRType> Ty) {
    if (!Ty) Ty = INTType::getInt32Ty();
    //SCEV 对 i1 统一采用 ConstBool 的逻辑 0/1 表示；位模式的符号解释只在
    //具体 signed 操作内完成，不能让同一个 true 同时以 1 和 -1 驻留。
    if (auto Bits = getSCEVIntegerBitWidth(Ty); Bits && *Bits == 1)
        V = V == 0 ? 0 : 1;
    auto E = std::make_unique<SCEVExpr>();
    E->Kind = SCEVExprKind::Constant;
    E->ConstValue = V;
    E->Ty = Ty;
    E->Key = std::string("c:") + typeKey(Ty) + ":" + std::to_string(V);
    return intern(std::move(E));
}
//创建或复用给定原始IR值对应的未知表达式
const SCEVExpr *SCEVResult::makeUnknown(Value *V) {
    auto E = std::make_unique<SCEVExpr>();
    E->Kind = SCEVExprKind::Unknown;
    E->RawValue = V;
    E->Ty = V ? V->getIRType() : nullptr;
    E->Key = std::string("u:") + IRKey::pointer(V);
    return intern(std::move(E));
}
//创建或折叠给定种类和操作数对应的二元表达式
const SCEVExpr *SCEVResult::makeBinary(SCEVExprKind K, const SCEVExpr *L,
                                       const SCEVExpr *R,
                                       std::shared_ptr<IRType> Ty) {
    if (!L || !R) return nullptr;
    const bool IsCommutative =
        K == SCEVExprKind::Add || K == SCEVExprKind::Mul ||
        K == SCEVExprKind::Min || K == SCEVExprKind::Max ||
        K == SCEVExprKind::BitAnd || K == SCEVExprKind::BitOr ||
        K == SCEVExprKind::BitXor;
    if (IsCommutative &&
        L->Key > R->Key) {
        std::swap(L, R);
    }
    if (sameExpr(L, R)) {
        if (K == SCEVExprKind::Min || K == SCEVExprKind::Max ||
            K == SCEVExprKind::BitAnd || K == SCEVExprKind::BitOr) {
            return L;
        }
        if (K == SCEVExprKind::BitXor &&
            isSafeToDiscard(L))
            return makeConstant(0, Ty ? Ty : L->Ty);
    }
    if ((K == SCEVExprKind::Shl || K == SCEVExprKind::LShr ||
         K == SCEVExprKind::AShr) &&
        R->isConstant(0)) {
        return L;
    }
    if ((K == SCEVExprKind::BitOr || K == SCEVExprKind::BitXor) &&
        (L->isConstant(0) || R->isConstant(0))) {
        return L->isConstant(0) ? R : L;
    }
    if (K == SCEVExprKind::BitAnd &&
        (L->isConstant(0) || R->isConstant(0))) {
        const SCEVExpr *Discarded =
            L->isConstant(0) ? R : L;
        if (isSafeToDiscard(Discarded)) {
            return makeConstant(
                0, Ty ? Ty : (L->Ty ? L->Ty : R->Ty));
        }
    }
    if ((K == SCEVExprKind::Min || K == SCEVExprKind::Max)) {
        if (auto LC = asConstant(L)) {
            if (auto RC = asConstant(R)) {
                std::shared_ptr<IRType> ResultTy =
                    Ty ? Ty : (L->Ty ? L->Ty : R->Ty);
                auto ResultBits = getSCEVIntegerBitWidth(ResultTy);
                //i1 的 true 在 SCEV 中保存为逻辑 1，而 signed i1 位模式代表
                //-1；在缺少显式谓词时不能把它交给宿主整数 min/max。
                if (ResultBits && *ResultBits != 1 &&
                    isSCEVSignedValueRepresentable(*LC, ResultTy) &&
                    isSCEVSignedValueRepresentable(*RC, ResultTy)) {
                    int64_t V =
                        K == SCEVExprKind::Min
                            ? std::min(*LC, *RC)
                            : std::max(*LC, *RC);
                    return makeConstant(V, ResultTy);
                }
            }
        }
    }
    //位运算常量折叠严格按 IR 整数位宽进行；非法移位量只保留符号节点，
    //不能把潜在未定义行为转化成任意常量。
    if (auto LC = asConstant(L)) {
        if (auto RC = asConstant(R)) {
            std::shared_ptr<IRType> ResultTy =
                Ty ? Ty : (L->Ty ? L->Ty : R->Ty);
            const int Bits =
                ResultTy && ResultTy->isInt()
                    ? ResultTy->getIntegerBitWidth(32)
                    : 0;
            if (Bits > 0 && Bits <= 64) {
                const uint64_t Mask =
                    Bits == 64 ? ~uint64_t{0}
                               : ((uint64_t{1} << Bits) - 1);
                const uint64_t LRaw =
                    static_cast<uint64_t>(*LC) & Mask;
                const uint64_t RRaw =
                    static_cast<uint64_t>(*RC) & Mask;
                uint64_t ResultRaw = 0;
                bool Folded = true;
                switch (K) {
                case SCEVExprKind::BitAnd:
                    ResultRaw = LRaw & RRaw;
                    break;
                case SCEVExprKind::BitOr:
                    ResultRaw = LRaw | RRaw;
                    break;
                case SCEVExprKind::BitXor:
                    ResultRaw = LRaw ^ RRaw;
                    break;
                case SCEVExprKind::Shl:
                    if (*RC < 0 || static_cast<uint64_t>(*RC) >=
                                       static_cast<uint64_t>(Bits)) {
                        Folded = false;
                    } else {
                        ResultRaw =
                            (LRaw << static_cast<unsigned>(*RC)) & Mask;
                    }
                    break;
                case SCEVExprKind::LShr:
                    if (*RC < 0 || static_cast<uint64_t>(*RC) >=
                                       static_cast<uint64_t>(Bits)) {
                        Folded = false;
                    } else {
                        ResultRaw =
                            LRaw >> static_cast<unsigned>(*RC);
                    }
                    break;
                case SCEVExprKind::AShr:
                    if (*RC < 0 || static_cast<uint64_t>(*RC) >=
                                       static_cast<uint64_t>(Bits)) {
                        Folded = false;
                    } else {
                        const unsigned Shift =
                            static_cast<unsigned>(*RC);
                        ResultRaw = LRaw >> Shift;
                        const uint64_t SignBit =
                            uint64_t{1} << (Bits - 1);
                        if ((LRaw & SignBit) != 0 && Shift != 0) {
                            ResultRaw |=
                                (~uint64_t{0} << (Bits - Shift));
                        }
                        ResultRaw &= Mask;
                    }
                    break;
                default:
                    Folded = false;
                    break;
                }
                if (Folded) {
                    if (Bits == 1)
                        return makeConstant(
                            ResultRaw == 0 ? 0 : 1, ResultTy);
                    int64_t Result = 0;
                    std::memcpy(&Result, &ResultRaw, sizeof(Result));
                    return makeConstant(
                        IRMath::normalizeSignedToWidth(Result, Bits),
                        ResultTy);
                }
            }
        }
    }
    auto E = std::make_unique<SCEVExpr>();
    E->Kind = K;
    E->Ty = Ty ? Ty : (L->Ty ? L->Ty : R->Ty);
    E->Operands = {L, R};
    E->Key = std::string(SCEVExpr::kindName(K)) + ":" + typeKey(E->Ty) + "(" +
             L->Key + "," + R->Key + ")";
    return intern(std::move(E));
}
//创建或折叠给定转换种类、操作数和目标类型对应的转换表达式
const SCEVExpr *SCEVResult::makeCast(SCEVCastKind K, const SCEVExpr *Op,
                                     std::shared_ptr<IRType> Ty) {
    if (!Op) return nullptr;
    if (K == SCEVCastKind::BitCast && Op->Ty == Ty) return Op;
    if (Op->Kind == SCEVExprKind::Constant && Op->Ty && Ty &&
        Op->Ty->isInt() && Ty->isInt()) {
        const int SourceBits = Op->Ty->getIntegerBitWidth(32);
        const int TargetBits = Ty->getIntegerBitWidth(32);
        if (SourceBits > 0 && SourceBits <= 64 && TargetBits > 0 &&
            TargetBits <= 64) {
            auto Mask = [](int Bits) -> uint64_t {
                return Bits == 64 ? ~uint64_t{0}
                                  : ((uint64_t{1} << Bits) - 1);
            };
            auto SignedFromRaw = [](uint64_t Raw) {
                int64_t Result = 0;
                std::memcpy(&Result, &Raw, sizeof(Result));
                return Result;
            };
            const uint64_t SourceRaw =
                static_cast<uint64_t>(Op->ConstValue) & Mask(SourceBits);
            int64_t V = 0;
            bool Folded = true;
            switch (K) {
            case SCEVCastKind::Trunc: {
                if (TargetBits >= SourceBits) {
                    Folded = false;
                    break;
                }
                const uint64_t Raw = SourceRaw & Mask(TargetBits);
                //i1 常量在 SCEV 内保持逻辑 0/1，不把 true 规范成 signed -1。
                V = TargetBits == 1
                    ? (Raw == 0 ? 0 : 1)
                    : IRMath::normalizeSignedToWidth(
                          SignedFromRaw(Raw), TargetBits);
                break;
            }
            case SCEVCastKind::ZExt:
                if (TargetBits <= SourceBits) Folded = false;
                else V = SignedFromRaw(SourceRaw);
                break;
            case SCEVCastKind::SExt:
                if (TargetBits <= SourceBits) {
                    Folded = false;
                } else {
                    V = IRMath::normalizeSignedToWidth(
                        SignedFromRaw(SourceRaw), SourceBits);
                }
                break;
            case SCEVCastKind::BitCast:
                if (TargetBits != SourceBits) Folded = false;
                else if (TargetBits == 1)
                    V = SourceRaw == 0 ? 0 : 1;
                else
                    V = IRMath::normalizeSignedToWidth(
                        SignedFromRaw(SourceRaw), TargetBits);
                break;
            }
            if (Folded) return makeConstant(V, Ty);
        }
    }
    if (Op->Kind == SCEVExprKind::Cast) {
        const SCEVExpr *Inner = Op->Operands.empty() ? nullptr : Op->Operands[0];
        if (Inner && K == SCEVCastKind::Trunc && Inner->Ty == Ty &&
            (Op->CastKind == SCEVCastKind::ZExt ||
             Op->CastKind == SCEVCastKind::SExt ||
             Op->CastKind == SCEVCastKind::BitCast)) {
            return Inner;
        }
        if (Op->CastKind == K) {
            return makeCast(K, Inner, Ty);
        }
    }
    auto E = std::make_unique<SCEVExpr>();
    E->Kind = SCEVExprKind::Cast;
    E->CastKind = K;
    E->Ty = Ty;
    E->Operands = {Op};
    E->Key = std::string("cast:") + SCEVExpr::castName(K) + ":" + typeKey(Ty) +
             "(" + Op->Key + ")";
    return intern(std::move(E));
}
//创建或折叠给定谓词和操作数对应的比较表达式
const SCEVExpr *SCEVResult::makeCompare(BinaryInst::Operation Pred,
                                        const SCEVExpr *L,
                                        const SCEVExpr *R) {
    if (!L || !R) return nullptr;
    if (auto LC = asConstant(L)) {
        if (auto RC = asConstant(R)) {
            const bool SameScalarType =
                L->Ty && R->Ty &&
                L->Ty->hasSameScalarType(*R->Ty);
            auto OperandBits = getSCEVIntegerBitWidth(L->Ty);
            const bool IsEquality =
                Pred == BinaryInst::E || Pred == BinaryInst::NE;
            //i1 true 以逻辑 1 保存，但 signed icmp 应把其位模式解释为 -1。
            //等值比较与表示无关；顺序比较保留节点，避免以宿主 0/1 排序。
            const bool CanFoldPredicate =
                !OperandBits || *OperandBits != 1 || IsEquality;
            if (SameScalarType && CanFoldPredicate &&
                isSCEVSignedValueRepresentable(*LC, L->Ty) &&
                isSCEVSignedValueRepresentable(*RC, R->Ty)) {
                bool Res = false;
                switch (Pred) {
                case BinaryInst::E:  Res = *LC == *RC; break;
                case BinaryInst::NE: Res = *LC != *RC; break;
                case BinaryInst::GE: Res = *LC >= *RC; break;
                case BinaryInst::L:  Res = *LC <  *RC; break;
                case BinaryInst::LE: Res = *LC <= *RC; break;
                case BinaryInst::G:  Res = *LC >  *RC; break;
                default: break;
                }
                return makeConstant(
                    Res ? 1 : 0, INTType::getBoolTy());
            }
        }
    }
    auto E = std::make_unique<SCEVExpr>();
    E->Kind = SCEVExprKind::Compare;
    E->Predicate = Pred;
    E->Ty = INTType::getBoolTy();
    E->Operands = {L, R};
    E->Key = std::string("cmp:") + SCEVExpr::predicateName(Pred) + "(" + L->Key + "," +
             R->Key + ")";
    return intern(std::move(E));
}
//创建或折叠条件、真值和假值对应的选择表达式
const SCEVExpr *SCEVResult::makeSelect(
    const SCEVExpr *Cond, const SCEVExpr *TrueExpr,
    const SCEVExpr *FalseExpr, std::shared_ptr<IRType> Ty) {
    if (!Cond || !TrueExpr || !FalseExpr) return nullptr;
    if (sameExpr(TrueExpr, FalseExpr) &&
        isSafeToDiscard(Cond)) {
        return TrueExpr;
    }
    if (auto C = asConstant(Cond))
        return *C != 0 ? TrueExpr : FalseExpr;
    auto E = std::make_unique<SCEVExpr>();
    E->Kind = SCEVExprKind::Select;
    E->Ty = Ty ? Ty : (TrueExpr->Ty ? TrueExpr->Ty : FalseExpr->Ty);
    E->Operands = {Cond, TrueExpr, FalseExpr};
    E->Key = std::string("select:") + typeKey(E->Ty) + "(" +
             Cond->Key + "," + TrueExpr->Key + "," + FalseExpr->Key + ")";
    return intern(std::move(E));
}
//创建或折叠给定迭代数和阶数对应的精确二项式表达式
const SCEVExpr *SCEVResult::makeBinomial(
    const SCEVExpr *N, unsigned K, std::shared_ptr<IRType> Ty) {
    if (!N || K > MaxAddRecDegree) return nullptr;
    std::shared_ptr<IRType> ResultTy = Ty ? Ty : N->Ty;
    if (K == 0) return makeConstant(1, ResultTy);
    if (K == 1) return N;
    if (auto NC = asConstant(N)) {
        if (*NC >= 0) {
            const uint64_t UnsignedN = static_cast<uint64_t>(*NC);
            if (static_cast<uint64_t>(K) > UnsignedN)
                return makeConstant(0, ResultTy);
            __int128 Result = 1;
            bool Fits = true;
            for (unsigned I = 1; I <= K; ++I) {
                Result =
                    (Result *
                     static_cast<__int128>(UnsignedN - I + 1)) /
                    static_cast<__int128>(I);
                if (Result >
                    static_cast<__int128>(
                        std::numeric_limits<int64_t>::max())) {
                    Fits = false;
                    break;
                }
            }
            if (Fits &&
                isSCEVSignedValueRepresentable(Result, ResultTy))
                return makeConstant(static_cast<int64_t>(Result), ResultTy);
        }
    }
    const SCEVExpr *Degree =
        makeConstant(static_cast<int64_t>(K),
                     N->Ty ? N->Ty : INTType::getInt32Ty());
    auto E = std::make_unique<SCEVExpr>();
    E->Kind = SCEVExprKind::Binomial;
    E->Ty = ResultTy;
    E->Operands = {N, Degree};
    E->Key = std::string("binomial:") + typeKey(ResultTy) + "(" +
             N->Key + "," + std::to_string(K) + ")";
    return intern(std::move(E));
}
//创建或复用给定循环和二项式系数对应的加法递推表达式
const SCEVExpr *SCEVResult::makeAddRec(
    const Loop *L, const std::vector<const SCEVExpr *> &Coefficients,
    std::shared_ptr<IRType> Ty) {
    if (!L || Coefficients.empty()) return nullptr;
    std::vector<const SCEVExpr *> Normalized = Coefficients;
    while (Normalized.size() > 1 && Normalized.back() &&
           Normalized.back()->isConstant(0)) {
        Normalized.pop_back();
    }
    if (Normalized.size() == 1) return Normalized.front();
    if (Normalized.size() - 1 > MaxAddRecDegree) return nullptr;
    for (const SCEVExpr *Coefficient : Normalized) {
        if (!Coefficient || !isLoopInvariantExpr(Coefficient, L))
            return nullptr;
    }
    auto E = std::make_unique<SCEVExpr>();
    E->Kind = SCEVExprKind::AddRec;
    E->RecLoop = L;
    E->Ty = Ty ? Ty : Normalized.front()->Ty;
    E->Operands = Normalized;
    std::ostringstream Key;
    Key << "addrec@" << IRKey::pointer(L) << ":" << typeKey(E->Ty) << "(";
    for (std::size_t I = 0; I < Normalized.size(); ++I) {
        if (I != 0) Key << ",";
        Key << Normalized[I]->Key;
    }
    Key << ")";
    E->Key = Key.str();
    return intern(std::move(E));
}
//根据基础递推和更新增量构造高一阶加法递推
const SCEVExpr *SCEVResult::makeAddRecFromDelta(
    const Loop *L, const SCEVExpr *Base, const SCEVExpr *Delta,
    std::shared_ptr<IRType> Ty) {
    if (!L || !Base || !Delta || !isLoopInvariantExpr(Base, L))
        return nullptr;
    std::vector<const SCEVExpr *> Coefficients;
    Coefficients.push_back(Base);
    if (isPolynomialAddRec(Delta, L)) {
        Coefficients.insert(Coefficients.end(),
                            Delta->Operands.begin(), Delta->Operands.end());
    } else if (isLoopInvariantExpr(Delta, L)) {
        Coefficients.push_back(Delta);
    } else {
        return nullptr;
    }
    return makeAddRec(L, Coefficients, Ty);
}
//创建或复用给定种类、循环、起点和步长对应的递推表达式
const SCEVExpr *SCEVResult::makeRec(SCEVExprKind K, const Loop *L,
                                    const SCEVExpr *Base,
                                    const SCEVExpr *Step,
                                    std::shared_ptr<IRType> Ty) {
    if (!L || !Base || !Step) return nullptr;
    if (K == SCEVExprKind::AddRec)
        return makeAddRec(L, {Base, Step}, Ty);
    if ((K == SCEVExprKind::MulRec || K == SCEVExprKind::DivRec) &&
        Step->isConstant(1)) {
        return Base;
    }
    auto E = std::make_unique<SCEVExpr>();
    E->Kind = K;
    E->RecLoop = L;
    E->Ty = Ty ? Ty : Base->Ty;
    E->Operands = {Base, Step};
    E->Key = std::string(SCEVExpr::kindName(K)) + "@" + IRKey::pointer(L) + ":" +
             typeKey(E->Ty) + "(" + Base->Key + "," + Step->Key + ")";
    return intern(std::move(E));
}
//尝试从给定SCEV表达式提取整数常量
std::optional<int64_t> SCEVResult::asConstant(const SCEVExpr *E) const {
    if (!E || E->Kind != SCEVExprKind::Constant) return std::nullopt;
    return E->ConstValue;
}
//判断两个SCEV表达式是否表示同一驻留结构
bool SCEVResult::sameExpr(const SCEVExpr *A, const SCEVExpr *B) const {
    if (A == B) return true;
    if (!A || !B) return false;
    return A->Key == B->Key;
}
//构造给定SCEV表达式的相反数
const SCEVExpr *SCEVResult::neg(const SCEVExpr *E) {
    return mul(makeConstant(-1, E ? E->Ty : nullptr), E);
}
//返回两个二项式基函数乘积对指定结果阶数的整数系数
std::optional<int64_t> SCEVResult::getBinomialProductFactor(
    unsigned LeftDegree, unsigned RightDegree,
    unsigned ResultDegree) const {
    if (ResultDegree < std::max(LeftDegree, RightDegree) ||
        ResultDegree > LeftDegree + RightDegree ||
        ResultDegree > MaxAddRecDegree) {
        return std::nullopt;
    }
    auto Factorial = [](unsigned N) -> __int128 {
        __int128 Result = 1;
        for (unsigned I = 2; I <= N; ++I)
            Result *= static_cast<__int128>(I);
        return Result;
    };
    const unsigned Shared =
        LeftDegree + RightDegree - ResultDegree;
    const unsigned LeftOnly = ResultDegree - RightDegree;
    const unsigned RightOnly = ResultDegree - LeftDegree;
    const __int128 Denominator =
        Factorial(Shared) * Factorial(LeftOnly) *
        Factorial(RightOnly);
    if (Denominator == 0) return std::nullopt;
    const __int128 Factor =
        Factorial(ResultDegree) / Denominator;
    if (Factor <= 0 ||
        Factor >
            static_cast<__int128>(
                std::numeric_limits<int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<int64_t>(Factor);
}
//使用循环不变因子缩放加法递推的全部系数
const SCEVExpr *SCEVResult::scaleAddRec(
    const SCEVExpr *Rec, const SCEVExpr *Factor) {
    if (!Rec || Rec->Kind != SCEVExprKind::AddRec || !Rec->RecLoop ||
        !Factor || !isLoopInvariantExpr(Factor, Rec->RecLoop)) {
        return nullptr;
    }
    std::vector<const SCEVExpr *> Coefficients;
    Coefficients.reserve(Rec->Operands.size());
    for (const SCEVExpr *Coefficient : Rec->Operands) {
        const SCEVExpr *Scaled = mul(Coefficient, Factor);
        if (!Scaled) return nullptr;
        Coefficients.push_back(Scaled);
    }
    return makeAddRec(Rec->RecLoop, Coefficients, Rec->Ty);
}
//按照二项式基底乘法规则合并两个同循环加法递推
const SCEVExpr *SCEVResult::multiplyAddRecs(
    const SCEVExpr *L, const SCEVExpr *R) {
    if (!L || !R || L->Kind != SCEVExprKind::AddRec ||
        R->Kind != SCEVExprKind::AddRec ||
        !L->RecLoop || L->RecLoop != R->RecLoop ||
        L->Operands.empty() || R->Operands.empty()) {
        return nullptr;
    }
    const unsigned LeftDegree =
        static_cast<unsigned>(L->Operands.size() - 1);
    const unsigned RightDegree =
        static_cast<unsigned>(R->Operands.size() - 1);
    if (LeftDegree + RightDegree > MaxAddRecDegree)
        return nullptr;
    std::shared_ptr<IRType> Ty = L->Ty ? L->Ty : R->Ty;
    std::vector<const SCEVExpr *> Coefficients(
        LeftDegree + RightDegree + 1, makeConstant(0, Ty));
    for (unsigned I = 0; I <= LeftDegree; ++I) {
        for (unsigned J = 0; J <= RightDegree; ++J) {
            const SCEVExpr *Product =
                mul(L->Operands[I], R->Operands[J]);
            if (!Product) return nullptr;
            for (unsigned Degree = std::max(I, J);
                 Degree <= I + J; ++Degree) {
                auto Factor =
                    getBinomialProductFactor(I, J, Degree);
                if (!Factor) return nullptr;
                const SCEVExpr *Term = Product;
                if (*Factor != 1) {
                    Term = mul(
                        Term, makeConstant(*Factor, Ty));
                }
                if (!Term) return nullptr;
                Coefficients[Degree] =
                    add(Coefficients[Degree], Term);
                if (!Coefficients[Degree]) return nullptr;
            }
        }
    }
    return makeAddRec(L->RecLoop, Coefficients, Ty);
}
//构造或折叠两个SCEV表达式的和
const SCEVExpr *SCEVResult::add(const SCEVExpr *L, const SCEVExpr *R) {
    if (!L || !R) return nullptr;
    if (L->isConstant(0)) return R;
    if (R->isConstant(0)) return L;
    if (auto LC = asConstant(L)) {
        if (auto RC = asConstant(R)) {
            const __int128 Sum =
                static_cast<__int128>(*LC) +
                static_cast<__int128>(*RC);
            std::shared_ptr<IRType> ResultTy =
                L->Ty ? L->Ty : R->Ty;
            if (isSCEVSignedValueRepresentable(Sum, ResultTy)) {
                return makeConstant(
                    static_cast<int64_t>(Sum), ResultTy);
            }
        }
    }
    if (L->Kind == SCEVExprKind::AddRec && R->Kind == SCEVExprKind::AddRec &&
        L->RecLoop == R->RecLoop) {
        const std::size_t Count =
            std::max(L->Operands.size(), R->Operands.size());
        std::vector<const SCEVExpr *> Coefficients;
        Coefficients.reserve(Count);
        for (std::size_t I = 0; I < Count; ++I) {
            const SCEVExpr *LC =
                I < L->Operands.size()
                    ? L->Operands[I]
                    : makeConstant(0, L->Ty);
            const SCEVExpr *RC =
                I < R->Operands.size()
                    ? R->Operands[I]
                    : makeConstant(0, R->Ty);
            const SCEVExpr *Sum = add(LC, RC);
            if (!Sum) return nullptr;
            Coefficients.push_back(Sum);
        }
        return makeAddRec(L->RecLoop, Coefficients,
                          L->Ty ? L->Ty : R->Ty);
    }
    if (L->Kind == SCEVExprKind::AddRec &&
        isLoopInvariantExpr(R, L->RecLoop)) {
        std::vector<const SCEVExpr *> Coefficients = L->Operands;
        Coefficients[0] = add(Coefficients[0], R);
        return Coefficients[0]
            ? makeAddRec(L->RecLoop, Coefficients, L->Ty)
            : nullptr;
    }
    if (R->Kind == SCEVExprKind::AddRec &&
        isLoopInvariantExpr(L, R->RecLoop)) {
        std::vector<const SCEVExpr *> Coefficients = R->Operands;
        Coefficients[0] = add(Coefficients[0], L);
        return Coefficients[0]
            ? makeAddRec(R->RecLoop, Coefficients, R->Ty)
            : nullptr;
    }
    return makeBinary(SCEVExprKind::Add, L, R);
}
//构造或折叠两个SCEV表达式的差
const SCEVExpr *SCEVResult::sub(const SCEVExpr *L, const SCEVExpr *R) {
    if (!L || !R) return nullptr;
    if (sameExpr(L, R) && isSafeToDiscard(L))
        return makeConstant(0, L->Ty);
    if (R->isConstant(0)) return L;
    if (auto LC = asConstant(L)) {
        if (auto RC = asConstant(R)) {
            const __int128 Difference =
                static_cast<__int128>(*LC) -
                static_cast<__int128>(*RC);
            std::shared_ptr<IRType> ResultTy =
                L->Ty ? L->Ty : R->Ty;
            if (isSCEVSignedValueRepresentable(
                    Difference, ResultTy)) {
                return makeConstant(
                    static_cast<int64_t>(Difference), ResultTy);
            }
        }
    }
    if (R->Kind == SCEVExprKind::AddRec)
        return add(L, neg(R));
    if (L->Kind == SCEVExprKind::AddRec &&
        isLoopInvariantExpr(R, L->RecLoop)) {
        std::vector<const SCEVExpr *> Coefficients = L->Operands;
        Coefficients[0] = sub(Coefficients[0], R);
        return Coefficients[0]
            ? makeAddRec(L->RecLoop, Coefficients, L->Ty)
            : nullptr;
    }
    return makeBinary(SCEVExprKind::Sub, L, R);
}
//构造或折叠两个SCEV表达式的积
const SCEVExpr *SCEVResult::mul(const SCEVExpr *L, const SCEVExpr *R) {
    if (!L || !R) return nullptr;
    if (L->isConstant(0) || R->isConstant(0)) {
        const SCEVExpr *Discarded =
            L->isConstant(0) ? R : L;
        if (isSafeToDiscard(Discarded)) {
            return makeConstant(
                0, L->Ty ? L->Ty : R->Ty);
        }
    }
    if (L->isConstant(1)) return R;
    if (R->isConstant(1)) return L;
    if (auto LC = asConstant(L)) {
        if (auto RC = asConstant(R)) {
            const __int128 Product =
                static_cast<__int128>(*LC) *
                static_cast<__int128>(*RC);
            std::shared_ptr<IRType> ResultTy =
                L->Ty ? L->Ty : R->Ty;
            if (isSCEVSignedValueRepresentable(Product, ResultTy)) {
                return makeConstant(
                    static_cast<int64_t>(Product), ResultTy);
            }
        }
    }
    if (L->Kind == SCEVExprKind::AddRec &&
        R->Kind == SCEVExprKind::AddRec &&
        L->RecLoop == R->RecLoop) {
        if (const SCEVExpr *Product = multiplyAddRecs(L, R))
            return Product;
    }
    if (L->Kind == SCEVExprKind::AddRec &&
        isLoopInvariantExpr(R, L->RecLoop)) {
        if (const SCEVExpr *Scaled = scaleAddRec(L, R))
            return Scaled;
    }
    if (R->Kind == SCEVExprKind::AddRec &&
        isLoopInvariantExpr(L, R->RecLoop)) {
        if (const SCEVExpr *Scaled = scaleAddRec(R, L))
            return Scaled;
    }
    if (L->Key > R->Key) std::swap(L, R);
    return makeBinary(SCEVExprKind::Mul, L, R);
}
//构造或折叠两个SCEV表达式的商
const SCEVExpr *SCEVResult::div(const SCEVExpr *L, const SCEVExpr *R) {
    if (!L || !R) return nullptr;
    if (R->isConstant(1)) return L;
    if (auto LC = asConstant(L)) {
        if (auto RC = asConstant(R)) {
            std::shared_ptr<IRType> ResultTy =
                L->Ty ? L->Ty : R->Ty;
            auto SignedMinimum = getSCEVSignedMinimum(ResultTy);
            if (*RC != 0 && SignedMinimum &&
                !(*LC == *SignedMinimum && *RC == -1) &&
                isSCEVSignedValueRepresentable(*LC, ResultTy) &&
                isSCEVSignedValueRepresentable(*RC, ResultTy)) {
                return makeConstant(
                    *LC / *RC, ResultTy);
            }
        }
    }
    return makeBinary(SCEVExprKind::Div, L, R);
}
//构造或折叠两个SCEV表达式的取余结果
const SCEVExpr *SCEVResult::mod(const SCEVExpr *L, const SCEVExpr *R) {
    if (!L || !R) return nullptr;
    if (R->isConstant(1) && isSafeToDiscard(L))
        return makeConstant(0, L->Ty ? L->Ty : R->Ty);
    if (auto LC = asConstant(L)) {
        if (auto RC = asConstant(R)) {
            std::shared_ptr<IRType> ResultTy =
                L->Ty ? L->Ty : R->Ty;
            auto SignedMinimum = getSCEVSignedMinimum(ResultTy);
            if (*RC != 0 && SignedMinimum &&
                !(*LC == *SignedMinimum && *RC == -1) &&
                isSCEVSignedValueRepresentable(*LC, ResultTy) &&
                isSCEVSignedValueRepresentable(*RC, ResultTy)) {
                return makeConstant(
                    *LC % *RC, ResultTy);
            }
        }
    }
    return makeBinary(SCEVExprKind::Mod, L, R);
}
//根据IR值种类分派并构造对应的SCEV表达式
const SCEVExpr *SCEVResult::create(Value *V) {
    if (auto *CI = dynamic_cast<ConstInt *>(V))
        return makeConstant(CI->getVal(), V->getIRType());
    if (auto *CB = dynamic_cast<ConstBool *>(V))
        return makeConstant(CB->getVal() ? 1 : 0, V->getIRType());
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) return makeUnknown(V);
    if (auto *BI = dynamic_cast<BinaryInst *>(I))
        return createFromBinary(BI);
    if (auto *Phi = dynamic_cast<PhiInst *>(I))
        return createFromPhi(Phi);
    if (auto *Sel = dynamic_cast<SelectInst *>(I))
        return createFromSelect(Sel);
    if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I))
        return createFromGEP(GEP);
    if (dynamic_cast<ZextInst *>(I))
        return createFromCast(I, SCEVCastKind::ZExt);
    if (dynamic_cast<SextInst *>(I))
        return createFromCast(I, SCEVCastKind::SExt);
    if (dynamic_cast<TruncInst *>(I))
        return createFromCast(I, SCEVCastKind::Trunc);
    if (dynamic_cast<BitCastInst *>(I))
        return createFromCast(I, SCEVCastKind::BitCast);
    return makeUnknown(V);
}
//根据二元IR指令构造对应的SCEV表达式
const SCEVExpr *SCEVResult::createFromBinary(BinaryInst *BI) {
    Value *LHS = BI->getOperand(0);
    Value *RHS = BI->getOperand(1);
    if (!LHS || !RHS) return makeUnknown(BI);
    BinaryInst::Operation Op = BI->getOp();
    //当前 SCEV 算术域只定义整数位向量语义；浮点运算不能借同名
    //BinaryInst 操作码进入整数常量折叠。
    if (!LHS->getIRType() || !RHS->getIRType() ||
        !LHS->getIRType()->isInt() ||
        !RHS->getIRType()->isInt()) {
        return makeUnknown(BI);
    }
    if (BinaryInst::isCompareOp(Op))
        return makeCompare(Op, getSCEV(LHS), getSCEV(RHS));
    if (Op == BinaryInst::SHL) {
        if (auto *CI = dynamic_cast<ConstInt *>(RHS)) {
            const int64_t Shift = CI->getVal();
            const int Bits = LHS->getIRType() && LHS->getIRType()->isInt()
                ? LHS->getIRType()->getIntegerBitWidth(32)
                : 0;
            //只有 2^Shift 能由该有符号 IR 类型直接表示时才改写为乘法；
            //符号位移位仍保留 Shl 节点，避免制造越界的 typed constant。
            if (Shift >= 0 && Shift < Bits - 1 && Shift < 63) {
                return mul(
                    getSCEV(LHS),
                    makeConstant(
                        static_cast<int64_t>(uint64_t{1} << Shift),
                        LHS->getIRType()));
            }
        }
        return makeBinary(SCEVExprKind::Shl, getSCEV(LHS),
                          getSCEV(RHS), BI->getIRType());
    }
    switch (Op) {
    case BinaryInst::ADD: return add(getSCEV(LHS), getSCEV(RHS));
    case BinaryInst::SUB: return sub(getSCEV(LHS), getSCEV(RHS));
    case BinaryInst::MUL: return mul(getSCEV(LHS), getSCEV(RHS));
    case BinaryInst::DIV: return div(getSCEV(LHS), getSCEV(RHS));
    case BinaryInst::MOD: return mod(getSCEV(LHS), getSCEV(RHS));
    case BinaryInst::LSHR:
        return makeBinary(SCEVExprKind::LShr, getSCEV(LHS),
                          getSCEV(RHS), BI->getIRType());
    case BinaryInst::ASHR:
        return makeBinary(SCEVExprKind::AShr, getSCEV(LHS),
                          getSCEV(RHS), BI->getIRType());
    case BinaryInst::AND:
        return makeBinary(SCEVExprKind::BitAnd, getSCEV(LHS),
                          getSCEV(RHS), BI->getIRType());
    case BinaryInst::OR:
        return makeBinary(SCEVExprKind::BitOr, getSCEV(LHS),
                          getSCEV(RHS), BI->getIRType());
    case BinaryInst::XOR:
        return makeBinary(SCEVExprKind::BitXor, getSCEV(LHS),
                          getSCEV(RHS), BI->getIRType());
    case BinaryInst::UDIV:
    case BinaryInst::UREM: return makeUnknown(BI);
    default: break;
    }
    return makeUnknown(BI);
}
//根据IR转换指令构造对应的SCEV转换表达式
const SCEVExpr *SCEVResult::createFromCast(Instruction *I, SCEVCastKind CK) {
    Value *Op = I->getOperand(0);
    if (!Op) return makeUnknown(I);
    return makeCast(CK, getSCEV(Op), I->getIRType());
}
//根据PHI指令构造未知值或循环递推表达式
const SCEVExpr *SCEVResult::createFromPhi(PhiInst *Phi) {
    if (!Phi) return nullptr;
    BasicBlock *BB = Phi->getParent();
    Loop *L = LI ? LI->getLoopFor(BB) : nullptr;
    if (L && L->getHeader() == BB) {
        if (const SCEVExpr *R = tryHeaderPhi(Phi, L))
            return R;
    }
    if (Phi->getNumIncomingValues() == 1)
        return getSCEV(Phi->getIncomingValue(0));
    const SCEVExpr *Common = nullptr;
    if (!forEachPhiIncoming(
            Phi, [&](unsigned, Value *In, BasicBlock *) {
                if (!In || In == Phi) return true;
                const SCEVExpr *Cur = getSCEV(In);
                if (!Common) {
                    Common = Cur;
                    return true;
                }
                return sameExpr(Common, Cur);
            })) {
        return makeUnknown(Phi);
    }
    return Common ? Common : makeUnknown(Phi);
}
//尝试将循环Header PHI识别为加法、乘法或除法递推
const SCEVExpr *SCEVResult::tryHeaderPhi(PhiInst *Phi, const Loop *L) {
    Value *Start = nullptr;
    Value *Update = nullptr;
    if (!forEachPhiIncoming(
            Phi, [&](unsigned, Value *V, BasicBlock *Pred) {
                if (!V || !Pred) return false;
                if (L->contains(Pred)) {
                    if (Update && Update != V) return false;
                    Update = V;
                } else {
                    if (Start && Start != V) return false;
                    Start = V;
                }
                return true;
            })) {
        return nullptr;
    }
    if (!Start || !Update) return nullptr;
    if (auto Delta = findAdditiveDelta(Phi, Update, L)) {
        if (Delta->SeenSelf && Delta->Delta) {
            return makeAddRecFromDelta(
                L, getSCEV(Start), Delta->Delta, Phi->getIRType());
        }
    }
    return trySimpleMulDivRec(Phi, Update, Start, L);
}
//尝试从PHI回边更新值中提取统一加法增量
std::optional<SCEVResult::UpdateDelta>
SCEVResult::findAdditiveDelta(PhiInst *Self, Value *V, const Loop *L) {
    std::unordered_set<Value *> Active;
    return findAdditiveDeltaImpl(Self, V, L, Active, 0);
}
//递归遍历PHI更新表达式并提取统一加法增量
std::optional<SCEVResult::UpdateDelta>
SCEVResult::findAdditiveDeltaImpl(
    PhiInst *Self, Value *V, const Loop *L,
    std::unordered_set<Value *> &Active, unsigned Depth) {
    if (!V) return std::nullopt;
    if (V == Self) return UpdateDelta{true, makeConstant(0, Self->getIRType())};
    if (Depth > 64) return std::nullopt;
    if (LoopInfo::isLoopInvariant(V, L))
        return UpdateDelta{false, getSCEV(V)};
    if (auto *Merge = dynamic_cast<PhiInst *>(V)) {
        //另一个 Header PHI 可以作为当前递推的多项式增量。仅接受已经
        //成功表示为同循环 AddRec 的值，互相递归的 PHI 会因活动值保护
        //退化为 Unknown，不会被错误积分。
        if (Merge != Self && Merge->getParent() == L->getHeader()) {
            const SCEVExpr *OtherRec = getSCEV(Merge);
            if (isPolynomialAddRec(OtherRec, L))
                return UpdateDelta{false, OtherRec};
            return std::nullopt;
        }
        if (!Merge->getParent() || !L->contains(Merge->getParent()) ||
            !Active.insert(Merge).second) {
            return std::nullopt;
        }
        std::optional<UpdateDelta> Common;
        bool Valid = Merge->getNumIncomingValues() != 0;
        forEachPhiIncoming(
            Merge, [&](unsigned, Value *Incoming, BasicBlock *Pred) {
                if (!Valid || !Incoming || !Pred || Incoming == Merge ||
                    !L->contains(Pred)) {
                    Valid = false;
                    return false;
                }
                auto Current = findAdditiveDeltaImpl(
                    Self, Incoming, L, Active, Depth + 1);
                if (!Current || !Current->SeenSelf || !Current->Delta) {
                    Valid = false;
                    return false;
                }
                if (!Common) {
                    Common = *Current;
                    return true;
                }
                if (Common->SeenSelf != Current->SeenSelf ||
                    !sameExpr(Common->Delta, Current->Delta)) {
                    Valid = false;
                    return false;
                }
                return true;
            });
        Active.erase(Merge);
        return Valid ? Common : std::nullopt;
    }
    auto *BI = dynamic_cast<BinaryInst *>(V);
    if (!BI) return std::nullopt;
    Value *A = BI->getOperand(0);
    Value *B = BI->getOperand(1);
    if (!A || !B) return std::nullopt;
    BinaryInst::Operation Op = BI->getOp();
    auto DA = findAdditiveDeltaImpl(Self, A, L, Active, Depth + 1);
    auto DB = findAdditiveDeltaImpl(Self, B, L, Active, Depth + 1);
    if (!DA || !DB) return std::nullopt;
    if (Op == BinaryInst::ADD) {
        if (DA->SeenSelf && DB->SeenSelf) return std::nullopt;
        return UpdateDelta{DA->SeenSelf || DB->SeenSelf,
                           add(DA->Delta, DB->Delta)};
    }
    if (Op == BinaryInst::SUB) {
        if (DA->SeenSelf && DB->SeenSelf) return std::nullopt;
        if (DB->SeenSelf) return std::nullopt;
        return UpdateDelta{DA->SeenSelf, sub(DA->Delta, DB->Delta)};
    }
    if (!DA->SeenSelf && !DB->SeenSelf) {
        if (Op == BinaryInst::MUL) return UpdateDelta{false, mul(DA->Delta, DB->Delta)};
        if (Op == BinaryInst::DIV)
            return UpdateDelta{false, div(DA->Delta, DB->Delta)};
        if (Op == BinaryInst::SHL) {
            if (auto *CI = dynamic_cast<ConstInt *>(B)) {
                const int64_t Shift = CI->getVal();
                const int Bits =
                    A->getIRType() && A->getIRType()->isInt()
                        ? A->getIRType()->getIntegerBitWidth(0)
                        : 0;
                if (Shift >= 0 && Bits > 0 && Shift < Bits) {
                    const SCEVExpr *ShiftExpr =
                        getSCEV(V);
                    if (ShiftExpr &&
                        isLoopInvariantExpr(ShiftExpr, L)) {
                        return UpdateDelta{false, ShiftExpr};
                    }
                }
            }
        }
    }
    return std::nullopt;
}
//尝试将PHI更新识别为简单乘法或除法递推
const SCEVExpr *SCEVResult::trySimpleMulDivRec(PhiInst *Self, Value *Update,
                                               Value *Start, const Loop *L) {
    auto *BI = dynamic_cast<BinaryInst *>(Update);
    if (!BI) return nullptr;
    Value *A = BI->getOperand(0);
    Value *B = BI->getOperand(1);
    if (!A || !B) return nullptr;
    if (BI->getOp() == BinaryInst::MUL) {
        if (A == Self && LoopInfo::isLoopInvariant(B, L))
            return makeRec(SCEVExprKind::MulRec, L, getSCEV(Start), getSCEV(B),
                           Self->getIRType());
        if (B == Self && LoopInfo::isLoopInvariant(A, L))
            return makeRec(SCEVExprKind::MulRec, L, getSCEV(Start), getSCEV(A),
                           Self->getIRType());
    }
    if (BI->getOp() == BinaryInst::DIV &&
        A == Self && LoopInfo::isLoopInvariant(B, L)) {
        return makeRec(SCEVExprKind::DivRec, L, getSCEV(Start), getSCEV(B),
                       Self->getIRType());
    }
    return nullptr;
}
//判断给定SCEV表达式是否对指定循环保持不变
bool SCEVResult::isLoopInvariantExpr(const SCEVExpr *E,
                                     const Loop *L) const {
    if (!E || !L) return true;
    if (E->isRecurrence()) {
        if (E->RecLoop == L) return false;
        //只有祖先循环的递推值在一次内层循环执行期间保持不变；
        //后代或兄弟循环的未求值递推不能被当作当前循环系数。
        bool IsAncestorRecurrence = false;
        for (const Loop *Parent = L->getParentLoop();
             Parent; Parent = Parent->getParentLoop()) {
            if (Parent == E->RecLoop) {
                IsAncestorRecurrence = true;
                break;
            }
        }
        if (!IsAncestorRecurrence) return false;
    }
    if (E->Kind == SCEVExprKind::Unknown)
        return LoopInfo::isLoopInvariant(E->RawValue, L);
    for (const SCEVExpr *Op : E->Operands)
        if (!isLoopInvariantExpr(Op, L)) return false;
    return true;
}
//判断给定表达式是否为指定循环上的多项式加法递推
bool SCEVResult::isPolynomialAddRec(const SCEVExpr *E,
                                    const Loop *L) const {
    if (!E || !L || E->Kind != SCEVExprKind::AddRec ||
        E->RecLoop != L || E->Operands.size() < 2 ||
        E->Operands.size() - 1 > MaxAddRecDegree) {
        return false;
    }
    for (const SCEVExpr *Coefficient : E->Operands)
        if (!Coefficient || !isLoopInvariantExpr(Coefficient, L))
            return false;
    return true;
}
//判断给定SCEV表达式对应的IR计算是否可安全丢弃
bool SCEVResult::isSafeToDiscard(const SCEVExpr *E) const {
    std::unordered_set<const SCEVExpr *> Active;
    return isSafeToDiscardImpl(E, Active, 0);
}
//递归判断SCEV表达式及其子表达式对应的IR计算是否可安全丢弃
bool SCEVResult::isSafeToDiscardImpl(
    const SCEVExpr *E,
    std::unordered_set<const SCEVExpr *> &Active,
    unsigned Depth) const {
    if (!E || Depth > 64) return false;
    ScopedActiveMark ActiveMark(Active, E);
    if (!ActiveMark.inserted()) return false;
    if (E->Kind == SCEVExprKind::Constant)
        return true;
    if (E->Kind == SCEVExprKind::Unknown) {
        if (!E->RawValue || Value::isUndef(E->RawValue))
            return false;
        auto *Binary = dynamic_cast<BinaryInst *>(E->RawValue);
        if (!Binary) return true;
        return Binary->getOp() != BinaryInst::UDIV &&
               Binary->getOp() != BinaryInst::UREM;
    }
    if (E->Kind == SCEVExprKind::Div ||
        E->Kind == SCEVExprKind::Mod) {
        if (E->Operands.size() != 2) return false;
        auto Divisor = asConstant(E->Operands[1]);
        if (!Divisor || *Divisor == 0 || *Divisor == -1)
            return false;
    }
    if (E->Kind == SCEVExprKind::Shl ||
        E->Kind == SCEVExprKind::LShr ||
        E->Kind == SCEVExprKind::AShr) {
        if (E->Operands.size() != 2) return false;
        auto Shift = asConstant(E->Operands[1]);
        const int Bits =
            E->Ty && E->Ty->isInt()
                ? E->Ty->getIntegerBitWidth(32)
                : 0;
        if (!Shift || *Shift < 0 || Bits <= 0 ||
            *Shift >= Bits) {
            return false;
        }
    }
    for (const SCEVExpr *Operand : E->Operands)
        if (!isSafeToDiscardImpl(
                Operand, Active, Depth + 1)) {
            return false;
        }
    return true;
}
//判断给定基本块是否直接属于循环而非其子循环
bool SCEVResult::isDirectLoopBlock(const Loop *L, BasicBlock *BB) const {
    if (!L || !BB) return false;
    if (LI) return LI->getLoopFor(BB) == L;
    const auto &Blocks = L->getBlocks();
    return std::find(Blocks.begin(), Blocks.end(), BB) != Blocks.end();
}
//根据选择指令构造选择、最小值或最大值表达式
const SCEVExpr *SCEVResult::createFromSelect(SelectInst *Sel) {
    Value *Cond = Sel->getCondition();
    Value *TrueV = Sel->getTrueValue();
    Value *FalseV = Sel->getFalseValue();
    if (!Cond || !TrueV || !FalseV) return makeUnknown(Sel);
    bool CondValue = false;
    if (Cond->tryGetBoolConst(CondValue))
        return getSCEV(CondValue ? TrueV : FalseV);
    const SCEVExpr *TE = getSCEV(TrueV);
    const SCEVExpr *FE = getSCEV(FalseV);
    if (sameExpr(TE, FE) &&
        isSafeToDiscard(getSCEV(Cond))) {
        return TE;
    }
    auto *Cmp = dynamic_cast<BinaryInst *>(Cond);
    if (!Cmp || !BinaryInst::isCompareOp(Cmp->getOp()))
        return makeSelect(getSCEV(Cond), TE, FE, Sel->getIRType());
    Value *A = nullptr;
    Value *B = nullptr;
    if (!Cmp->getOperands(A, B))
        return makeSelect(getSCEV(Cond), TE, FE, Sel->getIRType());
    const bool TrueIsA = sameExpr(TE, getSCEV(A));
    const bool TrueIsB = sameExpr(TE, getSCEV(B));
    const bool FalseIsA = sameExpr(FE, getSCEV(A));
    const bool FalseIsB = sameExpr(FE, getSCEV(B));
    switch (Cmp->getOp()) {
    case BinaryInst::G:
    case BinaryInst::GE:
        if (TrueIsA && FalseIsB)
            return makeBinary(SCEVExprKind::Max, getSCEV(A), getSCEV(B), Sel->getIRType());
        if (TrueIsB && FalseIsA)
            return makeBinary(SCEVExprKind::Min, getSCEV(A), getSCEV(B), Sel->getIRType());
        break;
    case BinaryInst::L:
    case BinaryInst::LE:
        if (TrueIsA && FalseIsB)
            return makeBinary(SCEVExprKind::Min, getSCEV(A), getSCEV(B), Sel->getIRType());
        if (TrueIsB && FalseIsA)
            return makeBinary(SCEVExprKind::Max, getSCEV(A), getSCEV(B), Sel->getIRType());
        break;
    default:
        break;
    }
    return makeSelect(getSCEV(Cond), TE, FE, Sel->getIRType());
}
//根据GEP指令构造基础地址和索引偏移表达式
const SCEVExpr *SCEVResult::createFromGEP(GetElementPtrInst *GEP) {
    Value *Base = GEP ? GEP->getBasePointer() : nullptr;
    if (!Base || !Base->getIRType()) return makeUnknown(GEP);
    const SCEVExpr *Result = getSCEV(Base);
    std::shared_ptr<IRType> CurType = Base->getIRType();
    __int128 ConstantOffset = 0;
    for (size_t I = 0; I < GEP->getNumIndices(); ++I) {
        CurType = CurType ? CurType->getIndexableElementType() : nullptr;
        if (!CurType) return makeUnknown(GEP);
        std::size_t Stride = CurType->getSize();
        if (Stride >
            static_cast<std::size_t>(
                std::numeric_limits<int64_t>::max())) {
            return makeUnknown(GEP);
        }
        Value *Idx = GEP->getIndex(I);
        if (!Idx) return makeUnknown(GEP);
        if (auto *CI = dynamic_cast<ConstInt *>(Idx)) {
            ConstantOffset +=
                static_cast<__int128>(CI->getVal()) *
                static_cast<__int128>(Stride);
            if (ConstantOffset <
                    std::numeric_limits<int64_t>::min() ||
                ConstantOffset >
                    std::numeric_limits<int64_t>::max()) {
                return makeUnknown(GEP);
            }
        } else {
            const SCEVExpr *Scaled = mul(getSCEV(Idx),
                                         makeConstant(static_cast<int64_t>(Stride),
                                                      Idx->getIRType()));
            Result = add(Result, Scaled);
        }
    }
    if (ConstantOffset != 0)
        Result = add(
            Result,
            makeConstant(static_cast<int64_t>(ConstantOffset),
                         INTType::getInt32Ty()));
    return Result ? Result : makeUnknown(GEP);
}
//按照函数顺序收集直接属于给定循环的基本块
std::vector<BasicBlock *>
SCEVResult::collectDirectLoopBlocks(const Loop *L) const {
    std::vector<BasicBlock *> Blocks;
    if (!L) return Blocks;
    for (BasicBlock *BB : L->getBlocks()) {
        if (!BB) continue;
        if (LI && LI->getLoopFor(BB) != L) continue;
        Blocks.push_back(BB);
    }
    return Blocks;
}
//计算给定循环的总迭代次数表达式
const SCEVExpr *SCEVResult::computeTripCount(const Loop *L) {
    auto Info = analyzeLoopTripCount(L);
    return Info ? Info->TripCount : nullptr;
}
//从给定循环退出比较直接提取总迭代次数表达式
const SCEVExpr *SCEVResult::tripFromComparison(const Loop *L, BinaryInst *Cmp,
                                               bool ContinueOnTrue) {
    if (!L || !Cmp) return nullptr;
    LoopTripCountInfo Info;
    Info.L = L;
    Info.Header = L->getHeader();
    Info.ExitingBlock = Cmp->getParent();
    L->getLoopSimplifyForm(Info.PreHeader, Info.Latch);
    Info.ExitCmp = Cmp;
    Info.ContinueOnTrue = ContinueOnTrue;
    if (!fillTripCountFromComparison(Info)) return nullptr;
    return Info.TripCount;
}
//分析给定循环并返回完整的迭代次数信息
std::optional<LoopTripCountInfo>
SCEVResult::analyzeLoopTripCount(const Loop *L) {
    //1.要求唯一预头、Header、Latch 和专用出口基础形状。
    if (!L) return std::nullopt;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch)) return std::nullopt;
    BasicBlock *Header = L->getHeader();
    if (!PreHeader || !Header || !Latch) return std::nullopt;
    LoopTripCountInfo Info;
    Info.L = L;
    Info.PreHeader = PreHeader;
    Info.Header = Header;
    Info.Latch = Latch;
    //2.只扫描当前循环直接块，要求所有离环边由同一条件块指向同一出口。
    for (BasicBlock *BB : collectDirectLoopBlocks(L)) {
        if (!BB) continue;
        bool HasOutsideSucc = false;
        for (BasicBlock *Succ : BB->getSuccessors()) {
            if (Succ && !L->contains(Succ)) {
                HasOutsideSucc = true;
                break;
            }
        }
        if (!HasOutsideSucc) continue;
        if (Info.ExitingBlock && Info.ExitingBlock != BB)
            return std::nullopt;
        auto *Br = BB->getBranchTerminator();
        if (!BasicBlock::isCondBr(Br)) return std::nullopt;
        BasicBlock *TrueBB = Br->getTrueSuccessor();
        BasicBlock *FalseBB = Br->getFalseSuccessor();
        bool TrueIn = TrueBB && L->contains(TrueBB);
        bool FalseIn = FalseBB && L->contains(FalseBB);
        if (TrueIn == FalseIn) return std::nullopt;
        BasicBlock *ThisExit = TrueIn ? FalseBB : TrueBB;
        if (!ThisExit || L->contains(ThisExit)) return std::nullopt;
        if (Info.ExitBlock && Info.ExitBlock != ThisExit)
            return std::nullopt;
        Info.ExitingBlock = BB;
        Info.ExitBlock = ThisExit;
        Info.ContinueOnTrue = TrueIn;
    }
    if (!Info.ExitingBlock) return std::nullopt;
    if (DT && !DT->dominates(Info.ExitingBlock, Latch)) return std::nullopt;
    //3.退出块必须支配 Latch，且条件必须是可建模的整数比较。
    auto *Br = Info.ExitingBlock->getBranchTerminator();
    Value *Cond = Br->getOperand(0);
    auto *Cmp = dynamic_cast<BinaryInst *>(Cond);
    if (!Cmp || !BinaryInst::isCompareOp(Cmp->getOp())) return std::nullopt;
    Info.ExitCmp = Cmp;
    //4.将比较转换成距离、次数、比较阶段和无回绕信息后封装结果。
    if (!fillTripCountFromComparison(Info)) return std::nullopt;
    Info.Valid = true;
    return Info;
}
//仅依赖基础SCEV事实判断给定循环是否已知有限
bool SCEVResult::isKnownFiniteLoop(const Loop *L) {
    auto Induction = getCanonicalInduction(L);
    auto Trip = analyzeLoopTripCount(L);
    if (!Induction || !Trip || !Induction->Valid || !Trip->Valid ||
        !Induction->Phi || !Induction->ConstantStep) {
        return false;
    }
    if (Trip->ProvenNoWrap) return true;
    if (!hasStrictUnitStepExit(*Induction, *Trip)) {
        return false;
    }
    return Trip->ComparePhase == LoopExitComparePhase::HeaderPhi ||
           (Trip->ComparePhase == LoopExitComparePhase::BackedgeUpdate &&
            isBackedgeUpdateStartSafe(L, *Induction, *Trip));
}
//结合Range和支配信息判断给定循环是否有限且归纳更新不回绕
bool SCEVResult::isKnownFiniteLoop(
    const Loop *L, RangeInfo &RI, DominatorTree &DomTree) {
    if (isKnownFiniteLoop(L)) return true;
    if (!L) return false;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    BasicBlock *Header = L->getHeader();
    if (!Header ||
        !L->getLoopSimplifyForm(PreHeader, Latch) ||
        !PreHeader || !Latch) {
        return false;
    }
    //1.与基础次数分析保持相同的单退出形状，并要求退出测试支配锁存块。
    BasicBlock *Exiting = nullptr;
    BasicBlock *Exit = nullptr;
    bool ContinueOnTrue = true;
    for (BasicBlock *BB : collectDirectLoopBlocks(L)) {
        bool HasOutsideSuccessor = false;
        for (BasicBlock *Successor : BB->getSuccessors()) {
            if (Successor && !L->contains(Successor)) {
                HasOutsideSuccessor = true;
                break;
            }
        }
        if (!HasOutsideSuccessor) continue;
        if (Exiting && Exiting != BB) return false;
        BrInst *Branch = BB->getBranchTerminator();
        if (!BasicBlock::isCondBr(Branch)) return false;
        BasicBlock *TrueBlock = Branch->getTrueSuccessor();
        BasicBlock *FalseBlock = Branch->getFalseSuccessor();
        const bool TrueInside = TrueBlock && L->contains(TrueBlock);
        const bool FalseInside = FalseBlock && L->contains(FalseBlock);
        if (TrueInside == FalseInside) return false;
        BasicBlock *CurrentExit =
            TrueInside ? FalseBlock : TrueBlock;
        if (!CurrentExit || L->contains(CurrentExit) ||
            (Exit && Exit != CurrentExit)) {
            return false;
        }
        Exiting = BB;
        Exit = CurrentExit;
        ContinueOnTrue = TrueInside;
    }
    if (!Exiting || !DomTree.dominates(Exiting, Latch))
        return false;
    BrInst *Branch = Exiting->getBranchTerminator();
    auto *Compare =
        dynamic_cast<BinaryInst *>(Branch->getCondition());
    if (!Compare || !BinaryInst::isCompareOp(Compare->getOp()))
        return false;
    //2.将当前循环的 affine 值定向到左侧，另一侧必须对循环不变。
    BinaryInst::Operation Predicate =
        ContinueOnTrue
            ? Compare->getOp()
            : BinaryInst::negatePredicate(Compare->getOp());
    Value *ComparedValue = Compare->getOperand(0);
    Value *BoundValue = Compare->getOperand(1);
    if (!ComparedValue || !BoundValue) return false;
    const SCEVExpr *ComparedExpr = getSCEV(ComparedValue);
    const SCEVExpr *BoundExpr = getSCEV(BoundValue);
    auto ComparedAffine = getAffine(ComparedExpr);
    if (!ComparedAffine || ComparedAffine->RecLoop != L) {
        if (!isLoopInvariantExpr(ComparedExpr, L))
            return false;
        auto BoundAffine = getAffine(BoundExpr);
        if (!BoundAffine || BoundAffine->RecLoop != L)
            return false;
        std::swap(ComparedValue, BoundValue);
        std::swap(ComparedExpr, BoundExpr);
        ComparedAffine = BoundAffine;
        Predicate = BinaryInst::reversePredicate(Predicate);
    }
    if (!ComparedAffine || ComparedAffine->RecLoop != L ||
        !isLoopInvariantExpr(BoundExpr, L)) {
        return false;
    }
    //3.比较值只能是 Header PHI 本身或该 PHI 的唯一直接回边更新。
    PhiInst *InductionPhi =
        dynamic_cast<PhiInst *>(ComparedValue);
    if (InductionPhi &&
        InductionPhi->getParent() != Header) {
        return false;
    }
    if (!InductionPhi) {
        for (Instruction *InstructionValue : *Header) {
            auto *Candidate =
                dynamic_cast<PhiInst *>(InstructionValue);
            if (!Candidate) break;
            bool HasComparedUpdate = false;
            bool InvalidIncoming = false;
            forEachPhiIncoming(
                Candidate,
                [&](unsigned, Value *Incoming,
                    BasicBlock *Predecessor) {
                    if (!Incoming || !Predecessor) {
                        InvalidIncoming = true;
                        return false;
                    }
                    if (L->contains(Predecessor) &&
                        Incoming == ComparedValue) {
                        if (HasComparedUpdate) {
                            InvalidIncoming = true;
                            return false;
                        }
                        HasComparedUpdate = true;
                    }
                    return true;
                });
            if (InvalidIncoming || !HasComparedUpdate)
                continue;
            if (InductionPhi && InductionPhi != Candidate)
                return false;
            InductionPhi = Candidate;
        }
    }
    if (!InductionPhi) return false;
    if (!IRType::hasIntegerBitWidth(
            InductionPhi->getIRType(), 32) ||
        !IRType::hasIntegerBitWidth(
            BoundValue->getIRType(), 32)) {
        return false;
    }
    Value *StartValue = nullptr;
    Value *UpdateValue = nullptr;
    bool InvalidIncoming = false;
    forEachPhiIncoming(
        InductionPhi,
        [&](unsigned, Value *Incoming,
            BasicBlock *Predecessor) {
            if (!Incoming || !Predecessor) {
                InvalidIncoming = true;
                return false;
            }
            if (L->contains(Predecessor)) {
                if (Predecessor != Latch ||
                    (UpdateValue && UpdateValue != Incoming)) {
                    InvalidIncoming = true;
                    return false;
                }
                UpdateValue = Incoming;
            } else {
                if (StartValue && StartValue != Incoming) {
                    InvalidIncoming = true;
                    return false;
                }
                StartValue = Incoming;
            }
            return true;
        });
    if (InvalidIncoming || !StartValue || !UpdateValue ||
        (ComparedValue != InductionPhi &&
         ComparedValue != UpdateValue)) {
        return false;
    }
    if (!IRType::hasIntegerBitWidth(StartValue->getIRType(), 32) ||
        !IRType::hasIntegerBitWidth(UpdateValue->getIRType(), 32)) {
        return false;
    }
    const bool ComparesUpdate = ComparedValue == UpdateValue;
    if ((!ComparesUpdate &&
         (Exiting != Header || Header == Latch)) ||
        (ComparesUpdate && Exiting != Latch)) {
        return false;
    }
    //4.仅接受 phi +/- loop-invariant-step 的直接更新，并核对其 SCEV 步长。
    auto *Update = dynamic_cast<BinaryInst *>(UpdateValue);
    if (!Update) return false;
    Value *LeftValue = Update->getOperand(0);
    Value *RightValue = Update->getOperand(1);
    const SCEVExpr *DirectStep = nullptr;
    if (Update->getOp() == BinaryInst::ADD) {
        if (LeftValue == InductionPhi)
            DirectStep = getSCEV(RightValue);
        else if (RightValue == InductionPhi)
            DirectStep = getSCEV(LeftValue);
    } else if (Update->getOp() == BinaryInst::SUB &&
               LeftValue == InductionPhi) {
        DirectStep = neg(getSCEV(RightValue));
    }
    auto PhiAffine = getAffine(InductionPhi);
    if (!DirectStep || !PhiAffine ||
        PhiAffine->RecLoop != L ||
        !sameExpr(DirectStep, PhiAffine->Step) ||
        !isLoopInvariantExpr(DirectStep, L)) {
        return false;
    }
    auto StepInfo = analyzeAffineStepRange(
        InductionPhi, PreHeader, RI, DomTree);
    if (!StepInfo || !StepInfo->Valid)
        return false;
    const bool Increasing =
        StepInfo->Sign == SCEVStepSign::StrictlyPositive;
    const bool Decreasing =
        StepInfo->Sign == SCEVStepSign::StrictlyNegative;
    if (!Increasing && !Decreasing) return false;
    if ((Increasing &&
         Predicate != BinaryInst::L &&
         Predicate != BinaryInst::LE) ||
        (Decreasing &&
         Predicate != BinaryInst::G &&
         Predicate != BinaryInst::GE)) {
        return false;
    }
    IntRange StartRange =
        RI.query(StartValue, PreHeader, DomTree);
    IntRange BoundRange =
        RI.query(BoundValue, PreHeader, DomTree);
    if (StartRange.isBottom() || BoundRange.isBottom() ||
        StepInfo->Range.isBottom()) {
        return false;
    }
    //5.以宽整数检查最坏末次更新。Header 比较只在条件成立后更新；
    //BackedgeUpdate 还必须单独覆盖第一次无条件更新。
    if (Increasing) {
        const __int128 LargestCompared =
            static_cast<__int128>(BoundRange.max) -
            (Predicate == BinaryInst::L ? 1 : 0);
        const __int128 LargestUpdate =
            LargestCompared +
            static_cast<__int128>(StepInfo->Range.max);
        if (LargestUpdate > IntRange::I32_MAX)
            return false;
        if (ComparesUpdate) {
            const __int128 FirstUpdate =
                static_cast<__int128>(StartRange.max) +
                static_cast<__int128>(StepInfo->Range.max);
            if (FirstUpdate > IntRange::I32_MAX)
                return false;
        }
    } else {
        const __int128 SmallestCompared =
            static_cast<__int128>(BoundRange.min) +
            (Predicate == BinaryInst::G ? 1 : 0);
        const __int128 SmallestUpdate =
            SmallestCompared +
            static_cast<__int128>(StepInfo->Range.min);
        if (SmallestUpdate < IntRange::I32_MIN)
            return false;
        if (ComparesUpdate) {
            const __int128 FirstUpdate =
                static_cast<__int128>(StartRange.min) +
                static_cast<__int128>(StepInfo->Range.min);
            if (FirstUpdate < IntRange::I32_MIN)
                return false;
        }
    }
    return true;
}
//判断给定值是否属于已证明不回绕的循环归纳链
bool SCEVResult::isKnownNoWrapInductionValue(const Loop *L, Value *V) {
    if (!L || !V) return false;
    auto Induction = getCanonicalInduction(L);
    auto Trip = analyzeLoopTripCount(L);
    if (!Induction || !Trip || !Induction->Valid || !Trip->Valid ||
        (V != Induction->Phi && V != Induction->UpdateValue)) {
        return false;
    }
    if (Trip->ProvenNoWrap) return true;
    if (!hasStrictUnitStepExit(*Induction, *Trip)) return false;
    return Trip->ComparePhase == LoopExitComparePhase::HeaderPhi ||
           (Trip->ComparePhase == LoopExitComparePhase::BackedgeUpdate &&
            isBackedgeUpdateStartSafe(L, *Induction, *Trip));
}
//判断退出比较是否以严格单位步长推进到边界
bool SCEVResult::hasStrictUnitStepExit(
    const InductionDescriptor &Induction,
    const LoopTripCountInfo &Trip) const {
    auto InductionType = Induction.Phi
        ? Induction.Phi->getIRType()
        : nullptr;
    if (!Trip.Valid || !Trip.ExitCmp || !Trip.Header ||
        (Trip.ExitingBlock != Trip.Header &&
         Trip.ExitingBlock != Trip.Latch) || !Trip.IndVar ||
        Trip.IndVar != Induction.Phi || !Induction.Phi ||
        !Induction.UpdateValue || !Induction.ConstantStep ||
        Trip.ComparePhase == LoopExitComparePhase::Unknown ||
        !IRType::hasIntegerBitWidth(InductionType, 32)) {
        return false;
    }
    if (Trip.ComparePhase == LoopExitComparePhase::HeaderPhi &&
        Trip.ExitingBlock == Trip.Latch) {
        return false;
    }
    BinaryInst::Operation Predicate = Trip.ContinueOnTrue
        ? Trip.ExitCmp->getOp()
        : BinaryInst::negatePredicate(Trip.ExitCmp->getOp());
    Value *LHS = Trip.ExitCmp->getOperand(0);
    Value *RHS = Trip.ExitCmp->getOperand(1);
    Value *Compared = Trip.ComparePhase == LoopExitComparePhase::HeaderPhi
        ? static_cast<Value *>(Induction.Phi)
        : Induction.UpdateValue;
    if (RHS == Compared) {
        std::swap(LHS, RHS);
        Predicate = BinaryInst::reversePredicate(Predicate);
    }
    if (LHS != Compared) return false;
    return (*Induction.ConstantStep == 1 &&
            Predicate == BinaryInst::L) ||
           (*Induction.ConstantStep == -1 &&
            Predicate == BinaryInst::G);
}
//判断回边更新阶段使用的起点是否能够安全套用次数公式
bool SCEVResult::isBackedgeUpdateStartSafe(
    const Loop *L, const InductionDescriptor &Induction,
    const LoopTripCountInfo &) {
    if (!L || !Induction.StartValue || !Induction.ConstantStep)
        return false;
    if (Induction.ConstantStart) {
        if (*Induction.ConstantStart < IntRange::I32_MIN ||
            *Induction.ConstantStart > IntRange::I32_MAX) {
            return false;
        }
        if (*Induction.ConstantStep == 1)
            return *Induction.ConstantStart < IntRange::I32_MAX;
        if (*Induction.ConstantStep == -1)
            return *Induction.ConstantStart > IntRange::I32_MIN;
        return false;
    }
    for (Loop *Ancestor = L->getParentLoop(); Ancestor;
         Ancestor = Ancestor->getParentLoop()) {
        auto AncestorInduction = getCanonicalInduction(Ancestor);
        if (!AncestorInduction || !AncestorInduction->Valid ||
            !AncestorInduction->Phi ||
            AncestorInduction->ConstantStep != Induction.ConstantStep) {
            continue;
        }
        if (Induction.StartValue != AncestorInduction->Phi &&
            !sameExpr(Induction.Start,
                      getSCEV(AncestorInduction->Phi))) {
            continue;
        }
        return isKnownNoWrapInductionValue(
            Ancestor, AncestorInduction->Phi);
    }
    return false;
}
//在循环预头处结合Range与支配信息证明运行时归纳更新不回绕
bool SCEVResult::proveRuntimeNoWrapAtPreheader(
    const LoopTripCountInfo &Info, RangeInfo &RI, DominatorTree &DomTree) {
    //1.只接受有效运行时次数和直接的 i32 加一/减一规范归纳。
    if (!Info.Valid || !Info.IsRuntime || !Info.L || !Info.PreHeader ||
        !Info.Header || !Info.Latch || !Info.ExitingBlock || !Info.ExitCmp ||
        !Info.IndVar) {
        return false;
    }
    auto IV = getCanonicalInduction(Info.L);
    if (!IV || !IV->Valid || !IV->Phi || !IV->StartValue ||
        !IV->UpdateValue || Info.IndVar != IV->Phi || !IV->ConstantStep ||
        (*IV->ConstantStep != 1 && *IV->ConstantStep != -1)) {
        return false;
    }
    auto IsI32 = [](Value *V) {
        auto Ty = V ? V->getIRType() : nullptr;
        return IRType::hasIntegerBitWidth(Ty, 32);
    };
    if (!IsI32(IV->Phi) || !IsI32(IV->StartValue) ||
        !IsI32(IV->UpdateValue)) {
        return false;
    }
    auto *Update = dynamic_cast<BinaryInst *>(IV->UpdateValue);
    if (!Update) return false;
    Value *A = Update->getOperand(0);
    Value *B = Update->getOperand(1);
    auto *CA = dynamic_cast<ConstInt *>(A);
    auto *CB = dynamic_cast<ConstInt *>(B);
    bool DirectStep = false;
    if (*IV->ConstantStep == 1 && Update->getOp() == BinaryInst::ADD) {
        DirectStep = (A == IV->Phi && CB && CB->getVal() == 1) ||
                     (B == IV->Phi && CA && CA->getVal() == 1);
    } else if (*IV->ConstantStep == -1 &&
               Update->getOp() == BinaryInst::SUB) {
        DirectStep = A == IV->Phi && CB && CB->getVal() == 1;
    }
    if (!DirectStep) return false;
    //2.用 SSA 身份区分 Header PHI 与 BackedgeUpdate，拒绝单块循环的错误阶段解释。
    Value *Compared = nullptr;
    if (Info.ComparePhase == LoopExitComparePhase::HeaderPhi) {
        if (Info.ExitingBlock != Info.Header ||
            Info.ExitingBlock == Info.Latch) {
            return false;
        }
        Compared = IV->Phi;
    } else if (Info.ComparePhase ==
               LoopExitComparePhase::BackedgeUpdate) {
        if (Info.ExitingBlock != Info.Latch)
            return false;
        Compared = IV->UpdateValue;
    } else {
        return false;
    }
    BinaryInst::Operation Pred = Info.ContinueOnTrue
        ? Info.ExitCmp->getOp()
        : BinaryInst::negatePredicate(Info.ExitCmp->getOp());
    Value *LHS = Info.ExitCmp->getOperand(0);
    Value *RHS = Info.ExitCmp->getOperand(1);
    if (RHS == Compared) {
        std::swap(LHS, RHS);
        Pred = BinaryInst::reversePredicate(Pred);
    }
    if (LHS != Compared || !IsI32(RHS)) return false;
    if ((*IV->ConstantStep == 1 && Pred != BinaryInst::L) ||
        (*IV->ConstantStep == -1 && Pred != BinaryInst::G)) {
        return false;
    }
    //3.在预头查询起点和边界范围，并在宽整数中验证距离计算不会回绕。
    IntRange StartRange = RI.query(IV->StartValue, Info.PreHeader, DomTree);
    IntRange BoundRange = RI.query(RHS, Info.PreHeader, DomTree);
    auto FitsI32Range = [](const IntRange &R) {
        return !R.isBottom() && R.min >= IntRange::I32_MIN &&
               R.max <= IntRange::I32_MAX;
    };
    if (!FitsI32Range(StartRange) || !FitsI32Range(BoundRange))
        return false;
    __int128 ComparedMin = StartRange.min;
    __int128 ComparedMax = StartRange.max;
    const bool ComparesBackedgeUpdate =
        Info.ComparePhase == LoopExitComparePhase::BackedgeUpdate;
    if (ComparesBackedgeUpdate) {
        ComparedMin += *IV->ConstantStep;
        ComparedMax += *IV->ConstantStep;
        if (ComparedMin < IntRange::I32_MIN ||
            ComparedMax > IntRange::I32_MAX) {
            return false;
        }
    }
    __int128 MinDistance = 0;
    __int128 MaxDistance = 0;
    if (*IV->ConstantStep == 1) {
        MinDistance =
            static_cast<__int128>(BoundRange.min) - ComparedMax;
        MaxDistance =
            static_cast<__int128>(BoundRange.max) - ComparedMin;
    } else {
        MinDistance =
            ComparedMin - static_cast<__int128>(BoundRange.max);
        MaxDistance =
            ComparedMax - static_cast<__int128>(BoundRange.min);
    }
    if (MinDistance < IntRange::I32_MIN ||
        MaxDistance > IntRange::I32_MAX) {
        return false;
    }
    //4.BackedgeUpdate 还需为首轮更新、次数加一和末次更新包络提供额外空间。
    if (!ComparesBackedgeUpdate)
        return true;
    if (MaxDistance >
        static_cast<__int128>(IntRange::I32_MAX) - 1) {
        return false;
    }
    __int128 FinalMin = 0;
    __int128 FinalMax = 0;
    if (*IV->ConstantStep == 1) {
        FinalMin = std::max(
            ComparedMin, static_cast<__int128>(BoundRange.min));
        FinalMax = std::max(
            ComparedMax, static_cast<__int128>(BoundRange.max));
    } else {
        FinalMin = std::min(
            ComparedMin, static_cast<__int128>(BoundRange.min));
        FinalMax = std::min(
            ComparedMax, static_cast<__int128>(BoundRange.max));
    }
    return FinalMin >= IntRange::I32_MIN &&
           FinalMax <= IntRange::I32_MAX;
}
//递归计算SCEV表达式在指定上下文中用于方向判断的整数范围
std::optional<IntRange> SCEVResult::evaluateRangeForDirection(
    const SCEVExpr *E, BasicBlock *Context, RangeInfo &RI,
    DominatorTree &DomTree,
    std::unordered_set<const SCEVExpr *> &Active,
    unsigned Depth) const {
    if (!E || Depth > 64) return std::nullopt;
    ScopedActiveMark ActiveMark(Active, E);
    if (!ActiveMark.inserted()) return std::nullopt;
    if (E->Kind == SCEVExprKind::Constant) {
        if (E->ConstValue < IntRange::I32_MIN ||
            E->ConstValue > IntRange::I32_MAX) {
            return std::nullopt;
        }
        return IntRange(E->ConstValue);
    }
    if (E->Kind == SCEVExprKind::Unknown) {
        if (!E->RawValue || !Context) return std::nullopt;
        return RI.query(E->RawValue, Context, DomTree);
    }
    if (E->Kind == SCEVExprKind::Select) {
        if (E->Operands.size() != 3) return std::nullopt;
        auto TrueRange = evaluateRangeForDirection(
            E->Operands[1], Context, RI, DomTree, Active, Depth + 1);
        auto FalseRange = evaluateRangeForDirection(
            E->Operands[2], Context, RI, DomTree, Active, Depth + 1);
        if (!TrueRange || !FalseRange) return std::nullopt;
        return TrueRange->unionWith(*FalseRange);
    }
    if (E->Kind == SCEVExprKind::Compare)
        return IntRange(0, 1);
    if (E->Operands.size() != 2) return std::nullopt;
    auto Left = evaluateRangeForDirection(
        E->Operands[0], Context, RI, DomTree, Active, Depth + 1);
    auto Right = evaluateRangeForDirection(
        E->Operands[1], Context, RI, DomTree, Active, Depth + 1);
    if (!Left || !Right) return std::nullopt;
    switch (E->Kind) {
    case SCEVExprKind::Add:
        return Left->add(*Right);
    case SCEVExprKind::Sub:
        return Left->sub(*Right);
    case SCEVExprKind::Mul:
        return Left->mul(*Right);
    case SCEVExprKind::Div:
        return Left->sdiv(*Right);
    case SCEVExprKind::Mod:
        return Left->srem(*Right);
    case SCEVExprKind::Shl:
        return Left->shl(*Right);
    case SCEVExprKind::LShr:
        return Left->lshr(*Right);
    case SCEVExprKind::AShr:
        return Left->ashr(*Right);
    case SCEVExprKind::BitAnd:
        return Left->band(*Right);
    case SCEVExprKind::BitOr:
        return Left->bor(*Right);
    case SCEVExprKind::Min:
        return IntRange(std::min(Left->min, Right->min),
                        std::min(Left->max, Right->max));
    case SCEVExprKind::Max:
        return IntRange(std::max(Left->min, Right->min),
                        std::max(Left->max, Right->max));
    default:
        return std::nullopt;
    }
}
//根据整数范围分类步长的符号方向
SCEVStepSign SCEVResult::classifyStepSign(
    const IntRange &Range) const {
    if (Range.isBottom() || Range.isTop())
        return SCEVStepSign::Unknown;
    if (Range.min == 0 && Range.max == 0)
        return SCEVStepSign::Zero;
    if (Range.min > 0)
        return SCEVStepSign::StrictlyPositive;
    if (Range.max < 0)
        return SCEVStepSign::StrictlyNegative;
    if (Range.min >= 0)
        return SCEVStepSign::NonNegative;
    if (Range.max <= 0)
        return SCEVStepSign::NonPositive;
    return SCEVStepSign::Unknown;
}
//分析给定仿射SCEV表达式在指定上下文中的步长范围和符号
std::optional<SCEVStepRangeInfo>
SCEVResult::analyzeAffineStepRange(
    const SCEVExpr *E, BasicBlock *Context, RangeInfo &RI,
    DominatorTree &DomTree) const {
    auto Affine = getAffine(E);
    if (!Affine || !Affine->RecLoop || !Affine->Step)
        return std::nullopt;
    if (!Context) {
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Latch = nullptr;
        if (!Affine->RecLoop->getLoopSimplifyForm(PreHeader, Latch) ||
            !PreHeader) {
            return std::nullopt;
        }
        Context = PreHeader;
    }
    std::unordered_set<const SCEVExpr *> Active;
    auto StepRange = evaluateRangeForDirection(
        Affine->Step, Context, RI, DomTree, Active, 0);
    if (!StepRange || StepRange->isBottom())
        return std::nullopt;
    SCEVStepRangeInfo Info;
    Info.RecLoop = Affine->RecLoop;
    Info.Step = Affine->Step;
    Info.Range = *StepRange;
    Info.Sign = classifyStepSign(*StepRange);
    Info.Valid = true;
    return Info;
}
//分析给定IR值在指定上下文中的仿射步长范围和符号
std::optional<SCEVStepRangeInfo>
SCEVResult::analyzeAffineStepRange(
    Value *V, BasicBlock *Context, RangeInfo &RI,
    DominatorTree &DomTree) {
    return analyzeAffineStepRange(
        getSCEV(V), Context, RI, DomTree);
}
//递归提取只由一致PHI或Select路径传播的精确整数常量
std::optional<int64_t> SCEVResult::getTrivialExactIntConstant(
    Value *V, std::unordered_set<Value *> &Active, unsigned Depth) {
    if (!V || Value::isUndef(V) || Depth > 32) return std::nullopt;
    if (auto *CI = dynamic_cast<ConstInt *>(V)) return CI->getVal();
    ScopedActiveMark ActiveMark(Active, V);
    if (!ActiveMark.inserted()) return std::nullopt;
    std::optional<int64_t> Result;
    if (auto *Phi = dynamic_cast<PhiInst *>(V)) {
        if (!forEachPhiIncoming(
                Phi, [&](unsigned, Value *Incoming, BasicBlock *) {
                    if (Incoming == V) return true;
                    auto C = getTrivialExactIntConstant(
                        Incoming, Active, Depth + 1);
                    if (!C || (Result && *Result != *C)) return false;
                    Result = *C;
                    return true;
                })) {
            return std::nullopt;
        }
    } else if (auto *Sel = dynamic_cast<SelectInst *>(V)) {
        if (Sel->getNumOperands() >= 3) {
            auto T = getTrivialExactIntConstant(
                Sel->getOperand(1), Active, Depth + 1);
            auto F = getTrivialExactIntConstant(
                Sel->getOperand(2), Active, Depth + 1);
            if (T && F && *T == *F) Result = *T;
        }
    }
    return Result;
}
//从已绑定的退出比较补全迭代次数与无回绕信息
bool SCEVResult::fillTripCountFromComparison(LoopTripCountInfo &Info) {
    //1.把循环递推统一放到比较左侧，右侧必须对当前循环不变。
    const Loop *L = Info.L;
    BinaryInst *Cmp = Info.ExitCmp;
    if (!L || !Cmp) return false;
    BinaryInst::Operation Pred = Info.ContinueOnTrue
                                     ? Cmp->getOp()
                                     : BinaryInst::negatePredicate(Cmp->getOp());
    Value *LHSVal = Cmp->getOperand(0);
    Value *RHSVal = Cmp->getOperand(1);
    if (!LHSVal || !RHSVal) return false;
    const SCEVExpr *LHS = getSCEV(LHSVal);
    const SCEVExpr *RHS = getSCEV(RHSVal);
    auto Affine = getAffine(LHS);
    if (!Affine || Affine->RecLoop != L) {
        if (!isLoopInvariantExpr(LHS, L)) return false;
        auto RHSAffine = getAffine(RHS);
        if (RHSAffine && RHSAffine->RecLoop == L) {
            std::swap(LHS, RHS);
            std::swap(LHSVal, RHSVal);
            Pred = BinaryInst::reversePredicate(Pred);
            Affine = RHSAffine;
        } else {
            return false;
        }
    }
    if (!Affine || Affine->RecLoop != L) return false;
    if (!isLoopInvariantExpr(RHS, L)) return false;
    auto StepC = Affine->ConstantStep;
    if (!StepC || *StepC == 0) return false;
    if (*StepC == std::numeric_limits<int64_t>::min()) return false;
    const SCEVExpr *Start = Affine->Start;
    const SCEVExpr *Bound = RHS;
    const SCEVExpr *Distance = nullptr;
    int64_t Step = *StepC;
    //2.根据谓词和步长方向构造非负距离；运行时 NE 因缺少整除证明而失败。
    int64_t AbsStep = Step < 0 ? -Step : Step;
    switch (Pred) {
    case BinaryInst::L:
        if (Step <= 0) return false;
        Distance = sub(Bound, Start);
        break;
    case BinaryInst::LE:
        if (Step <= 0) return false;
        Distance = sub(add(Bound, makeConstant(1, Bound->Ty)), Start);
        break;
    case BinaryInst::G:
        if (Step >= 0) return false;
        Distance = sub(Start, Bound);
        break;
    case BinaryInst::GE:
        if (Step >= 0) return false;
        Distance = sub(Start, sub(Bound, makeConstant(1, Bound->Ty)));
        break;
    case BinaryInst::NE:
        if (Step > 0) Distance = sub(Bound, Start);
        else Distance = sub(Start, Bound);
        break;
    default:
        return false;
    }
    if (!Distance) return false;
    const SCEVExpr *TC = nullptr;
    if (auto D = asConstant(Distance)) {
        if (Pred == BinaryInst::NE) {
            if (*D < 0 || (*D % AbsStep) != 0) return false;
            TC = makeConstant(*D / AbsStep, Distance->Ty);
        } else {
            __int128 ExactTripCount = 0;
            if (*D > 0) {
                ExactTripCount =
                    (static_cast<__int128>(*D) +
                     static_cast<__int128>(AbsStep) - 1) /
                    static_cast<__int128>(AbsStep);
            }
            if (ExactTripCount >
                static_cast<__int128>(
                    std::numeric_limits<int64_t>::max())) {
                return false;
            }
            TC = makeConstant(
                static_cast<int64_t>(ExactTripCount),
                Distance->Ty);
        }
    } else {
        if (Pred == BinaryInst::NE) return false;
        const SCEVExpr *Raw = nullptr;
        if (AbsStep == 1) {
            Raw = Distance;
        } else {
            Raw = div(add(Distance, makeConstant(AbsStep - 1, Distance->Ty)),
                      makeConstant(AbsStep, Distance->Ty));
        }
        if (!Raw) return false;
        TC = makeBinary(SCEVExprKind::Max, makeConstant(0, Raw->Ty), Raw, Raw->Ty);
    }
    if (!TC) return false;
    //3.通过 SSA 身份绑定 HeaderPhi 或唯一回边 Update，并据执行阶段校正一次迭代偏差。
    bool ComparesExactHeaderPhi = false;
    bool ComparesExactBackedgeUpdate = false;
    if (auto *ComparedPhi = dynamic_cast<PhiInst *>(LHSVal)) {
        auto ComparedAffine = getAffine(ComparedPhi);
        if (ComparedPhi->getParent() == Info.Header && ComparedAffine &&
            ComparedAffine->RecLoop == L &&
            sameExpr(ComparedAffine->Start, Affine->Start) &&
            sameExpr(ComparedAffine->Step, Affine->Step)) {
            Info.IndVar = ComparedPhi;
            ComparesExactHeaderPhi = true;
        }
    }
    if (!Info.IndVar) {
        auto *Update = dynamic_cast<BinaryInst *>(LHSVal);
        PhiInst *Candidate = nullptr;
        if (Update) {
            Value *A = Update->getOperand(0);
            Value *B = Update->getOperand(1);
            auto *PA = dynamic_cast<PhiInst *>(A);
            auto *PB = dynamic_cast<PhiInst *>(B);
            auto *CA = dynamic_cast<ConstInt *>(A);
            auto *CB = dynamic_cast<ConstInt *>(B);
            if (Update->getOp() == BinaryInst::ADD) {
                if (PA && CB) Candidate = PA;
                else if (PB && CA) Candidate = PB;
            } else if (Update->getOp() == BinaryInst::SUB && PA && CB) {
                Candidate = PA;
            }
        }
        if (Candidate && Candidate->getParent() == Info.Header) {
            bool UpdateIsBackedge = false;
            bool BadIncoming = false;
            forEachPhiIncoming(
                Candidate,
                [&](unsigned, Value *Incoming, BasicBlock *PredBB) {
                    if (!PredBB || !Incoming) {
                        BadIncoming = true;
                        return false;
                    }
                    if (L->contains(PredBB)) {
                        if (UpdateIsBackedge || Incoming != Update ||
                            (Info.Latch && PredBB != Info.Latch)) {
                            BadIncoming = true;
                            return false;
                        }
                        UpdateIsBackedge = true;
                    }
                    return true;
                });
            auto CandidateAffine = getAffine(Candidate);
            if (!BadIncoming && UpdateIsBackedge && CandidateAffine &&
                CandidateAffine->RecLoop == L &&
                sameExpr(CandidateAffine->Step, Affine->Step)) {
                Info.IndVar = Candidate;
                ComparesExactBackedgeUpdate = true;
            }
        }
    }
    if (ComparesExactBackedgeUpdate ||
        (ComparesExactHeaderPhi && Info.ExitingBlock == Info.Latch)) {
        TC = add(TC, makeConstant(1, TC->Ty));
        if (!TC) return false;
    } else if (!ComparesExactHeaderPhi) {
        return false;
    }
    Info.ComparePhase = ComparesExactBackedgeUpdate
        ? LoopExitComparePhase::BackedgeUpdate
        : LoopExitComparePhase::HeaderPhi;
    //4.发布符号次数；常量次数仍需独立验证直接更新与 i32 退出值均不回绕。
    Info.Start = Start;
    Info.Step = Affine->Step;
    Info.Bound = Bound;
    Info.TripCount = TC;
    Info.ConstantTripCount = asConstant(TC);
    Info.IsRuntime = !Info.ConstantTripCount.has_value();
    Info.MayBeZeroTrip = !Info.ConstantTripCount || *Info.ConstantTripCount == 0;
    if ((Info.ExitingBlock == Info.Header ||
         Info.ExitingBlock == Info.Latch) && Info.IndVar &&
        Info.ConstantTripCount && *Info.ConstantTripCount >= 0) {
        auto StepConst = asConstant(Affine->Step);
        auto Ty = Info.IndVar->getIRType();
        std::optional<int64_t> ExactStart;
        Value *DirectUpdate = nullptr;
        forEachPhiIncoming(
            Info.IndVar,
            [&](unsigned, Value *Incoming, BasicBlock *PredBB) {
                if (!PredBB || !Incoming) {
                    ExactStart.reset();
                    DirectUpdate = nullptr;
                    return false;
                }
                if (L->contains(PredBB)) {
                    if (DirectUpdate && DirectUpdate != Incoming) {
                        DirectUpdate = nullptr;
                        return false;
                    }
                    DirectUpdate = Incoming;
                } else {
                    std::unordered_set<Value *> Active;
                    auto C = getTrivialExactIntConstant(Incoming, Active);
                    if (!C || (ExactStart && *ExactStart != *C)) {
                        ExactStart.reset();
                        return false;
                    }
                    ExactStart = *C;
                }
                return true;
            });
        std::optional<int64_t> DirectStep;
        if (auto *Update = dynamic_cast<BinaryInst *>(DirectUpdate)) {
            Value *A = Update->getOperand(0);
            Value *B = Update->getOperand(1);
            auto *CA = dynamic_cast<ConstInt *>(A);
            auto *CB = dynamic_cast<ConstInt *>(B);
            if (Update->getOp() == BinaryInst::ADD) {
                if (A == Info.IndVar && CB) DirectStep = CB->getVal();
                else if (B == Info.IndVar && CA) DirectStep = CA->getVal();
            } else if (Update->getOp() == BinaryInst::SUB &&
                       A == Info.IndVar && CB &&
                       CB->getVal() != std::numeric_limits<int32_t>::min()) {
                DirectStep = -static_cast<int64_t>(CB->getVal());
            }
        }
        auto *DirectBound = dynamic_cast<ConstInt *>(RHSVal);
        if (ExactStart && DirectBound && DirectStep && StepConst &&
            *DirectStep == *StepConst &&
            IRType::hasIntegerBitWidth(Ty, 32)) {
            const int64_t StartValue = *ExactStart;
            const __int128 ExitValue = static_cast<__int128>(StartValue) +
                static_cast<__int128>(*StepConst) *
                    static_cast<__int128>(*Info.ConstantTripCount);
            Info.ProvenNoWrap =
                StartValue >= IntRange::I32_MIN &&
                StartValue <= IntRange::I32_MAX &&
                ExitValue >= IntRange::I32_MIN &&
                ExitValue <= IntRange::I32_MAX;
        }
    }
    //5.从总次数构造回边次数，运行时路径用 max(0, TC-1) 保持非负。
    if (Info.ConstantTripCount) {
        int64_t BE = *Info.ConstantTripCount <= 0 ? 0 : *Info.ConstantTripCount - 1;
        Info.BackedgeTakenCount = makeConstant(BE, TC->Ty);
        Info.ConstantBackedgeTakenCount = BE;
    } else {
        Info.BackedgeTakenCount =
            makeBinary(SCEVExprKind::Max, makeConstant(0, TC->Ty),
                       sub(TC, makeConstant(1, TC->Ty)), TC->Ty);
        Info.ConstantBackedgeTakenCount = asConstant(Info.BackedgeTakenCount);
    }
    return Info.BackedgeTakenCount != nullptr;
}
//查找给定循环中对应指定加法递推的Header PHI
PhiInst *SCEVResult::findPhiForAddRec(const Loop *L,
                                      const SCEVExpr *AddRec) {
    if (!L || !AddRec || AddRec->Kind != SCEVExprKind::AddRec)
        return nullptr;
    BasicBlock *Header = L->getHeader();
    if (!Header) return nullptr;
    PhiInst *Found = nullptr;
    for (Instruction *I : *Header) {
        auto *Phi = dynamic_cast<PhiInst *>(I);
        if (!Phi) break;
        const SCEVExpr *PhiSCEV = getSCEV(Phi);
        if (!sameExpr(PhiSCEV, AddRec)) continue;
        if (Found && Found != Phi) return nullptr;
        Found = Phi;
    }
    return Found;
}
//查找给定循环中对应指定仿射递推的Header PHI
PhiInst *SCEVResult::findPhiForAffineRec(const Loop *L,
                                         const SCEVAffineInfo &Affine) {
    if (!L) return nullptr;
    BasicBlock *Header = L->getHeader();
    if (!Header) return nullptr;
    PhiInst *Found = nullptr;
    for (Instruction *I : *Header) {
        auto *Phi = dynamic_cast<PhiInst *>(I);
        if (!Phi) break;
        auto PhiAff = getAffine(Phi);
        if (!PhiAff || PhiAff->RecLoop != L) continue;
        if (!sameExpr(PhiAff->Start, Affine.Start) ||
            !sameExpr(PhiAff->Step, Affine.Step))
            continue;
        if (Found && Found != Phi) return nullptr;
        Found = Phi;
    }
    return Found;
}
//返回给定展开因子对应的主循环和余数循环次数信息
std::optional<LoopRemainderInfo>
SCEVResult::getRemainderInfo(const Loop *L, int64_t Factor) {
    if (!L || Factor <= 1) return std::nullopt;
    auto TCInfo = analyzeLoopTripCount(L);
    if (!TCInfo || !TCInfo->TripCount)
        return std::nullopt;
    if (TCInfo->ConstantTripCount && !TCInfo->ProvenNoWrap)
        return std::nullopt;
    LoopRemainderInfo Info;
    Info.L = L;
    Info.Factor = Factor;
    Info.TripCount = TCInfo->TripCount;
    Info.ConstantTripCount = TCInfo->ConstantTripCount;
    if (Info.ConstantTripCount) {
        int64_t Rem = *Info.ConstantTripCount % Factor;
        int64_t Main = *Info.ConstantTripCount - Rem;
        Info.RemainderCount = makeConstant(Rem, Info.TripCount->Ty);
        Info.MainTripCount = makeConstant(Main, Info.TripCount->Ty);
        Info.ConstantRemainderCount = Rem;
        Info.ConstantMainTripCount = Main;
        Info.NeedRemainderLoop = Rem != 0;
        Info.IsRuntimeRemainder = false;
        Info.Valid = Info.RemainderCount && Info.MainTripCount;
        return Info.Valid ? std::optional<LoopRemainderInfo>(Info) : std::nullopt;
    }
    const SCEVExpr *FactorExpr = makeConstant(Factor, Info.TripCount->Ty);
    Info.RemainderCount = mod(Info.TripCount, FactorExpr);
    Info.MainTripCount = sub(Info.TripCount, Info.RemainderCount);
    if (!Info.RemainderCount || !Info.MainTripCount) return std::nullopt;
    Info.NeedRemainderLoop = true;
    Info.IsRuntimeRemainder = true;
    Info.Valid = true;
    return Info;
}
//返回给定循环能够识别出的规范归纳变量
std::optional<InductionDescriptor>
SCEVResult::getCanonicalInduction(const Loop *L) {
    auto TCInfo = analyzeLoopTripCount(L);
    if (!TCInfo || !TCInfo->IndVar) return std::nullopt;
    PhiInst *Phi = TCInfo->IndVar;
    Value *StartValue = nullptr;
    Value *UpdateValue = nullptr;
    if (!forEachPhiIncoming(
            Phi, [&](unsigned, Value *V, BasicBlock *Pred) {
                if (!V || !Pred) return false;
                if (L->contains(Pred)) {
                    if (UpdateValue && UpdateValue != V) return false;
                    UpdateValue = V;
                } else {
                    if (StartValue && StartValue != V) return false;
                    StartValue = V;
                }
                return true;
            })) {
        return std::nullopt;
    }
    if (!StartValue || !UpdateValue) return std::nullopt;
    auto PhiAff = getAffine(Phi);
    if (!PhiAff || PhiAff->RecLoop != L) return std::nullopt;
    InductionDescriptor Desc;
    Desc.Phi = Phi;
    Desc.L = L;
    Desc.StartValue = StartValue;
    Desc.UpdateValue = UpdateValue;
    Desc.Start = PhiAff->Start;
    Desc.Step = PhiAff->Step;
    Desc.ConstantStart = PhiAff->ConstantStart;
    Desc.ConstantStep = PhiAff->ConstantStep;
    Desc.Valid = Desc.Start && Desc.Step;
    return Desc.Valid ? std::optional<InductionDescriptor>(Desc) : std::nullopt;
}
//将给定循环递推表达式求值到指定迭代表达式
const SCEVExpr *SCEVResult::getSCEVAtIteration(const SCEVExpr *E,
                                               const SCEVExpr *Iter,
                                               const Loop *L) {
    if (!E) return nullptr;
    switch (E->Kind) {
    case SCEVExprKind::Constant:
    case SCEVExprKind::Unknown:
        return E;
    case SCEVExprKind::Add:
    case SCEVExprKind::Sub:
    case SCEVExprKind::Mul:
    case SCEVExprKind::Div:
    case SCEVExprKind::Mod:
    case SCEVExprKind::Shl:
    case SCEVExprKind::LShr:
    case SCEVExprKind::AShr:
    case SCEVExprKind::BitAnd:
    case SCEVExprKind::BitOr:
    case SCEVExprKind::BitXor:
    case SCEVExprKind::Min:
    case SCEVExprKind::Max: {
        if (E->Operands.size() != 2) return nullptr;
        const SCEVExpr *LHS = getSCEVAtIteration(E->Operands[0], Iter, L);
        const SCEVExpr *RHS = getSCEVAtIteration(E->Operands[1], Iter, L);
        if (!LHS || !RHS) return nullptr;
        switch (E->Kind) {
        case SCEVExprKind::Add: return add(LHS, RHS);
        case SCEVExprKind::Sub: return sub(LHS, RHS);
        case SCEVExprKind::Mul: return mul(LHS, RHS);
        case SCEVExprKind::Div: return div(LHS, RHS);
        case SCEVExprKind::Mod: return mod(LHS, RHS);
        case SCEVExprKind::Shl:
        case SCEVExprKind::LShr:
        case SCEVExprKind::AShr:
        case SCEVExprKind::BitAnd:
        case SCEVExprKind::BitOr:
        case SCEVExprKind::BitXor:
            return makeBinary(E->Kind, LHS, RHS, E->Ty);
        case SCEVExprKind::Min:
        case SCEVExprKind::Max:
            return makeBinary(E->Kind, LHS, RHS, E->Ty);
        default:
            return nullptr;
        }
    }
    case SCEVExprKind::Binomial: {
        if (E->Operands.size() != 2) return nullptr;
        auto Degree = asConstant(E->Operands[1]);
        if (!Degree || *Degree < 0 ||
            static_cast<uint64_t>(*Degree) > MaxAddRecDegree) {
            return nullptr;
        }
        const SCEVExpr *N =
            getSCEVAtIteration(E->Operands[0], Iter, L);
        return makeBinomial(
            N, static_cast<unsigned>(*Degree), E->Ty);
    }
    case SCEVExprKind::Select:
        if (E->Operands.size() != 3) return nullptr;
        return makeSelect(
            getSCEVAtIteration(E->Operands[0], Iter, L),
            getSCEVAtIteration(E->Operands[1], Iter, L),
            getSCEVAtIteration(E->Operands[2], Iter, L), E->Ty);
    case SCEVExprKind::Cast:
        if (E->Operands.size() != 1) return nullptr;
        return makeCast(E->CastKind,
                        getSCEVAtIteration(E->Operands[0], Iter, L), E->Ty);
    case SCEVExprKind::Compare:
        if (E->Operands.size() != 2) return nullptr;
        return makeCompare(E->Predicate,
                           getSCEVAtIteration(E->Operands[0], Iter, L),
                           getSCEVAtIteration(E->Operands[1], Iter, L));
    case SCEVExprKind::AddRec: {
        if (!E->RecLoop || E->Operands.size() < 2 ||
            E->Operands.size() - 1 > MaxAddRecDegree) {
            return nullptr;
        }
        if (E->RecLoop != L) {
            std::vector<const SCEVExpr *> Coefficients;
            Coefficients.reserve(E->Operands.size());
            for (const SCEVExpr *Coefficient : E->Operands) {
                const SCEVExpr *AtIteration =
                    getSCEVAtIteration(Coefficient, Iter, L);
                if (!AtIteration) return nullptr;
                Coefficients.push_back(AtIteration);
            }
            return makeAddRec(E->RecLoop, Coefficients, E->Ty);
        }
        if (!Iter) return nullptr;
        const SCEVExpr *Result =
            makeConstant(0, E->Ty ? E->Ty : Iter->Ty);
        for (std::size_t Degree = 0;
             Degree < E->Operands.size(); ++Degree) {
            const SCEVExpr *Coefficient =
                getSCEVAtIteration(E->Operands[Degree], Iter, L);
            const SCEVExpr *Choose = makeBinomial(
                Iter, static_cast<unsigned>(Degree), Iter->Ty);
            if (!Coefficient || !Choose) return nullptr;
            const SCEVExpr *Term = mul(Coefficient, Choose);
            Result = add(Result, Term);
            if (!Result) return nullptr;
        }
        return Result;
    }
    case SCEVExprKind::MulRec:
    case SCEVExprKind::DivRec: {
        if (E->Operands.size() != 2) return nullptr;
        if (E->RecLoop == L) return E;
        const SCEVExpr *Base =
            getSCEVAtIteration(E->Operands[0], Iter, L);
        const SCEVExpr *Step =
            getSCEVAtIteration(E->Operands[1], Iter, L);
        return makeRec(E->Kind, E->RecLoop, Base, Step, E->Ty);
    }
    }
    return nullptr;
}
//遗忘与给定IR值有关的全部按需分析缓存
void SCEVResult::forgetValue(Value *) {
    clear();
}
//遗忘与给定循环有关的全部按需分析缓存
void SCEVResult::forgetLoop(const Loop *) {
    clear();
}
//遗忘当前函数中所有循环相关的按需分析缓存
void SCEVResult::forgetAllLoops() {
    clear();
}
//清空全部表达式存储和分析缓存
void SCEVResult::clear() {
    ExprStorage.clear();
    Interned.clear();
    ValueCache.clear();
    ActiveValues.clear();
    TripCountCache.clear();
    BackedgeCountCache.clear();
    LoopInductions.clear();
    LoopInductionsReady = false;
}
//返回函数中识别出的全部LoopInductionInfo映射
const std::unordered_map<Value *, LoopInductionInfo> &
SCEVResult::getLoopInductions() {
    if (LoopInductionsReady) return LoopInductions;
    LoopInductionsReady = true;
    LoopInductions.clear();
    if (!Func) return LoopInductions;
    for (BasicBlock *BB : *Func) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            auto *Phi = dynamic_cast<PhiInst *>(I);
            if (!Phi) break;
            auto Aff = getAffine(Phi);
            if (!Aff || !Aff->ConstantStart || !Aff->ConstantStep) continue;
            LoopInductionInfo Info;
            Info.RecLoop = Aff->RecLoop;
            Info.initial = *Aff->ConstantStart;
            Info.step = *Aff->ConstantStep;
            if (auto TCI = analyzeLoopTripCount(Aff->RecLoop)) {
                if (TCI->ProvenNoWrap && TCI->ConstantTripCount) {
                    const int64_t C = *TCI->ConstantTripCount;
                    if (C > 0) {
                        const __int128 Last = static_cast<__int128>(Info.initial) +
                            static_cast<__int128>(Info.step) *
                                static_cast<__int128>(C - 1);
                        if (Last < IntRange::I32_MIN ||
                            Last > IntRange::I32_MAX)
                            continue;
                        Info.hasUpperBound = true;
                        Info.noWrap = true;
                        Info.upperBound = static_cast<int64_t>(Last);
                    }
                }
            }
            LoopInductions[Phi] = Info;
        }
    }
    return LoopInductions;
}
//将给定SCEV表达式格式化为可读字符串
std::string SCEVResult::format(const SCEVExpr *E) const {
    if (!E) return "<null>";
    switch (E->Kind) {
    case SCEVExprKind::Constant:
        return std::to_string(E->ConstValue);
    case SCEVExprKind::Unknown:
        return E->RawValue ? E->RawValue->getIdent() : "<unknown>";
    case SCEVExprKind::Add:
        return "(" + format(E->Operands[0]) + " + " + format(E->Operands[1]) + ")";
    case SCEVExprKind::Sub:
        return "(" + format(E->Operands[0]) + " - " + format(E->Operands[1]) + ")";
    case SCEVExprKind::Mul:
        return "(" + format(E->Operands[0]) + " * " + format(E->Operands[1]) + ")";
    case SCEVExprKind::Div:
        return "(" + format(E->Operands[0]) + " / " + format(E->Operands[1]) + ")";
    case SCEVExprKind::Mod:
        return "(" + format(E->Operands[0]) + " % " + format(E->Operands[1]) + ")";
    case SCEVExprKind::Shl:
        return "(" + format(E->Operands[0]) + " << " +
               format(E->Operands[1]) + ")";
    case SCEVExprKind::LShr:
        return "lshr(" + format(E->Operands[0]) + ", " +
               format(E->Operands[1]) + ")";
    case SCEVExprKind::AShr:
        return "ashr(" + format(E->Operands[0]) + ", " +
               format(E->Operands[1]) + ")";
    case SCEVExprKind::BitAnd:
        return "(" + format(E->Operands[0]) + " & " +
               format(E->Operands[1]) + ")";
    case SCEVExprKind::BitOr:
        return "(" + format(E->Operands[0]) + " | " +
               format(E->Operands[1]) + ")";
    case SCEVExprKind::BitXor:
        return "(" + format(E->Operands[0]) + " ^ " +
               format(E->Operands[1]) + ")";
    case SCEVExprKind::Binomial:
        return "C(" + format(E->Operands[0]) + ", " +
               format(E->Operands[1]) + ")";
    case SCEVExprKind::Min:
        return "min(" + format(E->Operands[0]) + ", " + format(E->Operands[1]) + ")";
    case SCEVExprKind::Max:
        return "max(" + format(E->Operands[0]) + ", " + format(E->Operands[1]) + ")";
    case SCEVExprKind::Select:
        return "select(" + format(E->Operands[0]) + ", " +
               format(E->Operands[1]) + ", " +
               format(E->Operands[2]) + ")";
    case SCEVExprKind::Cast:
        return std::string(SCEVExpr::castName(E->CastKind)) + "(" +
               format(E->Operands[0]) + ")";
    case SCEVExprKind::Compare:
        return "(" + format(E->Operands[0]) + " " +
               SCEVExpr::predicateName(E->Predicate) +
               " " + format(E->Operands[1]) + ")";
    case SCEVExprKind::AddRec:
    {
        std::ostringstream OS;
        OS << "{";
        for (std::size_t I = 0; I < E->Operands.size(); ++I) {
            if (I != 0) OS << ",+, ";
            OS << format(E->Operands[I]);
        }
        OS << "}<"
           << (E->RecLoop && E->RecLoop->getHeader()
                   ? E->RecLoop->getHeader()->getIdent()
                   : "?")
           << ">";
        return OS.str();
    }
    case SCEVExprKind::MulRec:
        return "{" + format(E->Operands[0]) + ",*, " + format(E->Operands[1]) +
               "}<" + (E->RecLoop && E->RecLoop->getHeader()
                            ? E->RecLoop->getHeader()->getIdent()
                            : "?") +
               ">";
    case SCEVExprKind::DivRec:
        return "{" + format(E->Operands[0]) + ",/, " + format(E->Operands[1]) +
               "}<" + (E->RecLoop && E->RecLoop->getHeader()
                            ? E->RecLoop->getHeader()->getIdent()
                            : "?") +
               ">";
    }
    return "<bad-scev>";
}
//运行SCEV分析并返回绑定循环信息与支配树的按需结果
SCEVAnalysis::Result
SCEVAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    //1.循环递推识别依赖 LoopInfo，退出形状验证依赖 DominatorTree。
    auto &Loops = FAM.getResult<LoopAnalysis>(F);
    auto &Dom = FAM.getResult<DominantAnalysis>(F);
    //2.返回惰性上下文；值表达式和次数信息在查询时驻留并缓存。
    return SCEVResult(&F, &Loops, &Dom);
}
