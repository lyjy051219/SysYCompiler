#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <optional>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <vector>

//LoopInfo提供循环归属和循环不变量查询
class LoopInfo;
//Loop表示单个自然循环
class Loop;
//SCEVResult提供归纳变量和无回绕证明
class SCEVResult;
//RangeInfo提供指定程序点成立的整数范围事实
class RangeInfo;
//DominatorTree提供范围事实消费所需的支配关系
class DominatorTree;

//AliasInfo表示一次别名查询可证明的最强关系，证据不足时返回MayAlias
enum class AliasInfo {
    //NoAlias表示两个指针或内存区间确定不重叠
    NoAlias,
    //MayAlias表示现有证据无法排除重叠
    MayAlias,
    //MustAlias表示两个指针确定指向同一地址
    MustAlias
};

//MemoryLocation把访问指针与实际访问字节数绑定
struct MemoryLocation {
    //Pointer记录内存访问使用的指针
    Value *Pointer = nullptr;
    //Size记录访问字节数，无法确定时为空
    std::optional<std::size_t> Size;

    //构造一个空内存位置
    MemoryLocation();
    //使用给定指针和访问字节数构造内存位置
    MemoryLocation(Value *P, std::optional<std::size_t> S);
    //从Load指令提取读取位置和读取字节数
    static MemoryLocation get(const LoadInst *Load);
    //从Store指令提取写入位置和写入字节数
    static MemoryLocation get(const StoreInst *Store);
};

//PointerProvenanceInfo汇总指针的底层对象、字节偏移、对象大小和对齐事实
struct PointerProvenanceInfo {
    //Bases记录指针所有可能的已知底层对象
    std::unordered_set<Value *> Bases;
    //MinByteOffset记录相对底层对象的最小已知字节偏移
    std::optional<long long> MinByteOffset;
    //MaxByteOffset记录相对底层对象的最大已知字节偏移
    std::optional<long long> MaxByteOffset;
    //ObjectSize记录所有来源一致的底层对象字节数
    std::optional<std::size_t> ObjectSize;
    //KnownAlignment记录指针至少满足的字节对齐
    std::size_t KnownAlignment = 1;
    //HasUnknownBase标记来源中是否包含无法识别的底层对象
    bool HasUnknownBase = true;
    //MayBeStack标记来源是否可能包含栈对象
    bool MayBeStack = false;
    //MayBeGlobal标记来源是否可能包含全局对象
    bool MayBeGlobal = false;
    //MayBeParameter标记来源是否可能包含函数形参
    bool MayBeParameter = false;

    //判断来源是否恰好包含一个已知底层对象
    bool hasUniqueBase() const;
    //返回唯一底层对象，不唯一或含未知来源时返回空
    Value *getUniqueBase() const;
    //判断最小和最大字节偏移是否构成同一个精确偏移
    bool hasExactByteOffset() const;
    //返回精确字节偏移，无法确定时返回空
    std::optional<long long> getExactByteOffset() const;
    //判断该指针是否确定只来源于栈对象
    bool isMustStack() const;
    //判断该指针是否确定只来源于全局对象
    bool isMustGlobal() const;
    //判断该指针是否确定只来源于函数形参
    bool isMustParameter() const;
};

//BasicAAResult按需分解指针并缓存函数内的基础别名分析结果
class BasicAAResult {
public:
    //构造不使用SCEV距离证明的空分析结果
    BasicAAResult();
    //构造使用给定SCEV结果辅助距离证明的分析结果
    explicit BasicAAResult(SCEVResult *SE);
    //移动构造分析结果并转移其缓存和对象互斥事实
    BasicAAResult(BasicAAResult&&);
    //移动赋值分析结果并转移其缓存和对象互斥事实
    BasicAAResult& operator=(BasicAAResult&&);
    //禁止拷贝分析结果，避免复制带有IR指针键的缓存
    BasicAAResult(const BasicAAResult&) = delete;
    //禁止拷贝赋值分析结果，避免复制带有IR指针键的缓存
    BasicAAResult& operator=(const BasicAAResult&) = delete;
    //根据保留状态判断结果是否失效，并在失效时清理查询缓存
    bool invalidate(Function &F, const PreservationStatus &PA);

    //记录两个底层对象在当前函数中确定互不别名
    void addObjectNoAliasFact(Value* V1, Value* V2);
    //查询两个指针的基础别名关系
    AliasInfo alias(Value* V1, Value* V2, const LoopInfo& LI);
    //查询两个指针的别名关系并允许使用循环下标距离证明
    AliasInfo aliasWithIndexRange(Value* V1, Value* V2, const LoopInfo& LI);
    //查询两个显式内存区间的基础别名关系
    AliasInfo alias(const MemoryLocation &L1, const MemoryLocation &L2,
                    const LoopInfo &LI);
    //查询两个显式内存区间的别名关系并允许使用循环下标距离证明
    AliasInfo aliasWithIndexRange(const MemoryLocation &L1,
                                  const MemoryLocation &L2,
                                  const LoopInfo &LI);
    //判断两个指针的所有已知底层对象是否两两互斥
    bool haveKnownDistinctUnderlyingObjects(Value *V1, Value *V2) const;
    //判断调用指令的任一指针实参是否可能与目标指针别名
    bool callMayAliasPointer(CallInst *CI, Value *P, const LoopInfo &LI);
    //返回指针的完整来源摘要并复用缓存
    const PointerProvenanceInfo &getPointerProvenance(Value *V) const;
    //按稳定顺序返回指针所有已知底层对象
    std::vector<Value *> getUnderlyingObjects(Value *V) const;
    //返回指针唯一的已知底层对象，不唯一时返回空
    Value *getUniqueUnderlyingObject(Value *V) const;
    //返回指针相对底层对象的常量字节偏移
    std::optional<long long> getConstantByteOffset(Value *V) const;
    //在指定程序点证明两个同对象指针只相差一个编译期常量字节偏移
    std::optional<long long> getConstantByteOffsetDifferenceAt(
        Value *Pointer, Value *Reference, Instruction *Context,
        SCEVResult &SE, RangeInfo &RI,
        const DominatorTree &DT) const;
    //在指定程序点查询两个内存区间，允许消费已证明不回绕的共同动态地址项
    AliasInfo aliasAt(const MemoryLocation &L1,
                      const MemoryLocation &L2,
                      Instruction *Context, const LoopInfo &LI,
                      SCEVResult &SE, RangeInfo &RI,
                      const DominatorTree &DT);
    //返回指针底层对象的已知字节数
    std::optional<std::size_t> getObjectSize(Value *V) const;
    //返回指针至少满足的已知字节对齐
    std::size_t getKnownAlignment(Value *V) const;
    //证明一次完整内存访问在指定程序点始终位于唯一栈或全局对象内
    bool isLocationProvablyInBoundsAt(
        const MemoryLocation &Location, Instruction *Context,
        SCEVResult &SE, RangeInfo &RI,
        const DominatorTree &DT) const;
    //累计规范start0/step1访问所需的共同运行时上界和对象安全容量
    bool accumulateLocationSafeUpperBound(
        const MemoryLocation &Location, Instruction *Context,
        const DominatorTree &DT, const LoopInfo &LI,
        SCEVResult &SE, Value *&SharedBound,
        std::int64_t &SafeUpperBound,
        Value *AdditionalNonNegativeStart = nullptr) const;

private:
    //允许Pass入口构造并播种BasicAAResult
    friend class BasicAliasAnalysis;

    //I32Modulus表示32位整数模运算的模数
    static constexpr long long I32Modulus = 1LL << 32;
    //MaxProvenanceDepth限制指针来源和整数倍数递归深度
    static constexpr unsigned MaxProvenanceDepth = 64;
    //MaxProvenanceBases限制单个来源摘要保存的底层对象数量
    static constexpr std::size_t MaxProvenanceBases = 64;
    //MaxProvenanceQueryNodes限制单次指针来源查询访问的SSA节点数
    static constexpr std::size_t MaxProvenanceQueryNodes = 4096;
    //MaxIntegerMultipleQueryNodes限制单次整数倍数查询访问的SSA节点数
    static constexpr std::size_t MaxIntegerMultipleQueryNodes = 4096;
    //MaxGuardGEPChainDepth限制运行时对象边界守卫分解的带类型GEP层数
    static constexpr std::size_t MaxGuardGEPChainDepth = 12;

    //执行带溢出检查的有符号加法
    static bool checkedAdd(long long A, long long B, long long &Result);
    //执行带溢出检查的有符号减法
    static bool checkedSub(long long A, long long B, long long &Result);
    //执行带溢出检查的有符号乘法
    static bool checkedMul(long long A, long long B, long long &Result);
    //递归检查透明指针来源中是否出现undef
    static bool hasUndefPointerOrigin(
        Value *V, std::unordered_set<Value *> &Active, unsigned Depth = 0);
    //从由header或latch控制回边的规范归纳下标提取严格上界
    static bool getCanonicalIndexUpperBound(
        Value *Index, Instruction *Context,
        const DominatorTree &DT, const LoopInfo &LI,
        SCEVResult &SE, Value *&Bound,
        Value *AdditionalNonNegativeStart);

    //ValuePairHash为规范化Value指针对提供哈希值
    struct ValuePairHash {
        //计算一对Value指针的组合哈希
        std::size_t operator()(const std::pair<Value*, Value*>& P) const;
    };

    //ProvenanceQueryState保存单次指针来源查询的递归状态
    struct ProvenanceQueryState {
        //Active记录当前递归路径上的活动节点以识别环
        std::unordered_set<Value *> Active;
        //Completed缓存本次查询中已完整求得的节点来源
        std::unordered_map<Value *, PointerProvenanceInfo> Completed;
        //VisitedNodes记录本次查询已访问的SSA节点数
        std::size_t VisitedNodes = 0;
        //BudgetExceeded标记本次查询是否耗尽节点预算
        bool BudgetExceeded = false;
    };

    //ProvenanceBuildResult携带来源摘要及其对活动环的依赖状态
    struct ProvenanceBuildResult {
        //Info保存当前节点合成出的指针来源摘要
        PointerProvenanceInfo Info;
        //DependsOnActiveCycle标记摘要是否仍依赖当前递归活动环
        bool DependsOnActiveCycle = false;
    };

    //IntegerMultipleQueryState保存单次整数二次幂倍数查询的递归状态
    struct IntegerMultipleQueryState {
        //Active记录当前递归路径上的活动整数节点
        std::unordered_set<Value *> Active;
        //Completed缓存已求得节点的已知二次幂因子
        std::unordered_map<Value *, std::size_t> Completed;
        //VisitedNodes记录本次倍数查询已访问的SSA节点数
        std::size_t VisitedNodes = 0;
        //BudgetExceeded标记本次倍数查询是否耗尽节点预算
        bool BudgetExceeded = false;
    };

    //LinearGEP表示GEP链归一化后的底层对象和线性字节偏移
    struct LinearGEP {
        //Base记录GEP链最终指向的稳定底层对象
        Value* Base = nullptr;
        //ConstantOffset记录所有常量下标贡献的字节偏移
        long long ConstantOffset = 0;
        //VariableOffsets记录每个SSA下标对应的字节系数
        std::unordered_map<Value*, long long> VariableOffsets;
        //Valid标记GEP链是否成功线性化且未发生溢出
        bool Valid = true;
    };

    //AliasCache缓存不使用循环下标范围的规范化指针对查询结果
    std::unordered_map<std::pair<Value*, Value*>, AliasInfo, ValuePairHash> AliasCache;
    //ObjectNoAlias保存已证明互不别名的规范化底层对象对
    std::unordered_set<std::pair<Value*, Value*>, ValuePairHash> ObjectNoAlias;
    //ActiveSelectQueries记录正在展开的PHI或Select指针对以截断递归环
    std::unordered_set<std::pair<Value*, Value*>, ValuePairHash> ActiveSelectQueries;
    //ProvenanceCache缓存跨查询复用的指针来源摘要
    mutable std::unordered_map<Value *, PointerProvenanceInfo> ProvenanceCache;

    //AdditiveDecomposition表示整数值相对某个SSA基值的常量加法分解
    struct AdditiveDecomposition {
        //Base记录剥离常量加减后的SSA基值
        Value* Base = nullptr;
        //Offset记录相对SSA基值累积的常量偏移
        long long Offset = 0;
        //Valid标记加法分解是否成功且未发生溢出
        bool Valid = true;
    };

    //PositivePhiRecurrence描述SCEV证明的正步长无回绕PHI递推
    struct PositivePhiRecurrence {
        //Start记录归纳变量的循环入口初值
        Value* Start = nullptr;
        //Update记录归纳变量的回边更新值
        Value* Update = nullptr;
        //Step记录每次迭代增加的正整数步长
        long long Step = 0;
        //L记录该归纳递推所属的自然循环
        const Loop* L = nullptr;
    };

    //SE指向可选的SCEV分析结果，用于循环距离和无回绕证明
    SCEVResult *SE = nullptr;

    //剥离零偏移GEP和单入PHI以规范化普通别名查询
    Value* stripBaseInst(Value* V) const;
    //穿过GEP、BitCast和单入PHI获取稳定底层地址
    Value* getBaseAddress(Value* V) const;
    //递归构建单个指针的来源摘要
    ProvenanceBuildResult buildPointerProvenance(
        Value *V, ProvenanceQueryState &State, unsigned Depth) const;
    //合并两条控制流来源的指针摘要
    PointerProvenanceInfo mergePointerProvenance(
        const PointerProvenanceInfo &First,
        const PointerProvenanceInfo &Second) const;
    //判断来源摘要是否仅表示正在求解的活动环
    static bool isEmptyCycleProvenance(const PointerProvenanceInfo &Info);
    //把常量字节偏移安全地累加到来源偏移区间
    static bool addOffsetToProvenance(PointerProvenanceInfo &Info,
                                      long long Offset);
    //返回底层对象由DataLayout确定的ABI对齐
    static std::size_t baseObjectAlignment(Value *Base);
    //返回底层对象由DataLayout确定的分配字节数
    static std::optional<std::size_t> baseObjectSize(Value *Base);
    //返回无符号整数可整除的最大二次幂
    static std::size_t greatestPowerOfTwoDivisor(std::uint64_t Value);
    //组合两个已知二次幂因子并处理宿主整数溢出
    static std::size_t multiplyAlignmentFactors(std::size_t First,
                                                std::size_t Second);
    //递归计算整数值至少包含的二次幂因子
    std::size_t getKnownIntegerMultiple(
        Value *V, IntegerMultipleQueryState &State,
        unsigned Depth) const;
    //根据类型步长和下标低零位计算GEP偏移的已知对齐
    std::size_t getGEPOffsetAlignment(GetElementPtrInst *GEP) const;
    //判断整个内存访问区间是否确定处于底层对象边界内
    bool isLocationKnownWithinObject(const MemoryLocation &Location) const;
    //撤销依赖潜在越界对象身份得出的不安全NoAlias结论
    AliasInfo enforceLocationObjectBounds(
        AliasInfo Result, const MemoryLocation &First,
        const MemoryLocation &Second) const;

    //判断两个规范化指针是否为同一个SSA值
    bool isEqualAlias(Value* V1, Value* V2, const LoopInfo& LI) const;
    //判断已播种的对象互斥事实中是否包含给定对象对
    bool hasObjectNoAlias(Value* V1, Value* V2) const;
    //执行指针别名查询的核心分派和缓存流程
    AliasInfo aliasImpl(Value* V1, Value* V2, const LoopInfo& LI,
                        bool UseIndexRange);
    //递归查询两个显式内存区间并保持各自访问宽度
    AliasInfo aliasLocationImpl(
        const MemoryLocation &L1, const MemoryLocation &L2,
        const LoopInfo &LI, bool UseIndexRange, unsigned Depth,
        std::unordered_set<std::pair<Value *, Value *>, ValuePairHash> &Active);
    //按前驱路径展开PHI并合并各路径别名结论
    AliasInfo aliasPHI(PhiInst* Phi, Value* Other, const LoopInfo& LI,
                       bool UseIndexRange);
    //按条件分支展开Select并合并两条路径的别名结论
    AliasInfo aliasSelect(SelectInst *Select, Value *Other,
                          const LoopInfo &LI, bool UseIndexRange);
    //合并两条控制流路径上的别名结论
    AliasInfo mergePhiAlias(AliasInfo A, AliasInfo B) const;
    //按地址顺序构造与参数顺序无关的规范化指针对
    std::pair<Value*, Value*> makeCanonicalPair(Value* A, Value* B) const;

    //把单条GEP的类型化下标累加到线性字节表达式
    bool accumulateGEPIndices(GetElementPtrInst* GEP, LinearGEP& Expr) const;
    //沿完整GEP和BitCast链构造线性字节表达式
    LinearGEP fillLinearGEP(Value* V) const;
    //比较同一底层对象上的两个GEP字节区间
    AliasInfo aliasGEP(Value* GEP1, Value* GEP2,
                       const LoopInfo& LI, bool UseIndexRange,
                       std::optional<std::size_t> AccessSize1 = std::nullopt,
                       std::optional<std::size_t> AccessSize2 = std::nullopt);

    //返回指针类型指向元素的名义访问字节数
    std::size_t getPointeeAccessSize(Value* V) const;
    //把整数加减链分解为SSA基值和常量偏移
    AdditiveDecomposition decomposeAdditive(Value* V) const;
    //返回整数值相对指定SSA基值的常量偏移
    std::optional<long long> getAdditiveOffsetFrom(Value* V, Value* Base) const;
    //识别并返回正步长无回绕的规范PHI递推
    std::optional<PositivePhiRecurrence> getPositivePhiRecurrence(PhiInst* Phi,
                                                                  const LoopInfo& LI) const;
    //证明两个整数值之差的非负下界
    std::optional<long long> lowerBoundDiff(Value* Greater, Value* Lesser,
                                            const LoopInfo& LI) const;
    //利用模差值或SCEV下界判断两个变量GEP下标是否分离
    AliasInfo aliasGEPByIndexDistance(
        const std::unordered_map<Value*, long long>& DiffVars,
        long long ConstDiff, std::size_t Threshold, const LoopInfo& LI) const;
};

//BasicAliasAnalysis是构建函数级BasicAAResult的分析Pass
class BasicAliasAnalysis : public AnalysisDescriptor<BasicAliasAnalysis> {
public:
    //Result指定该分析Pass返回BasicAAResult
    using Result = BasicAAResult;
    //InvalidateAcrossIRUnits要求跨IR单元变化时重新检查结果有效性
    static constexpr bool InvalidateAcrossIRUnits = true;
    //构建函数级BasicAA结果并提炼跨调用点对象互斥事实
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    //允许AnalysisDescriptor访问本Pass的唯一标识
    friend AnalysisDescriptor<BasicAliasAnalysis>;
    //Key保存BasicAliasAnalysis的唯一Pass标识
    inline static PassID Key;
};
