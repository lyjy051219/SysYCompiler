#include "../../include/lib/IRGenerator.hpp"
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>

extern std::unique_ptr<Module> ModuleIR;

bool IRGenerator::ResolvedLValue::isVectorLane() const {
    return vectorIndex != nullptr && vectorType != nullptr;
}

//核心接口1：已归一化的 AST 类型 -> IR 类型。
std::shared_ptr<IRType> IRGenerator::lowerSourceType(
    const ASTType& type, bool allowVoid) const {
    if (!type.isValid())
        return nullptr;

    std::shared_ptr<IRType> elementType;
    switch (type.getScalarKind()) {
    case ASTScalarKind::Int32:
        elementType = INTType::getInt32Ty();
        break;
    case ASTScalarKind::Float32:
        elementType = FLOATType::NewFloat();
        break;
    case ASTScalarKind::Void:
        if (!type.isScalar() || !allowVoid)
            return nullptr;
        return VOIDType::NewVoid();
    default:
        return nullptr;
    }

    if (type.isFixedVector())
        return VECTORType::NewVector(elementType, type.getLaneCount());
    return type.isScalar() ? elementType : nullptr;
}

bool IRGenerator::isConstantPrimary(const BaseAST& node) const {
    // 字面量和标量常量可直接求值；常量数组只有在维度数完整且每个下标
    // 都是常量表达式时，才允许进入后续的编译期元素折叠。
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

    const SymbolTable::ObjectBinding* binding =
        symbols.lookupObject(lvalue->getIdent());
    if (binding == nullptr)
        return false;

    if (binding->kind == SymbolTable::ObjectKind::Constant) {
        if (!lvalue->hasArray())
            return true;
        const auto vectorType = IRType::asVector(binding->type);
        const auto& indices = lvalue->getArray()->getExpressions();
        return vectorType != nullptr && indices.size() == 1 &&
               isConstantExpression(*indices.front());
    }
    if (binding->isMutable || binding->constantInitializer == nullptr ||
        binding->type == nullptr || !lvalue->hasArray()) {
        return false;
    }

    std::size_t arrayRank = 0;
    auto indexedType = binding->type;
    for (auto type = binding->type;
         type != nullptr && type->getTypeSystem() == IR_ARRAY;) {
        ++arrayRank;
        type = std::dynamic_pointer_cast<ARRAYType>(type)->getElementType();
        indexedType = type;
    }
    const bool hasVectorLeaf =
        indexedType != nullptr && indexedType->isVector();
    const auto& indices = lvalue->getArray()->getExpressions();
    if (indices.size() != arrayRank &&
        (!hasVectorLeaf || indices.size() != arrayRank + 1)) {
        return false;
    }
    for (const auto& index : indices) {
        if (!isConstantExpression(*index))
            return false;
    }
    return true;
}

bool IRGenerator::isConstantExpression(const UnaryExp& node) const {
    return node.getOperandCount() == 1 &&
           isConstantPrimary(*node.getOperands().front());
}

template <typename OperandT>
bool IRGenerator::isConstantExpression(
    const BaseExp<OperandT>& node) const {
    // Mul/Add/Rel/Eq 四类同级表达式具有相同结构，统一检查每个操作数即可。
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

bool IRGenerator::isConstantInitializerSyntax(const InitVal& node) const {
    if (auto expression = dynamic_cast<const AddExp*>(node.getValue().get()))
        return isConstantExpression(*expression);
    if (auto list = dynamic_cast<const InitValList*>(node.getValue().get())) {
        for (const auto& initializer : list->getInitVals()) {
            if (!isConstantInitializerSyntax(*initializer))
                return false;
        }
        return true;
    }
    return node.getValue() == nullptr;
}

Value* IRGenerator::zeroValue(const std::shared_ptr<IRType>& type) const {
    // 按目标类型创建零值；数组的“零值”由空聚合初始化器表示。
    if (type == nullptr)
        return nullptr;
    switch (type->getTypeSystem()) {
    case IR_INT:
        if (auto integerType = std::dynamic_pointer_cast<INTType>(type);
            integerType && integerType->getIsBool()) {
            return ConstBool::newConstBool(false);
        }
        return ConstInt::newConstIntForType(
            0, std::dynamic_pointer_cast<INTType>(type));
    case IR_FLOAT:
        return ConstFloat::newConstFloat(0.0F);
    case IR_POINTER:
        return ConstPtr::newConstPtr(type);
    case IR_ARRAY:
        return new Initializer(type);
    case IR_VECTOR:
        return Value::makeZeroForType(type);
    default:
        return nullptr;
    }
}

Value* IRGenerator::toCondition(Value* value) {
    if (value == nullptr)
        return nullptr;
    const auto type = value->getIRType();
    if (type == nullptr)
        return nullptr;
    // SysY2026 尚未定义向量条件是 any、all 还是禁止。
    // 在规范公布前不将 <N x i1> 误传给标量 br。
    if (type->isVector())
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
    // 先执行 SysY 常用算术转换，再交给基本块统一完成常量折叠或指令生成。
    if (left == nullptr || right == nullptr)
        return nullptr;
    const auto leftType = left->getIRType();
    const auto rightType = right->getIRType();
    if (leftType == nullptr || rightType == nullptr)
        return nullptr;

    if (leftType->isVector() || rightType->isVector()) {
        const auto leftVector = IRType::asVector(leftType);
        const auto rightVector = IRType::asVector(rightType);
        if (leftVector == nullptr || rightVector == nullptr ||
            leftVector->getElementCount() != rightVector->getElementCount()) {
            return nullptr;
        }

        const auto leftElement = leftVector->getElementType();
        const auto rightElement = rightVector->getElementType();
        if (leftElement == nullptr || rightElement == nullptr)
            return nullptr;

        if (operation == BinaryInst::MOD &&
            (!leftElement->isInt() || !rightElement->isInt())) {
            return UndefValue::NewUndefValue(leftType);
        }

        const bool useFloatingPoint =
            !BinaryInst::isIntegerOnlyOp(operation) &&
            operation != BinaryInst::MOD &&
            (leftElement->isFloat() || rightElement->isFloat());
        std::shared_ptr<IRType> commonElementType;
        if (useFloatingPoint) {
            commonElementType = FLOATType::NewFloat();
        } else {
            const auto leftInteger =
                std::dynamic_pointer_cast<INTType>(leftElement);
            const auto rightInteger =
                std::dynamic_pointer_cast<INTType>(rightElement);
            if (leftInteger == nullptr || rightInteger == nullptr)
                return nullptr;
            commonElementType = INTType::get(std::max(
                leftInteger->getBitWidth(), rightInteger->getBitWidth()));
        }
        const auto commonType = VECTORType::NewVector(
            commonElementType, leftVector->getElementCount());
        left = BasicBlock::coerceValueToTypeWithoutBitCast(
            currentBlock, left, commonType);
        right = BasicBlock::coerceValueToTypeWithoutBitCast(
            currentBlock, right, commonType);
    } else if (operation == BinaryInst::MOD &&
               (leftType->getTypeSystem() != IR_INT ||
                rightType->getTypeSystem() != IR_INT)) {
        return UndefValue::NewUndefValue(INTType::getInt32Ty());
    } else if (IRType::isScalar(leftType) && IRType::isScalar(rightType)) {
        const bool useFloatingPoint =
            !BinaryInst::isIntegerOnlyOp(operation) &&
            (leftType->getTypeSystem() == IR_FLOAT ||
             rightType->getTypeSystem() == IR_FLOAT);
        if (useFloatingPoint) {
            left = BasicBlock::coerceValueToTypeWithoutBitCast(
                currentBlock, left, FLOATType::NewFloat());
            right = BasicBlock::coerceValueToTypeWithoutBitCast(
                currentBlock, right, FLOATType::NewFloat());
        } else {
            // 小于 i32 的整数先做语言级提升，i64 则保持其位宽。
            unsigned commonWidth = 32;
            if (auto integer =
                    std::dynamic_pointer_cast<INTType>(leftType))
                commonWidth =
                    std::max(commonWidth, integer->getBitWidth());
            if (auto integer =
                    std::dynamic_pointer_cast<INTType>(rightType))
                commonWidth =
                    std::max(commonWidth, integer->getBitWidth());
            const auto commonType = INTType::get(commonWidth);
            left = BasicBlock::coerceValueToTypeWithoutBitCast(
                currentBlock, left, commonType);
            right = BasicBlock::coerceValueToTypeWithoutBitCast(
                currentBlock, right, commonType);
        }
    }

    if (left == nullptr || right == nullptr)
        return nullptr;
    if ((!left->isConst() || !right->isConst()) && currentBlock == nullptr)
        return nullptr;
    // 生成二元指令，条件表达式常量折叠由 BasicBlock::genBinaryInst 内部完成
    Value* result = BasicBlock::genBinaryInst(currentBlock, left, operation, right);
    return result;
}

template <typename OperandT>
Value* IRGenerator::lowerExpression(const BaseExp<OperandT>& node) {
    // 同一优先级的二元表达式按源码顺序从左向右生成，
    // 每一步都复用 emitBinary 完成类型提升、常量折叠和指令插入。
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

Value* IRGenerator::lowerPrimary(const BaseAST& node) {
    if (auto expression = dynamic_cast<const AddExp*>(&node))
        return lowerExpression(*expression);
    if (auto lvalue = dynamic_cast<const LVal*>(&node))
        return lowerRValue(*lvalue);
    if (auto call = dynamic_cast<const FuncCall*>(&node))
        return lowerExpression(*call);
    if (auto integer = dynamic_cast<const ConValue<int>*>(&node))
        return ConstInt::newConstInt(integer->getValue());
    if (auto floating = dynamic_cast<const ConValue<float>*>(&node))
        return ConstFloat::newConstFloat(floating->getValue());

    return nullptr;
}

Value* IRGenerator::lowerExpression(const UnaryExp& node) {
    if (node.getOperandCount() != 1)
        return nullptr;
    Value* result = lowerPrimary(*node.getOperands().front());
    if (result == nullptr)
        return nullptr;

    // 语法分析器把外层一元操作头插保存，因此逆序应用即可按由内到外求值。
    for (auto operation = node.getOps().rbegin();
         operation != node.getOps().rend(); ++operation) {
        switch (*operation) {
        case SY_ADD:
            break;
        case SY_SUB: {
            if (IRType::isBool(result->getIRType()))
                result = BasicBlock::coerceValueToTypeWithoutBitCast(
                    currentBlock, result, INTType::getInt32Ty());
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

bool IRGenerator::coerceCallArguments(const std::string& callee,
                                      std::vector<Value*>& arguments) {
    // 用户函数严格匹配形参数量与剩余数组形状，标量实参允许常用算术转换。
    if (Function* function = symbols.lookupFunction(callee)) {
        const auto& parameters = function->getParams();
        if (arguments.size() != parameters.size())
            return false;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (parameters[index] == nullptr || arguments[index] == nullptr)
                return false;
            const auto targetType = parameters[index]->getIRType();
            const auto sourceType = arguments[index]->getIRType();
            if (targetType == nullptr || sourceType == nullptr)
                return false;
            if (targetType->getTypeSystem() != IR_POINTER) {
                if (sourceType->getTypeSystem() == IR_POINTER ||
                    sourceType->getTypeSystem() == IR_ARRAY) {
                    return false;
                }
                Value* converted =
                    BasicBlock::coerceValueToTypeWithoutBitCast(
                        currentBlock, arguments[index], targetType);
                if (converted == nullptr)
                    return false;
                arguments[index] = converted;
            } else {
                if (sourceType->getTypeSystem() != IR_POINTER)
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

    // 运行库没有 Function 参数对象，按每个固定接口的 ABI 单独验证。
    auto coerceArgument = [&](std::size_t index,
                              const std::shared_ptr<IRType>& type) {
        if (index >= arguments.size()) {
            return false;
        }
        Value* converted =
            BasicBlock::coerceValueToTypeWithoutBitCast(
                currentBlock, arguments[index], type);
        if (converted == nullptr)
            return false;
        arguments[index] = converted;
        return true;
    };

    auto coercePointerArgument = [&](std::size_t index,
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
        if (arguments.size() != 1)
            return false;
        if (!coerceArgument(0, INTType::getInt32Ty()))
            return false;
    } else if (callee == "putfloat") {
        if (arguments.size() != 1)
            return false;
        if (!coerceArgument(0, FLOATType::NewFloat()))
            return false;
    } else if (callee == "putarray") {
        if (arguments.size() != 2)
            return false;
        if (!coerceArgument(0, INTType::getInt32Ty()) ||
            !coercePointerArgument(1, INTType::getInt32Ty())) {
            return false;
        }
    } else if (callee == "putfarray") {
        if (arguments.size() != 2)
            return false;
        if (!coerceArgument(0, INTType::getInt32Ty()) ||
            !coercePointerArgument(1, FLOATType::NewFloat())) {
            return false;
        }
    } else if (callee == "getarray") {
        if (arguments.size() != 1)
            return false;
        if (!coercePointerArgument(0, INTType::getInt32Ty()))
            return false;
    } else if (callee == "getfarray") {
        if (arguments.size() != 1)
            return false;
        if (!coercePointerArgument(0, FLOATType::NewFloat()))
            return false;
    } else if (callee == "starttime" || callee == "stoptime") {
        if (arguments.size() != 1)
            return false;
        if (!coerceArgument(0, INTType::getInt32Ty()))
            return false;
    } else if (callee == "getint" || callee == "getch" ||
               callee == "getfloat") {
        if (!arguments.empty())
            return false;
    }
    return true;
}

Value* IRGenerator::lowerExpression(const FuncCall& node) {
    // 调用表达式在一个函数内完整处理：先按源码顺序生成实参，
    // 再按调用约定转换类型，最后解析被调函数并插入 CallInst。
    std::vector<Value*> arguments;
    if (node.hasParams()) {
        const auto& parameters = node.getParams()->getParams();
        arguments.reserve(parameters.size());
        for (const auto& parameter : parameters)
            arguments.push_back(lowerExpression(*parameter));
    }

    // 计时接口的源码行号是运行库隐式实参，必须在参数校验前补入。
    if (node.getIdent() == "starttime" || node.getIdent() == "stoptime")
        arguments.push_back(ConstInt::newConstInt(node.getLineNo()));

    if (!coerceCallArguments(node.getIdent(), arguments))
        return UndefValue::NewUndefValue(INTType::getInt32Ty());

    Value* callee = nullptr;
    if (BuildInFunction::isSupportedName(node.getIdent())) {
        ModuleIR->registerBuiltin(node.getIdent());
        callee = BuildInFunction::genBuildInFunction(node.getIdent());
    } else {
        // 调用只查询函数命名空间，不受同名局部对象的遮蔽影响。
        Function* function = symbols.lookupFunction(node.getIdent());
        if (function == nullptr) {
            return UndefValue::NewUndefValue(INTType::getInt32Ty());
        }
        callee = function;
    }

    if (currentBlock == nullptr)
        return UndefValue::NewUndefValue(callee->getIRType());
    auto* instruction = new CallInst(callee, arguments);
    currentBlock->pushBack(instruction);
    return instruction;
}

//核心IR生成逻辑函数1.1：构造数组类型
std::shared_ptr<IRType> IRGenerator::lowerArrayType(
    const ArrayList& dimensions, std::shared_ptr<IRType> elementType) {
    // 维度从内向外包成数组类型
    if (elementType == nullptr)
        return nullptr;
    for (auto dimension = dimensions.getExpressions().rbegin();
         dimension != dimensions.getExpressions().rend(); ++dimension) {
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
// 生成函数形参类型、入口栈槽以及对应的源码名称绑定。
void IRGenerator::lowerParameter(FuncParam& node) {
    if (currentFunction == nullptr)
        return;
    const std::string& parameterName = node.getIdent();
    // 同一形参作用域拒绝重复名字，并在创建参数槽位前终止。
    if (parameterName.empty() ||
        symbols.containsObjectInCurrentScope(parameterName)) {
        return;
    }

    std::shared_ptr<IRType> parameterType =
        lowerSourceType(node.getType());
    if (parameterType == nullptr)
        return;

    if (node.hasDimension()) {
        parameterType = lowerArrayType(*node.getExpList(), parameterType);
        if (node.hasEmptyBrackets()) {
            parameterType = POINTERType::NewPointer(parameterType);
        } else {
            auto arrayType = std::dynamic_pointer_cast<ARRAYType>(parameterType);
            if (arrayType == nullptr)
                return;
            // `T a[N][M]` 作为形参时第一维退化，等价于 `T a[][M]`。
            parameterType = POINTERType::NewPointer(arrayType->getElementType());
        }
    } else if (node.hasEmptyBrackets()) {
        parameterType = POINTERType::NewPointer(parameterType);
    }

    const bool isArrayParameter =
        parameterType->getTypeSystem() == IR_POINTER;
    std::shared_ptr<IRType> sourceObjectType = parameterType;
    if (isArrayParameter) {
        const auto pointerType =
            std::dynamic_pointer_cast<POINTERType>(parameterType);
        if (pointerType == nullptr)
            return;
        sourceObjectType = pointerType->getPointerType();
    }

    AllocaInst* parameterSlot = currentFunction->pushParamWithStackSlot(
        new Param(parameterType, parameterName));
    if (parameterSlot == nullptr)
        return;

    // 标量参数的栈槽就是普通对象地址；数组参数的栈槽中还保存着一层指针，
    // 左值解析时要先 load，因此必须在绑定中保留这一区别。
    const auto kind = isArrayParameter
                          ? SymbolTable::ObjectKind::ArrayParameter
                          : SymbolTable::ObjectKind::Address;
    (void)symbols.declareObject(
        parameterName,
        SymbolTable::ObjectBinding{
            kind, parameterSlot, sourceObjectType, true, nullptr});
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
    // 标量表达式降低为叶值，花括号列表交给聚合初始化器处理。
    // 数组叶位置的 float -> int 维持既有“填零”规则，不在这里改写语言语义。
    if (expectedType == nullptr)
        return nullptr;
    if (auto expression = dynamic_cast<const AddExp*>(node.getValue().get())) {
        if (role == InitializerRole::CompleteObject &&
            expectedType->getTypeSystem() == IR_ARRAY) {
            return new Initializer(expectedType);
        }

        Value* value = lowerExpression(*expression);
        const auto targetType = initializerLeafType(expectedType);
        if (value == nullptr || targetType == nullptr ||
            value->getIRType() == nullptr) {
            return nullptr;
        }
        if (role == InitializerRole::ArrayElement &&
            targetType->getTypeSystem() == IR_INT &&
            value->getIRType()->getTypeSystem() == IR_FLOAT) {
            return zeroValue(targetType);
        }
        return BasicBlock::coerceValueToTypeWithoutBitCast(
            currentBlock, value, targetType);
    }
    if (auto list = dynamic_cast<const InitValList*>(node.getValue().get()))
        return lowerInitializerList(*list, expectedType);
    if (expectedType->getTypeSystem() == IR_ARRAY)
        return new Initializer(expectedType);
    return zeroValue(expectedType);
}

Value* IRGenerator::lowerInitializerList(
    const InitValList& node, const std::shared_ptr<IRType>& expectedType) {
    if (const auto vectorType = IRType::asVector(expectedType))
        return lowerVectorInitializerList(node, vectorType);

    auto arrayType = std::dynamic_pointer_cast<ARRAYType>(expectedType);
    if (arrayType == nullptr)
        return nullptr;

    auto* result = new Initializer(expectedType);
    std::size_t initializedLeaves = 0;

    // 建立“子类型到外层数组类型”的映射，用于把连续叶元素逐层回卷成聚合值。
    // 类型工厂会驻留复合类型，因此可直接用共享指针标识数组形状。
    std::map<std::shared_ptr<IRType>, std::shared_ptr<ARRAYType>> parentType;
    for (auto current = arrayType; current != nullptr;
         current = std::dynamic_pointer_cast<ARRAYType>(
             current->getElementType())) {
        parentType[current->getElementType()] = current;
    }

    // 根据已经消费的叶元素数，选择当前位置允许开始的最大对齐子聚合类型。
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

    for (const auto& initializer : node.getInitVals()) {
        // 每个显式初始化项先生成叶值或子聚合，再在元素数满足时向外回卷。
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

Value* IRGenerator::lowerVectorInitializerList(
    const InitValList& node,
    const std::shared_ptr<VECTORType>& expectedType) {
    if (expectedType == nullptr || expectedType->getElementType() == nullptr)
        return nullptr;
    const auto& initializers = node.getInitVals();
    if (initializers.size() > expectedType->getElementCount())
        return nullptr;

    std::vector<Value*> lanes;
    lanes.reserve(expectedType->getElementCount());
    for (const auto& initializer : initializers) {
        if (initializer == nullptr ||
            dynamic_cast<const InitValList*>(
                initializer->getValue().get()) != nullptr) {
            return nullptr;
        }
        Value* lane = lowerInitializer(
            *initializer, expectedType->getElementType(),
            InitializerRole::CompleteObject);
        lane = BasicBlock::coerceValueToTypeWithoutBitCast(
            currentBlock, lane, expectedType->getElementType());
        if (lane == nullptr)
            return nullptr;
        lanes.push_back(lane);
    }
    while (lanes.size() < expectedType->getElementCount()) {
        Value* lane = zeroValue(expectedType->getElementType());
        if (lane == nullptr)
            return nullptr;
        lanes.push_back(lane);
    }

    std::vector<ConstantSystem*> constantLanes;
    constantLanes.reserve(lanes.size());
    for (Value* lane : lanes) {
        auto* constant = dynamic_cast<ConstantSystem*>(lane);
        if (constant == nullptr) {
            constantLanes.clear();
            break;
        }
        constantLanes.push_back(constant);
    }
    if (constantLanes.size() == lanes.size())
        return ConstantVector::get(expectedType, constantLanes);
    if (currentBlock == nullptr)
        return nullptr;

    Value* result = zeroValue(expectedType);
    if (result == nullptr)
        return nullptr;
    for (std::size_t index = 0; index < lanes.size(); ++index) {
        result = currentBlock->genInsertElementInst(
            result, lanes[index],
            ConstInt::newConstInt(static_cast<std::int64_t>(index)));
        if (result == nullptr)
            return nullptr;
    }
    return result;
}

void IRGenerator::emitDynamicInitializerStores(
    Value* destination, Initializer& initializer, std::vector<int>& indices) {
    // 深度优先遍历聚合值，仅为非常量叶元素生成 GEP 和写内存指令。
    if (destination == nullptr || currentBlock == nullptr)
        return;
    for (std::size_t index = 0; index < initializer.size(); ++index) {
        Value*& element = initializer[index];
        indices.push_back(static_cast<int>(index));

        if (auto* nested = dynamic_cast<Initializer*>(element)) {
            emitDynamicInitializerStores(destination, *nested, indices);
        } else if (element != nullptr && !element->isConst()) {
            auto* address = dynamic_cast<GetElementPtrInst*>(
                currentBlock->genGepInst(destination));
            if (address == nullptr) {
                indices.pop_back();
                return;
            }
            address->addUse(ConstInt::newConstInt(0));
            for (int currentIndex : indices)
                address->addUse(ConstInt::newConstInt(currentIndex));
            address->updateType();
            currentBlock->genStoreInst(element, address);
            element = zeroValue(element->getIRType());
        }

        indices.pop_back();
    }
}

void IRGenerator::emitLocalArrayInitialization(
    AllocaInst* destination, const std::shared_ptr<IRType>& arrayType,
    Value* initializer) {
    // 先用常量模板整体初始化，再用逐元素写入覆盖运行期才能求值的叶节点。
    if (destination == nullptr || arrayType == nullptr || initializer == nullptr ||
        currentBlock == nullptr) {
        return;
    }
    auto* aggregate = dynamic_cast<Initializer*>(initializer);
    if (aggregate == nullptr)
        return;

    auto* constantStorage =
        new Variable(Variable::GlobalConstant, arrayType, "");
    constantStorage->addUse(initializer);

    std::vector<Value*> arguments = {
        destination,
        constantStorage,
        ConstInt::newConstInt(static_cast<int>(arrayType->getSize())),
        ConstBool::newConstBool(false),
    };
    currentBlock->genCallInst("llvm.memcpy.p0.p0.i32", arguments);

    // 动态元素在常量模板中以零占位，并直接针对目标地址生成写内存指令。
    std::vector<int> indices;
    emitDynamicInitializerStores(destination, *aggregate, indices);
}

//核心IR生成逻辑函数1：全局/局部的标量、定长向量与数组对象生成。
/*
                         全局                  局部
first-class 常量       global constant       直接绑定常量值
first-class 变量       global                alloca + 可选 store
数组常量                constant array        alloca + 初始化
数组变量                global array          alloca + 初始化
 */
template <typename DefinitionT>
void IRGenerator::lowerDefinition(
    DefinitionT& node, const std::shared_ptr<IRType>& declaredType,
    bool isConstant) {
    //1.获得变量/常量名
    const std::string name = node.getIdent();
    //在创建存储前拒绝同层重定义
    if (name.empty() || symbols.containsObjectInCurrentScope(name))
        return;

    //2.构造基础对象类型：标量/向量直接使用声明类型，
    // 数组则调用 lowerArrayType 逐层包装。
    const auto scalarType = declaredType;
    if (scalarType == nullptr)
        return;

    std::shared_ptr<IRType> objectType = scalarType;
    if (node.hasArray())
        objectType = lowerArrayType(*node.getArray(), scalarType);
    if (objectType == nullptr)
        return;

    //3.检查是否有初始化器，并在常量或全局变量的情况下，确保初始化器是常量表达式，否则忽略初始化器
    bool hasInitializer = node.hasInitVal();
    if (hasInitializer && (isConstant || currentFunction == nullptr) &&
        !isConstantInitializerSyntax(*node.getInitVal())) {
        hasInitializer = false;
    }

    // 向量是 first-class value，先完整验证并生成初值，再创建
    // 存储和符号绑定，避免过长/非法 lane 列表留下半截 IR。
    Value* preparedVectorInitializer = nullptr;
    if (hasInitializer && objectType->isVector()) {
        preparedVectorInitializer = lowerInitializer(
            *node.getInitVal(), objectType);
        if (preparedVectorInitializer == nullptr)
            return;
    }

    if (objectType->getTypeSystem() == IR_ARRAY) {
        Value* storage = nullptr;
        //4.1局部数组：在当前基本块生成AllocaInst作为存储
        if (currentFunction != nullptr) {
            if (currentBlock == nullptr)
                return;
            storage = currentBlock->genAllocaInst(objectType);
            if (storage == nullptr)
                return;
        } else {
            //4.2全局数组：根据是否为常量选择GlobalConstant或GlobalVariable作为存储
            const auto storageKind = isConstant
                                         ? Variable::GlobalConstant
                                         : Variable::GlobalVariable;
            auto* global = new Variable(storageKind, objectType, name);
            if (isConstant)
                global->setIdent(".C." + name);
            storage = global;
        }

        //4.3登记符号表
        SymbolTable::ObjectBinding* binding = symbols.declareObject(
            name, SymbolTable::ObjectBinding{
                      SymbolTable::ObjectKind::Address, storage, objectType,
                      !isConstant, nullptr});
        if (binding == nullptr) {
            return;
        }

        if (hasInitializer) {
            Value* initializer =
                lowerInitializer(*node.getInitVal(), objectType);
            if (initializer == nullptr)
                return;

            if (isConstant) {
                auto* aggregate = dynamic_cast<Initializer*>(initializer);
                if (aggregate == nullptr)
                    return;
                // 常量数组保留聚合初值，供后续常量下标直接折叠。
                binding->constantInitializer = aggregate;
            }

            if (currentFunction != nullptr) {
                auto* localStorage = dynamic_cast<AllocaInst*>(storage);
                if (localStorage == nullptr)
                    return;
                emitLocalArrayInitialization(
                    localStorage, objectType, initializer);
            } else {
                auto* globalStorage = dynamic_cast<Variable*>(storage);
                if (globalStorage != nullptr)
                    globalStorage->addUse(initializer);
            }
        }
        return;
    }

    if (isConstant) {
        Value* initializer = hasInitializer
                                 ? (preparedVectorInitializer != nullptr
                                        ? preparedVectorInitializer
                                        : lowerInitializer(
                                              *node.getInitVal(), objectType))
                                 : zeroValue(objectType);
        initializer = BasicBlock::coerceValueToTypeWithoutBitCast(
            currentBlock, initializer, objectType);
        if (initializer == nullptr)
            return;

        if (currentFunction == nullptr) {
            auto* global =
                new Variable(Variable::GlobalConstant, objectType, name);
            global->setIdent(".C." + name);
            global->addUse(initializer);
        }
        // first-class 常量按值绑定，后续表达式无需生成内存读取。
        (void)symbols.declareObject(
            name, SymbolTable::ObjectBinding{
                      SymbolTable::ObjectKind::Constant, initializer,
                      objectType, false, nullptr});
        return;
    }

    if (currentFunction != nullptr) {
        if (currentBlock == nullptr)
            return;
        auto* storage = currentBlock->genAllocaInst(objectType);
        if (storage == nullptr)
            return;
        if (symbols.declareObject(
                name, SymbolTable::ObjectBinding{
                          SymbolTable::ObjectKind::Address, storage,
                          objectType, true, nullptr}) == nullptr) {
            return;
        }
        if (hasInitializer) {
            Value* initializer = preparedVectorInitializer != nullptr
                                     ? preparedVectorInitializer
                                     : lowerInitializer(
                                           *node.getInitVal(), objectType);
            Value* converted =
                BasicBlock::coerceValueToTypeWithoutBitCast(
                    currentBlock, initializer, objectType);
            if (converted != nullptr)
                currentBlock->genStoreInst(converted, storage);
        }
    } else {
        auto* global = new Variable(Variable::GlobalVariable, objectType, name);
        if (symbols.declareObject(
                name, SymbolTable::ObjectBinding{
                          SymbolTable::ObjectKind::Address, global,
                          objectType, true, nullptr}) == nullptr) {
            return;
        }
        if (hasInitializer) {
            Value* initializer = preparedVectorInitializer != nullptr
                                     ? preparedVectorInitializer
                                     : lowerInitializer(
                                           *node.getInitVal(), objectType);
            Value* converted =
                BasicBlock::coerceValueToTypeWithoutBitCast(
                    currentBlock, initializer, objectType);
            if (converted != nullptr)
                global->addUse(converted);
        }
    }
}

Value* IRGenerator::tryLowerConstantArrayElement(
    const SymbolTable::ObjectBinding& binding,
    const std::vector<Value*>& indices) {
    // 仅折叠完整的常量数组元素访问；部分下标仍需走普通 GEP 路径，
    // 越界、动态下标和非常量叶值也都保持为运行期访问。
    if (binding.isMutable || binding.constantInitializer == nullptr ||
        indices.empty()) {
        return nullptr;
    }

    std::shared_ptr<IRType> currentType = binding.type;
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

    if (currentType == nullptr || currentType->getTypeSystem() == IR_ARRAY)
        return nullptr;
    if (currentValue == nullptr)
        return zeroValue(currentType);
    if (!currentValue->isConst())
        return nullptr;
    return BasicBlock::coerceValueToTypeWithoutBitCast(
        currentBlock, currentValue, currentType);
}

bool IRGenerator::classifyIndexedAccess(
    const std::shared_ptr<IRType>& rootType,
    bool isArrayParameter,
    const std::vector<Value*>& indices,
    std::vector<Value*>& aggregateIndices,
    std::shared_ptr<IRType>& selectedType,
    Value*& vectorIndex) const {
    aggregateIndices.clear();
    selectedType = rootType;
    vectorIndex = nullptr;
    if (selectedType == nullptr)
        return false;

    std::size_t indexPosition = 0;
    // 数组形参已经是指向首元素的指针，第一个下标只在
    // 该指针序列上移动，不剥离 binding 中保存的 pointee 类型。
    if (isArrayParameter && indexPosition < indices.size()) {
        aggregateIndices.push_back(indices[indexPosition]);
        ++indexPosition;
    }

    while (indexPosition < indices.size()) {
        if (const auto arrayType =
                std::dynamic_pointer_cast<ARRAYType>(selectedType)) {
            aggregateIndices.push_back(indices[indexPosition]);
            selectedType = arrayType->getElementType();
            ++indexPosition;
            continue;
        }
        if (IRType::asVector(selectedType) != nullptr &&
            vectorIndex == nullptr) {
            vectorIndex = indices[indexPosition];
            ++indexPosition;
            continue;
        }
        return false;
    }
    return true;
}

IRGenerator::ResolvedLValue IRGenerator::lowerLValue(const LVal& node) {
    // 第一阶段解析符号和所有下标，随后按绑定种类决定返回值还是构造元素地址。
    const SymbolTable::ObjectBinding* binding =
        symbols.lookupObject(node.getIdent());
    if (binding == nullptr || binding->value == nullptr ||
        binding->type == nullptr) {
        return {};
    }
    Value* symbol = binding->value;

    std::vector<Value*> indices;
    if (node.hasArray()) {
        indices.reserve(node.getArray()->getExpressions().size());
        for (const auto& expression : node.getArray()->getExpressions()) {
            Value* index = lowerExpression(*expression);
            if (index == nullptr || index->getIRType() == nullptr ||
                index->getIRType()->getTypeSystem() != IR_INT) {
                return {};
            }
            Value* converted =
                BasicBlock::coerceValueToTypeWithoutBitCast(
                    currentBlock, index, INTType::getInt32Ty());
            if (converted == nullptr)
                return {};
            indices.push_back(converted);
        }
    }

    const bool isArrayParameter =
        binding->kind == SymbolTable::ObjectKind::ArrayParameter;
    std::vector<Value*> aggregateIndices;
    std::shared_ptr<IRType> selectedType;
    Value* vectorIndex = nullptr;
    if (!classifyIndexedAccess(
            binding->type, isArrayParameter, indices,
            aggregateIndices, selectedType, vectorIndex)) {
        return {};
    }
    const auto selectedVector = IRType::asVector(selectedType);
    if (vectorIndex != nullptr && selectedVector == nullptr)
        return {};

    if (binding->kind == SymbolTable::ObjectKind::Constant) {
        if (!aggregateIndices.empty())
            return {};
        if (vectorIndex != nullptr) {
            return ResolvedLValue{
                symbol, selectedVector->getElementType(), false, false,
                vectorIndex, selectedVector};
        }
        return ResolvedLValue{symbol, binding->type, false, false};
    }

    if (!aggregateIndices.empty()) {
        if (Value* constant = tryLowerConstantArrayElement(
                *binding, aggregateIndices)) {
            if (vectorIndex != nullptr) {
                return ResolvedLValue{
                    constant, selectedVector->getElementType(), false,
                    false, vectorIndex, selectedVector};
            }
            return ResolvedLValue{
                constant, selectedType, false, false};
        }
    }

    // 数组形参保存为“指针的栈槽”，先读取源码层数组指针，再按下标构造元素地址。
    if (isArrayParameter) {
        if (currentBlock == nullptr)
            return {};
        Value* parameterPointer = currentBlock->genLoadInst(symbol);
        if (parameterPointer == nullptr)
            return {};
        if (aggregateIndices.empty())
            return ResolvedLValue{
                parameterPointer,
                binding->type,
                false,
                binding->isMutable,
            };

        auto* address = dynamic_cast<GetElementPtrInst*>(
            currentBlock->genGepInst(parameterPointer));
        if (address == nullptr)
            return {};
        for (Value* index : aggregateIndices)
            address->addUse(index);
        address->updateType();
        const auto resultType =
            std::dynamic_pointer_cast<POINTERType>(address->getIRType());
        if (resultType == nullptr || resultType->getPointerType() == nullptr)
            return {};
        if (vectorIndex != nullptr) {
            return ResolvedLValue{
                address, selectedVector->getElementType(), true,
                binding->isMutable, vectorIndex, selectedVector};
        }
        return ResolvedLValue{
            address,
            selectedType,
            true,
            binding->isMutable,
        };
    }

    if (binding->kind != SymbolTable::ObjectKind::Address)
        return {};
    Value* address = symbol;
    if (!aggregateIndices.empty()) {
        if (currentBlock == nullptr)
            return {};
        auto* aggregateAddress = dynamic_cast<GetElementPtrInst*>(
            currentBlock->genGepInst(symbol));
        if (aggregateAddress == nullptr)
            return {};
        aggregateAddress->addUse(ConstInt::newConstInt(0));
        for (Value* index : aggregateIndices)
            aggregateAddress->addUse(index);
        aggregateAddress->updateType();
        const auto resultType = std::dynamic_pointer_cast<POINTERType>(
            aggregateAddress->getIRType());
        if (resultType == nullptr || resultType->getPointerType() == nullptr)
            return {};
        address = aggregateAddress;
    }

    if (vectorIndex != nullptr) {
        return ResolvedLValue{
            address, selectedVector->getElementType(), true,
            binding->isMutable, vectorIndex, selectedVector};
    }
    return ResolvedLValue{
        address,
        selectedType,
        true,
        binding->isMutable,
    };
}

Value* IRGenerator::decayArrayPointer(Value* pointer) {
    // 普通数组作为右值时只去掉最外层数组维度，对应 GEP [0, 0]。
    if (pointer == nullptr || pointer->getIRType() == nullptr ||
        currentBlock == nullptr) {
        return nullptr;
    }
    auto pointerType = std::dynamic_pointer_cast<POINTERType>(pointer->getIRType());
    if (pointerType == nullptr || pointerType->getPointerType() == nullptr ||
        pointerType->getPointerType()->getTypeSystem() != IR_ARRAY) {
        return nullptr;
    }

    auto* decay = dynamic_cast<GetElementPtrInst*>(
        currentBlock->genGepInst(pointer));
    if (decay == nullptr)
        return nullptr;
    decay->addUse(ConstInt::newConstInt(0));
    decay->addUse(ConstInt::newConstInt(0));
    decay->updateType();
    return decay;
}

Value* IRGenerator::lowerRValue(const LVal& node) {
    ResolvedLValue reference = lowerLValue(node);
    if (reference.value == nullptr || reference.type == nullptr)
        return nullptr;

    if (reference.isVectorLane()) {
        Value* vectorValue = reference.value;
        if (reference.isAddress) {
            if (currentBlock == nullptr)
                return nullptr;
            vectorValue = currentBlock->genLoadInst(reference.value);
        }
        if (vectorValue == nullptr)
            return nullptr;

        std::int32_t constantIndex = 0;
        if (auto* constantVector =
                dynamic_cast<ConstantVector*>(vectorValue);
            constantVector != nullptr &&
            reference.vectorIndex->tryGetIntConst(constantIndex) &&
            constantIndex >= 0 &&
            static_cast<std::size_t>(constantIndex) <
                constantVector->getNumElements()) {
            return constantVector->getElement(
                static_cast<std::size_t>(constantIndex));
        }
        if (currentBlock == nullptr)
            return nullptr;
        return currentBlock->genExtractElementInst(
            vectorValue, reference.vectorIndex);
    }

    if (!reference.isAddress)
        return reference.value;
    if (reference.type->getTypeSystem() == IR_ARRAY)
        return decayArrayPointer(reference.value);
    if (currentBlock == nullptr)
        return nullptr;
    return currentBlock->genLoadInst(reference.value);
}

void IRGenerator::emitCondition(const LAndExp& node, BasicBlock* entry,
                                BasicBlock* isTrue, BasicBlock* isFalse) {
    // 逻辑与仅在当前项为真时进入下一项，任一项为假都直接跳到假分支。
    if (entry == nullptr || isTrue == nullptr || isFalse == nullptr)
        return;
    const auto& operands = node.getOperands();
    if (operands.empty()) {
        entry->genUnCondInst(isFalse);
        return;
    }

    BasicBlock* block = entry;
    for (auto operand = operands.begin(); operand != operands.end(); ++operand) {
        currentBlock = block;
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
    // 逻辑或任一项为真即跳到真分支，只有当前项为假才继续计算下一项。
    if (entry == nullptr || isTrue == nullptr || isFalse == nullptr)
        return;
    const auto& operands = node.getOperands();
    if (operands.empty()) {
        entry->genUnCondInst(isFalse);
        return;
    }

    BasicBlock* block = entry;
    for (auto operand = operands.begin(); operand != operands.end(); ++operand) {
        currentBlock = block;
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
    BasicBlock* entry = currentBlock;
    if (entry == nullptr || entry->hasTerminator())
        return;

    BasicBlock* isTrue = entry->genBlock();
    BasicBlock* isFalse = entry->genBlock();
    BasicBlock* continuation = entry->genBlock();
    if (isTrue == nullptr || isFalse == nullptr || continuation == nullptr)
        return;
    emitCondition(node, entry, isTrue, isFalse);

    currentBlock = isTrue;
    currentBlock->genUnCondInst(continuation);
    currentBlock = isFalse;
    currentBlock->genUnCondInst(continuation);
    currentBlock = continuation;
}

//=========从这里开始进入IR生成核心逻辑===========

//IR生成1：CompUnit
void IRGenerator::visit(CompUnit& node) {
    if (currentFunction != nullptr || currentBlock != nullptr)
        return;
    //下一层：ConstDecl、VarDecl、FuncDef
    for (const auto& item : node.getItems())
        item->accept(*this);
}
//IR生成2：ConstDecl全局常量/局部常量
void IRGenerator::visit(ConstDecl& node) {
    const auto declaredType = lowerSourceType(node.getType());
    if (declaredType == nullptr)
        return;
    if (node.getConstDefList() == nullptr)
        return;
    for (const auto& definition : node.getConstDefList()->getDefs())
        lowerDefinition(*definition, declaredType, true);
}
//IR生成3：VarDecl全局变量/局部变量
void IRGenerator::visit(VarDecl& node) {
    const auto declaredType = lowerSourceType(node.getType());
    if (declaredType == nullptr)
        return;
    if (node.getVarDefList() == nullptr)
        return;
    for (const auto& definition : node.getVarDefList()->getDefs())
        lowerDefinition(*definition, declaredType, false);
}
void IRGenerator::visit(ConstDefList& node) {(void)node;}
void IRGenerator::visit(VarDefList& node) {(void)node;}
void IRGenerator::visit(ConstDef& node) {(void)node;}
void IRGenerator::visit(VarDef& node) {(void)node;}
/*全局变量和全局常量和局部变量和局部常量的生成集成到lowerDefinition中*/

//IR生成4：FuncDef
void IRGenerator::visit(FuncDef& node) {
    //1.获得函数名
    const std::string& functionName = node.getIdent();
    if (currentFunction != nullptr || functionName.empty() ||
        symbols.lookupFunction(functionName) != nullptr) {
        return;
    }
    //2.获得函数返回类型并映射
    const auto returnType = lowerSourceType(node.getType(), true);
    if (returnType == nullptr)
        return;
    if (node.getParams() != nullptr) {
        for (const auto& parameter : node.getParams()->getParams()) {
            if (parameter == nullptr ||
                lowerSourceType(parameter->getType()) == nullptr) {
                return;
            }
        }
    }

    //3.创建函数IR
    Function& function = ModuleIR->newFunction(returnType, functionName);
    //4.SymbolTable注册函数
    if (!symbols.declareFunction(functionName, &function))
        return;
    if (function.front() == nullptr)
        return;
    //4.保存目前函数和目前基本块状态
    currentFunction = &function;
    currentBlock = function.front();

    //5.SymbolTable进入函数作用域
    symbols.enterScope();
    //6.处理函数形参
    if (node.getParams() != nullptr) {
        for (const auto& parameter : node.getParams()->getParams())
            lowerParameter(*parameter);
    }
    //7.处理函数体
    if (node.getFuncBody() != nullptr)
        node.getFuncBody()->accept(*this);

    //8.函数收尾：若函数最后的基本块没有终结指令，则根据函数返回类型生成默认返回值
    //void 返回 ret void，main 返回 0，其他非 void 函数返回 undef
    if (currentBlock != nullptr && !currentBlock->hasTerminator()) {
        if (function.getIRType()->getTypeSystem() == IR_VOID) {
            currentBlock->genRetInst();
        } else if (function.getIdent() == "main") {
            currentBlock->genRetInst(ConstInt::newConstInt(0));
        } else {
            currentBlock->genRetInst(
                UndefValue::NewUndefValue(function.getIRType()));
        }
    }

    //9.退出函数作用域
    symbols.exitScope();
    //10.退出目前函数和基本块状态
    currentFunction = nullptr;
    currentBlock = nullptr;
}
//IR生成5：函数形参
void IRGenerator::visit(FuncParam& node) {lowerParameter(node);}
void IRGenerator::visit(FuncParamList& node) {
    if (currentFunction == nullptr)
        return;
    for (const auto& parameter : node.getParams())
        lowerParameter(*parameter);
}
void IRGenerator::visit(InitValList& node) {(void)node;}
void IRGenerator::visit(InitVal& node) {(void)node;}
/*函数形参的生成集成到lowerParameter中*/

//IR生成6：函数体，基本块级
//Block->BlockItemList
//->ConstDecl局部常量/VarDecl局部变量/Block嵌套基本块/Stmt语句
/*局部变量和局部常量的生成集成到lowerDefinition中*/
void IRGenerator::visit(Block& node) {
    symbols.enterScope();
    if (node.getItems() != nullptr)
        node.getItems()->accept(*this);
    symbols.exitScope();
}
void IRGenerator::visit(BlockItemList& node) {
    if (currentBlock == nullptr)
        return;
    for (const auto& item : node.getItems()) {
        if (currentBlock->hasTerminator())
            return;
        item->accept(*this);
    }
}

/*读到这里：
全局变量/全局常量/局部变量/局部常量IR生成集成在lowerDefinition中
函数形参IR生成集成在lowerParameter中
*/


//IR生成7：语句级
//AssignStmt-> LVal = Exp
//右值IR生成->左值IR生成->类型转换->生成Store指令
void IRGenerator::visit(AssignStmt& node) {
    if (currentBlock == nullptr || node.getExp() == nullptr ||
        node.getLVal() == nullptr) {
        return;
    }
    //1.生成右值表达式的IR
    Value* right = lowerExpression(*node.getExp());
    //2.生成左值表达式的IR
    ResolvedLValue left = lowerLValue(*node.getLVal());
    if (right == nullptr || left.value == nullptr || left.type == nullptr ||
        !left.isAddress || !left.isMutable) {
        return;
    }

    if (left.isVectorLane()) {
        Value* convertedLane =
            BasicBlock::coerceValueToTypeWithoutBitCast(
                currentBlock, right, left.type);
        Value* vectorValue = currentBlock->genLoadInst(left.value);
        if (convertedLane == nullptr || vectorValue == nullptr)
            return;
        Value* updatedVector = currentBlock->genInsertElementInst(
            vectorValue, convertedLane, left.vectorIndex);
        if (updatedVector != nullptr)
            currentBlock->genStoreInst(updatedVector, left.value);
        return;
    }
    if (left.type->getTypeSystem() == IR_ARRAY)
        return;
    //3.类型转换，保证右值类型与左值类型一致
    Value* converted =
        BasicBlock::coerceValueToTypeWithoutBitCast(
            currentBlock, right, left.type);
    //4.生成IR Store指令，将右值存入左值地址
    if (converted != nullptr)
        currentBlock->genStoreInst(converted, left.value);
}

//ExpStmt：表达式语句
void IRGenerator::visit(ExpStmt& node) {
    if (node.getExp() != nullptr)
        (void)lowerExpression(*node.getExp());
}

//IfStmt:
/*
没有 else:
                  true
            ┌─────────────► trueBlock
            │                   │
conditionBlock                  │
            │                   ▼
            └─────────────► continuation
                  false
带 else:
                       ┌──► trueBlock ──┐
conditionBlock ────────┤                ├──► continuation
                       └──► falseBlock ─┘
*/
void IRGenerator::visit(IfStmt& node) {
    // 条件先分流到真/假块，两个未终结分支再按需创建并连接同一继续块。
    BasicBlock* conditionBlock = currentBlock;
    if (conditionBlock == nullptr || node.getCondition() == nullptr ||
        node.getThenBranch() == nullptr) {
        return;
    }
    BasicBlock* trueBlock = conditionBlock->genBlock();
    if (trueBlock == nullptr)
        return;
    BasicBlock* falseBlock = nullptr;
    BasicBlock* continuation = nullptr;

    if (node.hasElse()) {
        falseBlock = conditionBlock->genBlock();
        if (falseBlock == nullptr)
            return;
        emitCondition(*node.getCondition(), conditionBlock, trueBlock,
                      falseBlock);
    } else {
        continuation = conditionBlock->genBlock();
        if (continuation == nullptr)
            return;
        emitCondition(*node.getCondition(), conditionBlock, trueBlock,
                      continuation);
    }

    currentBlock = trueBlock;
    node.getThenBranch()->accept(*this);
    if (!currentBlock->hasTerminator()) {
        if (continuation == nullptr)
            continuation = currentBlock->genBlock();
        if (continuation != nullptr)
            currentBlock->genUnCondInst(continuation);
    }

    if (falseBlock != nullptr) {
        currentBlock = falseBlock;
        if (node.getElseBranch() != nullptr)
            node.getElseBranch()->accept(*this);
        if (!currentBlock->hasTerminator()) {
            if (continuation == nullptr)
                continuation = currentBlock->genBlock();
            if (continuation != nullptr)
                currentBlock->genUnCondInst(continuation);
        }
    }

    if (continuation != nullptr)
        currentBlock = continuation;
}
//WhileStmt
/*
                 ┌──────────────────────┐
                 │                      │
                 ▼                      │
predecessor ──► conditionBlock ──true──► bodyBlock
                    │
                    false
                    │
                    ▼
               continuation
*/
void IRGenerator::visit(WhileStmt& node) {
    // 循环由前驱、条件、循环体和继续块组成，循环目标栈为 break/continue 提供落点。
    BasicBlock* predecessor = currentBlock;
    if (predecessor == nullptr || node.getCondition() == nullptr ||
        node.getBody() == nullptr) {
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
    emitCondition(*node.getCondition(), conditionBlock, bodyBlock,
                  continuation);

    loopStack.push_back(LoopTargets{conditionBlock, continuation});
    currentBlock = bodyBlock;
    node.getBody()->accept(*this);
    if (!currentBlock->hasTerminator())
        currentBlock->genUnCondInst(conditionBlock);

    loopStack.pop_back();
    currentBlock = continuation;
}

//BreakStmt
//当前块 -> 循环 continuation
void IRGenerator::visit(BreakStmt& node) {
    (void)node;
    if (currentBlock == nullptr || loopStack.empty())
        return;
    BasicBlock* target = loopStack.back().breakTarget;
    if (target != nullptr)
        currentBlock->genUnCondInst(target);
}
//ContinueStmt
//当前块 -> 循环 condition
void IRGenerator::visit(ContinueStmt& node) {
    (void)node;
    if (currentBlock == nullptr || loopStack.empty())
        return;
    BasicBlock* target = loopStack.back().continueTarget;
    if (target != nullptr)
        currentBlock->genUnCondInst(target);
}

//ReturnStmt->Exp?
//生成ret指令+类型转换（可选）
void IRGenerator::visit(ReturnStmt& node) {
    if (currentFunction == nullptr || currentBlock == nullptr)
        return;
    //获得当前函数的返回类型
    const auto returnType = currentFunction->getIRType();
    if (returnType == nullptr)
        return;
    //如果没有返回值
    if (!node.hasReturnValue()) {
        if (returnType->getTypeSystem() == IR_VOID)
            //生成ret void指令
            currentBlock->genRetInst();
        else
            //生成ret undef指令
            currentBlock->genRetInst(UndefValue::NewUndefValue(returnType));
        return;
    }
    //如果有返回值，先生成返回值表达式的IR，再进行类型转换，最后生成ret指令
    Value* value = lowerExpression(*node.getReturnValue());
    if (returnType->getTypeSystem() == IR_VOID) {
        currentBlock->genRetInst();
        return;
    }
    //类型转换
    Value* converted =
        BasicBlock::coerceValueToTypeWithoutBitCast(
            currentBlock, value, returnType);
    if (converted == nullptr)
        converted = UndefValue::NewUndefValue(returnType);
    //生成ret指令
    currentBlock->genRetInst(converted);
}
/*
普通值表达式的 IR 生成集中在 lowerExpression 系列函数中。
LVal 的统一解析集中在 lowerLValue 中：既可以返回常量值，也可以返回变量、数组元素的地址。
LVal 作为右值使用时集中在 lowerRValue 中：常量直接返回，标量地址生成 load，数组地址执行退化。
LAndExp 和 LOrExp 的短路控制流生成集中在 emitCondition 中。

emitDiscardedCondition 仅负责：
当 LAndExp/LOrExp 作为独立表达式、结果被丢弃时，
创建真块、假块和继续块，并调用 emitCondition 完成短路计算。
*/

// 这些入口仅满足通用访问器接口；内部生成流程始终通过显式返回值取得结果。
void IRGenerator::visit(LVal& node) { (void)lowerRValue(node); }
void IRGenerator::visit(FuncCall& node) { (void)lowerExpression(node); }

void IRGenerator::visit(ArrayList& node) {
    for (const auto& expression : node.getExpressions())
        (void)lowerExpression(*expression);
}

void IRGenerator::visit(FuncRParamList& node) {
    for (const auto& parameter : node.getParams())
        (void)lowerExpression(*parameter);
}

void IRGenerator::visit(UnaryExp& node) { (void)lowerExpression(node); }
void IRGenerator::visit(MulExp& node) { (void)lowerExpression(node); }
void IRGenerator::visit(AddExp& node) { (void)lowerExpression(node); }
void IRGenerator::visit(RelExp& node) { (void)lowerExpression(node); }
void IRGenerator::visit(EqExp& node) { (void)lowerExpression(node); }
void IRGenerator::visit(LAndExp& node) { emitDiscardedCondition(node); }
void IRGenerator::visit(LOrExp& node) { emitDiscardedCondition(node); }
void IRGenerator::visit(ConValue<int>& node) {
    (void)ConstInt::newConstInt(node.getValue());
}

void IRGenerator::visit(ConValue<float>& node) {
    (void)ConstFloat::newConstFloat(node.getValue());
}
