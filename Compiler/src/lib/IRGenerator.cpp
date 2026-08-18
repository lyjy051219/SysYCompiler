#include "../../include/lib/IRGenerator.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

extern std::unique_ptr<Module> ModuleIR;

std::shared_ptr<IRType> IRGenerator::lowerScalarKind(
    ASTScalarKind kind) const {
    switch (kind) {
    case ASTScalarKind::Int32:
        return INTType::getInt32Ty();
    case ASTScalarKind::Float32:
        return FLOATType::NewFloat();
    case ASTScalarKind::Void:
        return VOIDType::NewVoid();
    default:
        return nullptr;
    }
}

std::shared_ptr<IRType> IRGenerator::makeTensorStorageType(
    const std::shared_ptr<IRType>& elementType,
    const std::vector<std::size_t>& shape) const {
    if (elementType == nullptr || shape.empty())
        return nullptr;
    std::shared_ptr<IRType> result = elementType;
    for (auto dimension = shape.rbegin(); dimension != shape.rend();
         ++dimension) {
        if (*dimension == 0 ||
            *dimension > static_cast<std::size_t>(
                             std::numeric_limits<int>::max())) {
            return nullptr;
        }
        result = ARRAYType::NewArray(result, static_cast<int>(*dimension));
        if (result == nullptr)
            return nullptr;
    }
    return result;
}

std::size_t IRGenerator::tensorElementCount(
    const std::vector<std::size_t>& shape) const {
    if (shape.empty())
        return 0;
    std::size_t count = 1;
    for (std::size_t dimension : shape) {
        if (dimension == 0 ||
            count > std::numeric_limits<std::size_t>::max() / dimension) {
            return 0;
        }
        count *= dimension;
    }
    return count;
}

ASTScalarKind IRGenerator::sourceScalarKind(
    const std::shared_ptr<IRType>& type) const {
    if (type == nullptr)
        return ASTScalarKind::Invalid;
    if (type->getTypeSystem() == IR_INT)
        return ASTScalarKind::Int32;
    if (type->getTypeSystem() == IR_FLOAT)
        return ASTScalarKind::Float32;
    if (type->getTypeSystem() == IR_VOID)
        return ASTScalarKind::Void;
    return ASTScalarKind::Invalid;
}

[[noreturn]] void IRGenerator::reportTensorError(
    const std::string& message) const {
    throw std::runtime_error("tensor 语义错误：" + message);
}

bool IRGenerator::lowerStaticShape(
    const ArrayList& dimensions, std::vector<std::size_t>& shape) {
    shape.clear();
    for (const auto& expression : dimensions.getExpressions()) {
        if (expression == nullptr || !isConstantExpression(*expression))
            reportTensorError("tensor 的维度必须是整型常量表达式");
        Value* value = lowerExpression(*expression);
        std::int32_t count = 0;
        if (value == nullptr || !value->tryGetIntConst(count))
            reportTensorError("tensor 的维度必须能在编译期求值");
        if (count <= 0)
            reportTensorError("tensor 的每一维都必须大于零");
        shape.push_back(static_cast<std::size_t>(count));
    }
    if (shape.empty())
        reportTensorError("tensor 对象声明必须给出至少一维");
    if (tensorElementCount(shape) == 0)
        reportTensorError("tensor 的元素总数溢出");
    return true;
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

    return type.isScalar() ? elementType : nullptr;
}

Value* IRGenerator::registerTensor(
    Value* address, ASTScalarKind elementKind,
    const std::vector<std::size_t>& shape, bool hasFlatAddress,
    bool isTemporary) {
    if (address == nullptr || shape.empty())
        return nullptr;
    const auto elementType = lowerScalarKind(elementKind);
    if (elementType == nullptr || elementType->isVoid())
        return nullptr;
    tensorValues[address] = TensorValue{
        address, elementType, elementKind, shape, hasFlatAddress,
        isTemporary};
    return address;
}

const IRGenerator::TensorValue* IRGenerator::lookupTensor(
    Value* value) const {
    const auto found = tensorValues.find(value);
    return found == tensorValues.end() ? nullptr : &found->second;
}

Value* IRGenerator::allocateTensor(
    ASTScalarKind elementKind, const std::vector<std::size_t>& shape) {
    if (currentBlock == nullptr)
        reportTensorError("tensor 表达式不能出现在全局运行期初始化中");
    const auto storageType = makeTensorStorageType(
        lowerScalarKind(elementKind), shape);
    if (storageType == nullptr)
        reportTensorError("无法构造 tensor 的中端存储类型");
    Value* storage = currentBlock->genAllocaInst(storageType);
    if (storage == nullptr)
        reportTensorError("无法为 tensor 临时值分配存储");
    return registerTensor(storage, elementKind, shape, false, true);
}

Value* IRGenerator::tensorElementAddress(
    const TensorValue& value, std::size_t flatIndex) {
    const std::size_t count = tensorElementCount(value.shape);
    if (currentBlock == nullptr || value.address == nullptr ||
        flatIndex >= count) {
        return nullptr;
    }

    auto* address = dynamic_cast<GetElementPtrInst*>(
        currentBlock->genGepInst(value.address));
    if (address == nullptr)
        return nullptr;
    if (value.hasFlatAddress) {
        address->addUse(ConstInt::newConstInt(
            static_cast<std::int64_t>(flatIndex)));
    } else {
        address->addUse(ConstInt::newConstInt(0));
        std::size_t remainder = flatIndex;
        for (std::size_t index = 0; index < value.shape.size(); ++index) {
            std::size_t stride = 1;
            for (std::size_t nested = index + 1;
                 nested < value.shape.size(); ++nested) {
                stride *= value.shape[nested];
            }
            const std::size_t coordinate = remainder / stride;
            remainder %= stride;
            address->addUse(ConstInt::newConstInt(
                static_cast<std::int64_t>(coordinate)));
        }
    }
    address->updateType();
    const auto pointerType = std::dynamic_pointer_cast<POINTERType>(
        address->getIRType());
    if (pointerType == nullptr ||
        !IRType::hasSameShape(pointerType->getPointerType(),
                              value.elementType)) {
        return nullptr;
    }
    return address;
}

Value* IRGenerator::tensorElementAddress(
    const TensorValue& value, Value* flatIndex) {
    if (currentBlock == nullptr || value.address == nullptr ||
        flatIndex == nullptr || !value.hasFlatAddress ||
        flatIndex->getIRType() == nullptr ||
        !flatIndex->getIRType()->isInt()) {
        return nullptr;
    }

    auto* address = dynamic_cast<GetElementPtrInst*>(
        currentBlock->genGepInst(value.address));
    if (address == nullptr)
        return nullptr;
    address->addUse(flatIndex);
    address->updateType();
    const auto pointerType = std::dynamic_pointer_cast<POINTERType>(
        address->getIRType());
    if (pointerType == nullptr ||
        !IRType::hasSameShape(pointerType->getPointerType(),
                              value.elementType)) {
        return nullptr;
    }
    return address;
}

Value* IRGenerator::tensorIndexedAddress(
    const TensorValue& value, const std::vector<Value*>& indices,
    std::size_t consumedDimensions) {
    if (currentBlock == nullptr || value.address == nullptr ||
        consumedDimensions > indices.size() ||
        consumedDimensions > value.shape.size()) {
        return nullptr;
    }
    if (consumedDimensions == 0)
        return value.address;

    if (!value.hasFlatAddress) {
        auto* address = dynamic_cast<GetElementPtrInst*>(
            currentBlock->genGepInst(value.address));
        if (address == nullptr)
            return nullptr;
        address->addUse(ConstInt::newConstInt(0));
        for (std::size_t index = 0; index < consumedDimensions; ++index)
            address->addUse(indices[index]);
        address->updateType();
        return address;
    }

    Value* offset = ConstInt::newConstInt(0);
    for (std::size_t index = 0; index < consumedDimensions; ++index) {
        std::size_t stride = 1;
        for (std::size_t nested = index + 1;
             nested < value.shape.size(); ++nested) {
            stride *= value.shape[nested];
        }
        Value* term = indices[index];
        if (stride != 1) {
            term = emitBinary(
                term, BinaryInst::MUL,
                ConstInt::newConstInt(static_cast<std::int64_t>(stride)));
        }
        offset = emitBinary(offset, BinaryInst::ADD, term);
        if (offset == nullptr)
            return nullptr;
    }
    auto* address = dynamic_cast<GetElementPtrInst*>(
        currentBlock->genGepInst(value.address));
    if (address == nullptr)
        return nullptr;
    address->addUse(offset);
    address->updateType();
    return address;
}

Value* IRGenerator::tensorFlatAddress(
    const TensorValue& value) {
    if (value.hasFlatAddress)
        return value.address;
    if (currentBlock == nullptr || value.address == nullptr)
        return nullptr;
    auto* address = dynamic_cast<GetElementPtrInst*>(
        currentBlock->genGepInst(value.address));
    if (address == nullptr)
        return nullptr;
    address->addUse(ConstInt::newConstInt(0));
    for (std::size_t index = 0; index < value.shape.size(); ++index)
        address->addUse(ConstInt::newConstInt(0));
    address->updateType();
    const auto pointerType = std::dynamic_pointer_cast<POINTERType>(
        address->getIRType());
    if (pointerType == nullptr ||
        !IRType::hasSameShape(pointerType->getPointerType(),
                              value.elementType)) {
        return nullptr;
    }
    return address;
}

IRGenerator::TensorValue IRGenerator::makeFlatTensorValue(
    const TensorValue& value) {
    TensorValue result = value;
    result.address = tensorFlatAddress(value);
    result.hasFlatAddress = result.address != nullptr;
    return result;
}

Value* IRGenerator::tensorStorageRoot(Value* address) const {
    Value* current = address;
    while (current != nullptr) {
        if (auto* gep = dynamic_cast<GetElementPtrInst*>(current)) {
            if (gep->getUserList().empty())
                return current;
            current = gep->getUserList().front()->getValue();
            continue;
        }
        if (auto* bitcast = dynamic_cast<BitCastInst*>(current)) {
            if (bitcast->getUserList().empty())
                return current;
            current = bitcast->getUserList().front()->getValue();
            continue;
        }
        return current;
    }
    return nullptr;
}

bool IRGenerator::canWriteTensorResultDirectly(
    const TensorValue& destination,
    const std::vector<TensorValue>& inputs,
    bool allowExactInPlace) const {
    if (destination.address == nullptr)
        return false;

    // 临时对象和编译器隐藏返回槽由调用约定保证不与输入重叠。
    if (destination.isTemporary)
        return true;

    Value* destinationRoot = tensorStorageRoot(destination.address);
    const bool destinationIsLocal =
        dynamic_cast<AllocaInst*>(destinationRoot) != nullptr;
    const bool destinationIsGlobal =
        dynamic_cast<Variable*>(destinationRoot) != nullptr;
    if (!destinationIsLocal && !destinationIsGlobal)
        return false;

    for (const TensorValue& input : inputs) {
        if (input.address == nullptr)
            return false;
        if (input.address == destination.address) {
            if (allowExactInPlace)
                continue;
            return false;
        }

        Value* inputRoot = tensorStorageRoot(input.address);
        if (inputRoot == destinationRoot)
            return false;

        // 当前函数的局部 alloca 不可能与形参、全局对象或另一个 alloca
        // 指向同一存储；全局目标遇到未知来源指针时仍保持保守。
        if (destinationIsLocal)
            continue;
        if (dynamic_cast<AllocaInst*>(inputRoot) == nullptr &&
            dynamic_cast<Variable*>(inputRoot) == nullptr) {
            return false;
        }
    }
    return true;
}

Value* IRGenerator::selectTensorResult(
    TensorValue* destination,
    ASTScalarKind elementKind,
    const std::vector<std::size_t>& shape,
    const std::vector<TensorValue>& inputs,
    bool allowExactInPlace) {
    const auto elementType = lowerScalarKind(elementKind);
    const bool shapeMatches =
        destination != nullptr &&
        (destination->shape.empty() || destination->shape == shape);
    const bool typeMatches =
        destination != nullptr &&
        (destination->elementKind == ASTScalarKind::Invalid ||
         destination->elementKind == elementKind);

    if (destination != nullptr && destination->address != nullptr &&
        shapeMatches && typeMatches &&
        canWriteTensorResultDirectly(
            *destination, inputs, allowExactInPlace)) {
        destination->elementType = elementType;
        destination->elementKind = elementKind;
        destination->shape = shape;
        if (registerTensor(
                destination->address, elementKind, shape,
                destination->hasFlatAddress,
                destination->isTemporary) == nullptr) {
            reportTensorError("无法登记 tensor 目的存储");
        }
        return destination->address;
    }
    return allocateTensor(elementKind, shape);
}

Value* IRGenerator::materializeTensorResult(
    Value* source, TensorValue* destination) {
    if (destination == nullptr)
        return source;
    const TensorValue* sourceLookup = lookupTensor(source);
    if (sourceLookup == nullptr)
        return source;
    const TensorValue sourceValue = *sourceLookup;
    if (destination->address == nullptr)
        reportTensorError("tensor 目的地址无效");
    if (destination->elementKind != ASTScalarKind::Invalid &&
        destination->elementKind != sourceValue.elementKind) {
        reportTensorError("tensor 目的元素类型与表达式不匹配");
    }
    if (!destination->shape.empty() &&
        destination->shape != sourceValue.shape) {
        reportTensorError("tensor 目的形状与表达式不匹配");
    }

    destination->elementType = sourceValue.elementType;
    destination->elementKind = sourceValue.elementKind;
    destination->shape = sourceValue.shape;
    if (registerTensor(
            destination->address, destination->elementKind,
            destination->shape, destination->hasFlatAddress,
            destination->isTemporary) == nullptr) {
        reportTensorError("无法登记 tensor 表达式目的存储");
    }
    const TensorValue destinationValue = *lookupTensor(
        destination->address);
    if (!copyTensor(destinationValue, sourceValue))
        reportTensorError("tensor 表达式写入目的存储失败");
    return destination->address;
}

IRGenerator::TensorCountedLoop IRGenerator::beginTensorCountedLoop(
    std::size_t tripCount, const std::string& label,
    bool headerTested) {
    if (currentBlock == nullptr || tripCount == 0 ||
        tripCount > static_cast<std::size_t>(
                        std::numeric_limits<std::int64_t>::max()))
        reportTensorError("tensor 循环的静态迭代次数无法用 i64 表示");

    TensorCountedLoop loop;
    loop.preheader = currentBlock;
    loop.headerTested = headerTested;
    loop.tripCount = tripCount;
    loop.label = label;

    const std::shared_ptr<INTType> indexType =
        tripCount <= static_cast<std::size_t>(
                         std::numeric_limits<std::int32_t>::max())
            ? INTType::getInt32Ty()
            : INTType::getInt64Ty();
    loop.index = new PhiInst(indexType);

    if (!headerTested) {
        loop.body = currentBlock->genBlock(label + ".body");
        loop.header = loop.body;
        if (loop.body == nullptr)
            reportTensorError("无法创建 tensor 尾测循环基本块");
        loop.body->pushFront(loop.index);
        loop.index->addIncoming(
            ConstInt::newConstIntForType(0, indexType), loop.preheader);
        loop.preheader->genUnCondInst(loop.body);
        currentBlock = loop.body;
        return loop;
    }

    loop.header = currentBlock->genBlock(label + ".header");
    loop.body = currentBlock->genBlock(label + ".body");
    loop.exit = currentBlock->genBlock(label + ".exit");
    if (loop.header == nullptr || loop.body == nullptr ||
        loop.exit == nullptr) {
        reportTensorError("无法创建 tensor 规范循环基本块");
    }
    loop.header->pushFront(loop.index);
    loop.index->addIncoming(
        ConstInt::newConstIntForType(0, indexType), loop.preheader);
    loop.preheader->genUnCondInst(loop.header);

    currentBlock = loop.header;
    Value* enterLoop = emitBinary(
        loop.index, BinaryInst::L,
        ConstInt::newConstIntForType(
            static_cast<std::int64_t>(loop.tripCount), indexType));
    if (enterLoop == nullptr)
        reportTensorError("tensor 规范循环的入口条件生成失败");
    loop.header->genCondInst(enterLoop, loop.body, loop.exit);
    currentBlock = loop.body;
    return loop;
}

void IRGenerator::endTensorCountedLoop(
    const TensorCountedLoop& loop) {
    BasicBlock* latch = currentBlock;
    if (latch == nullptr || loop.header == nullptr || loop.body == nullptr ||
        loop.index == nullptr || latch->hasTerminator() ||
        (loop.headerTested && loop.exit == nullptr)) {
        reportTensorError("tensor 规范循环的回边不合法");
    }
    const auto indexType = std::dynamic_pointer_cast<INTType>(
        loop.index->getIRType());
    if (indexType == nullptr)
        reportTensorError("tensor 规范循环的归纳变量类型无效");

    Value* next = emitBinary(
        loop.index, BinaryInst::ADD,
        ConstInt::newConstIntForType(1, indexType));
    if (next == nullptr)
        reportTensorError("tensor 规范循环的归纳变量生成失败");
    loop.index->addIncoming(next, latch);
    if (loop.headerTested) {
        latch->genUnCondInst(loop.header);
        currentBlock = loop.exit;
        return;
    }

    BasicBlock* exit = latch->genBlock(loop.label + ".exit");
    Value* continueLoop = emitBinary(
        next, BinaryInst::L,
        ConstInt::newConstIntForType(
            static_cast<std::int64_t>(loop.tripCount), indexType));
    if (exit == nullptr || continueLoop == nullptr)
        reportTensorError("tensor 尾测循环的出口生成失败");
    latch->genCondInst(continueLoop, loop.body, exit);
    currentBlock = exit;
}

void IRGenerator::emitTensorLoopNest(
    const std::vector<std::size_t>& shape,
    std::size_t depth,
    std::vector<Value*>& coordinates,
    const std::string& label,
    const std::function<void(const std::vector<Value*>&)>& body) {
    if (depth == shape.size()) {
        body(coordinates);
        return;
    }
    if (depth > shape.size() || body == nullptr)
        reportTensorError("tensor 循环嵌套参数无效");

    TensorCountedLoop loop = beginTensorCountedLoop(
        shape[depth], label + ".axis" + std::to_string(depth));
    coordinates.push_back(loop.index);
    emitTensorLoopNest(shape, depth + 1, coordinates, label, body);
    coordinates.pop_back();
    endTensorCountedLoop(loop);
}

bool IRGenerator::copyTensor(
    const TensorValue& destination,
    const TensorValue& source) {
    if (destination.elementKind != source.elementKind ||
        destination.shape != source.shape || currentBlock == nullptr) {
        return false;
    }
    if (destination.address == source.address)
        return true;

    const std::size_t count = tensorElementCount(source.shape);
    const bool distinctDirectStorage =
        ((dynamic_cast<AllocaInst*>(destination.address) != nullptr ||
          dynamic_cast<Variable*>(destination.address) != nullptr) &&
         (dynamic_cast<AllocaInst*>(source.address) != nullptr ||
          dynamic_cast<Variable*>(source.address) != nullptr));
    const bool cannotOverlap = source.isTemporary || destination.isTemporary ||
                               distinctDirectStorage;

    // 形参和子 tensor 可能来自同一底层对象；无法证明不重叠时交给 memmove。
    if (!cannotOverlap) {
        TensorValue flatDestination = makeFlatTensorValue(destination);
        TensorValue flatSource = makeFlatTensorValue(source);
        if (flatDestination.address == nullptr || flatSource.address == nullptr)
            return false;
        const std::size_t elementBytes = source.elementType->getSize();
        if (elementBytes == 0 ||
            count > static_cast<std::size_t>(
                        std::numeric_limits<std::int32_t>::max()) /
                        elementBytes) {
            reportTensorError("tensor 复制的字节数无法用 i32 表示");
        }
        std::vector<Value*> arguments{
            flatDestination.address,
            flatSource.address,
            ConstInt::newConstInt(static_cast<std::int64_t>(
                count * elementBytes)),
            ConstBool::newConstBool(false)};
        if (currentBlock->genCallInst(
                BuildInFunction::getDefaultMemmoveName(), arguments) ==
            nullptr) {
            return false;
        }
        return true;
    }

    std::vector<Value*> coordinates;
    emitTensorLoopNest(
        source.shape, 0, coordinates, "tensor.copy",
        [&](const std::vector<Value*>& indices) {
            Value* sourceAddress = tensorIndexedAddress(
                source, indices, indices.size());
            Value* destinationAddress = tensorIndexedAddress(
                destination, indices, indices.size());
            Value* loaded = sourceAddress == nullptr
                                ? nullptr
                                : currentBlock->genLoadInst(sourceAddress);
            if (loaded == nullptr || destinationAddress == nullptr)
                reportTensorError("tensor 复制循环的元素寻址失败");
            currentBlock->genStoreInst(loaded, destinationAddress);
        });
    return true;
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
        return !lvalue->hasArray();
    }
    if (binding->isMutable || binding->constantInitializer == nullptr ||
        binding->type == nullptr || !lvalue->hasArray()) {
        return false;
    }

    std::size_t arrayRank = 0;
    for (auto type = binding->type;
         type != nullptr && type->getTypeSystem() == IR_ARRAY;) {
        ++arrayRank;
        type = std::dynamic_pointer_cast<ARRAYType>(type)->getElementType();
    }
    const auto& indices = lvalue->getArray()->getExpressions();
    if (indices.size() != arrayRank) {
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

std::size_t IRGenerator::countInitializerLeaves(
    const InitVal& node) const {
    if (dynamic_cast<const AddExp*>(node.getValue().get()) != nullptr)
        return 1;
    const auto* list = dynamic_cast<const InitValList*>(
        node.getValue().get());
    if (list == nullptr)
        return 0;
    std::size_t count = 0;
    for (const auto& initializer : list->getInitVals()) {
        const std::size_t nestedCount =
            countInitializerLeaves(*initializer);
        if (count > std::numeric_limits<std::size_t>::max() - nestedCount)
            return std::numeric_limits<std::size_t>::max();
        count += nestedCount;
    }
    return count;
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
    default:
        return nullptr;
    }
}

Value* IRGenerator::toCondition(Value* value) {
    if (value == nullptr)
        return nullptr;
    if (lookupTensor(value) != nullptr)
        reportTensorError("整个 tensor 不能作为 if、while 或逻辑运算的条件");
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
        throw std::runtime_error("无法降低未知的二元运算符");
    }
}

Value* IRGenerator::emitTensorBinary(
    Value* left, BinaryInst::Operation operation, Value* right,
    TensorValue* destination) {
    const TensorValue* leftLookup = lookupTensor(left);
    const TensorValue* rightLookup = lookupTensor(right);
    if (leftLookup == nullptr && rightLookup == nullptr)
        return nullptr;
    const bool hasLeftTensor = leftLookup != nullptr;
    const bool hasRightTensor = rightLookup != nullptr;
    const TensorValue leftValue =
        hasLeftTensor ? *leftLookup : TensorValue{};
    const TensorValue rightValue =
        hasRightTensor ? *rightLookup : TensorValue{};
    if (BinaryInst::isCompareOp(operation) || operation == BinaryInst::AND ||
        operation == BinaryInst::OR || operation == BinaryInst::XOR ||
        operation == BinaryInst::SHL || operation == BinaryInst::LSHR ||
        operation == BinaryInst::ASHR) {
        reportTensorError("整个 tensor 不支持关系运算或逻辑运算");
    }

    const TensorValue& resultModel =
        hasLeftTensor ? leftValue : rightValue;
    if (hasLeftTensor && hasRightTensor) {
        if (leftValue.elementKind != rightValue.elementKind)
            reportTensorError("两个 tensor 的元素类型必须完全相同");
        if (leftValue.shape != rightValue.shape)
            reportTensorError("逐元素运算要求两个 tensor 的形状完全相同");
    }

    if (operation == BinaryInst::MOD &&
        resultModel.elementKind != ASTScalarKind::Int32) {
        reportTensorError("% 只支持 int tensor 和 int 标量");
    }

    Value* scalarLeft = hasLeftTensor ? nullptr : left;
    Value* scalarRight = hasRightTensor ? nullptr : right;
    auto prepareScalar = [&](Value*& scalar) {
        if (scalar == nullptr)
            return;
        const ASTScalarKind scalarKind = sourceScalarKind(
            scalar->getIRType());
        if (scalarKind == ASTScalarKind::Invalid)
            reportTensorError("tensor 只能与 int 或 float 标量运算");
        if (resultModel.elementKind == ASTScalarKind::Int32 &&
            scalarKind != ASTScalarKind::Int32) {
            reportTensorError("int tensor 不能与 float 标量运算");
        }
        if (operation == BinaryInst::MOD &&
            scalarKind != ASTScalarKind::Int32) {
            reportTensorError("% 只支持 int tensor 和 int 标量");
        }
        // int 到 float 是安全提升；float 到 int 的隐式窄化在 tensor 运算中禁止。
        scalar = BasicBlock::coerceValueToTypeWithoutBitCast(
            currentBlock, scalar, resultModel.elementType);
        if (scalar == nullptr)
            reportTensorError("标量无法提升为 tensor 的元素类型");
    };
    prepareScalar(scalarLeft);
    prepareScalar(scalarRight);

    std::vector<TensorValue> inputs;
    if (hasLeftTensor)
        inputs.push_back(leftValue);
    if (hasRightTensor)
        inputs.push_back(rightValue);
    Value* result = selectTensorResult(
        destination, resultModel.elementKind, resultModel.shape,
        inputs, true);
    const TensorValue* resultTensor = lookupTensor(result);
    if (resultTensor == nullptr)
        reportTensorError("无法创建逐元素运算结果");

    const std::size_t count = tensorElementCount(resultModel.shape);
    if (count == 0)
        reportTensorError("逐元素运算的元素总数无效");
    std::vector<Value*> coordinates;
    emitTensorLoopNest(
        resultModel.shape, 0, coordinates, "tensor.elementwise",
        [&](const std::vector<Value*>& indices) {
            Value* lhs = scalarLeft;
            Value* rhs = scalarRight;
            if (hasLeftTensor) {
                Value* address = tensorIndexedAddress(
                    leftValue, indices, indices.size());
                lhs = address == nullptr
                          ? nullptr
                          : currentBlock->genLoadInst(address);
            }
            if (hasRightTensor) {
                Value* address = tensorIndexedAddress(
                    rightValue, indices, indices.size());
                rhs = address == nullptr
                          ? nullptr
                          : currentBlock->genLoadInst(address);
            }
            Value* element = emitBinary(lhs, operation, rhs);
            Value* destinationAddress = tensorIndexedAddress(
                *resultTensor, indices, indices.size());
            if (element == nullptr || destinationAddress == nullptr)
                reportTensorError("逐元素运算生成失败");
            currentBlock->genStoreInst(element, destinationAddress);
        });
    return result;
}

Value* IRGenerator::emitTensorUnaryMinus(
    Value* value, TensorValue* requestedDestination) {
    const TensorValue* sourceLookup = lookupTensor(value);
    if (sourceLookup == nullptr)
        return nullptr;
    const TensorValue source = *sourceLookup;
    Value* result = selectTensorResult(
        requestedDestination, source.elementKind, source.shape,
        std::vector<TensorValue>{source}, true);
    const TensorValue* destination = lookupTensor(result);
    if (destination == nullptr)
        reportTensorError("无法创建一元取反结果");
    const std::size_t count = tensorElementCount(source.shape);
    if (count == 0)
        reportTensorError("tensor 一元取反的元素总数无效");
    std::vector<Value*> coordinates;
    emitTensorLoopNest(
        source.shape, 0, coordinates, "tensor.negate",
        [&](const std::vector<Value*>& indices) {
            Value* sourceAddress = tensorIndexedAddress(
                source, indices, indices.size());
            Value* resultAddress = tensorIndexedAddress(
                *destination, indices, indices.size());
            Value* element = sourceAddress == nullptr
                                 ? nullptr
                                 : currentBlock->genLoadInst(sourceAddress);
            Value* negated = emitBinary(
                zeroValue(source.elementType), BinaryInst::SUB, element);
            if (negated == nullptr || resultAddress == nullptr)
                reportTensorError("tensor 一元取反生成失败");
            currentBlock->genStoreInst(negated, resultAddress);
        });
    return result;
}

Value* IRGenerator::emitMatrixMultiply(
    Value* left, Value* right, TensorValue* destination) {
    const TensorValue* leftLookup = lookupTensor(left);
    const TensorValue* rightLookup = lookupTensor(right);
    if (leftLookup == nullptr || rightLookup == nullptr)
        reportTensorError("@ 的两个操作数都必须是 tensor");
    const TensorValue leftTensor = *leftLookup;
    const TensorValue rightTensor = *rightLookup;
    if (leftTensor.elementKind != rightTensor.elementKind)
        reportTensorError("@ 的两个 tensor 必须具有完全相同的元素类型");
    if (leftTensor.shape.empty() || rightTensor.shape.empty())
        reportTensorError("@ 不接受空形状 tensor");

    const bool leftOneDimensional = leftTensor.shape.size() == 1;
    const bool rightOneDimensional = rightTensor.shape.size() == 1;
    const std::size_t leftK = leftTensor.shape.back();
    const std::size_t rightK = rightOneDimensional
                                   ? rightTensor.shape.back()
                                   : rightTensor.shape[
                                         rightTensor.shape.size() - 2];
    if (leftK != rightK)
        reportTensorError("@ 的收缩维长度不相等");

    const std::size_t leftM = leftOneDimensional
                                  ? 1
                                  : leftTensor.shape[
                                        leftTensor.shape.size() - 2];
    const std::size_t rightN = rightOneDimensional
                                   ? 1
                                   : rightTensor.shape.back();
    const std::size_t leftBatchRank =
        leftOneDimensional ? 0 : leftTensor.shape.size() - 2;
    const std::size_t rightBatchRank =
        rightOneDimensional ? 0 : rightTensor.shape.size() - 2;
    const std::size_t batchRank = std::max(leftBatchRank, rightBatchRank);
    std::vector<std::size_t> batchShape(batchRank, 1);
    for (std::size_t reverse = 0; reverse < batchRank; ++reverse) {
        const std::size_t leftDimension =
            reverse < leftBatchRank
                ? leftTensor.shape[leftBatchRank - reverse - 1]
                : 1;
        const std::size_t rightDimension =
            reverse < rightBatchRank
                ? rightTensor.shape[rightBatchRank - reverse - 1]
                : 1;
        if (leftDimension != rightDimension && leftDimension != 1 &&
            rightDimension != 1) {
            reportTensorError("@ 的批次维不能按 equal-or-one 规则广播");
        }
        batchShape[batchRank - reverse - 1] =
            std::max(leftDimension, rightDimension);
    }

    std::vector<std::size_t> resultShape = batchShape;
    if (!leftOneDimensional)
        resultShape.push_back(leftM);
    if (!rightOneDimensional)
        resultShape.push_back(rightN);
    if (!resultShape.empty() && tensorElementCount(resultShape) == 0)
        reportTensorError("@ 的结果元素总数溢出");

    Value* result = nullptr;
    const TensorValue* resultTensor = nullptr;
    if (!resultShape.empty()) {
        result = selectTensorResult(
            destination, leftTensor.elementKind, resultShape,
            std::vector<TensorValue>{leftTensor, rightTensor}, false);
        resultTensor = lookupTensor(result);
        if (resultTensor == nullptr)
            reportTensorError("无法创建 @ 的结果存储");
    }

    Value* scalarResult = nullptr;

    // 按静态 rank 建立 batch、行、列循环；归约循环保持 k 从零递增。
    std::vector<Value*> outputCoordinates;
    std::function<void(std::size_t)> emitOutputLoopNest;
    emitOutputLoopNest = [&](std::size_t depth) {
        if (depth < resultShape.size()) {
            TensorCountedLoop loop = beginTensorCountedLoop(
                resultShape[depth],
                "tensor.gemm.axis" + std::to_string(depth), false);
            outputCoordinates.push_back(loop.index);
            emitOutputLoopNest(depth + 1);
            outputCoordinates.pop_back();
            endTensorCountedLoop(loop);
            return;
        }

        TensorCountedLoop contractedLoop = beginTensorCountedLoop(
            leftK, "tensor.gemm.contract", false);
        auto* accumulator = new PhiInst(leftTensor.elementType);
        contractedLoop.header->pushFront(accumulator);
        accumulator->addIncoming(
            zeroValue(leftTensor.elementType),
            contractedLoop.preheader);

        std::vector<Value*> leftCoordinates;
        leftCoordinates.reserve(leftTensor.shape.size());
        for (std::size_t index = 0; index < leftBatchRank; ++index) {
            const std::size_t outputIndex =
                batchRank - leftBatchRank + index;
            leftCoordinates.push_back(
                leftTensor.shape[index] == 1
                    ? static_cast<Value*>(ConstInt::newConstInt(0))
                    : outputCoordinates[outputIndex]);
        }
        if (!leftOneDimensional)
            leftCoordinates.push_back(outputCoordinates[batchRank]);
        leftCoordinates.push_back(contractedLoop.index);

        std::vector<Value*> rightCoordinates;
        rightCoordinates.reserve(rightTensor.shape.size());
        for (std::size_t index = 0; index < rightBatchRank; ++index) {
            const std::size_t outputIndex =
                batchRank - rightBatchRank + index;
            rightCoordinates.push_back(
                rightTensor.shape[index] == 1
                    ? static_cast<Value*>(ConstInt::newConstInt(0))
                    : outputCoordinates[outputIndex]);
        }
        rightCoordinates.push_back(contractedLoop.index);
        if (!rightOneDimensional) {
            const std::size_t columnIndex =
                batchRank + (leftOneDimensional ? 0 : 1);
            rightCoordinates.push_back(
                outputCoordinates[columnIndex]);
        }

        Value* leftAddress = tensorIndexedAddress(
            leftTensor, leftCoordinates, leftCoordinates.size());
        Value* rightAddress = tensorIndexedAddress(
            rightTensor, rightCoordinates, rightCoordinates.size());
        Value* leftElement = leftAddress == nullptr
                                 ? nullptr
                                 : currentBlock->genLoadInst(leftAddress);
        Value* rightElement = rightAddress == nullptr
                                  ? nullptr
                                  : currentBlock->genLoadInst(rightAddress);
        Value* product = emitBinary(
            leftElement, BinaryInst::MUL, rightElement);
        Value* nextAccumulator = emitBinary(
            accumulator, BinaryInst::ADD, product);
        if (nextAccumulator == nullptr)
            reportTensorError("@ 的乘加生成失败");
        accumulator->addIncoming(nextAccumulator, currentBlock);
        endTensorCountedLoop(contractedLoop);
        Value* reducedValue = contractedLoop.headerTested
                                  ? static_cast<Value*>(accumulator)
                                  : nextAccumulator;

        if (resultShape.empty()) {
            scalarResult = reducedValue;
            return;
        }
        Value* destination = tensorIndexedAddress(
            *resultTensor, outputCoordinates,
            outputCoordinates.size());
        if (destination == nullptr)
            reportTensorError("@ 的结果寻址失败");
        currentBlock->genStoreInst(reducedValue, destination);
    };
    emitOutputLoopNest(0);
    return resultShape.empty() ? scalarResult : result;
}

IRGenerator::SourceValueType IRGenerator::sourceTypeOfValue(
    Value* value) const {
    if (const TensorValue* tensor = lookupTensor(value)) {
        return SourceValueType{
            true, true, false, tensor->elementKind, tensor->shape};
    }
    if (value == nullptr || value->getIRType() == nullptr)
        return {};
    const ASTScalarKind kind = sourceScalarKind(value->getIRType());
    if (kind == ASTScalarKind::Invalid)
        return {};
    return SourceValueType{
        true, false, kind == ASTScalarKind::Void, kind, {}};
}

std::string IRGenerator::makeSpecializationKey(
    const FuncDef& node,
    const std::vector<SourceValueType>& argumentTypes) const {
    std::ostringstream key;
    key << node.getIdent();
    bool hasTensorParameter = false;
    if (node.getParams() != nullptr) {
        const auto& parameters = node.getParams()->getParams();
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (parameters[index] == nullptr ||
                !parameters[index]->getType().isTensor()) {
                continue;
            }
            hasTensorParameter = true;
            if (index >= argumentTypes.size() ||
                !argumentTypes[index].valid ||
                !argumentTypes[index].isTensor) {
                reportTensorError("tensor 形参缺少可专门化的实参形状");
            }
            key << ".tensor" << index << ".";
            key << (argumentTypes[index].elementKind == ASTScalarKind::Int32
                        ? "i"
                        : "f");
            for (std::size_t dimension : argumentTypes[index].shape)
                key << "x" << dimension;
        }
    }
    if (!hasTensorParameter && node.getType().isTensor())
        key << ".tensor_return";
    return key.str();
}

bool IRGenerator::validateTensorParameter(
    const FuncParam& parameter, const SourceValueType& actualType) {
    if (!parameter.getType().isTensor() ||
        !parameter.hasEmptyBrackets() || !actualType.valid ||
        !actualType.isTensor ||
        parameter.getType().getScalarKind() != actualType.elementKind) {
        return false;
    }

    std::vector<std::size_t> trailingShape;
    if (parameter.hasDimension())
        lowerStaticShape(*parameter.getExpList(), trailingShape);
    if (actualType.shape.size() != trailingShape.size() + 1)
        return false;
    for (std::size_t index = 0; index < trailingShape.size(); ++index) {
        if (actualType.shape[index + 1] != trailingShape[index])
            return false;
    }
    return true;
}

IRGenerator::FunctionSpecialization*
IRGenerator::getOrCreateFunctionSpecialization(
    const FuncDef& node,
    const std::vector<SourceValueType>& argumentTypes) {
    const std::size_t parameterCount =
        node.getParams() == nullptr
            ? 0
            : node.getParams()->getParams().size();
    if (argumentTypes.size() != parameterCount)
        throw std::runtime_error(
            "函数 " + node.getIdent() + " 的实参数量不匹配");

    const std::string key = makeSpecializationKey(node, argumentTypes);
    auto found = functionSpecializations.find(key);
    if (found != functionSpecializations.end())
        return &found->second;

    FunctionSpecialization specialization;
    specialization.source = &node;
    specialization.irName = key;
    specialization.parameterTypes = argumentTypes;
    if (node.getType().isTensor()) {
        specialization.returnType.isTensor = true;
        specialization.returnType.elementKind =
            node.getType().getScalarKind();
    } else {
        specialization.returnType.valid = true;
        specialization.returnType.isVoid = node.getType().isVoid();
        specialization.returnType.elementKind =
            node.getType().getScalarKind();
    }

    auto inserted = functionSpecializations.emplace(
        key, std::move(specialization));
    FunctionSpecialization& result = inserted.first->second;
    lowerFunctionSpecialization(result);
    return &result;
}

void IRGenerator::lowerFunctionSpecialization(
    FunctionSpecialization& specialization) {
    if (specialization.isComplete || specialization.isLowering)
        return;
    if (specialization.source == nullptr)
        throw std::runtime_error("函数专门化缺少源码定义");

    const FuncDef& node = *specialization.source;
    const auto returnType = node.getType().isTensor()
                                ? std::shared_ptr<IRType>(
                                      VOIDType::NewVoid())
                                : lowerSourceType(node.getType(), true);
    if (returnType == nullptr)
        throw std::runtime_error(
            "函数 " + node.getIdent() + " 的返回类型无效");

    Function& function = ModuleIR->newFunction(
        returnType, specialization.irName);
    specialization.function = &function;
    specialization.isLowering = true;

    Function* savedFunction = currentFunction;
    BasicBlock* savedBlock = currentBlock;
    SymbolTable savedSymbols = symbols;
    std::vector<LoopTargets> savedLoopStack = loopStack;
    FunctionSpecialization* savedSpecialization = currentSpecialization;
    std::vector<SourceValueType> savedParameterTypes =
        currentParameterTypes;
    AllocaInst* savedReturnSlot = currentTensorReturnSlot;
    const ASTScalarKind savedReturnElement = currentTensorReturnElement;

    auto restoreContext = [&]() {
        currentFunction = savedFunction;
        currentBlock = savedBlock;
        symbols = std::move(savedSymbols);
        loopStack = std::move(savedLoopStack);
        currentSpecialization = savedSpecialization;
        currentParameterTypes = std::move(savedParameterTypes);
        currentTensorReturnSlot = savedReturnSlot;
        currentTensorReturnElement = savedReturnElement;
    };

    try {
        symbols.keepGlobalScopeOnly();
        symbols.enterScope();
        currentFunction = &function;
        currentBlock = function.front();
        loopStack.clear();
        currentSpecialization = &specialization;
        currentParameterTypes = specialization.parameterTypes;
        currentTensorReturnSlot = nullptr;
        currentTensorReturnElement = ASTScalarKind::Invalid;

        if (node.getType().isTensor()) {
            const auto elementType = lowerScalarKind(
                node.getType().getScalarKind());
            const auto pointerType = POINTERType::NewPointer(elementType);
            currentTensorReturnSlot = function.pushParamWithStackSlot(
                new Param(pointerType, "__tensor_result"));
            currentTensorReturnElement =
                node.getType().getScalarKind();
            if (currentTensorReturnSlot == nullptr)
                reportTensorError("tensor 返回地址形参创建失败");
        }

        if (node.getParams() != nullptr) {
            const auto& parameters = node.getParams()->getParams();
            for (std::size_t index = 0; index < parameters.size(); ++index) {
                const SourceValueType* concreteType =
                    parameters[index]->getType().isTensor()
                        ? &currentParameterTypes[index]
                        : nullptr;
                lowerParameter(*parameters[index], concreteType);
            }
        }

        if (node.getFuncBody() != nullptr)
            node.getFuncBody()->accept(*this);

        if (node.getType().isTensor() &&
            !specialization.returnType.valid) {
            reportTensorError(
                "tensor 返回函数无法从 return 推导结果形状");
        }
        if (currentBlock != nullptr && !currentBlock->hasTerminator()) {
            if (returnType->isVoid()) {
                currentBlock->genRetInst();
            } else if (function.getIdent() == "main") {
                currentBlock->genRetInst(ConstInt::newConstInt(0));
            } else {
                currentBlock->genRetInst(
                    UndefValue::NewUndefValue(returnType));
            }
        }
        symbols.exitScope();
        specialization.isComplete = true;
        specialization.isLowering = false;
    } catch (...) {
        specialization.isLowering = false;
        restoreContext();
        throw;
    }
    restoreContext();
}

Value* IRGenerator::lowerSourceFunctionCall(
    const FuncCall& node, const FuncDef& function,
    std::vector<Value*> arguments, TensorValue* destination) {
    const std::size_t formalCount =
        function.getParams() == nullptr
            ? 0
            : function.getParams()->getParams().size();
    if (arguments.size() != formalCount)
        throw std::runtime_error(
            "函数 " + node.getIdent() + " 的实参数量不匹配");

    std::vector<SourceValueType> argumentTypes;
    argumentTypes.reserve(arguments.size());
    std::vector<TensorValue> tensorInputs;
    for (Value* argument : arguments) {
        argumentTypes.push_back(sourceTypeOfValue(argument));
        const TensorValue* tensor = lookupTensor(argument);
        if (tensor != nullptr)
            tensorInputs.push_back(*tensor);
    }

    FunctionSpecialization* specialization =
        getOrCreateFunctionSpecialization(function, argumentTypes);
    if (specialization == nullptr || specialization->function == nullptr)
        throw std::runtime_error("函数专门化生成失败");
    if (currentBlock == nullptr)
        throw std::runtime_error("函数调用不能出现在全局常量表达式中");

    Value* result = nullptr;
    std::vector<Value*> irArguments;
    if (function.getType().isTensor()) {
        if (!specialization->returnType.valid ||
            specialization->returnType.shape.empty()) {
            reportTensorError(
                "递归 tensor 返回调用尚未得到可用的结果形状");
        }

        // 只有编译器新建的临时对象或当前函数的局部对象才能直接作为
        // 任意被调函数的隐藏返回槽。全局对象可能同时被被调函数读取，
        // 未知来源地址也可能与未显式传入的存储重叠，二者均保守回退。
        TensorValue* directDestination = nullptr;
        if (destination != nullptr && destination->address != nullptr) {
            Value* destinationRoot = tensorStorageRoot(
                destination->address);
            if (destination->isTemporary ||
                dynamic_cast<AllocaInst*>(destinationRoot) != nullptr) {
                directDestination = destination;
            }
        }
        result = selectTensorResult(
            directDestination,
            specialization->returnType.elementKind,
            specialization->returnType.shape,
            tensorInputs, false);
        const TensorValue* resultInfo = lookupTensor(result);
        if (resultInfo == nullptr)
            reportTensorError("tensor 调用结果分配失败");
        Value* flatResult = tensorFlatAddress(*resultInfo);
        if (flatResult == nullptr)
            reportTensorError("tensor 调用结果地址生成失败");
        irArguments.push_back(flatResult);
    }

    const auto& irParameters = specialization->function->getParams();
    const std::size_t hiddenCount =
        function.getType().isTensor() ? 1 : 0;
    if (function.getParams() == nullptr && !arguments.empty())
        throw std::runtime_error("函数源码形参列表为空");
    const auto* sourceParameters =
        function.getParams() == nullptr
            ? nullptr
            : &function.getParams()->getParams();
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        Value* argument = arguments[index];
        const FuncParam& formal = *(*sourceParameters)[index];
        if (formal.getType().isTensor()) {
            if (!validateTensorParameter(formal, argumentTypes[index]))
                reportTensorError("tensor 调用的形参和实参不匹配");
            const TensorValue* tensor = lookupTensor(argument);
            if (tensor == nullptr)
                reportTensorError("tensor 形参收到标量实参");
            argument = tensorFlatAddress(*tensor);
        } else {
            if (argumentTypes[index].isTensor)
                reportTensorError("普通形参不能接收整个 tensor");
            if (index + hiddenCount >= irParameters.size())
                throw std::runtime_error("函数 IR 形参数量不匹配");
            const auto targetType =
                irParameters[index + hiddenCount]->getIRType();
            if (targetType->isPointer()) {
                if (argument == nullptr || argument->getIRType() == nullptr ||
                    !argument->getIRType()->isPointer()) {
                    throw std::runtime_error("数组形参必须接收指针实参");
                }
                const auto sourcePointer =
                    std::dynamic_pointer_cast<POINTERType>(
                        argument->getIRType());
                const auto targetPointer =
                    std::dynamic_pointer_cast<POINTERType>(targetType);
                if (sourcePointer == nullptr || targetPointer == nullptr ||
                    !IRType::hasSameShape(
                        sourcePointer->getPointerType(),
                        targetPointer->getPointerType())) {
                    throw std::runtime_error("数组形参的剩余形状不匹配");
                }
            } else {
                argument = BasicBlock::coerceValueToTypeWithoutBitCast(
                    currentBlock, argument, targetType);
                if (argument == nullptr)
                    throw std::runtime_error("标量实参类型转换失败");
            }
        }
        irArguments.push_back(argument);
    }

    auto* call = new CallInst(specialization->function, irArguments);
    currentBlock->pushBack(call);
    return function.getType().isTensor() ? result : call;
}

Value* IRGenerator::emitBinary(Value* left,
                               BinaryInst::Operation operation,
                               Value* right,
                               TensorValue* destination) {
    // 先执行 SysY 常用算术转换，再交给基本块统一完成常量折叠或指令生成。
    if (left == nullptr || right == nullptr)
        return nullptr;
    if (lookupTensor(left) != nullptr ||
        lookupTensor(right) != nullptr) {
        return emitTensorBinary(left, operation, right, destination);
    }
    const auto leftType = left->getIRType();
    const auto rightType = right->getIRType();
    if (leftType == nullptr || rightType == nullptr)
        return nullptr;

    if (operation == BinaryInst::MOD &&
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
Value* IRGenerator::lowerExpression(
    const BaseExp<OperandT>& node, TensorValue* destination) {
    // 同一优先级的二元表达式按源码顺序从左向右生成，
    // 每一步都复用 emitBinary 完成类型提升、常量折叠和指令插入。
    const auto& operands = node.getOperands();
    const auto& operators = node.getOps();
    if (operands.empty() || operands.size() != operators.size() + 1)
        return nullptr;

    auto operand = operands.begin();
    if (operators.empty())
        return lowerExpression(*operand->get(), destination);

    Value* result = lowerExpression(*operand->get(), nullptr);
    auto sourceOperator = operators.begin();
    for (; sourceOperator != operators.end(); ++sourceOperator) {
        ++operand;
        Value* right = lowerExpression(*operand->get(), nullptr);
        TensorValue* operationDestination =
            std::next(sourceOperator) == operators.end()
                ? destination
                : nullptr;
        if (*sourceOperator == SY_GEMM)
            result = emitMatrixMultiply(
                result, right, operationDestination);
        else
            result = emitBinary(
                result, lowerBinaryOperator(*sourceOperator), right,
                operationDestination);
    }
    return materializeTensorResult(result, destination);
}

Value* IRGenerator::lowerPrimary(
    const BaseAST& node, TensorValue* destination) {
    if (auto expression = dynamic_cast<const AddExp*>(&node))
        return lowerExpression(*expression, destination);
    if (auto lvalue = dynamic_cast<const LVal*>(&node))
        return materializeTensorResult(
            lowerRValue(*lvalue), destination);
    if (auto call = dynamic_cast<const FuncCall*>(&node)) {
        return materializeTensorResult(
            lowerExpression(*call, destination), destination);
    }
    if (auto integer = dynamic_cast<const ConValue<int>*>(&node))
        return ConstInt::newConstInt(integer->getValue());
    if (auto floating = dynamic_cast<const ConValue<float>*>(&node))
        return ConstFloat::newConstFloat(floating->getValue());

    return nullptr;
}

Value* IRGenerator::lowerExpression(
    const UnaryExp& node, TensorValue* destination) {
    if (node.getOperandCount() != 1)
        return nullptr;
    if (node.getOps().empty())
        return lowerPrimary(
            *node.getOperands().front(), destination);

    Value* result = lowerPrimary(
        *node.getOperands().front(), nullptr);
    if (result == nullptr)
        return nullptr;

    // 语法分析器把外层一元操作头插保存，因此逆序应用即可按由内到外求值。
    for (auto operation = node.getOps().rbegin();
         operation != node.getOps().rend(); ++operation) {
        TensorValue* operationDestination =
            std::next(operation) == node.getOps().rend()
                ? destination
                : nullptr;
        switch (*operation) {
        case SY_ADD:
            break;
        case SY_SUB: {
            if (lookupTensor(result) != nullptr) {
                result = emitTensorUnaryMinus(
                    result, operationDestination);
                break;
            }
            if (IRType::isBool(result->getIRType()))
                result = BasicBlock::coerceValueToTypeWithoutBitCast(
                    currentBlock, result, INTType::getInt32Ty());
            Value* zero = zeroValue(result->getIRType());
            result = emitBinary(zero, BinaryInst::SUB, result);
            break;
        }
        case SY_NOT:
            if (lookupTensor(result) != nullptr)
                reportTensorError("整个 tensor 不支持逻辑非运算");
            result = logicalNot(result);
            break;
        default:
            return nullptr;
        }
        if (result == nullptr)
            return nullptr;
    }
    return materializeTensorResult(result, destination);
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

Value* IRGenerator::lowerExpression(
    const FuncCall& node, TensorValue* destination) {
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

    const auto sourceFunction = sourceFunctions.find(node.getIdent());
    if (sourceFunction != sourceFunctions.end()) {
        if (sourceFunction->second == nullptr)
            throw std::runtime_error("函数定义为空");
        return lowerSourceFunctionCall(
            node, *sourceFunction->second, std::move(arguments),
            destination);
    }

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
void IRGenerator::lowerParameter(
    FuncParam& node, const SourceValueType* concreteType) {
    if (currentFunction == nullptr)
        return;
    const std::string& parameterName = node.getIdent();
    // 同一形参作用域拒绝重复名字，并在创建参数槽位前终止。
    if (parameterName.empty() ||
        symbols.containsObjectInCurrentScope(parameterName)) {
        return;
    }

    if (node.getType().isTensor()) {
        if (concreteType == nullptr ||
            !validateTensorParameter(node, *concreteType)) {
            reportTensorError("tensor 形参与实参的类型或形状不匹配");
        }
        const auto elementType = lowerScalarKind(concreteType->elementKind);
        const auto parameterType = POINTERType::NewPointer(elementType);
        AllocaInst* parameterSlot = currentFunction->pushParamWithStackSlot(
            new Param(parameterType, parameterName));
        if (parameterSlot == nullptr)
            reportTensorError("tensor 形参槽位创建失败");
        if (symbols.declareObject(
                parameterName,
                SymbolTable::ObjectBinding{
                    SymbolTable::ObjectKind::ArrayParameter,
                    parameterSlot,
                    elementType,
                    true,
                    nullptr,
                    true,
                    concreteType->shape,
                    true}) == nullptr) {
            reportTensorError("tensor 形参重复定义");
        }
        return;
    }

    std::shared_ptr<IRType> parameterType = lowerSourceType(node.getType());
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
            kind, parameterSlot, sourceObjectType, true, nullptr,
            false, {}, false});
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
            if (strictTensorInitializer)
                reportTensorError(
                    "int tensor 的初始化元素不能是 float");
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

//核心IR生成逻辑函数1：全局/局部的标量、tensor 与数组对象生成。
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
    bool isConstant,
    const std::vector<std::size_t>* tensorShape) {
    //1.获得变量/常量名
    const std::string name = node.getIdent();
    //在创建存储前拒绝同层重定义
    if (name.empty() || symbols.containsObjectInCurrentScope(name))
        return;

    //2.构造基础对象类型：标量直接使用声明类型，
    // tensor 和数组按源码维度逐层包装。
    const auto scalarType = declaredType;
    if (scalarType == nullptr)
        return;

    std::shared_ptr<IRType> objectType = scalarType;
    if (tensorShape != nullptr) {
        if (!node.hasArray() || tensorShape->empty())
            reportTensorError("tensor 对象声明必须给出维度");
        objectType = makeTensorStorageType(
            scalarType, *tensorShape);
    } else if (node.hasArray()) {
        objectType = lowerArrayType(*node.getArray(), scalarType);
    }
    if (objectType == nullptr)
        return;

    //3.检查是否有初始化器，并在常量或全局变量的情况下，确保初始化器是常量表达式，否则忽略初始化器
    bool hasInitializer = node.hasInitVal();
    if (hasInitializer && (isConstant || currentFunction == nullptr) &&
        !isConstantInitializerSyntax(*node.getInitVal())) {
        if (tensorShape != nullptr)
            reportTensorError(
                "全局或 const tensor 的初始化必须是常量表达式");
        hasInitializer = false;
    }
    if (hasInitializer && tensorShape != nullptr &&
        dynamic_cast<const InitValList*>(
            node.getInitVal()->getValue().get()) != nullptr &&
        countInitializerLeaves(*node.getInitVal()) >
            tensorElementCount(*tensorShape)) {
        reportTensorError("tensor 初始化元素数量超过声明形状的容量");
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
                      !isConstant, nullptr,
                      tensorShape != nullptr,
                      tensorShape == nullptr
                          ? std::vector<std::size_t>{}
                          : *tensorShape,
                      false});
        if (binding == nullptr) {
            return;
        }

        if (hasInitializer) {
            const auto* expression = dynamic_cast<const AddExp*>(
                node.getInitVal()->getValue().get());
            if (tensorShape != nullptr && expression != nullptr) {
                if (currentFunction == nullptr)
                    reportTensorError(
                        "全局 tensor 只能使用数组式常量初始化列表");
                TensorValue destination{
                    storage, scalarType, sourceScalarKind(scalarType),
                    *tensorShape, false};
                Value* sourceValue = lowerExpression(
                    *expression, &destination);
                const TensorValue* source =
                    lookupTensor(sourceValue);
                if (source == nullptr)
                    reportTensorError(
                        "tensor 表达式初始化要求右侧也是 tensor");
                const TensorValue sourceCopy = *source;
                if (destination.elementKind != sourceCopy.elementKind ||
                    destination.shape != sourceCopy.shape) {
                    reportTensorError(
                        "tensor 表达式初始化要求元素类型和形状完全相同");
                }
                if (!copyTensor(destination, sourceCopy))
                    reportTensorError("tensor 表达式初始化复制失败");
                return;
            }

            const bool previousStrictMode = strictTensorInitializer;
            strictTensorInitializer = tensorShape != nullptr;
            Value* initializer = lowerInitializer(
                *node.getInitVal(), objectType);
            strictTensorInitializer = previousStrictMode;
            if (initializer == nullptr)
                reportTensorError("tensor 初始化列表不合法");

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
                                 ? lowerInitializer(
                                       *node.getInitVal(), objectType)
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
                      objectType, false, nullptr,
                      tensorShape != nullptr,
                      tensorShape == nullptr
                          ? std::vector<std::size_t>{}
                          : *tensorShape,
                      false});
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
                          objectType, true, nullptr,
                          tensorShape != nullptr,
                          tensorShape == nullptr
                              ? std::vector<std::size_t>{}
                              : *tensorShape,
                          false}) == nullptr) {
            return;
        }
        if (hasInitializer) {
            Value* initializer = lowerInitializer(
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
                          objectType, true, nullptr,
                          tensorShape != nullptr,
                          tensorShape == nullptr
                              ? std::vector<std::size_t>{}
                              : *tensorShape,
                          false}) == nullptr) {
            return;
        }
        if (hasInitializer) {
            Value* initializer = lowerInitializer(
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
    std::shared_ptr<IRType>& selectedType) const {
    aggregateIndices.clear();
    selectedType = rootType;
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

    if (binding->isTensor) {
        if (indices.size() > binding->tensorShape.size())
            reportTensorError("tensor 下标数量超过其维数");
        if (currentBlock == nullptr)
            return {};

        Value* baseAddress = symbol;
        bool hasFlatAddress = binding->hasFlatTensorAddress;
        if (binding->kind == SymbolTable::ObjectKind::ArrayParameter) {
            baseAddress = currentBlock->genLoadInst(symbol);
            hasFlatAddress = true;
        }
        TensorValue completeValue{
            baseAddress,
            lowerScalarKind(sourceScalarKind(
                initializerLeafType(binding->type))),
            sourceScalarKind(initializerLeafType(binding->type)),
            binding->tensorShape,
            hasFlatAddress};
        if (completeValue.elementType == nullptr ||
            completeValue.elementKind == ASTScalarKind::Invalid) {
            reportTensorError("tensor 的元素类型无效");
        }

        Value* address = tensorIndexedAddress(
            completeValue, indices, indices.size());
        if (address == nullptr)
            reportTensorError("tensor 下标寻址失败");

        ResolvedLValue result;
        result.value = address;
        result.isAddress = true;
        result.isMutable = binding->isMutable;
        if (indices.size() == binding->tensorShape.size()) {
            result.type = completeValue.elementType;
            return result;
        }

        result.isTensor = true;
        result.tensorShape.assign(
            binding->tensorShape.begin() +
                static_cast<std::ptrdiff_t>(indices.size()),
            binding->tensorShape.end());
        result.hasFlatTensorAddress = hasFlatAddress;
        result.type = makeTensorStorageType(
            completeValue.elementType, result.tensorShape);
        if (registerTensor(
                address, completeValue.elementKind,
                result.tensorShape, hasFlatAddress) == nullptr) {
            reportTensorError("无法登记 tensor 子对象");
        }
        return result;
    }

    const bool isArrayParameter =
        binding->kind == SymbolTable::ObjectKind::ArrayParameter;
    std::vector<Value*> aggregateIndices;
    std::shared_ptr<IRType> selectedType;
    if (!classifyIndexedAccess(
            binding->type, isArrayParameter, indices,
            aggregateIndices, selectedType)) {
        return {};
    }

    if (binding->kind == SymbolTable::ObjectKind::Constant) {
        if (!aggregateIndices.empty())
            return {};
        return ResolvedLValue{
            symbol, binding->type, false, false, false, {}, false};
    }

    if (!aggregateIndices.empty()) {
        if (Value* constant = tryLowerConstantArrayElement(
                *binding, aggregateIndices)) {
            return ResolvedLValue{
                constant, selectedType, false, false, false, {}, false};
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
                false,
                {},
                false,
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
        return ResolvedLValue{
            address,
            selectedType,
            true,
            binding->isMutable,
            false,
            {},
            false,
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

    return ResolvedLValue{
        address,
        selectedType,
        true,
        binding->isMutable,
        false,
        {},
        false,
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

    if (reference.isTensor)
        return reference.value;

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
    // 先收集全部函数，使前向调用和 tensor 专门化不依赖源码先后顺序。
    for (const auto& item : node.getItems()) {
        const auto* function = dynamic_cast<const FuncDef*>(item.get());
        if (function == nullptr)
            continue;
        if (!sourceFunctions.emplace(
                function->getIdent(), function).second) {
            throw std::runtime_error(
                "函数重复定义：" + function->getIdent());
        }
    }

    // 全局对象先生成，延迟生成的函数专门化只能继承这一层符号。
    for (const auto& item : node.getItems()) {
        if (dynamic_cast<const FuncDef*>(item.get()) == nullptr)
            item->accept(*this);
    }

    // 不含 tensor 形参的函数可以直接生成；含 tensor 形参者在调用点按形状专门化。
    for (const auto& item : node.getItems()) {
        auto* function = dynamic_cast<FuncDef*>(item.get());
        if (function == nullptr)
            continue;
        bool hasTensorParameter = false;
        std::vector<SourceValueType> parameterTypes;
        if (function->getParams() != nullptr) {
            for (const auto& parameter :
                 function->getParams()->getParams()) {
                if (parameter->getType().isTensor())
                    hasTensorParameter = true;
                parameterTypes.push_back(SourceValueType{
                    true,
                    false,
                    false,
                    parameter->getType().getScalarKind(),
                    {}});
            }
        }
        if (!hasTensorParameter)
            (void)getOrCreateFunctionSpecialization(
                *function, parameterTypes);
    }
}
//IR生成2：ConstDecl全局常量/局部常量
void IRGenerator::visit(ConstDecl& node) {
    const bool isTensor = node.getType().isTensor();
    const auto declaredType = isTensor
                                  ? lowerScalarKind(
                                        node.getType().getScalarKind())
                                  : lowerSourceType(node.getType());
    if (declaredType == nullptr)
        return;
    if (node.getConstDefList() == nullptr)
        return;
    for (const auto& definition : node.getConstDefList()->getDefs()) {
        if (isTensor) {
            if (definition == nullptr || !definition->hasArray())
                reportTensorError("tensor 常量声明必须给出完整形状");
            std::vector<std::size_t> shape;
            lowerStaticShape(*definition->getArray(), shape);
            lowerDefinition(*definition, declaredType, true, &shape);
        } else {
            lowerDefinition(*definition, declaredType, true);
        }
    }
}
//IR生成3：VarDecl全局变量/局部变量
void IRGenerator::visit(VarDecl& node) {
    const bool isTensor = node.getType().isTensor();
    const auto declaredType = isTensor
                                  ? lowerScalarKind(
                                        node.getType().getScalarKind())
                                  : lowerSourceType(node.getType());
    if (declaredType == nullptr)
        return;
    if (node.getVarDefList() == nullptr)
        return;
    for (const auto& definition : node.getVarDefList()->getDefs()) {
        if (isTensor) {
            if (definition == nullptr || !definition->hasArray())
                reportTensorError("tensor 对象声明必须给出完整形状");
            std::vector<std::size_t> shape;
            lowerStaticShape(*definition->getArray(), shape);
            lowerDefinition(*definition, declaredType, false, &shape);
        } else {
            lowerDefinition(*definition, declaredType, false);
        }
    }
}
void IRGenerator::visit(ConstDefList& node) {(void)node;}
void IRGenerator::visit(VarDefList& node) {(void)node;}
void IRGenerator::visit(ConstDef& node) {(void)node;}
void IRGenerator::visit(VarDef& node) {(void)node;}
/*全局变量和全局常量和局部变量和局部常量的生成集成到lowerDefinition中*/

//IR生成4：FuncDef
void IRGenerator::visit(FuncDef& node) {
    if (currentFunction != nullptr)
        return;
    bool hasTensorParameter = false;
    std::vector<SourceValueType> parameterTypes;
    if (node.getParams() != nullptr) {
        for (const auto& parameter : node.getParams()->getParams()) {
            if (parameter->getType().isTensor())
                hasTensorParameter = true;
            parameterTypes.push_back(SourceValueType{
                true, false, false,
                parameter->getType().getScalarKind(), {}});
        }
    }
    if (!hasTensorParameter)
        (void)getOrCreateFunctionSpecialization(node, parameterTypes);
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
    Value* right = nullptr;
    ResolvedLValue left;

    // 无下标的完整 tensor 左值不含求值副作用，可以先取得其地址，让右侧
    // 最终运算在别名安全时直接写入目标；子 tensor 仍维持先右后左的顺序。
    const SymbolTable::ObjectBinding* directBinding =
        node.getLVal()->hasArray()
            ? nullptr
            : symbols.lookupObject(node.getLVal()->getIdent());
    if (directBinding != nullptr && directBinding->isTensor) {
        left = lowerLValue(*node.getLVal());
        if (left.value != nullptr && left.type != nullptr &&
            left.isAddress && left.isMutable && left.isTensor) {
            const TensorValue* destinationLookup = lookupTensor(left.value);
            if (destinationLookup == nullptr)
                reportTensorError("tensor 左值的形状信息丢失");
            TensorValue destination = *destinationLookup;
            right = lowerExpression(*node.getExp(), &destination);
        } else {
            // 非法或只读左值不能成为写入目标，仍生成右侧以维持原有
            // 诊断路径，但绝不提前改写左侧存储。
            right = lowerExpression(*node.getExp());
        }
    } else {
        // 普通赋值和带下标赋值保持原有的右值优先求值顺序。
        right = lowerExpression(*node.getExp());
        left = lowerLValue(*node.getLVal());
    }
    if (right == nullptr || left.value == nullptr || left.type == nullptr ||
        !left.isAddress || !left.isMutable) {
        return;
    }

    const TensorValue* rightTensorLookup =
        lookupTensor(right);
    if (left.isTensor) {
        if (rightTensorLookup == nullptr)
            reportTensorError("整个 tensor 不能由标量赋值");
        const TensorValue rightTensor = *rightTensorLookup;
        const TensorValue* leftTensorLookup =
            lookupTensor(left.value);
        if (leftTensorLookup == nullptr)
            reportTensorError("tensor 左值的形状信息丢失");
        const TensorValue leftTensor = *leftTensorLookup;
        if (leftTensor.elementKind != rightTensor.elementKind)
            reportTensorError(
                "tensor 赋值不允许 int 与 float 之间逐元素转换");
        if (leftTensor.shape != rightTensor.shape)
            reportTensorError("tensor 赋值要求左右形状完全相同");
        if (!copyTensor(leftTensor, rightTensor))
            reportTensorError("tensor 整体赋值复制失败");
        return;
    }
    if (rightTensorLookup != nullptr)
        reportTensorError("标量左值不能接收整个 tensor");

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
    if (currentTensorReturnSlot != nullptr) {
        if (!node.hasReturnValue())
            reportTensorError("tensor 返回函数必须返回 tensor 表达式");
        Value* destinationAddress =
            currentBlock->genLoadInst(currentTensorReturnSlot);
        TensorValue destination{
            destinationAddress,
            lowerScalarKind(currentTensorReturnElement),
            currentTensorReturnElement,
            {},
            true,
            true};
        Value* value = lowerExpression(
            *node.getReturnValue(), &destination);
        const TensorValue* sourceLookup = lookupTensor(value);
        if (sourceLookup == nullptr)
            reportTensorError("tensor 返回函数不能返回标量");
        const TensorValue source = *sourceLookup;
        if (source.elementKind != currentTensorReturnElement)
            reportTensorError(
                "tensor 返回值元素类型必须与函数声明完全相同");
        if (currentSpecialization == nullptr)
            reportTensorError("tensor 返回值缺少函数专门化上下文");
        if (!currentSpecialization->returnType.valid) {
            currentSpecialization->returnType = SourceValueType{
                true, true, false, source.elementKind, source.shape};
        } else if (!currentSpecialization->returnType.isTensor ||
                   currentSpecialization->returnType.elementKind !=
                       source.elementKind ||
                   currentSpecialization->returnType.shape != source.shape) {
            reportTensorError(
                "同一函数的所有 tensor 返回路径必须具有相同类型和形状");
        }
        if (!copyTensor(destination, source))
            reportTensorError("tensor 返回值复制失败");
        currentBlock->genRetInst();
        return;
    }
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
    if (lookupTensor(value) != nullptr)
        reportTensorError("标量或 void 函数不能返回整个 tensor");
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
