#include "../../include/lib/IRGenerator.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>

extern std::unique_ptr<Module> ModuleIR;

class IRGeneratorSymbolScope final {
public:
    explicit IRGeneratorSymbolScope(SymbolTable& symbols) : symbols_(symbols) {
        symbols_.enter_scope();
    }

    ~IRGeneratorSymbolScope() { symbols_.exit_scope(); }

    IRGeneratorSymbolScope(const IRGeneratorSymbolScope&) = delete;
    IRGeneratorSymbolScope& operator=(const IRGeneratorSymbolScope&) = delete;

private:
    SymbolTable& symbols_;
};

template <typename T>
class IRGeneratorScopedRestore final {
public:
    IRGeneratorScopedRestore(T& slot, T value) : slot_(slot), oldValue_(slot) {
        slot_ = std::move(value);
    }

    ~IRGeneratorScopedRestore() { slot_ = std::move(oldValue_); }

    IRGeneratorScopedRestore(const IRGeneratorScopedRestore&) = delete;
    IRGeneratorScopedRestore& operator=(const IRGeneratorScopedRestore&) = delete;

private:
    T& slot_;
    T oldValue_;
};

template <typename T>
class IRGeneratorScopedPush final {
public:
    IRGeneratorScopedPush(std::vector<T>& values, T value) : values_(values) {
        values_.push_back(std::move(value));
    }

    ~IRGeneratorScopedPush() { values_.pop_back(); }

    IRGeneratorScopedPush(const IRGeneratorScopedPush&) = delete;
    IRGeneratorScopedPush& operator=(const IRGeneratorScopedPush&) = delete;

private:
    std::vector<T>& values_;
};

BasicBlock& IRGenerator::currentBlock() const {
    return *context_.block;
}

TypeSystem IRGenerator::lowerSourceType(Type type, bool allowVoid) const {
    switch (type) {
    case SY_INT:
        return IR_INT;
    case SY_FLOAT:
        return IR_FLOAT;
    case SY_VOID:
        return allowVoid ? IR_VOID : IR_UNDEF;
    default:
        return IR_UNDEF;
    }
}

std::shared_ptr<IRType> IRGenerator::lowerScalarType(Type type) const {
    return IRType::NewTypeByTypeSystem(lowerSourceType(type));
}

bool IRGenerator::isConstantPrimary(const HasOperand& node) const {
    if (auto expression = dynamic_cast<const AddExp*>(&node))
        return isConstantExpression(*expression);
    if (dynamic_cast<const ConValue<int>*>(&node) != nullptr ||
        dynamic_cast<const ConValue<float>*>(&node) != nullptr) {
        return true;
    }
    if (dynamic_cast<const FuncCall*>(&node) != nullptr)
        return false;

    const auto* lvalue = dynamic_cast<const LVal*>(&node);
    if (lvalue == nullptr)
        return false;

    Value* symbol =
        ModuleIR->getSymbolTable().getValueByName(lvalue->get_ident());
    const auto bindingEntry = bindings_.find(symbol);
    if (bindingEntry == bindings_.end())
        return false;
    const Binding& binding = bindingEntry->second;

    if (binding.kind == BindingKind::ConstantValue)
        return !lvalue->has_array();
    if (binding.isMutable || binding.constantInitializer == nullptr ||
        binding.objectType == nullptr || !lvalue->has_array()) {
        return false;
    }

    std::size_t rank = 0;
    for (auto type = binding.objectType;
         type != nullptr && type->getEnum() == IR_ARRAY;) {
        ++rank;
        type = std::dynamic_pointer_cast<ARRAYType>(type)->getElementType();
    }
    const auto& indices = lvalue->get_array()->get_expressions();
    if (indices.size() != rank)
        return false;
    for (const auto& index : indices) {
        if (!isConstantExpression(*index))
            return false;
    }
    return true;
}

bool IRGenerator::isConstantExpression(const UnaryExp& node) const {
    return node.get_operand_count() == 1 &&
           isConstantPrimary(*node.getOperands().front());
}

template <typename OperandT>
bool IRGenerator::hasConstantOperands(const BaseExp<OperandT>& node) const {
    if (node.getOperands().empty() ||
        node.getOperands().size() != node.getOps().size() + 1) {
        return false;
    }
    for (const auto& operand : node.getOperands()) {
        if (!isConstantExpression(*operand))
            return false;
    }
    return true;
}

bool IRGenerator::isConstantExpression(const MulExp& node) const {
    return hasConstantOperands(node);
}

bool IRGenerator::isConstantExpression(const AddExp& node) const {
    return hasConstantOperands(node);
}

bool IRGenerator::isConstantExpression(const RelExp& node) const {
    return hasConstantOperands(node);
}

bool IRGenerator::isConstantExpression(const EqExp& node) const {
    return hasConstantOperands(node);
}

bool IRGenerator::isConstantInitializerSyntax(const InitVal& node) const {
    if (auto expression = dynamic_cast<const AddExp*>(node.get_value().get()))
        return isConstantExpression(*expression);
    if (auto list = dynamic_cast<const InitValList*>(node.get_value().get())) {
        for (const auto& initializer : list->get_initVals()) {
            if (!isConstantInitializerSyntax(*initializer))
                return false;
        }
        return true;
    }
    return node.get_value() == nullptr;
}

Value* IRGenerator::zeroValue(const std::shared_ptr<IRType>& type) const {
    if (type == nullptr)
        return nullptr;
    switch (type->getEnum()) {
    case IR_INT:
        if (auto integerType = std::dynamic_pointer_cast<INTType>(type);
            integerType && integerType->getIsBool()) {
            return ConstBool::newConstBool(false);
        }
        return ConstInt::newConstInt(0, type->getSize());
    case IR_FLOAT:
        return ConstFloat::newConstFloat(0.0F);
    case IR_POINTER:
        return ConstPtr::newConstPtr(type);
    case IR_ARRAY:
        return new Initializer(type);
    default:
        return nullptr;
    }
}

Value* IRGenerator::coerce(Value* value,
                           const std::shared_ptr<IRType>& targetType) {
    if (value == nullptr || targetType == nullptr)
        return nullptr;
    const auto sourceType = value->getIRType();
    if (sourceType == nullptr)
        return nullptr;

    if (sourceType == targetType)
        return value;
    if (value->isUndef())
        return UndefValue::NewUndefValue(targetType);

    if (targetType->getEnum() == IR_INT) {
        const auto targetInteger = std::dynamic_pointer_cast<INTType>(targetType);
        if (targetInteger == nullptr)
            return nullptr;
        if (targetInteger->getIsBool())
            return toCondition(value);

        if (auto constant = dynamic_cast<ConstBool*>(value))
            return ConstInt::newConstInt(constant->getVal() ? 1 : 0,
                                         targetType->getSize());
        if (auto constant = dynamic_cast<ConstInt*>(value))
            return ConstInt::newConstInt(constant->getVal(), targetType->getSize());
        if (auto constant = dynamic_cast<ConstFloat*>(value))
            return ConstInt::newConstInt(static_cast<int>(constant->getVal()),
                                         targetType->getSize());

        if (sourceType->getEnum() == IR_FLOAT) {
            if (context_.block == nullptr)
                return nullptr;
            return currentBlock().genFPTOSIInst(value);
        }
        if (sourceType->getEnum() == IR_INT) {
            const auto sourceInteger = std::dynamic_pointer_cast<INTType>(sourceType);
            if (sourceInteger && sourceInteger->getIsBool()) {
                if (context_.block == nullptr)
                    return nullptr;
                return currentBlock().genZextInst(value);
            }
            return value;
        }
    }

    if (targetType->getEnum() == IR_FLOAT) {
        if (auto constant = dynamic_cast<ConstBool*>(value))
            return ConstFloat::newConstFloat(constant->getVal() ? 1.0F : 0.0F);
        if (auto constant = dynamic_cast<ConstInt*>(value))
            return ConstFloat::newConstFloat(static_cast<float>(constant->getVal()));
        if (auto constant = dynamic_cast<ConstFloat*>(value))
            return constant;

        if (sourceType->getEnum() == IR_INT) {
            const auto sourceInteger = std::dynamic_pointer_cast<INTType>(sourceType);
            if (context_.block == nullptr)
                return nullptr;
            if (sourceInteger && sourceInteger->getIsBool())
                value = currentBlock().genZextInst(value);
            return currentBlock().genSITOFPInst(value);
        }
        if (sourceType->getEnum() == IR_FLOAT)
            return value;
    }

    // Pointer/aggregate conversions are deliberately not guessed here. Their
    // compatibility belongs to the call/lvalue boundary and, later, vector ABI.
    if (targetType->getEnum() == IR_POINTER &&
        sourceType->getEnum() == IR_POINTER) {
        return value;
    }

    return nullptr;
}

Value* IRGenerator::toCondition(Value* value) {
    if (value == nullptr)
        return nullptr;
    const auto type = value->getIRType();
    if (type == nullptr)
        return nullptr;

    if (auto integerType = std::dynamic_pointer_cast<INTType>(type);
        integerType && integerType->getIsBool()) {
        return value;
    }

    if (auto constant = dynamic_cast<ConstInt*>(value))
        return ConstBool::newConstBool(constant->getVal() != 0);
    if (auto constant = dynamic_cast<ConstFloat*>(value))
        return ConstBool::newConstBool(constant->getVal() != 0.0F);
    if (dynamic_cast<ConstPtr*>(value) != nullptr)
        return ConstBool::newConstBool(false);

    Value* condition = emitBinary(value, BinaryInst::NE, zeroValue(type));
    return condition;
}

Value* IRGenerator::logicalNot(Value* value) {
    Value* condition = toCondition(value);
    if (condition == nullptr)
        return nullptr;
    Value* result = emitBinary(condition, BinaryInst::E,
                               ConstBool::newConstBool(false));
    return result;
}

BinaryInst::Operation IRGenerator::lowerBinaryOperator(Type type) const {
    switch (type) {
    case SY_ADD:
        return BinaryInst::ADD;
    case SY_SUB:
        return BinaryInst::SUB;
    case SY_MUL:
        return BinaryInst::MUL;
    case SY_DIV:
        return BinaryInst::DIV;
    case SY_MOD:
        return BinaryInst::MOD;
    case SY_EQ:
        return BinaryInst::E;
    case SY_NOTEQ:
        return BinaryInst::NE;
    case SY_GREAT:
        return BinaryInst::G;
    case SY_GREATEQ:
        return BinaryInst::GE;
    case SY_LESS:
        return BinaryInst::L;
    case SY_LESSEQ:
        return BinaryInst::LE;
    default:
        return BinaryInst::ADD;
    }
}

Value* IRGenerator::emitBinary(Value* left,
                               BinaryInst::Operation operation,
                               Value* right) {
    if (left == nullptr || right == nullptr)
        return nullptr;
    const auto leftType = left->getIRType();
    const auto rightType = right->getIRType();
    if (leftType == nullptr || rightType == nullptr)
        return nullptr;

    if (operation == BinaryInst::MOD &&
        (leftType->getEnum() != IR_INT || rightType->getEnum() != IR_INT)) {
        return UndefValue::NewUndefValue(INTType::NewInt(4));
    }

    if (IRType::isScalar(leftType) && IRType::isScalar(rightType)) {
        if (BinaryInst::isIntegerOnlyOp(operation)) {
            left = coerce(left, INTType::NewInt(4));
            right = coerce(right, INTType::NewInt(4));
        } else if (leftType->getEnum() == IR_FLOAT ||
                   rightType->getEnum() == IR_FLOAT) {
            left = coerce(left, FLOATType::NewFloat());
            right = coerce(right, FLOATType::NewFloat());
        } else {
            // i1 是条件结果而不是 SysY 的独立算术类型。
            left = coerce(left, INTType::NewInt(4));
            right = coerce(right, INTType::NewInt(4));
        }
    }

    if (left == nullptr || right == nullptr)
        return nullptr;
    if ((!left->isConst() || !right->isConst()) && context_.block == nullptr)
        return nullptr;

    Value* result = BasicBlock::genBinaryInst(
        context_.block, left, operation, right);
    return result;
}

template <typename OperandT>
Value* IRGenerator::lowerBinaryChain(const BaseExp<OperandT>& node) {
    const auto& operands = node.getOperands();
    const auto& operators = node.getOps();
    if (operands.empty() || operands.size() != operators.size() + 1)
        return nullptr;

    auto operand = operands.begin();
    Value* result = lowerExpression(*operand->get());
    for (Type sourceOperator : operators) {
        ++operand;
        Value* right = lowerExpression(*operand->get());
        result = emitBinary(result, lowerBinaryOperator(sourceOperator), right);
    }
    return result;
}

Value* IRGenerator::lowerPrimary(const HasOperand& node) {
    if (auto expression = dynamic_cast<const AddExp*>(&node))
        return lowerExpression(*expression);
    if (auto lvalue = dynamic_cast<const LVal*>(&node))
        return lowerRValue(*lvalue);
    if (auto call = dynamic_cast<const FuncCall*>(&node))
        return lowerExpression(*call);
    if (auto integer = dynamic_cast<const ConValue<int>*>(&node))
        return lowerExpression(*integer);
    if (auto floating = dynamic_cast<const ConValue<float>*>(&node))
        return lowerExpression(*floating);

    return nullptr;
}

Value* IRGenerator::lowerExpression(const UnaryExp& node) {
    if (node.get_operand_count() != 1)
        return nullptr;
    Value* result = lowerPrimary(*node.getOperands().front());
    if (result == nullptr)
        return nullptr;

    // Bison 将外层一元操作 push_front；逆序应用即可按由内到外求值。
    for (auto operation = node.getOps().rbegin();
         operation != node.getOps().rend(); ++operation) {
        switch (*operation) {
        case SY_ADD:
            break;
        case SY_SUB: {
            if (IRType::isBool(result->getIRType()))
                result = coerce(result, INTType::NewInt(4));
            Value* zero = zeroValue(result->getIRType());
            result = emitBinary(zero, BinaryInst::SUB, result);
            break;
        }
        case SY_NOT:
            result = logicalNot(result);
            break;
        default:
            return nullptr;
        }
        if (result == nullptr)
            return nullptr;
    }
    return result;
}

Value* IRGenerator::lowerExpression(const MulExp& node) {
    return lowerBinaryChain(node);
}

Value* IRGenerator::lowerExpression(const AddExp& node) {
    return lowerBinaryChain(node);
}

Value* IRGenerator::lowerExpression(const RelExp& node) {
    return lowerBinaryChain(node);
}

Value* IRGenerator::lowerExpression(const EqExp& node) {
    return lowerBinaryChain(node);
}

Value* IRGenerator::lowerExpression(const ConValue<int>& node) {
    return ConstInt::newConstInt(node.get_value());
}

Value* IRGenerator::lowerExpression(const ConValue<float>& node) {
    return ConstFloat::newConstFloat(node.get_value());
}

std::vector<Value*> IRGenerator::lowerArguments(const FuncRParamList& node) {
    std::vector<Value*> arguments;
    arguments.reserve(node.get_params().size());
    for (const auto& argument : node.get_params())
        arguments.push_back(lowerExpression(*argument));
    return arguments;
}

bool IRGenerator::coerceCallArguments(const std::string& callee,
                                      std::vector<Value*>& arguments) {
    if (auto function = functions_.find(callee); function != functions_.end()) {
        const auto& parameters = function->second->getParams();
        if (arguments.size() != parameters.size())
            return false;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (parameters[index] == nullptr || arguments[index] == nullptr)
                return false;
            const auto targetType = parameters[index]->getIRType();
            const auto sourceType = arguments[index]->getIRType();
            if (targetType == nullptr || sourceType == nullptr)
                return false;
            if (targetType->getEnum() != IR_POINTER) {
                if (sourceType->getEnum() == IR_POINTER ||
                    sourceType->getEnum() == IR_ARRAY) {
                    return false;
                }
                Value* converted = coerce(arguments[index], targetType);
                if (converted == nullptr)
                    return false;
                arguments[index] = converted;
            } else {
                if (sourceType->getEnum() != IR_POINTER)
                    return false;
                const auto targetPointer =
                    std::dynamic_pointer_cast<POINTERType>(targetType);
                const auto sourcePointer =
                    std::dynamic_pointer_cast<POINTERType>(sourceType);
                if (targetPointer == nullptr || sourcePointer == nullptr ||
                    !IRType::hasSameShape(targetPointer->getPointerType(),
                                          sourcePointer->getPointerType())) {
                    return false;
                }
            }
        }
        return true;
    }

    auto requireArity = [&](std::size_t count) {
        return arguments.size() == count;
    };

    auto coerceArgument = [&](std::size_t index,
                              const std::shared_ptr<IRType>& type) {
        if (index >= arguments.size()) {
            return false;
        }
        Value* converted = coerce(arguments[index], type);
        if (converted == nullptr)
            return false;
        arguments[index] = converted;
        return true;
    };

    auto requirePointerArgument = [&](std::size_t index,
                                      const std::shared_ptr<IRType>& pointeeType) {
        if (index >= arguments.size()) {
            return false;
        }
        if (arguments[index] == nullptr)
            return false;

        auto pointerType = std::dynamic_pointer_cast<POINTERType>(
            arguments[index]->getIRType());
        if (pointerType == nullptr || pointerType->getPointerType() == nullptr)
            return false;

        // SysY 运行库把数组视为连续的标量缓冲区。源语言的普通
        // 数组退化只去掉第一维；这里仅在运行库 ABI 边界继续退化，
        // 用户函数仍保留其形参所要求的剩余数组形状。
        std::shared_ptr<IRType> leafType = pointerType->getPointerType();
        while (auto arrayType = std::dynamic_pointer_cast<ARRAYType>(leafType))
            leafType = arrayType->getElementType();
        if (!IRType::hasSameShape(leafType, pointeeType))
            return false;

        while (!IRType::hasSameShape(pointerType->getPointerType(),
                                     pointeeType)) {
            Value* decayed = decayArrayPointer(arguments[index]);
            if (decayed == nullptr)
                return false;
            arguments[index] = decayed;
            pointerType = std::dynamic_pointer_cast<POINTERType>(
                decayed->getIRType());
            if (pointerType == nullptr ||
                pointerType->getPointerType() == nullptr) {
                return false;
            }
        }
        return true;
    };

    if (callee == "putint" || callee == "putch") {
        if (!requireArity(1))
            return false;
        if (!coerceArgument(0, INTType::NewInt(4)))
            return false;
    } else if (callee == "putfloat") {
        if (!requireArity(1))
            return false;
        if (!coerceArgument(0, FLOATType::NewFloat()))
            return false;
    } else if (callee == "putarray") {
        if (!requireArity(2))
            return false;
        if (!coerceArgument(0, INTType::NewInt(4)) ||
            !requirePointerArgument(1, INTType::NewInt(4))) {
            return false;
        }
    } else if (callee == "putfarray") {
        if (!requireArity(2))
            return false;
        if (!coerceArgument(0, INTType::NewInt(4)) ||
            !requirePointerArgument(1, FLOATType::NewFloat())) {
            return false;
        }
    } else if (callee == "getarray") {
        if (!requireArity(1))
            return false;
        if (!requirePointerArgument(0, INTType::NewInt(4)))
            return false;
    } else if (callee == "getfarray") {
        if (!requireArity(1))
            return false;
        if (!requirePointerArgument(0, FLOATType::NewFloat()))
            return false;
    } else if (callee == "starttime" || callee == "stoptime") {
        if (!requireArity(1))
            return false;
        if (!coerceArgument(0, INTType::NewInt(4)))
            return false;
    } else if (callee == "getint" || callee == "getch" ||
               callee == "getfloat") {
        if (!requireArity(0))
            return false;
    }
    return true;
}

Value* IRGenerator::emitCall(const FuncCall& node,
                             std::vector<Value*> arguments) {
    if (node.get_ident() == "starttime" || node.get_ident() == "stoptime")
        arguments.push_back(ConstInt::newConstInt(node.get_lineno()));

    if (!coerceCallArguments(node.get_ident(), arguments))
        return UndefValue::NewUndefValue(INTType::NewInt(4));

    Value* callee = nullptr;
    if (BuildInFunction::isSupportedName(node.get_ident())) {
        ModuleIR->registerBuiltin(node.get_ident());
        callee = BuildInFunction::genBuildInFunction(node.get_ident());
    } else {
        // SysY 允许变量与函数同名；调用语法因此使用独立的函数绑定表，
        // 不受当前变量作用域中的同名对象影响。
        const auto function = functions_.find(node.get_ident());
        if (function == functions_.end()) {
            return UndefValue::NewUndefValue(INTType::NewInt(4));
        }
        callee = function->second;
    }

    if (context_.block == nullptr)
        return UndefValue::NewUndefValue(callee->getIRType());
    auto* instruction = new CallInst(callee, arguments);
    currentBlock().push_back(instruction);
    return instruction->getOprand();
}

Value* IRGenerator::lowerExpression(const FuncCall& node) {
    std::vector<Value*> arguments;
    if (node.has_params())
        arguments = lowerArguments(*node.get_params());
    return emitCall(node, std::move(arguments));
}

std::shared_ptr<IRType> IRGenerator::lowerArrayType(
    const ArrayList& dimensions, std::shared_ptr<IRType> elementType) {
    if (elementType == nullptr)
        return nullptr;
    for (auto dimension = dimensions.get_expressions().rbegin();
         dimension != dimensions.get_expressions().rend(); ++dimension) {
        if (!isConstantExpression(**dimension))
            return elementType;
        Value* dimensionValue = lowerExpression(**dimension);
        if (dimensionValue == nullptr)
            return elementType;
        std::int32_t count = 0;
        const bool hasConstantCount = dimensionValue->tryGetIntConst(count);
        if (!hasConstantCount)
            return elementType;
        if (count < 0)
            return elementType;
        elementType = ARRAYType::NewArray(elementType, count);
    }
    return elementType;
}

void IRGenerator::lowerParameter(FuncParam& node) {
    if (context_.function == nullptr)
        return;
    std::shared_ptr<IRType> parameterType = lowerScalarType(node.get_type());
    if (parameterType == nullptr)
        return;

    if (node.has_dimension()) {
        parameterType = lowerArrayType(*node.get_exp_list(), parameterType);
        if (node.has_empty_brackets()) {
            parameterType = POINTERType::NewPointer(parameterType);
        } else {
            auto arrayType = std::dynamic_pointer_cast<ARRAYType>(parameterType);
            if (arrayType == nullptr)
                return;
            // `T a[N][M]` 作为形参时第一维退化，等价于 `T a[][M]`。
            parameterType = POINTERType::NewPointer(arrayType->getElementType());
        }
    } else if (node.has_empty_brackets()) {
        parameterType = POINTERType::NewPointer(parameterType);
    }

    context_.function->pushParam(
        node.get_ident(),
        new Variable(Variable::Param, parameterType, node.get_ident()));

    Value* storage =
        ModuleIR->getSymbolTable().getValueByName(node.get_ident());
    if (storage == nullptr)
        return;
    if (parameterType->getEnum() == IR_POINTER) {
        const auto pointerType =
            std::dynamic_pointer_cast<POINTERType>(parameterType);
        if (pointerType == nullptr)
            return;
        bindings_[storage] = Binding{
            BindingKind::ArrayParameter,
            pointerType->getPointerType(),
            true,
            nullptr,
        };
    } else {
        bindings_[storage] = Binding{
            BindingKind::ObjectAddress,
            parameterType,
            true,
            nullptr,
        };
    }
}

std::shared_ptr<IRType> IRGenerator::initializerLeafType(
    std::shared_ptr<IRType> type) const {
    if (type == nullptr)
        return nullptr;
    while (auto arrayType = std::dynamic_pointer_cast<ARRAYType>(type))
        type = arrayType->getElementType();
    return type;
}

std::size_t IRGenerator::initializerLeafCount(
    std::shared_ptr<IRType> type) const {
    if (type == nullptr)
        return 0;
    std::size_t count = 1;
    while (auto arrayType = std::dynamic_pointer_cast<ARRAYType>(type)) {
        count *= static_cast<std::size_t>(arrayType->getElementCount());
        type = arrayType->getElementType();
    }
    return count;
}

Value* IRGenerator::lowerInitializer(
    const InitVal& node, const std::shared_ptr<IRType>& expectedType,
    InitializerRole role) {
    if (expectedType == nullptr)
        return nullptr;
    if (auto expression = dynamic_cast<const AddExp*>(node.get_value().get())) {
        if (role == InitializerRole::CompleteObject &&
            expectedType->getEnum() == IR_ARRAY) {
            return new Initializer(expectedType);
        }

        Value* value = lowerExpression(*expression);
        const auto targetType = initializerLeafType(expectedType);
        if (value == nullptr || targetType == nullptr ||
            value->getIRType() == nullptr) {
            return nullptr;
        }
        if (role == InitializerRole::ArrayElement &&
            targetType->getEnum() == IR_INT &&
            value->getIRType()->getEnum() == IR_FLOAT) {
            return zeroValue(targetType);
        }
        return coerce(value, targetType);
    }
    if (auto list = dynamic_cast<const InitValList*>(node.get_value().get()))
        return lowerInitializerList(*list, expectedType);
    if (expectedType->getEnum() == IR_ARRAY)
        return new Initializer(expectedType);
    return zeroValue(expectedType);
}

Initializer* IRGenerator::lowerInitializerList(
    const InitValList& node, const std::shared_ptr<IRType>& expectedType) {
    auto arrayType = std::dynamic_pointer_cast<ARRAYType>(expectedType);
    if (arrayType == nullptr)
        return nullptr;

    auto* result = new Initializer(expectedType);
    std::size_t initializedLeaves = 0;

    // child type -> enclosing array type。类型工厂会驻留复合类型，因此该映射
    // 可以直接描述当前数组的形状，而不依赖任何生成器全局状态。
    std::map<std::shared_ptr<IRType>, std::shared_ptr<ARRAYType>> parentType;
    for (auto current = arrayType; current != nullptr;
         current = std::dynamic_pointer_cast<ARRAYType>(
             current->getElementType())) {
        parentType[current->getElementType()] = current;
    }

    auto largestAlignedType = [&](std::shared_ptr<IRType> type) {
        std::size_t leafCount = initializerLeafCount(type);
        while (leafCount != 0 && initializedLeaves % leafCount != 0) {
            auto nested = std::dynamic_pointer_cast<ARRAYType>(type);
            if (nested == nullptr)
                return std::shared_ptr<IRType>{};
            type = nested->getElementType();
            leafCount = initializerLeafCount(type);
        }
        return type;
    };

    const auto elementType = arrayType->getElementType();
    const std::size_t aggregateLeafCount =
        initializerLeafCount(expectedType);

    for (const auto& initializer : node.get_initVals()) {
        const auto expectedElementType = largestAlignedType(elementType);
        if (expectedElementType == nullptr)
            break;
        Value* value = lowerInitializer(
            *initializer, expectedElementType, InitializerRole::ArrayElement);
        if (value == nullptr || value->getIRType() == nullptr)
            break;

        const std::size_t valueLeafCount =
            initializerLeafCount(value->getIRType());
        if (initializedLeaves > aggregateLeafCount ||
            valueLeafCount > aggregateLeafCount - initializedLeaves) {
            break;
        }
        initializedLeaves += valueLeafCount;
        result->push_back(value);

        while (result->back()->getIRType() !=
               largestAlignedType(elementType)) {
            const auto parent = parentType.find(result->back()->getIRType());
            if (parent == parentType.end())
                return result;
            const auto upperType = parent->second;
            if (result->size() <
                static_cast<std::size_t>(upperType->getElementCount())) {
                return result;
            }
            auto* grouped = new Initializer(upperType);
            for (int index = 0; index < upperType->getElementCount(); ++index) {
                grouped->push_back(result->back());
                result->pop_back();
            }
            std::reverse(grouped->begin(), grouped->end());
            result->push_back(grouped);
        }
    }

    while (!result->empty() && result->back()->getIRType() != elementType) {
        const auto parent = parentType.find(result->back()->getIRType());
        if (parent == parentType.end())
            return result;
        const auto upperType = parent->second;
        auto* grouped = new Initializer(upperType);
        for (int index = 0;
             index < upperType->getElementCount() && !result->empty() &&
             result->back()->getIRType() == upperType->getElementType();
             ++index) {
            grouped->push_back(result->back());
            result->pop_back();
        }
        std::reverse(grouped->begin(), grouped->end());
        result->push_back(grouped);
    }

    return result;
}

void IRGenerator::emitDynamicInitializerStores(
    Value* destination, Initializer& initializer, std::vector<int>& indices) {
    if (destination == nullptr || context_.block == nullptr)
        return;
    for (std::size_t index = 0; index < initializer.size(); ++index) {
        Value*& element = initializer[index];
        indices.push_back(static_cast<int>(index));

        if (auto* nested = dynamic_cast<Initializer*>(element)) {
            emitDynamicInitializerStores(destination, *nested, indices);
        } else if (element != nullptr && !element->isConst()) {
            auto* address = dynamic_cast<GetElementPtrInst*>(
                currentBlock().genGepInst(destination));
            if (address == nullptr) {
                indices.pop_back();
                return;
            }
            address->addUse(ConstInt::newConstInt(0));
            for (int currentIndex : indices)
                address->addUse(ConstInt::newConstInt(currentIndex));
            address->updateType();
            currentBlock().genStoreInst(element, address);
            element = zeroValue(element->getIRType());
        }

        indices.pop_back();
    }
}

void IRGenerator::emitLocalArrayInitialization(
    AllocaInst* destination, const std::shared_ptr<IRType>& arrayType,
    Value* initializer) {
    if (destination == nullptr || arrayType == nullptr || initializer == nullptr ||
        context_.block == nullptr) {
        return;
    }
    auto* aggregate = dynamic_cast<Initializer*>(initializer);
    if (aggregate == nullptr)
        return;

    auto* constantStorage = new Variable(Variable::Constant, arrayType, "");
    constantStorage->addUse(initializer);

    std::vector<Value*> arguments = {
        destination,
        constantStorage,
        ConstInt::newConstInt(static_cast<int>(arrayType->getSize())),
        ConstBool::newConstBool(false),
    };
    currentBlock().genCallInst("llvm.memcpy.p0.p0.i32", arguments);

    // 动态元素不属于常量模板：把模板中的对应位置改成零，并针对实际
    // destination 直接生成 store，不再按变量名回查符号表。
    std::vector<int> indices;
    emitDynamicInitializerStores(destination, *aggregate, indices);
}

template <typename DefinitionT>
void IRGenerator::lowerDefinition(DefinitionT& node, TypeSystem declaredType,
                                  bool isConstant) {
    const std::string name = node.get_ident();
    const auto scalarType = IRType::NewTypeByTypeSystem(declaredType);
    if (scalarType == nullptr)
        return;

    std::shared_ptr<IRType> objectType = scalarType;
    if (node.has_array())
        objectType = lowerArrayType(*node.get_array(), scalarType);
    if (objectType == nullptr)
        return;

    bool hasInitializer = node.has_initval();
    if (hasInitializer && (isConstant || !context_.inFunction()) &&
        !isConstantInitializerSyntax(*node.get_initval())) {
        hasInitializer = false;
    }

    if (objectType->getEnum() == IR_ARRAY) {
        if (context_.inFunction()) {
            if (context_.block == nullptr)
                return;
            auto* storage = currentBlock().genAllocaInst(objectType, name);
            if (storage == nullptr)
                return;
            bindings_[storage] = Binding{
                BindingKind::ObjectAddress,
                objectType,
                !isConstant,
                nullptr,
            };
            if (hasInitializer) {
                Value* initializer = lowerInitializer(
                    *node.get_initval(), objectType);
                if (initializer == nullptr)
                    return;
                if (isConstant) {
                    auto* aggregate = dynamic_cast<Initializer*>(initializer);
                    if (aggregate == nullptr)
                        return;
                    bindings_[storage].constantInitializer = aggregate;
                }
                emitLocalArrayInitialization(storage, objectType, initializer);
            }
        } else {
            const auto storageKind = isConstant
                                         ? Variable::Constant
                                         : Variable::GlobalVariable;
            auto* global = new Variable(storageKind, objectType, name);
            if (isConstant) {
                global->setIdent(".C." + name);
                ModuleIR->getSymbolTable().register_symbolTable(name, global);
            }
            bindings_[global] = Binding{
                BindingKind::ObjectAddress,
                objectType,
                !isConstant,
                nullptr,
            };
            if (hasInitializer) {
                Value* initializer = lowerInitializer(
                    *node.get_initval(), objectType);
                if (initializer == nullptr)
                    return;
                global->addUse(initializer);
                if (isConstant) {
                    auto* aggregate = dynamic_cast<Initializer*>(initializer);
                    if (aggregate == nullptr)
                        return;
                    bindings_[global].constantInitializer = aggregate;
                }
            }
        }
        return;
    }

    if (isConstant) {
        Value* initializer = hasInitializer
                                 ? lowerInitializer(*node.get_initval(), objectType)
                                 : zeroValue(objectType);
        initializer = coerce(initializer, objectType);
        if (initializer == nullptr)
            return;
        bindings_[initializer] = Binding{
            BindingKind::ConstantValue,
            objectType,
            false,
            nullptr,
        };

        if (context_.inFunction()) {
            ModuleIR->getSymbolTable().register_symbolTable(name, initializer);
        } else {
            auto* global = new Variable(Variable::Constant, objectType, name);
            global->setIdent(".C." + name);
            bindings_[global] = Binding{
                BindingKind::ObjectAddress,
                objectType,
                false,
                nullptr,
            };
            global->addUse(initializer);
            // 标量常量按值绑定，后续表达式无需再经 load。
            ModuleIR->getSymbolTable().register_symbolTable(name, initializer);
        }
        return;
    }

    if (context_.inFunction()) {
        if (context_.block == nullptr)
            return;
        auto* storage = currentBlock().genAllocaInst(objectType, name);
        if (storage == nullptr)
            return;
        bindings_[storage] = Binding{
            BindingKind::ObjectAddress,
            objectType,
            true,
            nullptr,
        };
        if (hasInitializer) {
            Value* initializer = lowerInitializer(
                *node.get_initval(), objectType);
            Value* converted = coerce(initializer, objectType);
            if (converted != nullptr)
                currentBlock().genStoreInst(converted, storage);
        }
    } else {
        auto* global = new Variable(Variable::GlobalVariable, objectType, name);
        bindings_[global] = Binding{
            BindingKind::ObjectAddress,
            objectType,
            true,
            nullptr,
        };
        if (hasInitializer) {
            Value* initializer = lowerInitializer(
                *node.get_initval(), objectType);
            Value* converted = coerce(initializer, objectType);
            if (converted != nullptr)
                global->addUse(converted);
        }
    }
}

Value* IRGenerator::tryLowerConstantArrayElement(
    const Binding& binding, const std::vector<Value*>& indices) {
    if (binding.isMutable || binding.constantInitializer == nullptr ||
        indices.empty()) {
        return nullptr;
    }

    std::shared_ptr<IRType> currentType = binding.objectType;
    Value* currentValue = binding.constantInitializer;
    for (Value* indexValue : indices) {
        if (indexValue == nullptr)
            return nullptr;
        std::int32_t index = 0;
        if (!indexValue->tryGetIntConst(index))
            return nullptr;

        const auto arrayType =
            std::dynamic_pointer_cast<ARRAYType>(currentType);
        if (arrayType == nullptr || index < 0 ||
            index >= arrayType->getElementCount()) {
            return nullptr;
        }

        if (currentValue != nullptr) {
            auto* initializer = dynamic_cast<Initializer*>(currentValue);
            if (initializer == nullptr)
                return nullptr;
            const auto position = static_cast<std::size_t>(index);
            currentValue = position < initializer->size()
                               ? (*initializer)[position]
                               : nullptr;
        }
        currentType = arrayType->getElementType();
    }

    if (currentType == nullptr || currentType->getEnum() == IR_ARRAY)
        return nullptr;
    if (currentValue == nullptr)
        return zeroValue(currentType);
    if (!currentValue->isConst())
        return nullptr;
    return coerce(currentValue, currentType);
}

IRGenerator::LValueRef IRGenerator::lowerLValue(const LVal& node) {
    Value* symbol =
        ModuleIR->getSymbolTable().getValueByName(node.get_ident());
    if (symbol == nullptr)
        return {};

    const auto bindingEntry = bindings_.find(symbol);
    if (bindingEntry == bindings_.end())
        return {};
    const Binding& binding = bindingEntry->second;
    if (binding.objectType == nullptr)
        return {};

    std::vector<Value*> indices;
    if (node.has_array()) {
        indices.reserve(node.get_array()->get_expressions().size());
        for (const auto& expression : node.get_array()->get_expressions()) {
            Value* index = lowerExpression(*expression);
            if (index == nullptr || index->getIRType() == nullptr ||
                index->getIRType()->getEnum() != IR_INT) {
                return {};
            }
            Value* converted = coerce(index, INTType::NewInt(4));
            if (converted == nullptr)
                return {};
            indices.push_back(converted);
        }
    }

    if (binding.kind == BindingKind::ConstantValue) {
        if (!indices.empty())
            return {};
        return LValueRef{symbol, binding.objectType, false, false};
    }

    if (Value* constant = tryLowerConstantArrayElement(binding, indices))
        return LValueRef{constant, constant->getIRType(), false, false};

    // 数组形参被 Function::pushParam 保存为 pointer-to-pointer alloca。
    // 先 load 出源语言中的数组指针；无下标时它本身就是退化后的值。
    if (binding.kind == BindingKind::ArrayParameter) {
        if (context_.block == nullptr)
            return {};
        Value* parameterPointer = currentBlock().genLoadInst(symbol);
        if (parameterPointer == nullptr)
            return {};
        if (indices.empty())
            return LValueRef{
                parameterPointer,
                binding.objectType,
                false,
                binding.isMutable,
            };

        auto* address = dynamic_cast<GetElementPtrInst*>(
            currentBlock().genGepInst(parameterPointer));
        if (address == nullptr)
            return {};
        for (Value* index : indices)
            address->addUse(index);
        address->updateType();
        auto resultType =
            std::dynamic_pointer_cast<POINTERType>(address->getIRType());
        if (resultType == nullptr)
            return {};
        return LValueRef{
            address,
            resultType->getPointerType(),
            true,
            binding.isMutable,
        };
    }

    if (binding.kind != BindingKind::ObjectAddress)
        return {};
    if (binding.objectType->getEnum() == IR_ARRAY) {
        if (indices.empty())
            return LValueRef{
                symbol,
                binding.objectType,
                true,
                binding.isMutable,
            };

        if (context_.block == nullptr)
            return {};
        auto* address = dynamic_cast<GetElementPtrInst*>(
            currentBlock().genGepInst(symbol));
        if (address == nullptr)
            return {};
        address->addUse(ConstInt::newConstInt(0));
        for (Value* index : indices)
            address->addUse(index);
        address->updateType();
        auto resultType =
            std::dynamic_pointer_cast<POINTERType>(address->getIRType());
        if (resultType == nullptr)
            return {};
        return LValueRef{
            address,
            resultType->getPointerType(),
            true,
            binding.isMutable,
        };
    }

    if (!indices.empty())
        return {};
    return LValueRef{
        symbol,
        binding.objectType,
        true,
        binding.isMutable,
    };
}

Value* IRGenerator::decayArrayPointer(Value* pointer) {
    if (pointer == nullptr || pointer->getIRType() == nullptr ||
        context_.block == nullptr) {
        return nullptr;
    }
    auto pointerType = std::dynamic_pointer_cast<POINTERType>(pointer->getIRType());
    if (pointerType == nullptr || pointerType->getPointerType() == nullptr ||
        pointerType->getPointerType()->getEnum() != IR_ARRAY) {
        return nullptr;
    }

    auto* decay = dynamic_cast<GetElementPtrInst*>(
        currentBlock().genGepInst(pointer));
    if (decay == nullptr)
        return nullptr;
    decay->addUse(ConstInt::newConstInt(0));
    decay->addUse(ConstInt::newConstInt(0));
    decay->updateType();
    return decay;
}

Value* IRGenerator::lowerRValue(const LVal& node) {
    LValueRef reference = lowerLValue(node);
    if (reference.value == nullptr || reference.objectType == nullptr)
        return nullptr;

    if (!reference.isAddress)
        return reference.value;
    if (reference.objectType->getEnum() == IR_ARRAY)
        return decayArrayPointer(reference.value);
    if (context_.block == nullptr)
        return nullptr;
    return currentBlock().genLoadInst(reference.value);
}

void IRGenerator::emitCondition(const LAndExp& node, BasicBlock* entry,
                                BasicBlock* isTrue, BasicBlock* isFalse) {
    if (entry == nullptr || isTrue == nullptr || isFalse == nullptr)
        return;
    const auto& operands = node.getOperands();
    if (operands.empty()) {
        entry->genUnCondInst(isFalse);
        return;
    }

    BasicBlock* block = entry;
    for (auto operand = operands.begin(); operand != operands.end(); ++operand) {
        context_.block = block;
        Value* condition = toCondition(lowerExpression(**operand));
        if (condition == nullptr) {
            block->genUnCondInst(isFalse);
            return;
        }
        if (std::next(operand) == operands.end()) {
            block->genCondInst(condition, isTrue, isFalse);
        } else {
            BasicBlock* next = block->genBlock();
            if (next == nullptr)
                return;
            block->genCondInst(condition, next, isFalse);
            block = next;
        }
    }
}

void IRGenerator::emitCondition(const LOrExp& node, BasicBlock* entry,
                                BasicBlock* isTrue, BasicBlock* isFalse) {
    if (entry == nullptr || isTrue == nullptr || isFalse == nullptr)
        return;
    const auto& operands = node.getOperands();
    if (operands.empty()) {
        entry->genUnCondInst(isFalse);
        return;
    }

    BasicBlock* block = entry;
    for (auto operand = operands.begin(); operand != operands.end(); ++operand) {
        context_.block = block;
        if (std::next(operand) == operands.end()) {
            emitCondition(**operand, block, isTrue, isFalse);
        } else {
            BasicBlock* next = block->genBlock();
            if (next == nullptr)
                return;
            emitCondition(**operand, block, isTrue, next);
            block = next;
        }
    }
}

template <typename ConditionT>
void IRGenerator::emitDiscardedCondition(const ConditionT& node) {
    BasicBlock* entry = context_.block;
    if (entry == nullptr || entry->hasTerminator())
        return;

    BasicBlock* isTrue = entry->genBlock();
    BasicBlock* isFalse = entry->genBlock();
    BasicBlock* continuation = entry->genBlock();
    if (isTrue == nullptr || isFalse == nullptr || continuation == nullptr)
        return;
    emitCondition(node, entry, isTrue, isFalse);

    context_.block = isTrue;
    currentBlock().genUnCondInst(continuation);
    context_.block = isFalse;
    currentBlock().genUnCondInst(continuation);
    context_.block = continuation;
}

void IRGenerator::visit(CompUnit& node) {
    if (context_.inFunction() || context_.block != nullptr)
        return;
    for (const auto& item : node.get_items())
        item->accept(*this);
}

void IRGenerator::visit(ConstDecl& node) {
    const TypeSystem declaredType = lowerSourceType(node.get_type());
    if (node.get_ConstDefList() == nullptr)
        return;
    for (const auto& definition : node.get_ConstDefList()->get_defs())
        lowerDefinition(*definition, declaredType, true);
}

void IRGenerator::visit(VarDecl& node) {
    const TypeSystem declaredType = lowerSourceType(node.get_type());
    if (node.get_VarDefList() == nullptr)
        return;
    for (const auto& definition : node.get_VarDefList()->get_defs())
        lowerDefinition(*definition, declaredType, false);
}

void IRGenerator::visit(ConstDefList& node) {
    (void)node;
}

void IRGenerator::visit(VarDefList& node) {
    (void)node;
}

void IRGenerator::visit(ConstDef& node) {
    (void)node;
}

void IRGenerator::visit(VarDef& node) {
    (void)node;
}

void IRGenerator::visit(FuncDef& node) {
    if (context_.inFunction() ||
        functions_.find(node.get_ident()) != functions_.end()) {
        return;
    }
    const TypeSystem returnType = lowerSourceType(node.get_type(), true);
    if (returnType == IR_UNDEF)
        return;
    Function& function = ModuleIR->newFunction(returnType, node.get_ident());
    functions_.emplace(node.get_ident(), &function);
    if (function.front() == nullptr)
        return;

    IRGeneratorScopedRestore<Function*> activeFunction(context_.function,
                                                       &function);
    IRGeneratorScopedRestore<BasicBlock*> insertionBlock(context_.block,
                                                         function.front());
    IRGeneratorSymbolScope functionScope(ModuleIR->getSymbolTable());

    if (node.get_params() != nullptr) {
        for (const auto& parameter : node.get_params()->get_params())
            lowerParameter(*parameter);
    }

    if (node.get_funcBody() != nullptr)
        node.get_funcBody()->accept(*this);

    if (context_.block != nullptr && !currentBlock().hasTerminator()) {
        if (function.getIRType()->getEnum() == IR_VOID) {
            currentBlock().genRetInst();
        } else if (function.getIdent() == "main") {
            currentBlock().genRetInst(ConstInt::newConstInt(0));
        } else {
            currentBlock().genRetInst(
                UndefValue::NewUndefValue(function.getIRType()));
        }
    }
}

void IRGenerator::visit(FuncParam& node) {
    lowerParameter(node);
}

void IRGenerator::visit(FuncParamList& node) {
    if (context_.function == nullptr)
        return;
    for (const auto& parameter : node.get_params())
        lowerParameter(*parameter);
}

void IRGenerator::visit(InitValList& node) {
    (void)node;
}

void IRGenerator::visit(InitVal& node) {
    (void)node;
}

void IRGenerator::visit(Block& node) {
    IRGeneratorSymbolScope scope(ModuleIR->getSymbolTable());
    if (node.get_items() != nullptr)
        node.get_items()->accept(*this);
}

void IRGenerator::visit(BlockItemList& node) {
    if (context_.block == nullptr)
        return;
    for (const auto& statement : node.get_stmts()) {
        if (currentBlock().hasTerminator())
            return;
        statement->accept(*this);
    }
}

void IRGenerator::visit(AssignStmt& node) {
    if (context_.block == nullptr || node.get_exp() == nullptr ||
        node.get_lval() == nullptr) {
        return;
    }
    Value* right = lowerExpression(*node.get_exp());
    LValueRef left = lowerLValue(*node.get_lval());
    if (right == nullptr || left.value == nullptr || left.objectType == nullptr ||
        !left.isAddress || !left.isMutable ||
        left.objectType->getEnum() == IR_ARRAY) {
        return;
    }
    Value* converted = coerce(right, left.objectType);
    if (converted != nullptr)
        currentBlock().genStoreInst(converted, left.value);
}

void IRGenerator::visit(ExpStmt& node) {
    if (node.get_exp() != nullptr)
        (void)lowerExpression(*node.get_exp());
}

void IRGenerator::visit(IfStmt& node) {
    BasicBlock* conditionBlock = context_.block;
    if (conditionBlock == nullptr || node.get_condition() == nullptr ||
        node.get_if_stmt() == nullptr) {
        return;
    }
    BasicBlock* trueBlock = conditionBlock->genBlock();
    if (trueBlock == nullptr)
        return;
    BasicBlock* falseBlock = nullptr;
    BasicBlock* continuation = nullptr;

    if (node.has_else()) {
        falseBlock = conditionBlock->genBlock();
        if (falseBlock == nullptr)
            return;
        emitCondition(*node.get_condition(), conditionBlock, trueBlock,
                      falseBlock);
    } else {
        continuation = conditionBlock->genBlock();
        if (continuation == nullptr)
            return;
        emitCondition(*node.get_condition(), conditionBlock, trueBlock,
                      continuation);
    }

    context_.block = trueBlock;
    node.get_if_stmt()->accept(*this);
    if (!currentBlock().hasTerminator()) {
        if (continuation == nullptr)
            continuation = currentBlock().genBlock();
        if (continuation != nullptr)
            currentBlock().genUnCondInst(continuation);
    }

    if (falseBlock != nullptr) {
        context_.block = falseBlock;
        if (node.get_else_stmt() != nullptr)
            node.get_else_stmt()->accept(*this);
        if (!currentBlock().hasTerminator()) {
            if (continuation == nullptr)
                continuation = currentBlock().genBlock();
            if (continuation != nullptr)
                currentBlock().genUnCondInst(continuation);
        }
    }

    if (continuation != nullptr)
        context_.block = continuation;
}

void IRGenerator::visit(WhileStmt& node) {
    BasicBlock* predecessor = context_.block;
    if (predecessor == nullptr || node.get_condition() == nullptr ||
        node.get_while_stmt() == nullptr) {
        return;
    }
    BasicBlock* conditionBlock = predecessor->genBlock("wc");
    BasicBlock* bodyBlock = predecessor->genBlock("wloop");
    BasicBlock* continuation = predecessor->genBlock("wn");
    if (conditionBlock == nullptr || bodyBlock == nullptr ||
        continuation == nullptr) {
        return;
    }

    predecessor->genUnCondInst(conditionBlock);
    emitCondition(*node.get_condition(), conditionBlock, bodyBlock,
                  continuation);

    IRGeneratorScopedPush<LoopTargets> loop(
        context_.loops, LoopTargets{conditionBlock, continuation});
    context_.block = bodyBlock;
    node.get_while_stmt()->accept(*this);
    if (!currentBlock().hasTerminator())
        currentBlock().genUnCondInst(conditionBlock);

    context_.block = continuation;
}

void IRGenerator::visit(BreakStmt& node) {
    (void)node;
    if (context_.block == nullptr || context_.loops.empty())
        return;
    BasicBlock* target = context_.loops.back().breakTarget;
    if (target != nullptr)
        currentBlock().genUnCondInst(target);
}

void IRGenerator::visit(ContinueStmt& node) {
    (void)node;
    if (context_.block == nullptr || context_.loops.empty())
        return;
    BasicBlock* target = context_.loops.back().continueTarget;
    if (target != nullptr)
        currentBlock().genUnCondInst(target);
}

void IRGenerator::visit(ReturnStmt& node) {
    if (context_.function == nullptr || context_.block == nullptr)
        return;
    const auto returnType = context_.function->getIRType();
    if (returnType == nullptr)
        return;
    if (!node.has_return_value()) {
        if (returnType->getEnum() == IR_VOID)
            currentBlock().genRetInst();
        else
            currentBlock().genRetInst(UndefValue::NewUndefValue(returnType));
        return;
    }

    Value* value = lowerExpression(*node.get_return_value());
    if (returnType->getEnum() == IR_VOID) {
        currentBlock().genRetInst();
        return;
    }
    Value* converted = coerce(value, returnType);
    if (converted == nullptr)
        converted = UndefValue::NewUndefValue(returnType);
    currentBlock().genRetInst(converted);
}

// 这些入口仅满足通用 ASTVisitor 接口；内部 lowering 始终直接取得返回值。
void IRGenerator::visit(LVal& node) { (void)lowerRValue(node); }
void IRGenerator::visit(FuncCall& node) { (void)lowerExpression(node); }

void IRGenerator::visit(ArrayList& node) {
    for (const auto& expression : node.get_expressions())
        (void)lowerExpression(*expression);
}

void IRGenerator::visit(FuncRParamList& node) {
    (void)lowerArguments(node);
}

void IRGenerator::visit(UnaryExp& node) { (void)lowerExpression(node); }
void IRGenerator::visit(MulExp& node) { (void)lowerExpression(node); }
void IRGenerator::visit(AddExp& node) { (void)lowerExpression(node); }
void IRGenerator::visit(RelExp& node) { (void)lowerExpression(node); }
void IRGenerator::visit(EqExp& node) { (void)lowerExpression(node); }
void IRGenerator::visit(LAndExp& node) { emitDiscardedCondition(node); }
void IRGenerator::visit(LOrExp& node) { emitDiscardedCondition(node); }
void IRGenerator::visit(ConValue<int>& node) { (void)lowerExpression(node); }
void IRGenerator::visit(ConValue<float>& node) { (void)lowerExpression(node); }
