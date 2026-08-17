#include "ir/analysis/TargetCostAnalysis.hpp"
#include "lib/Target.hpp"
#include <algorithm>
#include <limits>
#include <vector>

bool TargetCostInfo::fitsSignedBits(std::int64_t Value, unsigned Bits) {
    if (Bits == 0)
        return false;
    if (Bits >= 64)
        return true;
    const std::int64_t Min = -(std::int64_t{1} << (Bits - 1));
    const std::int64_t Max = (std::int64_t{1} << (Bits - 1)) - 1;
    return Value >= Min && Value <= Max;
}

std::int64_t TargetCostInfo::normalizeImmediate(std::int64_t Value,
                                                unsigned BitWidth) {
    if (BitWidth == 0 || BitWidth >= 64)
        return Value;
    const std::uint64_t Mask = (std::uint64_t{1} << BitWidth) - 1;
    std::uint64_t Bits = static_cast<std::uint64_t>(Value) & Mask;
    const std::uint64_t Sign = std::uint64_t{1} << (BitWidth - 1);
    if ((Bits & Sign) != 0)
        Bits |= ~Mask;
    return static_cast<std::int64_t>(Bits);
}

std::int64_t TargetCostInfo::signExtend12(std::uint64_t Value) {
    std::uint64_t Bits = Value & 0xfffULL;
    if ((Bits & 0x800ULL) != 0)
        Bits |= ~0xfffULL;
    return static_cast<std::int64_t>(Bits);
}

bool TargetCostInfo::isFloatingType(const IRType *Type) {
    if (!Type)
        return false;
    if (Type->isFloat())
        return true;
    if (!Type->isVector())
        return false;
    const VECTORType *Vector = dynamic_cast<const VECTORType *>(Type);
    return Vector && Vector->getElementType() &&
           Vector->getElementType()->isFloat();
}

bool TargetCostInfo::isDirectScalarType(const IRType *Type) {
    return Type && (Type->isInt() || Type->isBool() || Type->isFloat() ||
                    Type->isPointer());
}

bool TargetCostInfo::isFloatingArithmeticOpcode(
    Instruction::InstType Opcode) {
    return Opcode == Instruction::FADD || Opcode == Instruction::FSUB ||
           Opcode == Instruction::FMUL || Opcode == Instruction::FDIV ||
           Opcode == Instruction::FREM;
}

bool TargetCostInfo::isIntegerArithmeticOpcode(
    Instruction::InstType Opcode) {
    switch (Opcode) {
    case Instruction::ADD:
    case Instruction::SUB:
    case Instruction::MUL:
    case Instruction::SDIV:
    case Instruction::UDIV:
    case Instruction::SREM:
    case Instruction::UREM:
    case Instruction::AND:
    case Instruction::OR:
    case Instruction::XOR:
    case Instruction::SHL:
    case Instruction::LSHR:
    case Instruction::ASHR:
        return true;
    default:
        return false;
    }
}

unsigned TargetCostInfo::getScalarIntegerBitWidth(const IRType *Type) {
    if (!Type)
        return 0;
    if (Type->isPointer())
        return 64;
    if (Type->isVector()) {
        const auto *Vector = dynamic_cast<const VECTORType *>(Type);
        return Vector && Vector->getElementType()
                   ? getScalarIntegerBitWidth(
                         Vector->getElementType().get())
                   : 0;
    }
    if (!Type->isInt() && !Type->isBool())
        return 0;
    const int Width = Type->getIntegerBitWidth(0);
    return Width > 0 ? static_cast<unsigned>(Width) : 0;
}

int TargetCostInfo::getRISCV64ImmediateCost(std::int64_t Value,
                                            unsigned BitWidth) const {
    Value = normalizeImmediate(Value, BitWidth);
    if (Value == 0)
        return 0;
    if (fitsSignedBits(Value, 12))
        return 1;

    // RV64先用LUI/ADDI(W)构造有符号32位主体，再按12位块向低位延伸。
    if (fitsSignedBits(Value, 32)) {
        const std::int64_t Low = signExtend12(
            static_cast<std::uint64_t>(Value));
        return Low == 0 ? 1 : 2;
    }

    const std::int64_t Low = signExtend12(
        static_cast<std::uint64_t>(Value));
    const __int128 Adjusted = static_cast<__int128>(Value) - Low;
    const std::int64_t High = static_cast<std::int64_t>(Adjusted / 4096);
    return getRISCV64ImmediateCost(High, 64) + 1 + (Low != 0 ? 1 : 0);
}

int TargetCostInfo::getIntImmediateCost(std::int64_t Value,
                                        unsigned BitWidth,
                                        TargetCostKind Kind) const {
    (void)Kind;
    return getRISCV64ImmediateCost(Value, BitWidth);
}

bool TargetCostInfo::isLegalIntImmediate(Instruction::InstType Opcode,
                                         unsigned OperandIndex,
                                         std::int64_t Value,
                                         unsigned OperandBitWidth) const {
    Value = normalizeImmediate(Value, OperandBitWidth);
    switch (Opcode) {
    case Instruction::ADD:
    case Instruction::AND:
    case Instruction::OR:
    case Instruction::XOR:
        // 交换律运算允许后端把常量交换到立即数槽。
        return OperandIndex < 2 && fitsSignedBits(Value, 12);
    case Instruction::SUB:
        // sub x,C可改写为addi x,-C，单独处理2048这一不对称端点。
        return OperandIndex == 1 && Value >= -2047 && Value <= 2048;
    case Instruction::SHL:
    case Instruction::LSHR:
    case Instruction::ASHR:
        return OperandIndex == 1 && Value >= 0 &&
               static_cast<std::uint64_t>(Value) < OperandBitWidth;
    case Instruction::ICMP:
        // 基础RV分支只能把零寄存器当作无物化比较操作数。
        return OperandIndex < 2 && Value == 0;
    default:
        return false;
    }
}

int TargetCostInfo::getIntImmediateOperandCost(
    Instruction::InstType Opcode, unsigned OperandIndex,
    std::int64_t Value, unsigned OperandBitWidth,
    TargetCostKind Kind) const {
    if (isLegalIntImmediate(Opcode, OperandIndex, Value,
                            OperandBitWidth))
        return 0;
    return getIntImmediateCost(Value, OperandBitWidth, Kind);
}

unsigned TargetCostInfo::getFixedVectorCostScale(
    const IRType *Type) const noexcept {
    const auto *Vector = dynamic_cast<const VECTORType *>(Type);
    if (!Vector || !Vector->getElementType())
        return 0;
    const auto &Element = Vector->getElementType();
    if ((!Element->isFloat() && !Element->isInt()) ||
        Vector->getElementCount() >
            std::numeric_limits<unsigned>::max() / 32U)
        return 0;
    const unsigned VectorBits = static_cast<unsigned>(
        Vector->getElementCount() * 32U);
    if (VectorBits != 64 && VectorBits != 128)
        return 0;
    return VectorBits / 64U;
}

int TargetCostInfo::getFixedVectorArithmeticCost(
    Instruction::InstType Opcode, const IRType *Type,
    TargetCostKind Kind) const {
    if (!isLegalVectorType(Type))
        return InvalidCost;
    const auto *Vector = dynamic_cast<const VECTORType *>(Type);
    if (!Vector || !Vector->getElementType())
        return InvalidCost;

    const unsigned Scale = getFixedVectorCostScale(Type);
    if (Scale == 0)
        return InvalidCost;
    const auto &Element = Vector->getElementType();
    const bool IsMask = Element->isInt() &&
                        Element->hasIntegerBitWidth(1);
    const bool IsInt32 = Element->isInt() &&
                         Element->hasIntegerBitWidth(32);
    const bool IsFloat32 = Element->isFloat();

    if (Opcode == Instruction::BITCAST)
        return 0;
    if (Kind == TargetCostKind::CodeSize) {
        // 白名单中的原生固定 SIMD 操作按一条未压缩指令计代码体积。
        if ((IsMask && (Opcode == Instruction::AND ||
                        Opcode == Instruction::OR ||
                        Opcode == Instruction::XOR)) ||
            (IsInt32 && (Opcode == Instruction::ADD ||
                         Opcode == Instruction::SUB ||
                         Opcode == Instruction::MUL ||
                         Opcode == Instruction::AND ||
                         Opcode == Instruction::OR ||
                         Opcode == Instruction::XOR)) ||
            (IsFloat32 && (Opcode == Instruction::FADD ||
                           Opcode == Instruction::FSUB ||
                           Opcode == Instruction::FMUL))) {
            return 1;
        }
        return InvalidCost;
    }

    if (IsMask) {
        if (Opcode == Instruction::AND || Opcode == Instruction::OR ||
            Opcode == Instruction::XOR)
            return static_cast<int>(Scale);
        return InvalidCost;
    }
    if (IsInt32) {
        switch (Opcode) {
        case Instruction::ADD:
        case Instruction::SUB:
        case Instruction::AND:
        case Instruction::OR:
        case Instruction::XOR:
            return static_cast<int>(Scale);
        case Instruction::MUL:
            return static_cast<int>(3U * Scale);
        default:
            return InvalidCost;
        }
    }
    if (IsFloat32) {
        switch (Opcode) {
        case Instruction::FADD:
        case Instruction::FSUB:
        case Instruction::FMUL:
            return static_cast<int>(3U * Scale);
        default:
            return InvalidCost;
        }
    }
    return InvalidCost;
}

int TargetCostInfo::getArithmeticCost(Instruction::InstType Opcode,
                                      const IRType *Type,
                                      TargetCostKind Kind) const {
    if (!Type)
        return InvalidCost;
    if (Type->isVector())
        return getFixedVectorArithmeticCost(Opcode, Type, Kind);
    if (!Type->isVector() && !isDirectScalarType(Type))
        return InvalidCost;

    const bool IsFloatOpcode = isFloatingArithmeticOpcode(Opcode);
    const bool IsIntegerOpcode = isIntegerArithmeticOpcode(Opcode);
    const unsigned IntegerWidth = getScalarIntegerBitWidth(Type);
    const bool UsesRV64WordOperation = IntegerWidth == 32;
    const bool UsesRV64XLenOperation = IntegerWidth == 64;
    const bool IsFloatingValue = isFloatingType(Type);
    if (IsFloatOpcode && !IsFloatingValue)
        return InvalidCost;
    if (IsIntegerOpcode) {
        if (IsFloatingValue || Type->isPointer())
            return InvalidCost;
        // i32明确使用RV64的W类操作，i64使用XLEN操作；二者当前成本相同，
        // 但其他整数宽度不能悄悄沿用同一模型。i1只允许位逻辑。
        if (!UsesRV64WordOperation && !UsesRV64XLenOperation &&
            !(IntegerWidth == 1 &&
              (Opcode == Instruction::AND || Opcode == Instruction::OR ||
               Opcode == Instruction::XOR))) {
            return InvalidCost;
        }
    }

    const bool IsIntegerCast = Opcode == Instruction::TRUNC ||
        Opcode == Instruction::ZEXT || Opcode == Instruction::SEXT ||
        Opcode == Instruction::FPTOSI;
    if (IsIntegerCast && IntegerWidth != 1 && IntegerWidth != 32 &&
        IntegerWidth != 64)
        return InvalidCost;
    if (Opcode == Instruction::SITOFP && !IsFloatingValue)
        return InvalidCost;
    if (Opcode == Instruction::BITCAST && !Type->isVector() &&
        !isDirectScalarType(Type))
        return InvalidCost;
    if (!IsFloatOpcode && !IsIntegerOpcode && !IsIntegerCast &&
        Opcode != Instruction::SITOFP && Opcode != Instruction::BITCAST)
        return InvalidCost;

    if (Kind == TargetCostKind::CodeSize) {
        if (Opcode == Instruction::BITCAST)
            return 0;
        if (Opcode == Instruction::FREM)
            return 2;
        return 1;
    }

    switch (Opcode) {
    case Instruction::ADD:
    case Instruction::SUB:
    case Instruction::AND:
    case Instruction::OR:
    case Instruction::XOR:
    case Instruction::SHL:
    case Instruction::LSHR:
    case Instruction::ASHR:
        // W类与XLEN类在RV64GC中都各由一条标量指令完成。
        return 1;
    case Instruction::MUL:
    case Instruction::FADD:
    case Instruction::FSUB:
    case Instruction::FMUL:
        return 3;
    case Instruction::SDIV:
    case Instruction::UDIV:
    case Instruction::SREM:
    case Instruction::UREM:
        return 10;
    case Instruction::FDIV:
        return 12;
    case Instruction::FREM:
        return 20;
    case Instruction::TRUNC:
    case Instruction::ZEXT:
    case Instruction::SEXT:
    case Instruction::FPTOSI:
    case Instruction::SITOFP:
        return 1;
    case Instruction::BITCAST:
        return 0;
    default:
        return InvalidCost;
    }
}

int TargetCostInfo::getCompareCost(const IRType *Type,
                                   TargetCostKind Kind) const {
    if (Type && Type->isVector()) {
        if (!isLegalVectorType(Type))
            return InvalidCost;
        const auto *Vector = dynamic_cast<const VECTORType *>(Type);
        if (!Vector || !Vector->getElementType() ||
            Vector->getElementType()->hasIntegerBitWidth(1))
            return InvalidCost;
        const unsigned Scale = getFixedVectorCostScale(Type);
        if (Scale == 0)
            return InvalidCost;
        if (Kind == TargetCostKind::CodeSize)
            return 1;
        return Vector->getElementType()->isFloat()
                   ? static_cast<int>(2U * Scale)
                   : static_cast<int>(Scale);
    }
    if (Type && !Type->isVector() && !isDirectScalarType(Type))
        return InvalidCost;
    if (Kind == TargetCostKind::CodeSize)
        return 1;
    return isFloatingType(Type) ? 2 : 1;
}

int TargetCostInfo::getBranchCost(bool IsConditional,
                                  TargetCostKind Kind) const {
    (void)IsConditional;
    (void)Kind;
    return 1;
}

int TargetCostInfo::getSelectCost(const IRType *Type,
                                  TargetCostKind Kind) const {
    return getSelectCost(Type, TargetSelectShape::Generic, Kind);
}

int TargetCostInfo::getSelectCost(const IRType *Type,
                                  TargetSelectShape Shape,
                                  TargetCostKind Kind) const {
    if (!Type)
        return InvalidCost;
    if (Type->isVector()) {
        if (!isLegalVectorType(Type) || Shape != TargetSelectShape::Generic)
            return InvalidCost;
        const auto *Vector = dynamic_cast<const VECTORType *>(Type);
        if (!Vector || !Vector->getElementType() ||
            Vector->getElementType()->hasIntegerBitWidth(1))
            return InvalidCost;
        const unsigned Scale = getFixedVectorCostScale(Type);
        if (Scale == 0)
            return InvalidCost;
        // 在缺少最终 mask 指令吞吐量前按“比较掩码 + 合并”保守估价。
        return Kind == TargetCostKind::CodeSize
                   ? 1
                   : static_cast<int>(2U * Scale);
    }
    if (!Type->isVector() && !isDirectScalarType(Type))
        return InvalidCost;
    if (Shape != TargetSelectShape::Generic) {
        if (Type->isVector())
            return InvalidCost;
        const unsigned Width = getScalarIntegerBitWidth(Type);
        // 两种无分支白名单只适用于i32/i64 GPR；浮点严格语义始终走
        // Generic，不把NaN/有序比较错误当作整数min/max。
        if (Type->isFloat() || Type->isPointer() ||
            (Width != 32 && Width != 64))
            return InvalidCost;
        if (Shape == TargetSelectShape::ConditionalAdd)
            return 3;
        if (Shape == TargetSelectShape::IntegerMinMax)
            return 4;
        return InvalidCost;
    }
    // 普通select静态上展开为条件边、两份拷贝和汇合跳转；热路径通常
    // 只执行条件边、一份可合并拷贝与一条汇合跳转。
    return Kind == TargetCostKind::CodeSize ? 6 : 3;
}

int TargetCostInfo::getMemoryCost(TargetMemoryOperation Operation,
                                  const IRType *Type,
                                  std::size_t Alignment,
                                  bool IsVolatile,
                                  TargetCostKind Kind) const {
    if (!Type)
        return InvalidCost;
    if (Type->isVector()) {
        if (!isLegalVectorType(Type))
            return InvalidCost;
        const auto *Vector = dynamic_cast<const VECTORType *>(Type);
        if (!Vector || !Vector->getElementType() ||
            Vector->getElementType()->hasIntegerBitWidth(1))
            return InvalidCost;
        const DataLayout Layout = DataLayout::forTarget();
        const std::size_t NaturalAlignment =
            Layout.getABITypeAlign(Type);
        // 当前 Load/Store IR 没有显式较小 alignment；在正式建模非对齐
        // 访存前，任何已知不足自然对齐的访问都必须保持非法。
        if (Alignment != 0 && Alignment < NaturalAlignment)
            return InvalidCost;
        const unsigned Scale = getFixedVectorCostScale(Type);
        if (Scale == 0)
            return InvalidCost;
        if (Kind == TargetCostKind::CodeSize)
            return 1;
        int VectorCost = Operation == TargetMemoryOperation::Load
                             ? static_cast<int>(2U * Scale)
                             : static_cast<int>(Scale);
        if (IsVolatile)
            ++VectorCost;
        return VectorCost;
    }
    if (!Type->isVector() && !isDirectScalarType(Type))
        return InvalidCost;

    if (Kind == TargetCostKind::CodeSize)
        return 1;

    const DataLayout Layout = DataLayout::forTarget();
    const std::size_t NaturalAlignment = Layout.getABITypeAlign(Type);
    int Cost = Operation == TargetMemoryOperation::Load ? 2 : 1;
    if (Alignment != 0 && Alignment < NaturalAlignment)
        Cost += 3;
    if (IsVolatile)
        ++Cost;
    return Cost;
}

int TargetCostInfo::getVectorConstantCost(
    const ConstantVector *Constant, TargetCostKind Kind) const {
    if (!Constant || !isLegalVectorType(Constant->getIRType().get()))
        return InvalidCost;
    ConstantSystem *Splat = Constant->getSplatValue();
    if (!Splat)
        return InvalidCost;

    const unsigned Scale = getFixedVectorCostScale(
        Constant->getIRType().get());
    if (Scale == 0)
        return InvalidCost;
    if (Kind == TargetCostKind::CodeSize)
        return 1;

    std::int64_t IntegerValue = 0;
    if (Splat->tryGetInt64Const(IntegerValue)) {
        if (IntegerValue == 0)
            return static_cast<int>(Scale);
        const int ScalarCost = getIntImmediateCost(
            IntegerValue, 32, Kind);
        if (ScalarCost >= InvalidCost)
            return InvalidCost;
        return ScalarCost + static_cast<int>(Scale);
    }
    float FloatValue = 0.0F;
    if (Splat->tryGetFloatConst(FloatValue)) {
        (void)FloatValue;
        // 缺少专用浮点立即数信息时，按标量常量池读取加逐数据块广播估价。
        return static_cast<int>(3U * Scale);
    }
    return InvalidCost;
}

int TargetCostInfo::getStaticAddressCost(
    TargetStaticAddressKind AddressKind, std::int64_t ByteOffset,
    TargetCostKind Kind) const {
    switch (AddressKind) {
    case TargetStaticAddressKind::StackObject:
        if (fitsSignedBits(ByteOffset, 12))
            return 0;
        return getIntImmediateCost(ByteOffset, 64, Kind) + 1;
    case TargetStaticAddressKind::GlobalObject:
    case TargetStaticAddressKind::ConstantPool:
        // medany以AUIPC加第二条PC相对指令取得静态对象地址。
        if (fitsSignedBits(ByteOffset, 12))
            return 2;
        return 3 + getIntImmediateCost(ByteOffset, 64, Kind);
    case TargetStaticAddressKind::Unknown:
        return 1 + getIntImmediateCost(ByteOffset, 64, Kind);
    }
    return InvalidCost;
}

TargetStaticAddressKind TargetCostInfo::classifyStaticAddress(
    const Value *Base) {
    if (!Base)
        return TargetStaticAddressKind::Unknown;
    Value *MutableBase = Value::stripPointerCastsAndGEP(
        const_cast<Value *>(Base));
    if (dynamic_cast<AllocaInst *>(MutableBase))
        return TargetStaticAddressKind::StackObject;
    if (MutableBase->isGlobal() || dynamic_cast<Function *>(MutableBase) ||
        dynamic_cast<BuildInFunction *>(MutableBase))
        return TargetStaticAddressKind::GlobalObject;
    return TargetStaticAddressKind::Unknown;
}

int TargetCostInfo::getStaticAddressCost(const Value *Base,
                                         std::int64_t ByteOffset,
                                         TargetCostKind Kind) const {
    return getStaticAddressCost(classifyStaticAddress(Base), ByteOffset, Kind);
}

unsigned TargetCostInfo::getRegisterBitWidth(
    TargetRegisterClass RegisterClass) const {
    switch (RegisterClass) {
    case TargetRegisterClass::GeneralPurpose:
        return 64;
    case TargetRegisterClass::FloatingPoint:
        // 当前IR和后端只暴露FPR32值，尽管D扩展物理寄存器可容纳64位。
        return 32;
    case TargetRegisterClass::Vector:
        return getNativeVectorBitWidth();
    }
    return 0;
}

unsigned TargetCostInfo::getRegisterCount(
    TargetRegisterClass RegisterClass) const {
    switch (RegisterClass) {
    case TargetRegisterClass::GeneralPurpose:
        // 与RISC-V寄存器分配器公开的t0-t5、a0-a7、s1-s11一致。
        return 25;
    case TargetRegisterClass::FloatingPoint:
        return 32;
    case TargetRegisterClass::Vector:
        // 位宽模式不能推出寄存器文件：64 位 packed SIMD 可能复用 GPR，
        // 128 位实现也可能使用寄存器对或独立寄存器。正式文档公布前以
        // 零表示未知，避免后续压力模型把猜测当成物理资源契约。
        return 0;
    }
    return 0;
}

unsigned TargetCostInfo::getActiveRegisterBudget(
    TargetRegisterClass RegisterClass) const {
    const unsigned Count = getRegisterCount(RegisterClass);
    // 给活跃区间增长保留四分之一寄存器处理调用、地址和溢出临时值。
    return Count * 3U / 4U;
}

unsigned TargetCostInfo::getArgumentRegisterCount(
    TargetRegisterClass RegisterClass) const {
    switch (RegisterClass) {
    case TargetRegisterClass::GeneralPurpose:
    case TargetRegisterClass::FloatingPoint:
        return 8;
    case TargetRegisterClass::Vector:
        return 0;
    }
    return 0;
}

unsigned TargetCostInfo::getCacheLineSize() const noexcept {
    return 64U;
}

unsigned TargetCostInfo::getEstimatedL1DataCacheSize() const noexcept {
    // 竞赛运行环境没有稳定的L1容量契约，零表示未知而非没有cache。
    return 0;
}

int TargetCostInfo::getCallCost(unsigned IntegerArguments,
                                unsigned FloatingArguments,
                                bool IsIndirect,
                                TargetCostKind Kind) const {
    const unsigned IntegerRegisterArguments = getArgumentRegisterCount(
        TargetRegisterClass::GeneralPurpose);
    const unsigned FloatingRegisterArguments = getArgumentRegisterCount(
        TargetRegisterClass::FloatingPoint);
    const unsigned StackArguments =
        (IntegerArguments > IntegerRegisterArguments
             ? IntegerArguments - IntegerRegisterArguments : 0) +
        (FloatingArguments > FloatingRegisterArguments
             ? FloatingArguments - FloatingRegisterArguments : 0);
    if (Kind == TargetCostKind::CodeSize) {
        int Cost = IsIndirect ? 1 : 2;
        Cost += static_cast<int>(StackArguments);
        if (StackArguments != 0)
            Cost += 2;
        return Cost;
    }
    return (IsIndirect ? 10 : 8) +
           static_cast<int>(StackArguments) * 2;
}

int TargetCostInfo::getCallCost(const CallInst *Call,
                                TargetCostKind Kind) const {
    if (!Call)
        return InvalidCost;
    unsigned IntegerArguments = 0;
    unsigned FloatingArguments = 0;
    for (unsigned Index = 0; Index < Call->getArgCount(); ++Index) {
        Value *Argument = Call->getArg(Index);
        const std::shared_ptr<IRType> Type =
            Argument ? Argument->getIRType() : nullptr;
        if (!Type)
            continue;
        if (Type->isVector())
            return InvalidCost;
        if (Type->isFloat())
            ++FloatingArguments;
        else
            ++IntegerArguments;
    }
    if (Call->getIRType() && Call->getIRType()->isVector())
        return InvalidCost;
    const bool IsIndirect =
        Call->getDirectCallee() == nullptr &&
        Call->getBuiltinCallee() == nullptr;
    return getCallCost(IntegerArguments, FloatingArguments,
                       IsIndirect, Kind);
}

bool TargetCostInfo::isLegalVectorType(const IRType *Type) const {
    if (!Type || !Type->isVector())
        return false;
    const DataLayout Layout = DataLayout::forTarget();
    return Layout.supportsNativeVectorType(Type);
}

unsigned TargetCostInfo::getNativeVectorBitWidth() const noexcept {
    return DataLayout::forTarget().getNativeVectorBitWidth();
}

int TargetCostInfo::getInstructionCost(const Instruction *Inst,
                                       TargetCostKind Kind) const {
    if (!Inst)
        return InvalidCost;

    const IRType *Type = Inst->getIRType().get();
    int Cost = InvalidCost;
    switch (Inst->instType) {
    case Instruction::RET:
        Cost = 1;
        break;
    case Instruction::BR: {
        const BrInst *Branch = dynamic_cast<const BrInst *>(Inst);
        Cost = getBranchCost(Branch && Branch->isConditional(), Kind);
        break;
    }
    case Instruction::ALLOCA:
    case Instruction::PHI:
        Cost = 0;
        break;
    case Instruction::LOAD:
        Cost = getMemoryCost(TargetMemoryOperation::Load, Type, 0,
                             false, Kind);
        break;
    case Instruction::STORE: {
        const StoreInst *Store = dynamic_cast<const StoreInst *>(Inst);
        Value *StoredValue = Store ? Store->getValueOperand() : nullptr;
        Cost = getMemoryCost(TargetMemoryOperation::Store,
                             StoredValue ? StoredValue->getIRType().get()
                                         : nullptr,
                             0, false, Kind);
        break;
    }
    case Instruction::GEP: {
        auto *GEP = dynamic_cast<const GetElementPtrInst *>(Inst);
        Value *Base = nullptr;
        std::vector<Value *> Indices;
        std::vector<int64_t> Strides;
        int64_t FinalPointeeStride = 0;
        if (!GEP || !GetElementPtrInst::getBaseAndIndices(
                        GEP, Base, Indices) ||
            !GetElementPtrInst::getIndexStrides(
                Base, Indices, Strides, FinalPointeeStride) ||
            Strides.size() != Indices.size()) {
            Cost = 1;
            break;
        }
        (void)FinalPointeeStride;
        __int128 ConstantOffset = 0;
        int DynamicCost = 0;
        bool OffsetFits = true;
        for (std::size_t Index = 0; Index < Indices.size(); ++Index) {
            int64_t ConstantIndex = 0;
            if (Indices[Index] &&
                Indices[Index]->tryGetInt64Const(ConstantIndex)) {
                ConstantOffset += static_cast<__int128>(ConstantIndex) *
                                  Strides[Index];
                if (ConstantOffset < std::numeric_limits<int64_t>::min() ||
                    ConstantOffset > std::numeric_limits<int64_t>::max())
                    OffsetFits = false;
                continue;
            }
            //动态索引按“缩放后相加”计价；2的幂次步长可由移位完成，
            //其余步长保守按乘法计价。该模型不检查具体数组维度或常量指纹。
            uint64_t Magnitude = Strides[Index] < 0
                ? static_cast<uint64_t>(
                      -static_cast<__int128>(Strides[Index]))
                : static_cast<uint64_t>(Strides[Index]);
            if (Magnitude > 1) {
                const bool IsPowerOfTwo =
                    (Magnitude & (Magnitude - 1)) == 0;
                DynamicCost += IsPowerOfTwo ? 1 : 3;
            }
            ++DynamicCost;
        }
        if (!OffsetFits) {
            Cost = InvalidCost;
            break;
        }
        const int64_t ByteOffset = static_cast<int64_t>(ConstantOffset);
        int AddressCost = InvalidCost;
        switch (classifyStaticAddress(Base)) {
        case TargetStaticAddressKind::StackObject:
            //通用GEP查询不假定结果紧接某条可吸收偏移的load/store；零偏移
            //可复用栈基址，非零12位偏移仍需一条ADDI。
            AddressCost = ByteOffset == 0 ? 0
                : (fitsSignedBits(ByteOffset, 12)
                       ? 1
                       : getIntImmediateCost(ByteOffset, 64, Kind) + 1);
            break;
        case TargetStaticAddressKind::GlobalObject:
        case TargetStaticAddressKind::ConstantPool:
            AddressCost = getStaticAddressCost(
                classifyStaticAddress(Base), ByteOffset, Kind);
            break;
        case TargetStaticAddressKind::Unknown:
            //参数或已有SSA基址本身已经可用；仅为非零偏移支付地址加法成本。
            AddressCost = ByteOffset == 0 ? 0
                : (fitsSignedBits(ByteOffset, 12)
                       ? 1
                       : getIntImmediateCost(ByteOffset, 64, Kind) + 1);
            break;
        }
        if (AddressCost >= InvalidCost ||
            DynamicCost >= InvalidCost - AddressCost) {
            Cost = InvalidCost;
            break;
        }
        Cost = AddressCost + DynamicCost;
        break;
    }
    case Instruction::EXTRACTELEMENT:
    case Instruction::INSERTELEMENT:
    case Instruction::SHUFFLEVECTOR:
        Cost = InvalidCost;
        break;
    case Instruction::ICMP:
    case Instruction::FCMP: {
        // 比较结果可能是 i1 或 <N x i1> 掩码；指令合法性与成本应按
        // 被比较的数据类型判断，不能把掩码宽度误当作数据向量宽度。
        const BinaryInst *Compare = dynamic_cast<const BinaryInst *>(Inst);
        Value *Left = nullptr;
        Value *Right = nullptr;
        const IRType *OperandType = nullptr;
        if (Compare && Compare->getOperands(Left, Right) && Left)
            OperandType = Left->getIRType().get();
        Cost = getCompareCost(OperandType, Kind);
        break;
    }
    case Instruction::SELECT:
        Cost = getSelectCost(Type, Kind);
        break;
    case Instruction::CALL:
        Cost = getCallCost(dynamic_cast<const CallInst *>(Inst), Kind);
        break;
    case Instruction::ADD:
    case Instruction::FADD:
    case Instruction::SUB:
    case Instruction::FSUB:
    case Instruction::MUL:
    case Instruction::FMUL:
    case Instruction::SDIV:
    case Instruction::UDIV:
    case Instruction::FDIV:
    case Instruction::SREM:
    case Instruction::UREM:
    case Instruction::FREM:
    case Instruction::AND:
    case Instruction::OR:
    case Instruction::XOR:
    case Instruction::SHL:
    case Instruction::LSHR:
    case Instruction::ASHR:
    case Instruction::TRUNC:
    case Instruction::ZEXT:
    case Instruction::SEXT:
    case Instruction::FPTOSI:
    case Instruction::SITOFP:
    case Instruction::BITCAST:
        Cost = getArithmeticCost(Inst->instType, Type, Kind);
        break;
    case Instruction::BinaryUnknown:
        Cost = InvalidCost;
        break;
    }

    if (Cost >= InvalidCost)
        return InvalidCost;

    const BinaryInst *Binary = dynamic_cast<const BinaryInst *>(Inst);
    if (!Binary)
        return Cost;
    Value *Left = nullptr;
    Value *Right = nullptr;
    if (!Binary->getOperands(Left, Right))
        return Cost;
    Value *Operands[2] = {Left, Right};
    for (unsigned Index = 0; Index < 2; ++Index) {
        if (!Operands[Index])
            continue;
        std::int64_t Constant = 0;
        if (!Operands[Index]->tryGetInt64Const(Constant))
            continue;
        const unsigned BitWidth = static_cast<unsigned>(
            std::max(1, Operands[Index]->integerBitWidth(64)));
        Cost += getIntImmediateOperandCost(Inst->instType, Index,
                                           Constant, BitWidth, Kind);
    }
    return std::min(Cost, InvalidCost);
}

unsigned TargetCostInfo::getInstructionCodeSize(
    const Instruction *Inst) const {
    const int Cost = getInstructionCost(Inst, TargetCostKind::CodeSize);
    if (Cost >= InvalidCost)
        return std::numeric_limits<unsigned>::max();
    // 选择指令前按未压缩的4字节RISC-V指令估计，避免虚构C扩展命中率。
    if (Cost > static_cast<int>(
                   std::numeric_limits<unsigned>::max() / 4U))
        return std::numeric_limits<unsigned>::max();
    return static_cast<unsigned>(Cost) * 4U;
}

bool TargetCostInfo::invalidate(Function &Function,
                                const PreservationStatus &Preserved) const {
    (void)Function;
    (void)Preserved;
    return false;
}

TargetCostInfo TargetCostAnalysis::run(
    Function &Function,
    FunctionAnalysisPassManager &AnalysisManager) const {
    (void)Function;
    (void)AnalysisManager;
    return TargetCostInfo();
}
