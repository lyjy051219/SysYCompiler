#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/Target.hpp"
#include <cstddef>
#include <cstdint>

// TargetCostKind区分面向吞吐量和面向静态代码体积的两种估价口径。
enum class TargetCostKind {
    Throughput,
    CodeSize,
};

// TargetRegisterClass描述中端真正能够使用的目标寄存器类别。
enum class TargetRegisterClass {
    GeneralPurpose,
    FloatingPoint,
    Vector,
};

// TargetMemoryOperation用于统一查询load和store的目标代价。
enum class TargetMemoryOperation {
    Load,
    Store,
};

// TargetSelectShape对应当前RV后端能够区别处理的select降低形态。
enum class TargetSelectShape {
    // Generic表示普通select，默认以控制流和并行拷贝实现。
    Generic,
    // ConditionalAdd表示一臂为另一臂加增量的整数条件累加。
    ConditionalAdd,
    // IntegerMinMax表示条件比较的两个整数正好也是select两臂。
    IntegerMinMax,
};

// TargetStaticAddressKind描述静态地址基址的来源，而不依赖符号名称。
enum class TargetStaticAddressKind {
    StackObject,
    GlobalObject,
    ConstantPool,
    Unknown,
};

// TargetCostInfo是轻量、只读的目标代价视图。当前精确模型面向RV64GC，
// 其余目标只返回保守结果，避免RISC-V中端决策意外污染其他后端。
class TargetCostInfo {
public:
    // InvalidCost表示当前目标无法直接合法化该IR操作。
    static constexpr int InvalidCost = 1 << 28;

    explicit TargetCostInfo(TargetArch Arch);

    TargetArch getArch() const noexcept;
    bool isRISCV64() const noexcept;

    // 查询一个整数常量脱离具体使用点时所需的最少物化指令数。
    int getIntImmediateCost(
        std::int64_t Value, unsigned BitWidth = 64,
        TargetCostKind Kind = TargetCostKind::Throughput) const;
    // 查询常量是否可以被指定操作数槽直接吸收而无需单独物化。
    bool isLegalIntImmediate(Instruction::InstType Opcode,
                             unsigned OperandIndex,
                             std::int64_t Value,
                             unsigned OperandBitWidth = 64) const;
    // 返回具体使用点额外付出的常量物化代价；可直接编码时为零。
    int getIntImmediateOperandCost(
        Instruction::InstType Opcode, unsigned OperandIndex,
        std::int64_t Value, unsigned OperandBitWidth = 64,
        TargetCostKind Kind = TargetCostKind::Throughput) const;

    // 标量算术、比较、控制流和选择操作的基础目标代价。
    int getArithmeticCost(
        Instruction::InstType Opcode, const IRType *Type,
        TargetCostKind Kind = TargetCostKind::Throughput) const;
    int getCompareCost(
        const IRType *Type,
        TargetCostKind Kind = TargetCostKind::Throughput) const;
    int getBranchCost(
        bool IsConditional,
        TargetCostKind Kind = TargetCostKind::Throughput) const;
    int getSelectCost(
        const IRType *Type,
        TargetCostKind Kind = TargetCostKind::Throughput) const;
    int getSelectCost(
        const IRType *Type, TargetSelectShape Shape,
        TargetCostKind Kind = TargetCostKind::Throughput) const;

    // Alignment为零表示调用者没有额外对齐信息，而不是已知未对齐。
    int getMemoryCost(
        TargetMemoryOperation Operation, const IRType *Type,
        std::size_t Alignment = 0, bool IsVolatile = false,
        TargetCostKind Kind = TargetCostKind::Throughput) const;

    // 静态地址查询按地址来源建模；Value重载复用IR统一的底层对象接口。
    int getStaticAddressCost(
        TargetStaticAddressKind AddressKind,
        std::int64_t ByteOffset = 0,
        TargetCostKind Kind = TargetCostKind::Throughput) const;
    int getStaticAddressCost(
        const Value *Base, std::int64_t ByteOffset = 0,
        TargetCostKind Kind = TargetCostKind::Throughput) const;

    // 返回中端可分配资源，而不是体系结构编码中的全部寄存器数量。
    unsigned getRegisterBitWidth(TargetRegisterClass RegisterClass) const;
    unsigned getRegisterCount(TargetRegisterClass RegisterClass) const;
    // 返回允许中端扩大活跃区间时采用的保守寄存器预算。
    unsigned getActiveRegisterBudget(
        TargetRegisterClass RegisterClass) const;
    // 返回LP64D中整数/浮点实参各自可用的ABI参数寄存器数。
    unsigned getArgumentRegisterCount(
        TargetRegisterClass RegisterClass) const;

    // 当前竞赛RV64GC代价模型采用64字节cache line；L1容量未知时返回零。
    unsigned getCacheLineSize() const noexcept;
    unsigned getEstimatedL1DataCacheSize() const noexcept;

    // 调用代价同时考虑直接/间接调用和超过ABI寄存器额度的实参。
    int getCallCost(
        unsigned IntegerArguments, unsigned FloatingArguments,
        bool IsIndirect,
        TargetCostKind Kind = TargetCostKind::Throughput) const;
    int getCallCost(
        const CallInst *Call,
        TargetCostKind Kind = TargetCostKind::Throughput) const;

    // RV64GC不含V扩展，因此固定向量IR在该模型下明确判为不合法。
    bool isLegalVectorType(const IRType *Type) const;
    unsigned getNativeVectorBitWidth() const noexcept;

    // 通用入口按IR指令综合基础操作、常量和调用约定代价。
    int getInstructionCost(
        const Instruction *Inst,
        TargetCostKind Kind = TargetCostKind::Throughput) const;
    // 返回后端选择前的保守静态字节数；无法合法化时返回UINT_MAX。
    unsigned getInstructionCodeSize(const Instruction *Inst) const;

    // 目标配置在一轮编译期间不变，IR变换不会使该结果失效。
    bool invalidate(Function &Function,
                    const PreservationStatus &Preserved) const;

private:
    TargetArch Arch;

    static bool fitsSignedBits(std::int64_t Value, unsigned Bits);
    static std::int64_t normalizeImmediate(std::int64_t Value,
                                           unsigned BitWidth);
    static std::int64_t signExtend12(std::uint64_t Value);
    static bool isFloatingType(const IRType *Type);
    static bool isDirectScalarType(const IRType *Type);
    static bool isFloatingArithmeticOpcode(Instruction::InstType Opcode);
    static bool isIntegerArithmeticOpcode(Instruction::InstType Opcode);
    static unsigned getScalarIntegerBitWidth(const IRType *Type);
    static TargetStaticAddressKind classifyStaticAddress(const Value *Base);
    int getRISCV64ImmediateCost(std::int64_t Value,
                               unsigned BitWidth) const;
};

// TargetCostAnalysis把进程级TargetInfo快照接入函数分析管理器。
class TargetCostAnalysis : public AnalysisDescriptor<TargetCostAnalysis> {
public:
    using Result = TargetCostInfo;

    Result run(Function &Function,
               FunctionAnalysisPassManager &AnalysisManager) const;

private:
    friend struct AnalysisDescriptor<TargetCostAnalysis>;
    inline static PassID Key;
};
