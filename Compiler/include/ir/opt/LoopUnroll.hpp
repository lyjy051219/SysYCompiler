#pragma once
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

//UnrollKind表示循环展开决策选用的具体变换方式
enum class UnrollKind {
    //None表示保留原循环而不执行展开
    None,
    //Full表示按照常量迭代次数完全展开循环
    Full,
    //Partial表示按整除因子部分展开且不需要余数路径
    Partial,
    //PartialWithRemainder表示部分展开常量次数循环并生成余数副本
    PartialWithRemainder,
    //Runtime表示根据运行时迭代次数生成主循环和动态尾部
    Runtime,
    //Peeling表示在原循环之前剥离若干次迭代
    Peeling
};

//LoopUnrollResult表示一次循环展开尝试对IR产生的修改程度
enum class LoopUnrollResult {
    //Unmodified表示循环未被任何展开方式修改
    Unmodified,
    //PartiallyUnrolled表示循环被部分展开或执行了peeling
    PartiallyUnrolled,
    //FullyUnrolled表示循环控制结构已被完全消除
    FullyUnrolled
};

//UnrollConfig集中配置各展开方式的开关、成本阈值和规模上限
struct UnrollConfig {
    //AllowFull控制是否允许执行完全展开
    bool AllowFull = true;
    //AllowPartial控制是否允许执行常量次数部分展开
    bool AllowPartial = true;
    //AllowRuntime控制是否允许运行时次数展开
    bool AllowRuntime = true;
    //AllowRemainder控制部分展开是否允许生成余数路径
    bool AllowRemainder = true;
    //AllowPeeling控制是否允许在原循环前剥离迭代
    bool AllowPeeling = true;
    //UnrollRemainder控制生成的余数循环是否继续参与展开
    bool UnrollRemainder = false;
    //FullThreshold限制完全展开后的抽象代码成本
    int FullThreshold = 192;
    //PartialThreshold限制常量次数部分展开后的抽象代码成本
    int PartialThreshold = 160;
    //RuntimeThreshold限制运行时展开后的抽象代码成本
    int RuntimeThreshold = 96;
    //FullUnrollMaxCount限制普通循环允许完全展开的最大次数
    int FullUnrollMaxCount = 16;
    //TinyLoopFullUnrollMaxCount放宽极小循环允许完全展开的最大次数
    int TinyLoopFullUnrollMaxCount = 32;
    //PartialMaxCount限制常量次数部分展开因子
    int PartialMaxCount = 4;
    //RuntimeDefaultCount指定运行时展开的默认因子
    int RuntimeDefaultCount = 4;
    //RuntimeMaxCount限制运行时展开因子
    int RuntimeMaxCount = 4;
    //MaxLoopBodyCostForPartial限制可部分展开的原循环体成本
    int MaxLoopBodyCostForPartial = 72;
    //MaxLoopBodyCostForRuntime限制可运行时展开的原循环体成本
    int MaxLoopBodyCostForRuntime = 48;
    //MaxPeelCount限制已证明足够长循环的最大剥离次数
    int MaxPeelCount = 2;
    //MaxGuardedPeelCount限制需要保留退出检查时的最大剥离次数
    int MaxGuardedPeelCount = 1;
    //PeelThreshold限制peeling产生的静态代码成本
    int PeelThreshold = 64;
    //MaxPeelBlocks限制peeling允许克隆的循环基本块数量
    int MaxPeelBlocks = 24;
    //PeelStoreCost估算每次可删除冗余store带来的动态收益
    int PeelStoreCost = 2;
    //PeelMinNetBenefit要求不变store触发peeling时至少获得的净收益
    int PeelMinNetBenefit = 1;
    //SCEVExpansionBudget限制运行时次数表达式可新增的IR指令数量
    int SCEVExpansionBudget = 8;
    //AllowExpensiveTripCount控制是否接受超过低成本预算的次数表达式
    bool AllowExpensiveTripCount = false;
    //ReductionAccumulatorMaxLanes限制单个整数归约同时保留的独立累加器数量
    int ReductionAccumulatorMaxLanes = 2;
    //ReductionRegisterBudget限制整数寄存器活跃状态和新增累加器的总预算
    int ReductionRegisterBudget = 12;
    //ReductionMinChainBenefit要求重关联后关键依赖链至少获得的动态净收益
    int ReductionMinChainBenefit = 1;
};

//UnrollDecision保存成本模型为单个循环选择的展开方式和执行参数
struct UnrollDecision {
    //Kind记录最终选中的循环展开方式
    UnrollKind Kind = UnrollKind::None;
    //Count记录展开因子或peeling次数
    int Count = 1;
    //ShouldUnroll标记是否应提交当前展开决策
    bool ShouldUnroll = false;
    //NeedRemainder标记部分展开是否需要生成余数路径
    bool NeedRemainder = false;
    //IsRuntime标记决策是否依赖运行时迭代次数
    bool IsRuntime = false;
    //IsPeeling标记决策是否执行循环迭代剥离
    bool IsPeeling = false;
    //TripCountIsProven标记运行时次数的固定位宽计算是否已证明无回绕
    bool TripCountIsProven = false;
};

//IntegerReductionPlan记录已决定展开循环中的一个整数结合律归约
struct IntegerReductionPlan {
    //Phi指向承载循环入口状态的原Header PHI
    PhiInst *Phi = nullptr;
    //Update指向计算一次归约更新的二元整数指令
    BinaryInst *Update = nullptr;
    //Contribution记录每次迭代合入累加状态的值
    Value *Contribution = nullptr;
    //InitialValue记录循环进入前的归约初值
    Value *InitialValue = nullptr;
    //Op记录可安全重关联的整数结合律操作
    BinaryInst::Operation Op = BinaryInst::ADD;
    //AccumulatorOperand记录Update中归约状态所在的操作数位置
    unsigned AccumulatorOperand = 0;
    //LaneCount记录收益与寄存器预算共同允许的累加器路数
    int LaneCount = 1;
    //SplitIndex记录展开副本在两条独立累加链之间的切分位置
    int SplitIndex = 0;
    //CriticalChainSaving记录每个展开组缩短的串行归约指令数
    int CriticalChainSaving = 0;
    //EstimatedNetBenefit记录扣除最终合并成本后的动态依赖链净收益
    std::int64_t EstimatedNetBenefit = 0;
};

//IntegerReductionRewrite保存提交重关联前已完整映射的展开副本
struct IntegerReductionRewrite {
    //Plan指向本次重写对应的只读归约计划
    const IntegerReductionPlan *Plan = nullptr;
    //PrimaryPhi指向部分展开后继续承载第一条累加链的Header PHI
    PhiInst *PrimaryPhi = nullptr;
    //Updates按展开迭代顺序保存每个归约更新副本
    std::vector<BinaryInst *> Updates;
    //Contributions按展开迭代顺序保存每次迭代的归约输入
    std::vector<Value *> Contributions;
    //ResultUses保存最终串行更新在循环外的原有使用
    std::vector<Use *> ResultUses;
    //ResultPhis保存Header退出时将由最终合并值整体替换的LCSSA PHI
    std::vector<PhiInst *> ResultPhis;
    //OriginalResult记录这些循环外use在提交前共同引用的归约状态值
    Value *OriginalResult = nullptr;
    //Identity保存第二条循环携带累加链使用的结合律单位元
    Value *Identity = nullptr;
};

//PeelPredicate保存归纳变量与常量比较对应的规范谓词
struct PeelPredicate {
    //Op记录规范化后的整数比较谓词
    BinaryInst::Operation Op = BinaryInst::E;
    //Constant记录与归纳变量比较的常量
    int64_t Constant = 0;
};

//LoopExecutionShape保存执行展开所需的规范循环CFG节点
struct LoopExecutionShape {
    //PreHeader指向循环唯一前置块
    BasicBlock *PreHeader = nullptr;
    //Header指向循环唯一头块
    BasicBlock *Header = nullptr;
    //Latch指向循环唯一回边块
    BasicBlock *Latch = nullptr;
    //Exiting指向循环唯一退出源块
    BasicBlock *Exiting = nullptr;
    //Exit指向循环唯一出口块
    BasicBlock *Exit = nullptr;
};

//UnrolledBodyCore持有原循环与全部展开副本的CFG节点和值映射
struct UnrolledBodyCore {
    //Shape保存原循环的规范执行形状
    LoopExecutionShape Shape;
    //Count记录原循环加克隆副本组成的总迭代副本数
    int Count = 0;
    //IterBlocks按迭代副本保存其中的基本块
    std::vector<std::vector<BasicBlock *>> IterBlocks;
    //Headers按迭代副本保存循环头块
    std::vector<BasicBlock *> Headers;
    //Latches按迭代副本保存循环回边块
    std::vector<BasicBlock *> Latches;
    //Exitings按迭代副本保存退出源块
    std::vector<BasicBlock *> Exitings;
    //ValueMaps按迭代副本保存原IR值到克隆值的映射
    std::vector<std::unordered_map<Value *, Value *>> ValueMaps;
};

//PeeledBodyCore持有原循环与前置剥离副本的CFG节点和值映射
struct PeeledBodyCore {
    //Shape保存原循环的规范执行形状
    LoopExecutionShape Shape;
    //PeelCount记录已经构造的前置剥离副本数量
    int PeelCount = 0;
    //IterBlocks按剥离迭代保存其中的基本块
    std::vector<std::vector<BasicBlock *>> IterBlocks;
    //Headers按剥离迭代保存克隆头块
    std::vector<BasicBlock *> Headers;
    //Latches按剥离迭代保存克隆回边块
    std::vector<BasicBlock *> Latches;
    //Exitings按剥离迭代保存克隆退出源块
    std::vector<BasicBlock *> Exitings;
    //ValueMaps按剥离迭代保存原IR值到克隆值的映射
    std::vector<std::unordered_map<Value *, Value *>> ValueMaps;
};

//ExitRegionDataUse快照出口可达区域中需要按真实退出迭代改写的数据use
struct ExitRegionDataUse {
    //OperandUse指向待改写的具体操作数use
    Use *OperandUse = nullptr;
    //OriginalValue记录快照时该use引用的原始值
    Value *OriginalValue = nullptr;
    //LookupValue记录用于迭代值映射查找的源值
    Value *LookupValue = nullptr;
    //ExpectedType记录替换值必须满足的IR类型
    std::shared_ptr<IRType> ExpectedType = nullptr;
};

//HeaderState跟踪运行时或带尾部展开过程中单个Header PHI的状态传递
struct HeaderState {
    //OriginalPhi指向变换前的原始Header PHI
    PhiInst *OriginalPhi = nullptr;
    //CurrentPhi指向当前主循环中承载状态的PHI
    PhiInst *CurrentPhi = nullptr;
    //CurrentValue记录当前迭代副本使用的状态值
    Value *CurrentValue = nullptr;
    //InitialValue记录循环进入时的初始状态值
    Value *InitialValue = nullptr;
    //BackedgeValue记录原循环回边提供的状态值
    Value *BackedgeValue = nullptr;
    //MainValue记录主展开循环结束后的状态值
    Value *MainValue = nullptr;
    //TailStateValue记录进入尾部副本时的状态值
    Value *TailStateValue = nullptr;
    //TailStartIter记录该状态开始进入尾部副本的迭代编号
    int TailStartIter = std::numeric_limits<int>::max();
    //MainValueIsOriginalPhi标记主循环状态是否仍由原始PHI承载
    bool MainValueIsOriginalPhi = false;
    //BackedgeIsOriginalPhi标记回边值是否直接引用原始PHI
    bool BackedgeIsOriginalPhi = false;
};

//PhiRewritePlan保存提交前完整校验的一次PHI整体重写计划
struct PhiRewritePlan {
    //OldPhi指向待替换或重建的原PHI
    PhiInst *OldPhi = nullptr;
    //Incoming保存校验后的全部新入边
    std::vector<PhiInst::IncomingEdge> Incoming;
    //Replacement保存原PHI所有use最终使用的替换值
    Value *Replacement = nullptr;
    //NewPhi指向需要插入时预先构造的新PHI
    PhiInst *NewPhi = nullptr;
};

//LoopUnrollPass评估并执行完全、部分、运行时展开与循环peeling
class LoopUnrollPass : public PassDescriptor<LoopUnrollPass> {
public:
    //使用给定配置构造循环展开Pass
    explicit LoopUnrollPass(UnrollConfig Config = UnrollConfig());
    //遍历函数内循环并提交至多四次安全展开
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
    //返回当前循环展开Pass使用的配置
    const UnrollConfig &getConfig() const;
    //为单个循环计算并执行一种展开决策
    LoopUnrollResult tryToUnrollLoop(Loop *L,
                                     LoopInfo &LI,
                                     DominatorTree &DT,
                                     SCEVResult &SE,
                                     BasicAAResult &AA,
                                     RangeInfo &RI);
    //判断循环是否具有展开所需的规范CFG形状
    bool isCanonicalLoop(Loop *L) const;
    //判断循环子树中的全部指令和控制流是否可安全克隆
    bool canCloneLoopBody(Loop *L, LoopInfo &LI) const;
    //估算循环及其子循环全部指令的抽象代码成本
    int estimateLoopBodyCost(Loop *L, LoopInfo &LI) const;
    //判断循环子树中是否包含普通函数调用
    bool containsCall(Loop *L, LoopInfo &LI) const;
    //递归估算物化给定SCEV表达式的抽象成本
    int estimateSCEVCost(const SCEVExpr *E) const;
    //判断SCEV表达式是否满足当前展开配置的物化成本限制
    bool isCheapSCEV(const SCEVExpr *E, const UnrollConfig &Config) const;
    //判断通用整数SCEV表达式能否在指定循环预头安全物化
    static bool canMaterializeRuntimeSCEV(
        const SCEVExpr *E, Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    //在指定循环预头物化已经通过通用合法性检查的整数SCEV表达式
    static Value *materializeRuntimeSCEV(
        const SCEVExpr *E, Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    //根据循环条件和不变store收益选择安全peeling次数
    int detectPeelCount(Loop *L,
                        const InductionDescriptor &IV,
                        const UnrollConfig &Config) const;
    //综合次数、成本、合法性与收益选择循环展开方式
    UnrollDecision computeUnrollDecision(Loop *L,
                                         const LoopTripCountInfo &TCI,
                                         int BodyCost,
                                         const UnrollConfig &Config,
                                         LoopInfo &LI,
                                         SCEVResult &SE,
                                         BasicAAResult &AA,
                                         RangeInfo &RI,
                                         DominatorTree &DT) const;
    //按照已知常量迭代次数完全展开循环
    LoopUnrollResult executeFullUnroll(Loop *L,
                                       const UnrollDecision &Decision,
                                       SCEVResult &SE,
                                       LoopInfo &LI,
                                       DominatorTree &DT);
    //按整除因子部分展开常量次数循环
    LoopUnrollResult executePartialUnroll(Loop *L,
                                          const UnrollDecision &Decision,
                                          SCEVResult &SE,
                                          LoopInfo &LI,
                                          DominatorTree &DT);
    //部分展开常量次数循环并生成余数副本
    LoopUnrollResult executePartialWithRemainder(
        Loop *L, const UnrollDecision &Decision,
        SCEVResult &SE, LoopInfo &LI, DominatorTree &DT);
    //使用运行时迭代次数构造分组主循环和动态尾部
    LoopUnrollResult executeRuntimeUnroll(Loop *L,
                                          const UnrollDecision &Decision,
                                          SCEVResult &SE,
                                          LoopInfo &LI,
                                          DominatorTree &DT);
    //在原循环之前剥离若干次迭代并修复残余循环
    LoopUnrollResult executePeeling(Loop *L,
                                    const UnrollDecision &Decision,
                                    SCEVResult &SE,
                                    LoopInfo &LI,
                                    DominatorTree &DT,
                                    BasicAAResult &AA);

private:
    //Config保存当前Pass的循环展开策略和成本阈值
    UnrollConfig Config;
    //TransformedLoopHeaders记录已成功变换的原循环头以避免固定点重复倍增
    std::unordered_map<Function *, std::unordered_set<std::string>>
        TransformedLoopHeaders;
    //递归判断值的透明计算链中是否包含undef
    static bool containsUndef(
        Value *V, std::unordered_set<Value *> &Active, int Depth = 0);
    //递归判断值是否由循环外纯计算链构成
    static bool isStructurallyLoopInvariant(
        Value *V, Loop *L, std::unordered_set<Value *> &Active);
    //收集peeling后可由DCE删除的循环不变冗余store
    static std::vector<StoreInst *> collectPeelRedundantStoreCandidates(
        Loop *L, LoopInfo &LI, BasicAAResult &AA,
        const DominatorTree &DT);
    //证明运行时迭代次数在preheader处可用且固定位宽计算不回绕
    static bool proveRuntimeTripCountAtPreheader(
        SCEVResult &SE, const LoopTripCountInfo &TCI, RangeInfo &RI,
        DominatorTree &DT);
    //断开use并销毁尚未提交的全部peeling克隆
    static void discardUncommittedPeelClones(PeeledBodyCore &Core);
    //断开use并销毁尚未提交的全部展开克隆
    static void discardUncommittedUnrollClones(UnrolledBodyCore &Core);
    //返回本进程内唯一递增的循环展开序号
    static int nextUnrollSerial();
    //生成不会与函数现有基本块冲突的展开副本标识
    static std::string makeUnrollBlockIdent(
        Function *F, const BasicBlock *BB, int Iter, int UnrollSerial);
    //生成不会与函数现有基本块冲突的peeling副本标识
    static std::string makePeelBlockIdent(
        Function *F, const BasicBlock *BB, int Iter, int PeelSerial);
    //判断指令种类是否由循环展开克隆器完整支持
    static bool isSupportedCloneInstruction(Instruction *I);
    //剥离用于谓词识别的简单整数扩展或截断
    static Value *stripSimpleCast(Value *V);
    //匹配归纳变量与常量比较并规范化谓词方向
    static std::optional<PeelPredicate> matchCompareWithIV(
        BinaryInst *Cmp, Value *IV);
    //根据规范化归纳谓词计算可直接消除分支的peeling次数
    static int peelCountForPredicate(const PeelPredicate &P);
    //判断条件分支的两个后继是否都位于同一循环内
    static bool isInternalConditionalBranch(Loop *L, BrInst *Br);
    //构造明确表示不执行展开的默认决策
    static UnrollDecision makeNoUnrollDecision();
    //判断估算成本是否为非负且不超过给定阈值
    static bool withinThreshold(int64_t Cost, int Threshold);
    //根据循环体成本、规模限制和上限选择最大展开因子
    static int chooseFactorBySize(
        int BodyCost, int SizeLimit, int MaxCount);
    //返回单个SCEV节点本身的抽象物化成本
    static int localSCEVNodeCost(const SCEVExpr *E);
    //提取展开要求的唯一preheader、header、latch、exiting和exit
    static std::optional<LoopExecutionShape> getLoopExecutionShape(
        Loop *L, SCEVResult &SE);
    //判断待克隆基本块中的全部指令是否受克隆器支持
    static bool canCloneAllInstructions(
        const std::vector<BasicBlock *> &Blocks);
    //把原值映射到指定展开迭代中的对应值
    static Value *mapValueAtIteration(
        Value *V, int Iter, const UnrolledBodyCore &Core);
    //判断值是否是可用于CFG操作数的基本块
    static bool isBlockValue(Value *V);
    //判断值是否是可用于数据操作数的非基本块值
    static bool isDataValue(Value *V);
    //判断值是否可作为指定PHI的同类型入边值
    static bool isValueCompatibleWithPhi(Value *V, PhiInst *Phi);
    //判断值是否与给定IR类型兼容
    static bool isValueCompatibleWithType(
        Value *V, std::shared_ptr<IRType> Ty);
    //判断操作是否属于可按整数位模式安全重关联的结合律归约
    static bool isAssociativeIntegerReductionOp(
        BinaryInst::Operation Op);
    //返回整数归约操作的通用依赖延迟权重
    static int getIntegerReductionLatency(BinaryInst::Operation Op);
    //构造给定整数结合律操作对应的单位元
    static Value *makeIntegerReductionIdentity(
        BinaryInst::Operation Op, std::shared_ptr<IRType> Ty);
    //判断归约输入是否依赖候选之外的不受支持Header循环状态
    static bool dependsOnUnsupportedHeaderState(
        Value *V, Loop *L, PhiInst *Candidate, PhiInst *ControlIV,
        const std::unordered_set<PhiInst *> &HeaderPhis,
        std::unordered_set<Value *> &Active, int Depth = 0);
    //估算循环内同时占用整数寄存器的循环状态与不变输入数量
    static int estimateIntegerReductionRegisterPressure(Loop *L);
    //为已决定的整除部分展开选择整数多累加器计划
    static std::vector<IntegerReductionPlan> collectIntegerReductionPlans(
        Loop *L, const LoopTripCountInfo &TCI, int UnrollCount,
        const UnrollConfig &Config, const DominatorTree &DT);
    //判断基本块是否属于原循环或任一展开副本
    static bool isBlockInUnrolledCore(
        BasicBlock *BB, const UnrolledBodyCore &Core);
    //把归约计划完整映射为各展开副本并快照循环外结果use
    static bool buildIntegerReductionRewrite(
        const IntegerReductionPlan &Plan, UnrolledBodyCore &Core,
        PhiInst *PrimaryPhi, Value *ResultValue, bool NeedIdentity,
        IntegerReductionRewrite &Rewrite);
    //对整除部分展开后的整数归约构造两条循环携带累加链
    static unsigned applyPartialIntegerReductionAccumulators(
        UnrolledBodyCore &Core,
        const std::vector<IntegerReductionPlan> &Plans,
        const std::unordered_map<PhiInst *, Value *> &HeaderReplacements);
    //把数据值映射到指定展开迭代且拒绝基本块结果
    static Value *mapDataValueAtIteration(
        Value *V, int Iter, const UnrolledBodyCore &Core);
    //把数据值映射到指定peeling迭代且拒绝基本块结果
    static Value *mapDataValueAtPeelIteration(
        Value *V, int Iter, const PeeledBodyCore &Core);
    //返回出口快照值在指定展开迭代中的安全映射
    static Value *getCapturedDataValueMappedAtIteration(
        Value *V, int Iter, const UnrolledBodyCore &Core);
    //校验值、前驱和类型后为PHI添加一条入边
    static bool addIncomingChecked(
        PhiInst *Phi, Value *V, BasicBlock *Pred);
    //校验全部入边后整体重建PHI并返回必要的替换值
    static bool rebuildPhiFromIncomingChecked(
        PhiInst *Phi, const std::vector<PhiInst::IncomingEdge> &Incoming,
        Value **Replacement = nullptr);
    //按照值映射改写一条指令的全部数据操作数
    static bool replaceDataOperandsUsingMap(
        Instruction *I,
        const std::unordered_map<Value *, Value *> &ValueMap);
    //折叠指定展开副本中的克隆Header PHI
    static bool removeClonedHeaderPhis(
        UnrolledBodyCore &Core, int Iter);
    //折叠指定peeling副本中的克隆Header PHI
    static bool removePeeledHeaderPhis(PeeledBodyCore &Core, int Iter);
    //按迭代克隆循环体并构造完整块和值映射
    static std::optional<UnrolledBodyCore> cloneUnrolledBodyCore(
        Loop *L, int Count, SCEVResult &SE);
    //克隆指定数量的前置peeling迭代并构造块和值映射
    static std::optional<PeeledBodyCore> clonePeeledIterations(
        Loop *L, int PeelCount, SCEVResult &SE);
    //把部分展开原Header PHI的回边改接到最后副本
    static bool rewriteOriginalHeaderPhisForPartial(
        UnrolledBodyCore &Core, int LastIter,
        std::unordered_map<PhiInst *, Value *> *Replacements = nullptr);
    //把peeling后原Header PHI的入口改接到最后剥离副本
    static bool rewriteOriginalHeaderPhisAfterPeeling(
        PeeledBodyCore &Core, bool ValidateOnly = false,
        std::unordered_map<PhiInst *, Value *> *Replacements = nullptr);
    //移除已删除peeling退出边在出口PHI中的对应入边
    static bool removeIncomingFromExitPhisForPeeledBlocks(
        PeeledBodyCore &Core, bool ValidateOnly = false);
    //为保留退出检查的peeling副本向出口PHI添加映射入边
    static bool addIncomingToExitPhisForPeeledBlocks(
        PeeledBodyCore &Core, bool ValidateOnly = false);
    //判断展开核心的唯一退出源是否就是唯一latch
    static bool hasLatchExitShape(const UnrolledBodyCore &Core);
    //收集原循环及全部展开副本的退出源块
    static std::unordered_set<BasicBlock *> collectCoreExitings(
        const UnrolledBodyCore &Core);
    //重建仅保留指定退出迭代时的全部出口PHI
    static bool rewriteExitPhisForSingleExiting(
        UnrolledBodyCore &Core, int ActiveIter);
    //收集出口开始且未越过停止块的可达CFG区域
    static std::vector<BasicBlock *> collectExitRegionBlocksFrom(
        BasicBlock *Exit,
        const std::unordered_set<BasicBlock *> &StopBlocks,
        bool StopAtConditionalBranch = true);
    //快照出口可达区域中需要按真实退出迭代改写的数据use
    static std::vector<ExitRegionDataUse> collectExitRegionDataUsesFrom(
        BasicBlock *Exit,
        const std::unordered_set<BasicBlock *> &StopBlocks,
        BasicBlock *Exiting = nullptr,
        bool StopAtConditionalBranch = true);
    //按照真实退出迭代映射出口可达区域中的数据操作数
    static bool rewriteExitRegionDataOperandsForIteration(
        UnrolledBodyCore &Core, int ActiveIter,
        const std::vector<ExitRegionDataUse> &CapturedUses,
        const std::unordered_map<Value *, Value *> *HeaderExitValues = nullptr);
    //在指定可达区域内按照替换表改写全部数据操作数
    static bool replaceDataOperandsInReachableRegionFrom(
        BasicBlock *Start,
        const std::unordered_set<BasicBlock *> &StopBlocks,
        const std::unordered_map<Value *, Value *> &Replacements);
    //使用PHI替换表刷新此前捕获的出口区域use快照
    static void refreshCapturedExitRegionUses(
        std::vector<ExitRegionDataUse> &Uses,
        const std::unordered_map<Value *, Value *> &Replacements);
    //完全展开时用preheader初值替换并删除原Header PHI
    static bool removeOriginalHeaderPhisForFull(UnrolledBodyCore &Core);
    //把若干展开副本串联为主路径并设置末副本目标
    static bool makeStraightLineMain(
        UnrolledBodyCore &Core, int MainCount, BasicBlock *FinalTarget,
        Value *ContinueCondition);
    //构造与给定整数类型一致的常量
    static Value *makeTypedIntConstant(
        int64_t V, std::shared_ptr<IRType> Ty);
    //判断值是否能够在循环preheader处安全使用
    static bool isValueAvailableAtPreheader(
        Value *V, Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    //递归判断SCEV是否能在preheader处无故障地物化
    static bool canMaterializeSCEV(
        const SCEVExpr *E, Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    //判断给定SCEV除法或取余的除数是否为安全常量
    static bool hasSafeSCEVDivisor(const SCEVExpr *E);
    //在preheader处递归物化SCEV并缓存公共子表达式
    static Value *materializeSCEV(
        const SCEVExpr *E, Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT,
        std::unordered_map<const SCEVExpr *, Value *> &Cache);
    //从指定迭代开始按照替换表改写全部展开副本
    static bool replaceValuesInIterationBlocks(
        UnrolledBodyCore &Core, int FirstIter,
        const std::unordered_map<Value *, Value *> &Replacements);
    //返回循环开始前可用的状态值并在缺失时构造undef
    static Value *getValueBeforeLoop(
        Value *V, const UnrolledBodyCore &Core,
        std::shared_ptr<IRType> FallbackTy);
    //返回退出源的两个后继中不是出口块的循环内后继
    static BasicBlock *getNonExitSuccessor(
        BasicBlock *Exiting, BasicBlock *Exit);
    //返回给定Header状态在指定迭代结束后的映射值
    static Value *mapValueAfterIteration(
        Value *V, int Iter, const std::vector<HeaderState> &States,
        const UnrolledBodyCore &Core);
    //构造指定退出迭代对应的Header PHI替换映射
    static bool buildHeaderExitValueMap(
        const std::vector<HeaderState> &HeaderStates, int ExitIter,
        const UnrolledBodyCore &Core, BasicBlock *PreHeader,
        std::unordered_map<Value *, Value *> &HeaderExitValues);
    //构造进入尾部副本时各Header PHI对应的初始状态映射
    static bool buildHeaderTailInputValueMap(
        const std::vector<HeaderState> &HeaderStates,
        BasicBlock *PreHeader,
        std::unordered_map<Value *, Value *> &TailInputValues);
    //执行共享的部分展开主循环和常量或运行时尾部构造流程
    static LoopUnrollResult executeUnrollWithTail(
        Loop *L, int MainCount, int TailCopies, Value *NumGroups,
        Value *RemainderValue, bool IsRuntime, SCEVResult &SE);
};
