#pragma once

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

//SCEVExprKind枚举SCEV表达式支持的所有符号节点种类
enum class SCEVExprKind {
    //整数常量表达式
    Constant,
    //无法进一步分析的原始IR值表达式
    Unknown,
    //加法表达式
    Add,
    //减法表达式
    Sub,
    //乘法表达式
    Mul,
    //除法表达式
    Div,
    //取余表达式
    Mod,
    //逻辑左移表达式
    Shl,
    //逻辑右移表达式
    LShr,
    //算术右移表达式
    AShr,
    //按位与表达式
    BitAnd,
    //按位或表达式
    BitOr,
    //按位异或表达式
    BitXor,
    //二项式系数表达式
    Binomial,
    //最小值表达式
    Min,
    //最大值表达式
    Max,
    //条件选择表达式
    Select,
    //类型转换表达式
    Cast,
    //比较表达式
    Compare,
    //加法循环递推表达式
    AddRec,
    //乘法循环递推表达式
    MulRec,
    //除法循环递推表达式
    DivRec
};

//SCEVCastKind枚举SCEV类型转换节点支持的转换种类
enum class SCEVCastKind {
    //截断整数位宽
    Trunc,
    //零扩展整数位宽
    ZExt,
    //符号扩展整数位宽
    SExt,
    //保持位模式的类型转换
    BitCast
};

//SCEVExpr表示一个经过驻留共享的不可变标量演化表达式节点
struct SCEVExpr {
    //当前表达式节点的种类
    SCEVExprKind Kind = SCEVExprKind::Unknown;
    //Cast节点使用的类型转换种类
    SCEVCastKind CastKind = SCEVCastKind::BitCast;
    //Compare节点使用的比较谓词
    BinaryInst::Operation Predicate = BinaryInst::E;
    //Unknown节点对应的原始IR值
    Value *RawValue = nullptr;
    //递推节点所属的循环
    const Loop *RecLoop = nullptr;
    //Constant节点保存的整数值
    int64_t ConstValue = 0;
    //当前表达式的IR类型
    std::shared_ptr<IRType> Ty;
    //当前表达式按照节点语义保存的操作数
    std::vector<const SCEVExpr *> Operands;
    //用于表达式驻留去重的结构化键
    std::string Key;

    //判断当前节点是否为给定整数常量
    bool isConstant(int64_t V) const;
    //判断当前节点是否为循环递推表达式
    bool isRecurrence() const;
    //判断当前表达式及其子表达式是否引用给定循环
    bool mentionsLoop(const Loop *L) const;
    //返回给定表达式种类的可读名称
    static const char *kindName(SCEVExprKind K);
    //返回给定类型转换种类的可读名称
    static const char *castName(SCEVCastKind K);
    //返回给定比较谓词的可读名称
    static const char *predicateName(BinaryInst::Operation Op);
};

//SCEVAffineInfo描述一阶仿射递推的循环、起点和步长
struct SCEVAffineInfo {
    //仿射递推所属的循环
    const Loop *RecLoop = nullptr;
    //仿射递推的起始表达式
    const SCEVExpr *Start = nullptr;
    //仿射递推每次迭代增加的步长表达式
    const SCEVExpr *Step = nullptr;
    //起始表达式能够折叠时保存的常量值
    std::optional<int64_t> ConstantStart;
    //步长表达式能够折叠时保存的常量值
    std::optional<int64_t> ConstantStep;
};

//SCEVAddRecInfo使用二项式基底系数描述加法循环递推
struct SCEVAddRecInfo {
    //加法递推所属的循环
    const Loop *RecLoop = nullptr;
    //按照E(n)=C0*C(n,0)+...+Cd*C(n,d)保存全部递推系数
    std::vector<const SCEVExpr *> Coefficients;

    //返回当前加法递推多项式的次数
    unsigned getDegree() const;
    //判断当前加法递推是否是一阶仿射递推
    bool isAffine() const;
};

//SCEVStepSign枚举Range分析能够证明的仿射步长符号
enum class SCEVStepSign {
    //无法确定步长符号
    Unknown,
    //步长恒等于零
    Zero,
    //步长严格大于零
    StrictlyPositive,
    //步长严格小于零
    StrictlyNegative,
    //步长大于等于零
    NonNegative,
    //步长小于等于零
    NonPositive
};

//SCEVStepRangeInfo描述仿射递推步长的范围与符号证据
struct SCEVStepRangeInfo {
    //步长所属递推的循环
    const Loop *RecLoop = nullptr;
    //被分析的步长表达式
    const SCEVExpr *Step = nullptr;
    //Range分析得到的步长整数范围
    IntRange Range;
    //根据整数范围分类得到的步长符号
    SCEVStepSign Sign = SCEVStepSign::Unknown;
    //标记当前步长范围信息是否有效
    bool Valid = false;
};

//InductionDescriptor描述规范归纳变量及其起点和更新步长
struct InductionDescriptor {
    //规范归纳变量对应的Header PHI
    PhiInst *Phi = nullptr;
    //规范归纳变量所属的循环
    const Loop *L = nullptr;
    //来自循环外的归纳起始IR值
    Value *StartValue = nullptr;
    //来自回边的归纳更新IR值
    Value *UpdateValue = nullptr;
    //归纳起始值对应的SCEV表达式
    const SCEVExpr *Start = nullptr;
    //归纳步长对应的SCEV表达式
    const SCEVExpr *Step = nullptr;
    //起始表达式能够折叠时保存的常量值
    std::optional<int64_t> ConstantStart;
    //步长表达式能够折叠时保存的常量值
    std::optional<int64_t> ConstantStep;
    //标记当前归纳变量描述是否有效
    bool Valid = false;
};

//LoopExitComparePhase枚举退出比较观察归纳值的迭代阶段
enum class LoopExitComparePhase {
    //无法确定退出比较阶段
    Unknown,
    //退出比较使用当前Header PHI值
    HeaderPhi,
    //退出比较使用本轮回边更新后的值
    BackedgeUpdate
};

//LoopTripCountInfo描述规范循环形状、退出条件、迭代次数和无回绕证明
struct LoopTripCountInfo {
    //被分析的循环
    const Loop *L = nullptr;
    //循环唯一的预头块
    BasicBlock *PreHeader = nullptr;
    //循环头基本块
    BasicBlock *Header = nullptr;
    //循环唯一的回边块
    BasicBlock *Latch = nullptr;
    //执行退出分支的循环内基本块
    BasicBlock *ExitingBlock = nullptr;
    //退出分支到达的循环外基本块
    BasicBlock *ExitBlock = nullptr;
    //控制循环退出的比较指令
    BinaryInst *ExitCmp = nullptr;
    //标记比较结果为真时是否继续循环
    bool ContinueOnTrue = true;
    //退出比较所观察的归纳值阶段
    LoopExitComparePhase ComparePhase = LoopExitComparePhase::Unknown;
    //退出比较使用的归纳PHI
    PhiInst *IndVar = nullptr;
    //归纳变量的起始SCEV表达式
    const SCEVExpr *Start = nullptr;
    //归纳变量的步长SCEV表达式
    const SCEVExpr *Step = nullptr;
    //退出比较的边界SCEV表达式
    const SCEVExpr *Bound = nullptr;
    //循环总迭代次数的SCEV表达式
    const SCEVExpr *TripCount = nullptr;
    //循环回边执行次数的SCEV表达式
    const SCEVExpr *BackedgeTakenCount = nullptr;
    //总迭代次数能够折叠时保存的常量值
    std::optional<int64_t> ConstantTripCount;
    //回边执行次数能够折叠时保存的常量值
    std::optional<int64_t> ConstantBackedgeTakenCount;
    //标记迭代次数是否依赖运行时值
    bool IsRuntime = false;
    //标记循环是否可能一次也不执行
    bool MayBeZeroTrip = true;
    //标记归纳更新已被证明不会发生有符号回绕
    bool ProvenNoWrap = false;
    //标记当前迭代次数描述是否有效
    bool Valid = false;
};

//LoopRemainderInfo描述按展开因子拆分后的主循环和余数循环次数
struct LoopRemainderInfo {
    //被拆分的循环
    const Loop *L = nullptr;
    //主循环每轮覆盖的原循环迭代数
    int64_t Factor = 1;
    //原循环总迭代次数表达式
    const SCEVExpr *TripCount = nullptr;
    //能够由主循环覆盖的迭代次数表达式
    const SCEVExpr *MainTripCount = nullptr;
    //需要由余数循环执行的迭代次数表达式
    const SCEVExpr *RemainderCount = nullptr;
    //总迭代次数能够折叠时保存的常量值
    std::optional<int64_t> ConstantTripCount;
    //主循环覆盖次数能够折叠时保存的常量值
    std::optional<int64_t> ConstantMainTripCount;
    //余数次数能够折叠时保存的常量值
    std::optional<int64_t> ConstantRemainderCount;
    //标记变换后是否需要保留余数循环
    bool NeedRemainderLoop = false;
    //标记余数次数是否只能在运行时确定
    bool IsRuntimeRemainder = false;
    //标记当前余数拆分信息是否有效
    bool Valid = false;
};

//SCEVResult表示函数级标量演化分析结果并按需缓存符号表达式和循环事实
class SCEVResult {
public:
    //构造一个不绑定函数和依赖分析的空SCEV结果
    SCEVResult();
    //构造绑定给定函数、循环信息和支配树的SCEV结果
    SCEVResult(Function *F, LoopInfo *LI, DominatorTree *DT);
    //移动构造SCEV结果并接管表达式与缓存存储
    SCEVResult(SCEVResult &&);
    //移动赋值SCEV结果并接管表达式与缓存存储
    SCEVResult &operator=(SCEVResult &&);
    //禁止拷贝构造以保证驻留表达式指针稳定
    SCEVResult(const SCEVResult &) = delete;
    //禁止拷贝赋值以保证驻留表达式指针稳定
    SCEVResult &operator=(const SCEVResult &) = delete;

    //返回给定IR值对应的SCEV表达式并按需创建缓存
    const SCEVExpr *getSCEV(Value *V);
    //返回给定循环的总迭代次数表达式
    const SCEVExpr *getTripCount(const Loop *L);
    //返回给定循环的回边执行次数表达式
    const SCEVExpr *getBackedgeTakenCount(const Loop *L);
    //返回给定IR值能够表示的一阶仿射递推信息
    std::optional<SCEVAffineInfo> getAffine(Value *V);
    //返回给定SCEV表达式能够表示的一阶仿射递推信息
    std::optional<SCEVAffineInfo> getAffine(const SCEVExpr *E) const;
    //返回给定IR值能够表示的加法递推信息
    std::optional<SCEVAddRecInfo> getAddRecurrence(Value *V);
    //返回给定SCEV表达式能够表示的加法递推信息
    std::optional<SCEVAddRecInfo>
    getAddRecurrence(const SCEVExpr *E) const;
    //返回函数中识别出的全部LoopInductionInfo映射
    const std::unordered_map<Value *, LoopInductionInfo> &getLoopInductions();

    //构造两个SCEV表达式的取余结果
    const SCEVExpr *mod(const SCEVExpr *L, const SCEVExpr *R);

    //分析给定循环并返回完整的迭代次数信息
    std::optional<LoopTripCountInfo> analyzeLoopTripCount(const Loop *L);
    //返回给定循环能够证明的较小常量总迭代次数
    std::optional<int64_t> getSmallConstantTripCount(const Loop *L);
    //返回给定循环能够证明的较小常量回边执行次数
    std::optional<int64_t> getSmallConstantBackedgeTakenCount(const Loop *L);
    //返回给定循环总迭代次数能够证明的常量倍数
    std::optional<int64_t> getSmallConstantTripMultiple(const Loop *L);
    //判断给定循环总迭代次数是否必为指定因子的倍数
    bool isTripCountMultipleOf(const Loop *L, int64_t Factor);
    //返回给定展开因子对应的主循环和余数循环次数信息
    std::optional<LoopRemainderInfo> getRemainderInfo(const Loop *L,
                                                      int64_t Factor);
    //返回给定循环能够识别出的规范归纳变量
    std::optional<InductionDescriptor> getCanonicalInduction(const Loop *L);

    //仅依赖基础SCEV事实判断给定循环是否已知有限
    bool isKnownFiniteLoop(const Loop *L);
    //结合Range和支配信息判断给定循环是否有限且归纳更新不回绕
    bool isKnownFiniteLoop(const Loop *L, RangeInfo &RI,
                           DominatorTree &DT);

    //判断给定值是否属于已证明不回绕的循环归纳链
    bool isKnownNoWrapInductionValue(const Loop *L, Value *V);

    //在循环预头处结合Range与支配信息证明运行时归纳更新不回绕
    bool proveRuntimeNoWrapAtPreheader(const LoopTripCountInfo &Info,
                                       RangeInfo &RI,
                                       DominatorTree &DT);
    //分析给定仿射SCEV表达式在指定上下文中的步长范围和符号
    std::optional<SCEVStepRangeInfo>
    analyzeAffineStepRange(const SCEVExpr *E, BasicBlock *Context,
                           RangeInfo &RI, DominatorTree &DT) const;
    //分析给定IR值在指定上下文中的仿射步长范围和符号
    std::optional<SCEVStepRangeInfo>
    analyzeAffineStepRange(Value *V, BasicBlock *Context,
                           RangeInfo &RI, DominatorTree &DT);
    //将给定循环递推表达式求值到指定迭代表达式
    const SCEVExpr *getSCEVAtIteration(const SCEVExpr *E,
                                       const SCEVExpr *Iter,
                                       const Loop *L);

    //遗忘与给定IR值有关的全部按需分析缓存
    void forgetValue(Value *V);
    //遗忘与给定循环有关的全部按需分析缓存
    void forgetLoop(const Loop *L);
    //遗忘当前函数中所有循环相关的按需分析缓存
    void forgetAllLoops();
    //清空全部表达式存储和分析缓存
    void clear();

    //将给定SCEV表达式格式化为可读字符串
    std::string format(const SCEVExpr *E) const;
    //根据PreservationStatus判断当前SCEV分析结果是否失效
    bool invalidate(Function &F, const PreservationStatus &PA);

private:
    //限制能够构造的加法递推最高多项式次数
    static constexpr unsigned MaxAddRecDegree = 8;

    //递归提取只由常量整数运算组成的IR值精确结果
    static std::optional<int64_t> getTrivialExactIntConstant(
        Value *V, std::unordered_set<Value *> &Active, unsigned Depth = 0);

    //UpdateDelta描述Header PHI更新表达式中的自引用和净增量
    struct UpdateDelta {
        //标记更新表达式中是否恰好观察到归纳PHI自身
        bool SeenSelf = false;
        //去除归纳PHI自身后得到的加法增量表达式
        const SCEVExpr *Delta = nullptr;
    };

    //当前分析结果绑定的函数
    Function *Func = nullptr;
    //当前函数对应的循环分析结果
    LoopInfo *LI = nullptr;
    //当前函数对应的支配树分析结果
    DominatorTree *DT = nullptr;

    //持有全部驻留SCEV表达式节点并保证其地址稳定
    std::vector<std::unique_ptr<SCEVExpr>> ExprStorage;
    //将结构化键映射到已经驻留的SCEV表达式
    std::unordered_map<std::string, const SCEVExpr *> Interned;
    //将IR值映射到按需构造的SCEV表达式
    std::unordered_map<Value *, const SCEVExpr *> ValueCache;
    //记录当前正在递归构造的IR值以截断SSA环
    std::unordered_map<Value *, bool> ActiveValues;
    //缓存每个循环的总迭代次数表达式
    std::unordered_map<const Loop *, const SCEVExpr *> TripCountCache;
    //缓存每个循环的回边执行次数表达式
    std::unordered_map<const Loop *, const SCEVExpr *> BackedgeCountCache;
    //缓存从函数循环中识别出的归纳信息
    std::unordered_map<Value *, LoopInductionInfo> LoopInductions;
    //标记LoopInductions缓存是否已经完整构建
    bool LoopInductionsReady = false;

    //创建或复用给定整数值和类型对应的常量表达式
    const SCEVExpr *makeConstant(int64_t V, std::shared_ptr<IRType> Ty = nullptr);
    //创建或复用给定原始IR值对应的未知表达式
    const SCEVExpr *makeUnknown(Value *V);
    //创建或折叠给定种类和操作数对应的二元表达式
    const SCEVExpr *makeBinary(SCEVExprKind K, const SCEVExpr *L,
                               const SCEVExpr *R, std::shared_ptr<IRType> Ty = nullptr);
    //创建或折叠给定转换种类、操作数和目标类型对应的转换表达式
    const SCEVExpr *makeCast(SCEVCastKind K, const SCEVExpr *Op,
                             std::shared_ptr<IRType> Ty);
    //创建或折叠给定谓词和操作数对应的比较表达式
    const SCEVExpr *makeCompare(BinaryInst::Operation Pred, const SCEVExpr *L,
                                const SCEVExpr *R);
    //创建或折叠条件、真值和假值对应的选择表达式
    const SCEVExpr *makeSelect(const SCEVExpr *Cond,
                               const SCEVExpr *TrueExpr,
                               const SCEVExpr *FalseExpr,
                               std::shared_ptr<IRType> Ty);
    //创建或折叠给定迭代数和阶数对应的二项式表达式
    const SCEVExpr *makeBinomial(const SCEVExpr *N, unsigned K,
                                 std::shared_ptr<IRType> Ty = nullptr);
    //创建或复用给定循环和二项式系数对应的加法递推表达式
    const SCEVExpr *makeAddRec(
        const Loop *L, const std::vector<const SCEVExpr *> &Coefficients,
        std::shared_ptr<IRType> Ty = nullptr);
    //根据基础递推和更新增量构造高一阶加法递推
    const SCEVExpr *makeAddRecFromDelta(
        const Loop *L, const SCEVExpr *Base, const SCEVExpr *Delta,
        std::shared_ptr<IRType> Ty = nullptr);
    //创建或复用给定种类、循环、起点和步长对应的递推表达式
    const SCEVExpr *makeRec(SCEVExprKind K, const Loop *L, const SCEVExpr *Base,
                            const SCEVExpr *Step, std::shared_ptr<IRType> Ty = nullptr);
    //按照结构化键驻留表达式并返回稳定节点指针
    const SCEVExpr *intern(std::unique_ptr<SCEVExpr> E);

    //构造或折叠两个SCEV表达式的和
    const SCEVExpr *add(const SCEVExpr *L, const SCEVExpr *R);
    //构造或折叠两个SCEV表达式的差
    const SCEVExpr *sub(const SCEVExpr *L, const SCEVExpr *R);
    //构造或折叠两个SCEV表达式的积
    const SCEVExpr *mul(const SCEVExpr *L, const SCEVExpr *R);
    //构造或折叠两个SCEV表达式的商
    const SCEVExpr *div(const SCEVExpr *L, const SCEVExpr *R);
    //构造给定SCEV表达式的相反数
    const SCEVExpr *neg(const SCEVExpr *E);
    //使用循环不变因子缩放加法递推的全部系数
    const SCEVExpr *scaleAddRec(const SCEVExpr *Rec,
                                const SCEVExpr *Factor);
    //按照二项式基底乘法规则合并两个同循环加法递推
    const SCEVExpr *multiplyAddRecs(const SCEVExpr *L,
                                    const SCEVExpr *R);
    //返回两个二项式基函数乘积对指定结果阶数的整数系数
    std::optional<int64_t>
    getBinomialProductFactor(unsigned LeftDegree, unsigned RightDegree,
                             unsigned ResultDegree) const;

    //根据IR值种类分派并构造对应的SCEV表达式
    const SCEVExpr *create(Value *V);
    //根据二元IR指令构造对应的SCEV表达式
    const SCEVExpr *createFromBinary(BinaryInst *BI);
    //根据PHI指令构造未知值或循环递推表达式
    const SCEVExpr *createFromPhi(PhiInst *Phi);
    //根据选择指令构造选择、最小值或最大值表达式
    const SCEVExpr *createFromSelect(SelectInst *Sel);
    //根据GEP指令构造基础地址和索引偏移表达式
    const SCEVExpr *createFromGEP(GetElementPtrInst *GEP);
    //根据IR转换指令构造对应的SCEV转换表达式
    const SCEVExpr *createFromCast(Instruction *I, SCEVCastKind CK);

    //尝试将循环Header PHI识别为加法、乘法或除法递推
    const SCEVExpr *tryHeaderPhi(PhiInst *Phi, const Loop *L);
    //尝试从PHI回边更新值中提取统一加法增量
    std::optional<UpdateDelta> findAdditiveDelta(PhiInst *Self, Value *V,
                                                 const Loop *L);
    //递归遍历PHI更新表达式并提取统一加法增量
    std::optional<UpdateDelta> findAdditiveDeltaImpl(
        PhiInst *Self, Value *V, const Loop *L,
        std::unordered_set<Value *> &Active, unsigned Depth);
    //尝试将PHI更新识别为简单乘法或除法递推
    const SCEVExpr *trySimpleMulDivRec(PhiInst *Self, Value *Update,
                                       Value *Start, const Loop *L);

    //判断给定SCEV表达式是否对指定循环保持不变
    bool isLoopInvariantExpr(const SCEVExpr *E, const Loop *L) const;
    //判断给定表达式是否为指定循环上的多项式加法递推
    bool isPolynomialAddRec(const SCEVExpr *E, const Loop *L) const;
    //判断给定SCEV表达式对应的IR计算是否可安全丢弃
    bool isSafeToDiscard(const SCEVExpr *E) const;
    //递归判断SCEV表达式及其子表达式对应的IR计算是否可安全丢弃
    bool isSafeToDiscardImpl(
        const SCEVExpr *E,
        std::unordered_set<const SCEVExpr *> &Active,
        unsigned Depth) const;
    //判断给定基本块是否直接属于循环而非其子循环
    bool isDirectLoopBlock(const Loop *L, BasicBlock *BB) const;
    //判断两个SCEV表达式是否表示同一驻留结构
    bool sameExpr(const SCEVExpr *A, const SCEVExpr *B) const;
    //尝试从给定SCEV表达式提取整数常量
    std::optional<int64_t> asConstant(const SCEVExpr *E) const;
    //返回给定IR类型用于表达式驻留的结构化键
    std::string typeKey(std::shared_ptr<IRType> Ty) const;
    //递归计算SCEV表达式在指定上下文中用于方向判断的整数范围
    std::optional<IntRange> evaluateRangeForDirection(
        const SCEVExpr *E, BasicBlock *Context, RangeInfo &RI,
        DominatorTree &DT, std::unordered_set<const SCEVExpr *> &Active,
        unsigned Depth) const;
    //根据整数范围分类步长的符号方向
    SCEVStepSign classifyStepSign(const IntRange &Range) const;

    //按照函数顺序收集直接属于给定循环的基本块
    std::vector<BasicBlock *> collectDirectLoopBlocks(const Loop *L) const;
    //计算给定循环的总迭代次数表达式
    const SCEVExpr *computeTripCount(const Loop *L);
    //从给定循环退出比较直接提取总迭代次数表达式
    const SCEVExpr *tripFromComparison(const Loop *L, BinaryInst *Cmp,
                                       bool ContinueOnTrue);
    //从已绑定的退出比较补全迭代次数信息
    bool fillTripCountFromComparison(LoopTripCountInfo &Info);
    //判断退出比较是否以严格单位步长推进到边界
    bool hasStrictUnitStepExit(
        const InductionDescriptor &Induction,
        const LoopTripCountInfo &Trip) const;
    //判断回边更新阶段使用的起点是否能够安全套用次数公式
    bool isBackedgeUpdateStartSafe(
        const Loop *L, const InductionDescriptor &Induction,
        const LoopTripCountInfo &Trip);
    //查找给定循环中对应指定加法递推的Header PHI
    PhiInst *findPhiForAddRec(const Loop *L, const SCEVExpr *AddRec);
    //查找给定循环中对应指定仿射递推的Header PHI
    PhiInst *findPhiForAffineRec(const Loop *L, const SCEVAffineInfo &Affine);
};

//SCEVAnalysis是构建函数级标量演化结果的分析Pass
class SCEVAnalysis : public AnalysisDescriptor<SCEVAnalysis> {
public:
    //分析Pass返回的结果类型
    using Result = SCEVResult;
    //运行SCEV分析并返回绑定循环信息与支配树的按需结果
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    //允许AnalysisDescriptor访问本分析Pass的唯一标识
    friend AnalysisDescriptor<SCEVAnalysis>;
    //当前分析Pass的唯一标识
    inline static PassID Key;
};
