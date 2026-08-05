#pragma once

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/opt/NewPassManager.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

//BasicAAResult前向声明用于访存区间别名查询
class BasicAAResult;
//Loop前向声明用于描述循环轴和分析范围
class Loop;
//LoopInfo前向声明用于查询基本块的循环归属
class LoopInfo;
//SCEVResult前向声明用于构造循环仿射表达式
class SCEVResult;
//SCEVExpr前向声明用于记录标量演化项
struct SCEVExpr;

//LoopAffineExpr以循环迭代坐标表示一个可证明的仿射表达式
struct LoopAffineExpr {
    //表达式是否能够被精确建模并用于依赖证明
    bool Exact = false;
    //表达式中的常量项
    int64_t Constant = 0;
    //每个循环轴对应的整数系数
    std::unordered_map<const Loop *, int64_t> LoopCoefficients;
    //每个循环不变量SCEV项对应的整数系数
    std::unordered_map<const SCEVExpr *, int64_t> InvariantTerms;
};

//LoopMemoryAccess描述循环中的一次标量或调用访存
struct LoopMemoryAccess {
    //产生本次访存的IR指令
    Instruction *Inst = nullptr;
    //本次访存使用的原始指针
    Value *Pointer = nullptr;
    //本次访存的指针和实际访问字节数
    MemoryLocation Location;
    //剥离GEP和BitCast后得到的底层根对象
    Value *Root = nullptr;
    //本次访存是否可能读取内存
    bool Reads = false;
    //本次访存是否可能写入内存
    bool Writes = false;
    //根对象、类型化下标和步长是否均已精确求得
    bool AddressExact = false;
    //按照地址带保存的循环仿射下标
    std::vector<LoopAffineExpr> Subscripts;
    //与Subscripts逐项对应的字节步长
    std::vector<int64_t> ByteStrides;
    //本次访存在函数稳定指令序中的位置
    std::size_t Order = 0;
};

//LoopDependenceKind描述源访存与宿访存之间的数据依赖种类
enum class LoopDependenceKind {
    //源写入且宿读取形成的真依赖
    Flow,
    //源读取且宿写入形成的反依赖
    Anti,
    //源和宿均写入形成的输出依赖
    Output,
    //无法精确分类的保守依赖
    Unknown
};

//LoopDependenceDirection描述宿访问相对源访问的循环迭代方向
enum LoopDependenceDirection : uint8_t {
    //宿访问可能发生在源访问之前的迭代
    LoopDepSinkEarlier = 1u << 0,
    //宿访问与源访问发生在同一次迭代
    LoopDepSameIteration = 1u << 1,
    //宿访问可能发生在源访问之后的迭代
    LoopDepSinkLater = 1u << 2,
    //宿访问可能具有任意迭代方向
    LoopDepAnyDirection = LoopDepSinkEarlier | LoopDepSameIteration |
                          LoopDepSinkLater
};

//LoopDependenceRecord按外到内循环轴记录一对访存的依赖方向
struct LoopDependenceRecord {
    //依赖边的源访存
    const LoopMemoryAccess *Source = nullptr;
    //依赖边的宿访存
    const LoopMemoryAccess *Sink = nullptr;
    //源访存与宿访存之间的依赖种类
    LoopDependenceKind Kind = LoopDependenceKind::Unknown;
    //依赖方向所对应的外到内循环轴
    std::vector<Loop *> Loops;
    //与Loops逐项对应的依赖方向掩码
    std::vector<uint8_t> DirectionMasks;
    //全部循环轴方向是否都已被精确证明
    bool Exact = false;
};

//LoopDependenceInfo按循环惰性构建访存依赖并提供变换合法性查询
class LoopDependenceInfo {
public:
    //InstructionSet表示循环分布中一组待归类指令
    using InstructionSet = std::unordered_set<Instruction *>;
    //构造不绑定任何函数分析结果的空查询器
    LoopDependenceInfo();
    //使用函数、循环、SCEV、别名和Pass管理器构造依赖查询器
    LoopDependenceInfo(Function *F, LoopInfo *LI, SCEVResult *SE,
                       BasicAAResult *AA,
                       FunctionAnalysisPassManager *FAM);
    //销毁循环依赖查询器及其惰性缓存
    ~LoopDependenceInfo();
    //移动构造查询器并接管全部循环摘要
    LoopDependenceInfo(LoopDependenceInfo &&) noexcept;
    //移动赋值查询器并接管全部循环摘要
    LoopDependenceInfo &operator=(LoopDependenceInfo &&) noexcept;
    //禁止拷贝包含IR裸指针和唯一摘要所有权的查询器
    LoopDependenceInfo(const LoopDependenceInfo &) = delete;
    //禁止拷贝赋值包含IR裸指针和唯一摘要所有权的查询器
    LoopDependenceInfo &operator=(const LoopDependenceInfo &) = delete;
    //返回指定循环及其子循环中的全部可建模访存
    const std::vector<LoopMemoryAccess> &getMemoryAccesses(Loop *L);
    //返回指定循环及其子循环中的全部保守依赖记录
    const std::vector<LoopDependenceRecord> &getDependences(Loop *L);
    //判断相邻外层和内层循环是否可以安全交换
    bool isInterchangeLegal(Loop *Outer, Loop *Inner);
    //判断两组指令能否保持依赖地分布到两个循环中
    bool isDistributionLegal(Loop *L, const InstructionSet &First,
                             const InstructionSet &Second);
    //判断两条指令之间的潜在冲突是否局限在同次循环迭代
    bool isConflictLocalToIteration(Instruction *A, Instruction *B,
                                    Loop *L);
    //判断外层不变读取是否与有限内层循环写入区间互不重叠
    bool isInvariantAccessDisjointFromLoopWrite(
        Instruction *Invariant, Instruction *Write,
        Loop *Scope, Loop *VaryingLoop);
    //返回指定访存沿目标循环轴变化的总字节步长
    std::optional<int64_t> getByteStride(Instruction *I, Loop *L);
    //判断循环摘要是否包含未知内存或不能跨越的外部效果
    bool hasUnknownMemory(Loop *L);
    //判断循环中是否存在程序内存之外的可观察效果
    bool hasObservableEffect(Loop *L);
    //判断循环中是否存在不可跨越的严格顺序屏障
    bool hasOrderingBarrier(Loop *L);
    //根据相关分析的保留状态判断当前结果是否失效
    bool invalidate(Function &F, const PreservationStatus &PA);

private:
    //LoopSummary保存一个循环树的访存、依赖和副作用摘要
    struct LoopSummary {
        //循环树中的全部可建模访存
        std::vector<LoopMemoryAccess> Accesses;
        //循环树中全部潜在写冲突形成的依赖记录
        std::vector<LoopDependenceRecord> Dependences;
        //当前循环树按照外到内前序排列的循环轴
        std::vector<Loop *> Loops;
        //循环树是否包含无法映射或可能越界的内存访问
        bool UnknownMemory = false;
        //循环树是否包含输入输出等外部可观察效果
        bool ObservableEffect = false;
        //循环树是否包含不可跨越的严格顺序屏障
        bool OrderingBarrier = false;
    };
    //单个循环摘要允许记录的最大访存数量
    static constexpr std::size_t MaxMemoryAccesses = 512;
    //单个循环摘要允许检查的最大访存对数量
    static constexpr std::size_t MaxDependencePairs = 8192;
    //单次指针链溯源允许经过的最大节点深度
    static constexpr unsigned MaxPointerChainDepth = 64;
    //单次GEP链建模允许处理的最大下标项数量
    static constexpr unsigned MaxGEPIndexTerms = 128;
    //无效循环查询返回的共享空访存列表
    inline static const std::vector<LoopMemoryAccess> EmptyMemoryAccesses;
    //无效循环查询返回的共享空依赖列表
    inline static const std::vector<LoopDependenceRecord>
        EmptyDependenceRecords;
    //当前查询器所属的函数
    Function *Func = nullptr;
    //当前函数的循环森林分析结果
    LoopInfo *LI = nullptr;
    //当前函数的标量演化分析结果
    SCEVResult *SE = nullptr;
    //当前函数的基础别名分析结果
    BasicAAResult *AA = nullptr;
    //用于查询调用副作用摘要的函数分析管理器
    FunctionAnalysisPassManager *FAM = nullptr;
    //将每个循环映射到按需构建且由查询器持有的摘要
    std::unordered_map<Loop *, std::unique_ptr<LoopSummary>> Cache;
    //执行带溢出检查的64位有符号加法
    static bool checkedAdd(int64_t A, int64_t B, int64_t &Result);
    //执行带溢出检查的64位有符号乘法
    static bool checkedMul(int64_t A, int64_t B, int64_t &Result);
    //移除仿射表达式中系数为零的循环轴和不变量项
    static void eraseZeroTerms(LoopAffineExpr &Expression);
    //将一条仿射表达式按比例安全地累加到目标表达式
    static bool addScaledAffine(LoopAffineExpr &Destination,
                                const LoopAffineExpr &Source, int64_t Scale);
    //判断候选循环是否等于或嵌套在给定分析范围中
    static bool loopIsInScope(const Loop *Candidate, const Loop *Scope);
    //按照前序遍历收集当前循环及其全部子循环
    static void collectLoopsPreorder(Loop *L, std::vector<Loop *> &Loops);
    //判断表达式是否只包含目标循环轴的非零系数
    static bool isPureAxisCoordinate(const LoopAffineExpr &Expression,
                                     Loop *Axis, int64_t &Coefficient);
    //返回指定循环的缓存摘要，尚未构建时立即构建并缓存
    LoopSummary &getOrBuildSummary(Loop *L);
    //构建指定循环树的全部访存和依赖摘要
    std::unique_ptr<LoopSummary> buildSummary(Loop *L);
    //将SCEV表达式转换为给定循环范围内的仿射表达式
    LoopAffineExpr buildAffine(const SCEVExpr *E, Loop *Scope) const;
    //从标量Load或Store指令构建循环访存记录
    bool buildMemoryAccess(Instruction *I, Loop *Scope, std::size_t Order,
                           LoopMemoryAccess &Access) const;
    //沿指针链构建带类型化下标和字节步长的访存记录
    bool buildPointerAccess(Instruction *I, Value *Pointer,
                            const MemoryLocation &Location,
                            bool Reads, bool Writes, Loop *Scope,
                            std::size_t Order,
                            LoopMemoryAccess &Access) const;
    //判断两次访存在目标循环轴上是否必定位于同次迭代
    bool areForcedEqual(const LoopMemoryAccess &A,
                        const LoopMemoryAccess &B, Loop *Axis) const;
    //判断两次访存的完整内存区间是否确定互不别名
    bool areNoAlias(const LoopMemoryAccess &A,
                    const LoopMemoryAccess &B) const;
    //判断两次访存之间是否存在至少一侧写入的潜在冲突
    static bool isWriteConflict(const LoopMemoryAccess &A,
                                const LoopMemoryAccess &B);
};

//LoopDependenceAnalysis组合循环、SCEV、别名和副作用结果构建依赖查询器
class LoopDependenceAnalysis
    : public AnalysisDescriptor<LoopDependenceAnalysis> {
public:
    //Pass管理器缓存的函数级分析结果类型
    using Result = LoopDependenceInfo;
    //构建函数级循环依赖查询器
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    //允许AnalysisDescriptor访问本Pass的唯一标识
    friend AnalysisDescriptor<LoopDependenceAnalysis>;
    //LoopDependenceAnalysis在Pass管理器中的唯一标识
    inline static PassID Key;
};
