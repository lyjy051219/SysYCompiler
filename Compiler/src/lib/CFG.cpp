#include "../../include/lib/CFG.hpp"
#include "../../include/lib/BaseManager.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <map>
#include <unordered_set>
#include <limits>
#include <queue>
#include <stdexcept>

extern std::unique_ptr<Module> ModuleIR;

std::shared_ptr<IRType> RetInst::validateAndGetResultType(
    Value *ReturnValue) {
  if (!ReturnValue ||
      !IRType::isFirstClassValueType(ReturnValue->getIRType()))
    throw std::invalid_argument(
        "ret 返回值必须是标量、指针或固定长度向量");
  return VOIDType::NewVoid();
}

std::shared_ptr<IRType> AllocaInst::makeResultType(
    const std::shared_ptr<IRType> &AllocatedType) {
  if (!AllocatedType || AllocatedType->isVoid())
    throw std::invalid_argument(
        "alloca 不能分配空类型或 void 类型");
  return POINTERType::NewPointer(AllocatedType);
}

std::shared_ptr<IRType> CallInst::validateAndGetResultType(
    Value *Callee, const std::vector<Value *> &Arguments) {
  if (!Callee ||
      (!Callee->getIRType() ||
       (!Callee->getIRType()->isVoid() &&
        !Callee->getIRType()->isFirstClassValueType())))
    throw std::invalid_argument(
        "call 要求具有合法返回类型的被调函数");
  if (!dynamic_cast<Function *>(Callee) &&
      !dynamic_cast<BuildInFunction *>(Callee))
    throw std::invalid_argument(
        "call 的第一个操作数必须是函数");

  for (Value *Argument : Arguments) {
    if (!Argument ||
        !IRType::isFirstClassValueType(Argument->getIRType()))
      throw std::invalid_argument(
          "call 实参必须是标量、指针或固定长度向量");
  }

  if (auto *DirectCallee = dynamic_cast<Function *>(Callee)) {
    const auto &Parameters = DirectCallee->getParams();
    if (Arguments.size() != Parameters.size())
      throw std::invalid_argument(
          "call 实参数量必须与函数形参数量一致");
    for (std::size_t I = 0; I < Arguments.size(); ++I) {
      if (!Parameters[I] || !Parameters[I]->getIRType() ||
          !Arguments[I]->getIRType()->hasSameShape(
              *Parameters[I]->getIRType()))
        throw std::invalid_argument(
            "call 实参类型必须与函数形参类型完全一致");
    }
  } else if (auto *BuiltinCallee =
                 dynamic_cast<BuildInFunction *>(Callee)) {
    auto Signature =
        BuildInFunction::getSignature(BuiltinCallee->getIdent());
    // 有显式签名的运行库/LLVM 内建必须精确匹配；少数历史伪内建
    // 没有参数元数据，只保留上面的 first-class 操作数约束。
    if (Signature) {
      if (!Signature->returnType ||
          !Callee->getIRType()->hasSameShape(
              *Signature->returnType))
        throw std::invalid_argument(
            "call 的内建函数签名无效");

      const std::size_t FixedArgumentCount =
          Signature->parameterTypes.size();
      if (Arguments.size() != FixedArgumentCount)
        throw std::invalid_argument(
            "call 实参数量必须符合内建函数签名");

      for (std::size_t I = 0; I < FixedArgumentCount; ++I) {
        const bool IsOpaquePointer =
            I < Signature->opaquePointerParameters.size() &&
            Signature->opaquePointerParameters[I];
        if (IsOpaquePointer) {
          if (!Arguments[I]->getIRType()->isPointer())
            throw std::invalid_argument(
                "call 的内建函数指针实参类型无效");
          continue;
        }
        if (!Signature->parameterTypes[I] ||
            !Arguments[I]->getIRType()->hasSameShape(
                *Signature->parameterTypes[I]))
          throw std::invalid_argument(
              "call 实参类型必须与内建函数形参类型完全一致");
      }
    }
  }
  return Callee->getIRType();
}

std::shared_ptr<IRType> GetElementPtrInst::getAggregateElementType(
    const std::shared_ptr<IRType> &Type) {
  if (auto ArrayType = std::dynamic_pointer_cast<ARRAYType>(Type))
    return ArrayType->getElementType();
  if (auto VectorType = IRType::asVector(Type))
    return VectorType->getElementType();
  return nullptr;
}

bool GetElementPtrInst::tryDeriveResultType(
    Value *Base, const std::vector<Value *> &Indices,
    std::shared_ptr<IRType> &ResultType,
    std::vector<int64_t> *Strides) {
  ResultType = nullptr;
  if (Strides) Strides->clear();

  auto BasePointer =
      Base && Base->getIRType()
          ? std::dynamic_pointer_cast<POINTERType>(
                Base->getIRType())
          : nullptr;
  if (!BasePointer || !BasePointer->getPointerType() ||
      BasePointer->getPointerType()->isVoid())
    return false;

  if (Indices.empty()) {
    ResultType = Base->getIRType();
    return true;
  }

  std::shared_ptr<IRType> CurrentType =
      BasePointer->getPointerType();
  for (std::size_t I = 0; I < Indices.size(); ++I) {
    Value *Index = Indices[I];
    if (!Index || !Index->getIRType() ||
        !Index->getIRType()->isInt())
      return false;

    // 首个索引在基指针所指对象序列中移动，不剥离 pointee；
    // 后续索引才进入数组或固定长度向量的元素。
    if (I != 0) {
      CurrentType = getAggregateElementType(CurrentType);
      if (!CurrentType) return false;
    }

    if (Strides) {
      const std::size_t Size = CurrentType->getSize();
      if (Size == 0 ||
          Size >
              static_cast<std::size_t>(
                  std::numeric_limits<int64_t>::max()))
        return false;
      Strides->push_back(static_cast<int64_t>(Size));
    }
  }

  ResultType = POINTERType::NewPointer(CurrentType);
  return true;
}

std::shared_ptr<IRType> GetElementPtrInst::requireResultType(
    Value *Base, const std::vector<Value *> &Indices) {
  std::shared_ptr<IRType> ResultType;
  if (!tryDeriveResultType(Base, Indices, ResultType))
    throw std::invalid_argument(
        "getelementptr 的标量索引与基地址类型不匹配");
  return ResultType;
}

std::shared_ptr<IRType> BitCastInst::validateAndGetResultType(
    Value *Source, const std::shared_ptr<IRType> &TargetType) {
  if (!Source || !Source->getIRType() || !TargetType)
    throw std::invalid_argument(
        "bitcast 要求有效的源值和目标类型");

  auto SourceType = Source->getIRType();
  if (SourceType->isPointer() || TargetType->isPointer()) {
    if (!SourceType->isPointer() || !TargetType->isPointer())
      throw std::invalid_argument(
          "bitcast 的指针源和目标必须同时为指针类型");
    return TargetType;
  }

  std::size_t SourceBits = 0;
  std::size_t TargetBits = 0;
  if (!IRType::getLogicalBitWidth(SourceType, SourceBits) ||
      !IRType::getLogicalBitWidth(TargetType, TargetBits))
    throw std::invalid_argument(
        "bitcast 只支持非聚合标量、固定长度向量或指针");
  if (SourceBits != TargetBits)
    throw std::invalid_argument(
        "bitcast 的源和目标逻辑总位数必须相同");
  return TargetType;
}

std::shared_ptr<IRType> LoadInst::validateAndGetResultType(
    Value *Pointer) {
  auto PointerType =
      Pointer && Pointer->getIRType()
          ? std::dynamic_pointer_cast<POINTERType>(
                Pointer->getIRType())
          : nullptr;
  if (!PointerType || !PointerType->getPointerType())
    throw std::invalid_argument(
        "load 的地址操作数必须是标量指针");
  return PointerType->getPointerType();
}

void StoreInst::validateOperands(Value *StoredValue, Value *Pointer) {
  auto PointerType =
      Pointer && Pointer->getIRType()
          ? std::dynamic_pointer_cast<POINTERType>(
                Pointer->getIRType())
          : nullptr;
  if (!StoredValue || !StoredValue->getIRType() ||
      !PointerType || !PointerType->getPointerType())
    throw std::invalid_argument(
        "store 要求有效的值和标量指针");
  if (!StoredValue->getIRType()->hasSameShape(
          *PointerType->getPointerType()))
    throw std::invalid_argument(
        "store 的值类型必须与指针指向类型完全一致");
}

std::shared_ptr<IRType> ExtractElementInst::validateAndGetResultType(
    Value *Vector, Value *Index) {
  auto VectorType =
      Vector && Vector->getIRType()
          ? IRType::asVector(Vector->getIRType())
          : nullptr;
  if (!VectorType || !VectorType->getElementType())
    throw std::invalid_argument(
        "extractelement 的第一个操作数必须是向量");
  if (!Index || !Index->getIRType() || !Index->getIRType()->isInt())
    throw std::invalid_argument(
        "extractelement 的索引必须是标量整数");
  return VectorType->getElementType();
}

std::shared_ptr<IRType> InsertElementInst::validateAndGetResultType(
    Value *Vector, Value *Element, Value *Index) {
  auto VectorType =
      Vector && Vector->getIRType()
          ? IRType::asVector(Vector->getIRType())
          : nullptr;
  if (!VectorType || !VectorType->getElementType())
    throw std::invalid_argument(
        "insertelement 的第一个操作数必须是向量");
  if (!Element || !Element->getIRType() ||
      !Element->getIRType()->hasSameShape(
          *VectorType->getElementType()))
    throw std::invalid_argument(
        "insertelement 的元素类型必须匹配向量元素类型");
  if (!Index || !Index->getIRType() || !Index->getIRType()->isInt())
    throw std::invalid_argument(
        "insertelement 的索引必须是标量整数");
  return Vector->getIRType();
}

std::shared_ptr<IRType> ShuffleVectorInst::validateAndGetResultType(
    Value *First, Value *Second, const std::vector<int> &Mask) {
  auto FirstType =
      First && First->getIRType()
          ? IRType::asVector(First->getIRType())
          : nullptr;
  auto SecondType =
      Second && Second->getIRType()
          ? IRType::asVector(Second->getIRType())
          : nullptr;
  if (!FirstType || !SecondType || !FirstType->getElementType())
    throw std::invalid_argument(
        "shufflevector 的前两个操作数必须是向量");
  if (!First->getIRType()->hasSameShape(*Second->getIRType()))
    throw std::invalid_argument(
        "shufflevector 的两个输入向量必须类型完全一致");
  if (Mask.empty())
    throw std::invalid_argument("shufflevector 的 mask 不能为空");

  const std::size_t SourceLanes = FirstType->getElementCount();
  for (int Lane : Mask) {
    if (Lane < -1)
      throw std::invalid_argument(
          "shufflevector 的 mask 只允许非负索引或 -1");
    if (Lane < 0)
      continue;
    const std::size_t LaneIndex = static_cast<std::size_t>(Lane);
    if (LaneIndex >= SourceLanes &&
        LaneIndex - SourceLanes >= SourceLanes)
      throw std::out_of_range("shufflevector 的 mask 索引越界");
  }

  return VECTORType::NewVector(FirstType->getElementType(),
                               Mask.size());
}

Value *BasicBlock::coerceValueToType(
    Value *Input,
    const std::shared_ptr<IRType> &TargetType) {
  if (!Input || !Input->getIRType() || !TargetType)
    return nullptr;
  const auto SourceType = Input->getIRType();
  if (SourceType->hasSameShape(*TargetType))
    return Input;

  if (auto DestinationInteger =
          std::dynamic_pointer_cast<INTType>(TargetType)) {
    if (DestinationInteger->getIsBool()) {
      if (auto *Boolean = dynamic_cast<ConstBool *>(Input))
        return Boolean;
      if (auto *Integer = dynamic_cast<ConstInt *>(Input))
        return ConstBool::newConstBool(Integer->getVal() != 0);
      if (auto *Floating = dynamic_cast<ConstFloat *>(Input))
        return ConstBool::newConstBool(Floating->getVal() != 0.0f);
      Value *Zero = Value::makeZeroForType(SourceType);
      return Zero ? BasicBlock::genBinaryInst(
                        this, Input, BinaryInst::NE, Zero)
                  : nullptr;
    }

    if (auto *Boolean = dynamic_cast<ConstBool *>(Input))
      return ConstInt::newConstIntForType(Boolean->getVal() ? 1 : 0,
                                          DestinationInteger);
    if (auto *Integer = dynamic_cast<ConstInt *>(Input))
      return ConstInt::newConstIntForType(Integer->getVal(),
                                          DestinationInteger);
    if (auto *Floating = dynamic_cast<ConstFloat *>(Input))
      return Instruction::foldConstantCast(
          Instruction::FPTOSI, Floating, DestinationInteger);

    if (auto SourceInteger =
            std::dynamic_pointer_cast<INTType>(SourceType)) {
      if (SourceInteger->getBitWidth() <
          DestinationInteger->getBitWidth()) {
        if (SourceInteger->getIsBool())
          return genZextInst(Input, DestinationInteger);
        return genSextInst(Input, DestinationInteger);
      }
      if (SourceInteger->getBitWidth() >
          DestinationInteger->getBitWidth())
        return genTruncInst(Input, DestinationInteger);
      return Input;
    }
    if (SourceType->isFloat())
      return genFPTOSIInst(Input, DestinationInteger);
    return nullptr;
  }

  if (TargetType->isFloat() && SourceType->isInt()) {
    if (auto *Boolean = dynamic_cast<ConstBool *>(Input))
      return ConstFloat::newConstFloat(Boolean->getVal() ? 1.0f : 0.0f);
    if (auto *Integer = dynamic_cast<ConstInt *>(Input))
      return ConstFloat::newConstFloat(
          static_cast<float>(Integer->getVal()));
    auto SourceInteger =
        std::dynamic_pointer_cast<INTType>(SourceType);
    if (SourceInteger && SourceInteger->getIsBool())
      Input = genZextInst(Input, INTType::getInt32Ty());
    return genSITOFPInst(Input);
  }

  if (TargetType->isPointer() && SourceType->isPointer()) {
    auto *Cast = new BitCastInst(Input, TargetType);
    pushBack(Cast);
    return Cast;
  }

  return nullptr;
}

Value *BasicBlock::coerceValueToTypeWithoutBitCast(
    BasicBlock *InsertionBlock, Value *Input,
    const std::shared_ptr<IRType> &TargetType) {
  if (Input == nullptr || TargetType == nullptr)
    return nullptr;
  //1.获得源类型
  const auto SourceType = Input->getIRType();
  if (SourceType == nullptr)
    return nullptr;

  if (SourceType == TargetType)
    return Input;
  if (Input->isUndef())
    return UndefValue::NewUndefValue(TargetType);

  //2.如果目标类型是整数
  if (TargetType->getTypeSystem() == IR_INT) {
    const auto TargetInteger =
        std::dynamic_pointer_cast<INTType>(TargetType);
    if (TargetInteger == nullptr)
      return nullptr;
    //2.1普通浮点常量/整数常量转bool，先按照语义判断是否为0,然后再生成icmp/fcmp生成cond
    if (TargetInteger->getIsBool()) {
      if (auto SourceInteger =
              std::dynamic_pointer_cast<INTType>(SourceType);
          SourceInteger && SourceInteger->getIsBool()) {
        return Input;
      }
      if (auto Constant = dynamic_cast<ConstInt *>(Input))
        return ConstBool::newConstBool(Constant->getVal() != 0);
      if (auto Constant = dynamic_cast<ConstFloat *>(Input))
        return ConstBool::newConstBool(Constant->getVal() != 0.0F);
      if (dynamic_cast<ConstPtr *>(Input) != nullptr)
        return ConstBool::newConstBool(false);

      Value *Zero = nullptr;
      switch (SourceType->getTypeSystem()) {
      case IR_INT:
        if (auto IntegerType =
                std::dynamic_pointer_cast<INTType>(SourceType);
            IntegerType && IntegerType->getIsBool()) {
          Zero = ConstBool::newConstBool(false);
        } else {
          Zero = ConstInt::newConstIntForType(
              0, std::dynamic_pointer_cast<INTType>(SourceType));
        }
        break;
      case IR_FLOAT:
        Zero = ConstFloat::newConstFloat(0.0F);
        break;
      case IR_POINTER:
        Zero = ConstPtr::newConstPtr(SourceType);
        break;
      case IR_ARRAY:
        Zero = new Initializer(SourceType);
        break;
      default:
        return nullptr;
      }
      return BasicBlock::genBinaryInst(
          InsertionBlock, Input, BinaryInst::NE, Zero);
    }

    //2.2bool常量转普通整数，普通整数常量转普通整数，直接转
    if (auto Constant = dynamic_cast<ConstBool *>(Input))
      return ConstInt::newConstIntForType(
          Constant->getVal() ? 1 : 0, TargetInteger);
    if (auto Constant = dynamic_cast<ConstInt *>(Input))
      return ConstInt::newConstIntForType(
          Constant->getVal(), TargetInteger);
    //2.3浮点常量转整数，因为是常量，所以直接转，不会生成FPTOSI指令
    if (auto Constant = dynamic_cast<ConstFloat *>(Input)) {
      Value *Folded = Instruction::foldConstantCast(
          Instruction::FPTOSI, Constant, TargetInteger);
      return Folded;
    }

    //2.4运行期浮点转整数：生成FPTOSI指令
    if (SourceType->getTypeSystem() == IR_FLOAT) {
      if (InsertionBlock == nullptr)
        return nullptr;
      return InsertionBlock->genFPTOSIInst(Input, TargetInteger);
    }
    //2.5运行期整数之间互转
    if (SourceType->getTypeSystem() == IR_INT) {
      const auto SourceInteger =
          std::dynamic_pointer_cast<INTType>(SourceType);
      if (!SourceInteger)
        return nullptr;
      if (InsertionBlock == nullptr)
        return nullptr;
      //源位宽<目标位宽
      if (SourceInteger->getBitWidth() <
          TargetInteger->getBitWidth()) {
        //源位宽为bool，目标位宽为普通整数，生成zext零扩展
        if (SourceInteger->getIsBool())
          return InsertionBlock->genZextInst(Input, TargetInteger);
        //源位宽为普通整数，目标位宽为普通整数，生成sext符号扩展
        return InsertionBlock->genSextInst(Input, TargetInteger);
      }
      //源位宽>目标位宽，生成trunc截断
      if (SourceInteger->getBitWidth() >
          TargetInteger->getBitWidth())
        return InsertionBlock->genTruncInst(Input, TargetInteger);
      return Input;
    }
  }
  //3.如果目标类型是浮点数
  if (TargetType->getTypeSystem() == IR_FLOAT) {
    //3.1常量之间的互转依然不生成任何指令
    if (auto Constant = dynamic_cast<ConstBool *>(Input))
      return ConstFloat::newConstFloat(
          Constant->getVal() ? 1.0F : 0.0F);
    if (auto Constant = dynamic_cast<ConstInt *>(Input))
      return ConstFloat::newConstFloat(
          static_cast<float>(Constant->getVal()));
    if (auto Constant = dynamic_cast<ConstFloat *>(Input))
      return Constant;

    //3.2运行期整数转浮点数，生成SITOFP指令
    if (SourceType->getTypeSystem() == IR_INT) {
      const auto SourceInteger =
          std::dynamic_pointer_cast<INTType>(SourceType);
      if (InsertionBlock == nullptr)
        return nullptr;
      //源位宽为bool，生成zext零扩展到i32，再生成SITOFP指令
      if (SourceInteger && SourceInteger->getIsBool())
        Input = InsertionBlock->genZextInst(
            Input, INTType::getInt32Ty());
      return InsertionBlock->genSITOFPInst(Input);
    }
    if (SourceType->getTypeSystem() == IR_FLOAT)
      return Input;
  }

  //指针与指针互转这里不接受
  if (TargetType->getTypeSystem() == IR_POINTER &&
      SourceType->getTypeSystem() == IR_POINTER) {
    return Input;
  }

  return nullptr;
}

bool BinaryInst::isIntegerOnlyOp(BinaryInst::Operation op) {
  switch (op) {
  case BinaryInst::UDIV:
  case BinaryInst::UREM:
  case BinaryInst::AND:
  case BinaryInst::OR:
  case BinaryInst::XOR:
  case BinaryInst::SHL:
  case BinaryInst::LSHR:
  case BinaryInst::ASHR:
    return true;
  default:
    return false;
  }
}

Instruction::InstType BinaryInst::getInstType(BinaryInst::Operation op,
                                              TypeSystem lhsTy) {
  switch (op) {
  case BinaryInst::ADD:
    return lhsTy == IR_FLOAT ? Instruction::FADD : Instruction::ADD;
  case BinaryInst::SUB:
    return lhsTy == IR_FLOAT ? Instruction::FSUB : Instruction::SUB;
  case BinaryInst::MUL:
    return lhsTy == IR_FLOAT ? Instruction::FMUL : Instruction::MUL;
  case BinaryInst::DIV:
    return lhsTy == IR_FLOAT ? Instruction::FDIV : Instruction::SDIV;
  case BinaryInst::UDIV:
    return Instruction::UDIV;
  case BinaryInst::MOD:
    return lhsTy == IR_FLOAT ? Instruction::FREM : Instruction::SREM;
  case BinaryInst::UREM:
    return Instruction::UREM;
  case BinaryInst::AND:
    return Instruction::AND;
  case BinaryInst::OR:
    return Instruction::OR;
  case BinaryInst::XOR:
    return Instruction::XOR;
  case BinaryInst::SHL:
    return Instruction::SHL;
  case BinaryInst::LSHR:
    return Instruction::LSHR;
  case BinaryInst::ASHR:
    return Instruction::ASHR;
  case BinaryInst::E:
  case BinaryInst::NE:
  case BinaryInst::G:
  case BinaryInst::GE:
  case BinaryInst::L:
  case BinaryInst::LE:
    return lhsTy == IR_FLOAT ? Instruction::FCMP : Instruction::ICMP;
  default:
    return Instruction::BinaryUnknown;
  }
}

Function& Module::newFunction(TypeSystem returnType, std::string functionName) {
    return newFunction(IRType::NewTypeByTypeSystem(returnType),
                       std::move(functionName));
}

Function& Module::newFunction(std::shared_ptr<IRType> returnType,
                              std::string functionName) {
    auto *function = new Function(std::move(returnType), std::move(functionName));
    functions.emplace_back(function);
    return *function;
}

std::vector<std::unique_ptr<Function>>& Module::getFunctions(){
    return functions;
}

Function* Module::getMainFunction(){
    for(auto& i: functions){
        if(i->getIdent() == "main") return i.get();
    }
    return nullptr;
}

Function *Module::findFunctionByName(const std::string &functionName) const {
  // 每次基于模块当前拥有的函数查找，删除函数后无需同步额外索引。
  for (const auto &function : functions) {
    if (function && function->getIdent() == functionName)
      return function.get();
  }
  return nullptr;
}

Module* Module::getActiveModule() {
  return ModuleIR.get();
}

FunctionCallGraph Module::buildDirectCallGraph() const {
  // 遍历该函数中所有Call指令，记录对应的函数
  FunctionCallGraph Graph;
  for (const auto &FunctionPtr : functions) {
    Function *F = FunctionPtr.get();
    if (!F) continue;
    auto &Callees = Graph[F];
    for (BasicBlock *BB : *F) {
      if (!BB) continue;
      for (Instruction *I : *BB) {
        auto *CI = dynamic_cast<CallInst *>(I);
        Function *Callee = CI ? CI->getDirectCallee() : nullptr;
        if (Callee) Callees.push_back(Callee);
      }
    }
  }
  return Graph;
}

std::unordered_set<Function *> Module::collectReachableFunctions(
    Function *Entry, const FunctionCallGraph &CallGraph) {
  //从传入函数开始，在其调用图上执行BFS广搜，记录可达的函数集
  std::unordered_set<Function *> Reachable;
  if (!Entry) return Reachable;

  // 从入口函数广度遍历调用边，去重集合同时充当访问标记
  std::queue<Function *> WorkList;
  Reachable.insert(Entry);
  WorkList.push(Entry);
  while (!WorkList.empty()) {
    Function *Current = WorkList.front();
    WorkList.pop();
    auto It = CallGraph.find(Current);
    if (It == CallGraph.end()) continue;
    for (Function *Callee : It->second) {
      if (Callee && Reachable.insert(Callee).second)
        WorkList.push(Callee);
    }
  }
  return Reachable;
}

bool Module::isFunctionRecursive(
    Function *Root, const FunctionCallGraph &CallGraph) {
  if (!Root) return false;
  auto RootIt = CallGraph.find(Root);
  if (RootIt == CallGraph.end()) return false;

  // 以 Root 的直接后继而不是 Root 本身为搜索根，要求返回 Root 的路径
  // 至少包含一条调用边；同时覆盖直接自递归和跨函数间接递归。
  return isReachableFromRootsCustomSucc<Function>(
      RootIt->second, Root,
      [&CallGraph](Function *Current) -> std::vector<Function *> {
        auto It = CallGraph.find(Current);
        return It == CallGraph.end() ? std::vector<Function *>{}
                                    : It->second;
      });
}

std::vector<std::unique_ptr<Variable>>& Module::getGlobalVar(){return globalvars;}

void Module::dumpIR() {
    for(auto& i : globalvars) i->dumpIR();
    for(auto& i : functions) i->dumpIR();
}

Function::Function(std::shared_ptr<IRType> returnType,
                   std::string ident)
    : Value(std::move(returnType), std::move(ident)) {
    if (!getIRType() || getIRType()->isArray())
      throw std::invalid_argument("函数返回类型不能为空或数组类型");
    // 每个函数至少拥有一个入口基本块。
    pushBack(new BasicBlock());
}

void Function::pushBlock(BasicBlock* bb){
    pushBack(bb);
}

AllocaInst *Function::pushParamWithStackSlot(Param *parameterValue) {
    if (!parameterValue || !front())
        return nullptr;

    // 形参归函数签名所有，入口槽位直接返回给 IRGenerator 建立源码绑定。
    auto *parameterSlot = new AllocaInst(parameterValue->getIRType());
    auto *initialStore = new StoreInst(parameterValue, parameterSlot);
    front()->pushFront(parameterSlot);
    front()->pushBack(initialStore);
    params.emplace_back(parameterValue);
    return parameterSlot;
}

std::vector<std::unique_ptr<Param>>& Function::getParams(){return params;}
const std::vector<std::unique_ptr<Param>>& Function::getParams() const {
  return params;
}

size_t Function::getParamCount() const { return params.size(); }

Param *Function::getParam(size_t Index) const {
  return Index < params.size() ? params[Index].get() : nullptr;
}

int Function::getParamIndex(const Value *Param) const {
  if (!Param) return -1;
  for (size_t I = 0; I < params.size(); ++I) {
    if (params[I].get() == Param) return static_cast<int>(I);
  }
  return -1;
}

bool Function::eraseParam(size_t Index) {
  if (Index >= params.size()) return false;
  params.erase(params.begin() + static_cast<std::ptrdiff_t>(Index));
  return true;
}

std::vector<CallInst *> Function::getDirectCallSites() const {
  std::vector<CallInst *> Calls;
  for (Use *U : getUses()) {
    auto *CI = dynamic_cast<CallInst *>(U->getUser());
    if (CI && CI->isCalleeUse(U)) Calls.push_back(CI);
  }
  return Calls;
}

bool Function::hasOnlyDirectCallUses() const {
  return static_cast<int>(getDirectCallSites().size()) == countUses();
}

bool Function::hasReservedSignature() const {
  return ident == "main" || BuildInFunction::isKnownRuntimeName(ident) ||
         const_cast<Function *>(this)->front() == nullptr;
}

bool Function::hasBlockIdent(const std::string &Name) const {
  for (const BasicBlock *BB : *this) {
    if (BB && BB->getIdent() == Name) return true;
  }
  return false;
}

std::string Function::makeUniqueBlockIdent(const std::string &Base) const {
  if (!hasBlockIdent(Base)) return Base;
  for (int Suffix = 1;; ++Suffix) {
    std::string Candidate = Base + "." + std::to_string(Suffix);
    if (!hasBlockIdent(Candidate)) return Candidate;
  }
}

void Function::collectReachableBlocks(
    std::unordered_set<BasicBlock *> &Reachable) {
  Reachable.clear();
  if (this->size() == 0 || !front()) return;

  // 从入口块沿终结指令深度遍历，同一块仅调度一次。
  std::vector<BasicBlock *> WorkList;
  BasicBlock *Entry = front();
  WorkList.push_back(Entry);
  Reachable.insert(Entry);

  while (!WorkList.empty()) {
    BasicBlock *Current = WorkList.back();
    WorkList.pop_back();
    if (!Current) continue;

    for (BasicBlock *Succ : Current->getSuccessors()) {
      if (Succ && Reachable.insert(Succ).second) {
        WorkList.push_back(Succ);
      }
    }
  }
}

// 返回限定块集合中非空且没有CFG后继的真实出口块。
std::vector<BasicBlock *> Function::findExitBlocks(
    const std::unordered_set<BasicBlock *> &Blocks) const {
  std::vector<BasicBlock *> Exits;
  for (BasicBlock *BB : *this) {
    if (!BB || !Blocks.count(BB) || BB->size() == 0) continue;
    if (BB->getSuccessors().empty()) Exits.push_back(BB);
  }
  return Exits;
}

// 将限定块集合内的每条正向边 From->To 记录为反向邻接 To->From。
std::unordered_map<BasicBlock *, std::vector<BasicBlock *>>
Function::buildReverseCFG(
    const std::unordered_set<BasicBlock *> &Blocks) const {
  std::unordered_map<BasicBlock *, std::vector<BasicBlock *>> ReverseCFG;
  for (BasicBlock *BB : *this) {
    if (!BB || !Blocks.count(BB)) continue;
    for (BasicBlock *Succ : BB->getSuccessors()) {
      if (Succ && Blocks.count(Succ)) ReverseCFG[Succ].push_back(BB);
    }
  }
  return ReverseCFG;
}

void Function::dropAllReferences() {
  // 先解除指令间的操作数关系，避免后续批量销毁时递归影响其他指令
  for (BasicBlock *BB : *this) {
    if (!BB) continue;
    for (Instruction *I : *BB) {
      if (!I) continue;
      I->clearOperands();
    }
  }
}

void Function::dumpIR(){
  std::cout << "define ";
  this->getIRType()->toString();
  std::cout << " @" << ident << "(";
  for (auto &i : params) {
    i->getIRType()->toString();
    std::cout << " %" << i->getIdent();
    if (i.get() != params.back().get())
      std::cout << ", ";
  }
  std::cout << "){\n";
  for (auto i : (*this))
    i->dumpIR();
  std::cout << "}\n";
}

BuildInFunction::BuildInFunction(std::shared_ptr<IRType> t,std::string ident):Value(t){
  this->ident = ident;
  if (this->ident == "starttime" || this->ident == "stoptime")
    this->ident = "_sysy_" + this->ident;
}

std::optional<BuildInFunction::Signature>
BuildInFunction::getSignature(const std::string &Name) {
  const auto Void = VOIDType::NewVoid();
  const auto I1 = INTType::getBoolTy();
  const auto I8 = INTType::getInt8Ty();
  const auto I32 = INTType::getInt32Ty();
  const auto I64 = INTType::getInt64Ty();
  const auto Float = FLOATType::NewFloat();
  const auto I8Pointer = POINTERType::NewPointer(I8);
  const auto I32Pointer = POINTERType::NewPointer(I32);
  const auto FloatPointer = POINTERType::NewPointer(Float);

  auto makeSignature =
      [](std::shared_ptr<IRType> ReturnType,
         std::vector<std::shared_ptr<IRType>> ParameterTypes,
         std::vector<bool> OpaquePointerParameters = {}) {
        Signature Result;
        Result.returnType = std::move(ReturnType);
        Result.parameterTypes = std::move(ParameterTypes);
        Result.opaquePointerParameters =
            std::move(OpaquePointerParameters);
        Result.opaquePointerParameters.resize(
            Result.parameterTypes.size(), false);
        return Result;
      };

  if (Name == "getint" || Name == "getch")
    return makeSignature(I32, {});
  if (Name == "getfloat")
    return makeSignature(Float, {});
  if (Name == "getarray")
    return makeSignature(I32, {I32Pointer});
  if (Name == "getfarray")
    return makeSignature(I32, {FloatPointer});
  if (Name == "putint" || Name == "putch")
    return makeSignature(Void, {I32});
  if (Name == "putfloat")
    return makeSignature(Void, {Float});
  if (Name == "putarray")
    return makeSignature(Void, {I32, I32Pointer});
  if (Name == "putfarray")
    return makeSignature(Void, {I32, FloatPointer});
  if (Name == "starttime" || Name == "stoptime" ||
      Name == "_sysy_starttime" || Name == "_sysy_stoptime")
    return makeSignature(Void, {I32});

  const bool IsMemcpy = isMemcpyName(Name);
  const bool IsMemmove = isMemmoveName(Name);
  const bool IsMemset = isMemsetName(Name);
  if (IsMemcpy || IsMemmove || IsMemset) {
    const bool HasI64Length =
        Name.size() >= 4 &&
        Name.compare(Name.size() - 4, 4, ".i64") == 0;
    const bool HasI32Length =
        Name.size() >= 4 &&
        Name.compare(Name.size() - 4, 4, ".i32") == 0;
    if (!HasI32Length && !HasI64Length)
      return std::nullopt;
    const auto LengthType = HasI64Length ? I64 : I32;
    if (IsMemset)
      return makeSignature(
          Void, {I8Pointer, I8, LengthType, I1},
          {true, false, false, false});
    return makeSignature(
        Void, {I8Pointer, I8Pointer, LengthType, I1},
        {true, true, false, false});
  }

  return std::nullopt;
}

bool BuildInFunction::dumpDeclaration(const std::string &Name) {
  auto Signature = getSignature(Name);
  if (!Signature || !Signature->returnType)
    return false;

  Signature->returnType->toString();
  const std::string PrintedName =
      Name == "starttime" || Name == "stoptime"
          ? "_sysy_" + Name
          : Name;
  std::cout << " @" << PrintedName << "(";
  for (std::size_t Index = 0;
       Index < Signature->parameterTypes.size(); ++Index) {
    if (Index != 0)
      std::cout << ", ";
    if (Index < Signature->opaquePointerParameters.size() &&
        Signature->opaquePointerParameters[Index]) {
      std::cout << "ptr";
    } else {
      Signature->parameterTypes[Index]->toString();
    }
  }
  std::cout << ")\n";
  return true;
}

void BuildInFunction::dumpDefaultDeclarations() {
  static const std::vector<std::string> RuntimeNames = {
      "getint", "getch", "getfloat", "getarray", "getfarray",
      "putint", "putch", "putfloat", "putarray", "putfarray",
      "starttime", "stoptime"};
  static const std::vector<std::string> MemoryIntrinsicNames = {
      "llvm.memcpy.p0.p0.i32", "llvm.memmove.p0.p0.i32",
      "llvm.memset.p0.i32", "llvm.memcpy.p0.p0.i64",
      "llvm.memmove.p0.p0.i64", "llvm.memset.p0.i64"};

  for (const std::string &Name : RuntimeNames) {
    std::cout << "declare ";
    dumpDeclaration(Name);
  }
  std::cout << "\n";
  for (const std::string &Name : MemoryIntrinsicNames) {
    std::cout << "declare ";
    dumpDeclaration(Name);
  }
  std::cout << "\n";
}

BuildInFunction *BuildInFunction::genBuildInFunction(std::string ident) {
  static std::map<std::string, BuildInFunction *> mp;
  auto gt = [&ident]() -> std::shared_ptr<IRType> {
    if (auto Signature = getSignature(ident))
      return Signature->returnType;
    return nullptr;
  };
  if (mp.find(ident) == mp.end()) {
    auto returnType = gt();
    if (!returnType) return nullptr;
    mp[ident] = new BuildInFunction(returnType, ident);
  }
  return mp[ident];
}

bool BuildInFunction::isSupportedName(const std::string &Name) {
  return getSignature(Name).has_value();
}

bool BuildInFunction::isMemcpyName(const std::string &Name) {
  return Name.find("memcpy") != std::string::npos;
}

bool BuildInFunction::isMemmoveName(const std::string &Name) {
  return Name.find("memmove") != std::string::npos;
}

bool BuildInFunction::isMemsetName(const std::string &Name) {
  return Name.find("memset") != std::string::npos;
}

bool BuildInFunction::isTimingName(const std::string &Name) {
  return Name == "starttime" || Name == "stoptime" ||
         Name == "_sysy_starttime" || Name == "_sysy_stoptime";
}

bool BuildInFunction::isScalarIOName(const std::string &Name) {
  static const std::unordered_set<std::string> Names = {
      "getint", "getch", "getfloat", "putint", "putch", "putfloat"};
  return Names.count(Name) != 0;
}

bool BuildInFunction::isScalarOutputName(const std::string &Name) {
  static const std::unordered_set<std::string> Names = {
      "putint", "putch", "putfloat", "putf"};
  return Names.count(Name) != 0;
}

bool BuildInFunction::isArrayInputName(const std::string &Name) {
  return Name == "getarray" || Name == "getfarray";
}

bool BuildInFunction::isArrayOutputName(const std::string &Name) {
  return Name == "putarray" || Name == "putfarray" || Name == "putf";
}

bool BuildInFunction::isOutputName(const std::string &Name) {
  return isScalarOutputName(Name) || isArrayOutputName(Name);
}

const char *BuildInFunction::getDefaultMemcpyName() {
  return "llvm.memcpy.p0.p0.i32";
}

const char *BuildInFunction::getDefaultMemmoveName() {
  return "llvm.memmove.p0.p0.i32";
}

const char *BuildInFunction::getDefaultMemsetName() {
  return "llvm.memset.p0.i32";
}

bool BuildInFunction::isKnownRuntimeName(const std::string &Name) {
  static const std::unordered_set<std::string> SysyLibNames = {
      "getint", "getch", "getfloat", "getarray", "getfarray",
      "putint", "putch", "putfloat", "putarray", "putfarray", "putf",
      "starttime", "stoptime", "_sysy_starttime", "_sysy_stoptime"};

  if (SysyLibNames.count(Name) || isSupportedName(Name)) return true;
  if (Name.find("_sysy_") != std::string::npos) return true;
  if (isMemcpyName(Name)) return true;
  if (isMemmoveName(Name)) return true;
  if (isMemsetName(Name)) return true;
  if (Name.find("llvm.") == 0) return true;
  return false;
}

BasicBlock::BasicBlock():Value(VOIDType::NewVoid()){};

void BasicBlock::dumpIR(){
  std::cout << this->getIdent() << ":\n";
  for (auto i : (*this)) {
    std::cout << "  ";
    i->dumpIR();
  }

}
//获取当前基本块的所有后继基本块
std::vector<BasicBlock*> BasicBlock::getSuccessors() {
    std::vector<BasicBlock*> succs;
    if (this->size() == 0) return succs;
    auto *Br = dynamic_cast<BrInst *>(back());
    if (!Br) return succs;
    for (unsigned I = 0; I < Br->getSuccessorCount(); ++I) {
        if (BasicBlock *Succ = Br->getSuccessor(I)) succs.push_back(Succ);
    }
    return succs;
}
//获取当前基本块的所有前驱基本块
std::vector<BasicBlock*> BasicBlock::getPredecessors() {
    std::vector<BasicBlock*> preds;
    Function* F = this->ChainNode<Function, BasicBlock>::getParent();
    if (!F) return preds;

    for (auto bb : *F) {
        auto succs = bb->getSuccessors();
        for (BasicBlock* succ : succs) {
            if (succ == this) {
                preds.push_back(bb);
                break;
            }
        }
    }
    return preds;
}

Instruction *BasicBlock::getTerminator() {
    if (size() == 0) return nullptr;
    Instruction *Term = back();
    return dynamic_cast<BrInst *>(Term) || dynamic_cast<RetInst *>(Term)
               ? Term
               : nullptr;
}

bool BasicBlock::hasMultipleDistinctSuccessors() {
    auto Successors = getSuccessors();
    if (Successors.size() < 2) return false;
    for (size_t I = 1; I < Successors.size(); ++I) {
        if (Successors[I] != Successors[0]) return true;
    }
    return false;
}

void BasicBlock::genRetInst(Value* retVal){
    if (!retVal || !getParent() || !getParent()->getIRType())
      return;
    if (getParent()->getIRType()->isVoid())
      return;
    //这一步是为了确保返回值类型与函数返回类型一致，即用sitofp、fptosi进行隐式类型转换（float<->int）
    retVal = coerceValueToType(retVal, getParent()->getIRType());
    if (!retVal)
      return;
    auto inst = new RetInst(retVal);
    pushBack(inst);
}

void BasicBlock::genRetInst(){
    if (!getParent() || !getParent()->getIRType() ||
        !getParent()->getIRType()->isVoid())
      return;
    auto inst = new RetInst();
    pushBack(inst);
}

void BasicBlock::genCondInst(Value* cond,BasicBlock* isTrue,BasicBlock* isFalse){
   auto inst = new BrInst(cond,isTrue,isFalse);
   pushBack(inst);
}

void BasicBlock::genUnCondInst(BasicBlock *block) {
  auto inst = new BrInst(block);
  pushBack(inst);
}

AllocaInst* BasicBlock::genAllocaInst(std::shared_ptr<IRType> allocatedType) {
    auto *allocation = new AllocaInst(std::move(allocatedType));
    getParent()->front()->pushFront(allocation);
    return allocation;
}

Value* BasicBlock::genLoadInst(Value* v){
    auto inst = new LoadInst(v);
    pushBack(inst);
    return inst;
}

void BasicBlock::genStoreInst(Value* val,Value* ptr){
    if (!val || !ptr) return;
    auto point = std::dynamic_pointer_cast<POINTERType>(ptr->getIRType());
    if (!point || !point->getPointerType() || !val->getIRType()) return;
    //依旧包括了类型转换
    val = coerceValueToType(val, point->getPointerType());
    if (!val) return;
    auto inst = new StoreInst(val,ptr);
    this->pushBack(inst);
}

Value* BasicBlock::genGepInst(Value* ptr){
    auto inst = new GetElementPtrInst(ptr);
    pushBack(inst);
    return inst;
}

Value* BasicBlock::genExtractElementInst(Value* vector, Value* index) {
    auto *inst = new ExtractElementInst(vector, index);
    pushBack(inst);
    return inst;
}

Value* BasicBlock::genInsertElementInst(
    Value* vector, Value* element, Value* index) {
    auto *inst = new InsertElementInst(vector, element, index);
    pushBack(inst);
    return inst;
}

Value* BasicBlock::genShuffleVectorInst(
    Value* first, Value* second, const std::vector<int>& mask) {
    auto *inst = new ShuffleVectorInst(first, second, mask);
    pushBack(inst);
    return inst;
}

Value* BasicBlock::genBinaryInst(Value* A, BinaryInst::Operation op, Value* B){
  if (!A || !B || !A->getIRType() || !B->getIRType())
    return nullptr;
  // 处理向量类型的二元操作，要求两个操作数都是向量且形状相同
  if (A->getIRType()->isVector() || B->getIRType()->isVector()) {
    if (!A->getIRType()->isVector() || !B->getIRType()->isVector() ||
        !A->getIRType()->hasSameShape(*B->getIRType()))
      return nullptr;
    auto *inst = new BinaryInst(A, op, B);
    pushBack(inst);
    return inst;
  }

  bool AIsInteger = A->getIRType()->isInt();
  bool BIsInteger = B->getIRType()->isInt();
  bool AIsFloat = A->getIRType()->isFloat();
  bool BIsFloat = B->getIRType()->isFloat();

  // 只允许整数的操作先把浮点值转换为 SysY 的 i32
  if (BinaryInst::isIntegerOnlyOp(op)) {
    if (AIsFloat) {
      A = genFPTOSIInst(A, INTType::getInt32Ty());
      AIsInteger = true;
      AIsFloat = false;
    }
    if (BIsFloat) {
      B = genFPTOSIInst(B, INTType::getInt32Ty());
      BIsInteger = true;
      BIsFloat = false;
    }
    if (!AIsInteger || !BIsInteger)
      return nullptr;
  } else if ((AIsInteger && BIsFloat) ||
             (AIsFloat && BIsInteger)) {
    if (AIsInteger)
      A = genSITOFPInst(A);
    else
      B = genSITOFPInst(B);
    AIsInteger = BIsInteger = false;
    AIsFloat = BIsFloat = true;
  }

  if (AIsInteger && BIsInteger) {
    auto AType = std::dynamic_pointer_cast<INTType>(A->getIRType());
    auto BType = std::dynamic_pointer_cast<INTType>(B->getIRType());
    if (!AType || !BType)
      return nullptr;
    const unsigned CommonWidth =
        std::max(AType->getBitWidth(), BType->getBitWidth());
    auto CommonType = INTType::get(CommonWidth);
    auto extendToCommon = [this, &CommonType](Value *Input) -> Value * {
      auto InputType =
          std::dynamic_pointer_cast<INTType>(Input->getIRType());
      if (!InputType ||
          InputType->getBitWidth() == CommonType->getBitWidth())
        return Input;
      if (InputType->getIsBool())
        return genZextInst(Input, CommonType);
      return genSextInst(Input, CommonType);
    };
    A = extendToCommon(A);
    B = extendToCommon(B);
  }

  if (!A || !B || !A->getIRType() || !B->getIRType() ||
      !A->getIRType()->hasSameShape(*B->getIRType()))
    return nullptr;

  auto *inst = new BinaryInst(A, op, B);
  pushBack(inst);
  return inst;
}

Value* BasicBlock::genBinaryInst(BasicBlock* bb, Value* left,
                                 BinaryInst::Operation op, Value* right){
  if (!left || !right) return nullptr;
  if ((left->getIRType() && left->getIRType()->isVector()) ||
      (right->getIRType() && right->getIRType()->isVector())) {
    return bb ? bb->genBinaryInst(left, op, right) : nullptr;
  }
  if (left->isConst() && right->isConst()) {
    Value *A = left;
    Value *B = right;

    if (!BinaryInst::isIntegerOnlyOp(op) &&
        (IRType::isFloat(A->getIRType()) ||
         IRType::isFloat(B->getIRType()))) {
      float AV = 0.0f;
      float BV = 0.0f;
      if (!A->tryGetNumericFloatConst(AV) ||
          !B->tryGetNumericFloatConst(BV))
        return nullptr;
      A = ConstFloat::newConstFloat(AV);
      B = ConstFloat::newConstFloat(BV);
    }

    if (Value *Folded = BinaryInst::foldConstants(op, A, B, true))
      return Folded;
    if (IRType::isFloat(A->getIRType()))
      if (Value *Folded = BinaryInst::foldConstants(op, A, B, false))
        return Folded;

    return nullptr;
  } else {
    if (!bb) return nullptr;
    return bb->genBinaryInst(left, op, right);
  }
}

Value* BasicBlock::genSITOFPInst(Value* x){
    auto inst = new SITOFPInst(x);
    pushBack(inst);
    return inst;
}

Value* BasicBlock::genSITOFPInst(
    Value* x, std::shared_ptr<IRType> targetType) {
    auto *inst = new SITOFPInst(x, std::move(targetType));
    pushBack(inst);
    return inst;
}

Value* BasicBlock::genFPTOSIInst(Value* x){
    auto inst = new FPTOSIInst(x);
    pushBack(inst);
    return inst;
}

Value* BasicBlock::genFPTOSIInst(
    Value* x, std::shared_ptr<IRType> targetType) {
    auto *inst = new FPTOSIInst(x, std::move(targetType));
    pushBack(inst);
    return inst;
}

Value* BasicBlock::genZextInst(Value* x){
    auto inst = new ZextInst(x);
    pushBack(inst);
    return inst;
}

Value* BasicBlock::genZextInst(
    Value* x, std::shared_ptr<IRType> targetType) {
  auto *inst = new ZextInst(x, std::move(targetType));
  pushBack(inst);
  return inst;
}

Value* BasicBlock::genTruncInst(Value* x){
  auto inst = new TruncInst(x);
  pushBack(inst);
  return inst;
}

Value* BasicBlock::genTruncInst(
    Value* x, std::shared_ptr<IRType> targetType) {
  auto *inst = new TruncInst(x, std::move(targetType));
  pushBack(inst);
  return inst;
}

Value* BasicBlock::genSextInst(Value* x){
  auto inst = new SextInst(x);
  pushBack(inst);
  return inst;
}

Value* BasicBlock::genSextInst(
    Value* x, std::shared_ptr<IRType> targetType) {
  auto *inst = new SextInst(x, std::move(targetType));
  pushBack(inst);
  return inst;
}

BasicBlock* BasicBlock::genBlock(){
    BasicBlock* bb = new BasicBlock();
    getParent()->pushBlock(bb);
    return bb;
}

BasicBlock* BasicBlock::genBlock(std::string ident){
    BasicBlock* bb = new BasicBlock();
    bb->ident += ident;
    getParent()->pushBlock(bb);
    return bb;
}

bool BasicBlock::moveInstructionsAfter(Instruction *Position,
                                       BasicBlock *Destination) {
  if (!Position || !Destination || Position->getParent() != this ||
      Destination == this)
    return false;

  // 在搬移前保存原终结指令的后继，然后按原顺序转移后半段指令。
  Instruction *OldTerminator = getTerminator();
  auto OldSuccessors = getSuccessors();
  for (Instruction *I = Position->getNext(); I;) {
    Instruction *Next = I->getNext();
    I->eraseFromParent();
    Destination->pushBack(I);
    I = Next;
  }
  // 终结指令被搬走时，其后继 PHI 的前驱也必须改为目标块。
  if (OldTerminator && OldTerminator->getParent() == Destination) {
    std::unordered_set<BasicBlock *> Updated;
    for (BasicBlock *Succ : OldSuccessors) {
      if (!Succ || !Updated.insert(Succ).second) continue;
      replaceIncomingPredInSuccPhis(Succ, this, Destination);
    }
  }
  return true;
}

BasicBlock *BasicBlock::splitBlockAfter(Instruction *Position) {
  if (!Position || Position->getParent() != this) return nullptr;
  Function *F = getParent();
  if (!F) return nullptr;

  // 新建延续块并让它接管切分点之后的全部指令。
  auto *Continuation = new BasicBlock();
  F->pushBlock(Continuation);
  if (!moveInstructionsAfter(Position, Continuation)) {
    delete Continuation;
    return nullptr;
  }
  return Continuation;
}

Value* BasicBlock::genCallInst(std::string ident, std::vector<Value*> args) {
  // 运行库调用按固定签名转换标量实参，然后注册并生成内建调用。
  if (BuildInFunction::isSupportedName(ident)) {
    auto Signature = BuildInFunction::getSignature(ident);
    if (!Signature ||
        args.size() != Signature->parameterTypes.size()) {
      return nullptr;
    }

    for (std::size_t Index = 0;
         Index < Signature->parameterTypes.size(); ++Index) {
      if (!args[Index] || !args[Index]->getIRType())
        return nullptr;
      const bool IsOpaquePointer =
          Index < Signature->opaquePointerParameters.size() &&
          Signature->opaquePointerParameters[Index];
      if (IsOpaquePointer) {
        if (!args[Index]->getIRType()->isPointer())
          return nullptr;
        continue;
      }
      Value *Converted = coerceValueToType(
          args[Index], Signature->parameterTypes[Index]);
      if (!Converted)
        return nullptr;
      args[Index] = Converted;
    }

    if (ModuleIR)
      ModuleIR->registerBuiltin(ident);
    auto *tmp =
        new CallInst(BuildInFunction::genBuildInFunction(ident), args);
    pushBack(tmp);
    return tmp;
  }

  // 用户函数要求实参数量一致；指针和数组保留原类型，仅标量整数与浮点数相互转换。
  if (auto *func = ModuleIR->findFunctionByName(ident)) {
    auto &params = func->getParams();
    if (args.size() != params.size()) return nullptr;
    auto i = args.begin();
    for (auto j = params.begin(); j != params.end(); j++, i++) {
      auto &ii = *i;
      auto jj = j->get();
      if (!ii || !jj || !ii->getIRType() || !jj->getIRType()) return nullptr;
      if (!jj->getIRType()->hasSameShape(*ii->getIRType())) {
        auto a = ii->getIRType()->getTypeSystem();
        auto b = jj->getIRType()->getTypeSystem();
        // 向量调用边界不做广播、lane 变化或隐式元素转换。
        if (a == IR_VECTOR || b == IR_VECTOR)
          return nullptr;
        // 复合类型必须在调用边界完全同形。
        if (b == IR_POINTER || a == IR_POINTER ||
            a == IR_ARRAY || b == IR_ARRAY)
          return nullptr;
        // 标量实参按形参类型在整数和浮点数之间转换。
        if ((a != IR_INT && a != IR_FLOAT) ||
            (b != IR_INT && b != IR_FLOAT))
          return nullptr;
        if (b == IR_FLOAT)
          ii = genSITOFPInst(ii);
        else
          ii = genFPTOSIInst(ii);
      }
    }
    auto inst = new CallInst(func, args);
    pushBack(inst);
    return inst;
  }
  return nullptr;
}

bool BasicBlock::hasTerminator(){
    if (auto data = dynamic_cast<BrInst*>(back()))
    return 1;
  else if (auto data = dynamic_cast<RetInst *>(back()))
    return 1;
  else
    return 0;
}

bool BasicBlock::insertBeforeTerminator(Instruction *I) {
  if (!I) return false;
  for (auto It = begin(); It != end(); ++It) {
    Instruction *Current = *It;
    if (!Current) break;
    if (Current->instType == Instruction::BR ||
        Current->instType == Instruction::RET) {
      It.insertBefore(I);
      return true;
    }
  }
  pushBack(I);
  return true;
}

bool BasicBlock::insertAfterPhiNodes(Instruction *I) {
  if (!I) return false;
  for (auto It = begin(); It != end(); ++It) {
    if (!dynamic_cast<PhiInst *>(*It)) {
      It.insertBefore(I);
      return true;
    }
  }
  pushBack(I);
  return true;
}

bool BasicBlock::insertBeforeTerminator(BasicBlock *BB, Instruction *I) {
  return BB && BB->insertBeforeTerminator(I);
}

bool BasicBlock::insertAfterPhiNodes(BasicBlock *BB, Instruction *I) {
  return BB && BB->insertAfterPhiNodes(I);
}

void BasicBlock::clear(){
    ChainList<BasicBlock,Instruction>::clear();
}

Function* Instruction::getUserFunction(Use* U) {
  if (!U) return nullptr;
  auto* I = dynamic_cast<Instruction*>(U->getUser());
  if (!I) return nullptr;
  return I->getParentFunction();
}

Function *Instruction::getParentFunction() {
  return getParentFunction(this);
}

Function *Instruction::getParentFunction(Instruction *I) {
  if (!I) return nullptr;
  BasicBlock *BB = I->getParent();
  if (!BB) return nullptr;
  return BB->getParent();
}

bool Instruction::insertBefore(Instruction *Pos, Instruction *NewI) {
  if (!Pos || !NewI || !Pos->getParent()) return false;
  BasicBlock::Iterator It(Pos);
  It.insertBefore(NewI);
  return true;
}

bool Instruction::insertAfter(Instruction *Pos, Instruction *NewI) {
  if (!Pos || !NewI || !Pos->getParent()) return false;
  BasicBlock::Iterator It(Pos);
  It.insertAfter(NewI);
  return true;
}

bool Instruction::insertBefore(Instruction *NewI) {
  return insertBefore(this, NewI);
}

bool Instruction::insertAfter(Instruction *NewI) {
  return insertAfter(this, NewI);
}

bool Instruction::isInBlock(const BasicBlock *BB) const {
  return BB && const_cast<Instruction *>(this)->getParent() == BB;
}

bool Instruction::comesBefore(const Instruction *Other) const {
  if (!Other || this == Other) return false;
  BasicBlock *BB = const_cast<Instruction *>(this)->getParent();
  if (!BB || const_cast<Instruction *>(Other)->getParent() != BB) return false;
  for (Instruction *I : *BB) {
    if (I == this) return true;
    if (I == Other) return false;
  }
  return false;
}

bool Instruction::comesBefore(const Instruction *A, const Instruction *B) {
  return A && A->comesBefore(B);
}

Value *Instruction::getUnaryOperand() const {
  return getNumOperands() == 1 ? getOperand(0) : nullptr;
}

Value *Instruction::getUnaryOperand(const Instruction *I) {
  return I ? I->getUnaryOperand() : nullptr;
}

Value *Instruction::foldConstantCast(
    InstType CastType, Value *Operand,
    const std::shared_ptr<IRType> &ResultType) {
  if (!Operand) return nullptr;

  int64_t IntValue = 0;
  float FloatValue = 0.0f;
  auto SourceInteger =
      std::dynamic_pointer_cast<INTType>(Operand->getIRType());
  auto DestinationInteger =
      std::dynamic_pointer_cast<INTType>(ResultType);

  auto makeIntegerResult = [](int64_t RawValue,
                              const std::shared_ptr<INTType> &Type)
      -> Value * {
    if (!Type) return nullptr;
    if (Type->getIsBool())
      return ConstBool::newConstBool((RawValue & 1) != 0);
    return ConstInt::newConstIntForType(RawValue, Type);
  };

  auto getLowBits = [](int64_t Value, unsigned Bits) {
    uint64_t Raw = static_cast<uint64_t>(Value);
    if (Bits < 64)
      Raw &= (uint64_t{1} << Bits) - 1U;
    return Raw;
  };

  auto signedFromBits = [](uint64_t Raw) {
    int64_t Result = 0;
    std::memcpy(&Result, &Raw, sizeof(Result));
    return Result;
  };

  switch (CastType) {
  case SITOFP:
    return Operand->tryGetInt64Const(IntValue)
               ? static_cast<Value *>(
                     ConstFloat::newConstFloat(static_cast<float>(IntValue)))
               : nullptr;
  case FPTOSI: {
    if (!Operand->tryGetFloatConst(FloatValue))
      return nullptr;
    if (!DestinationInteger)
      DestinationInteger = INTType::getInt32Ty();
    if (!std::isfinite(FloatValue))
      return nullptr;
    const long double Value = static_cast<long double>(FloatValue);
    const unsigned Bits = DestinationInteger->getBitWidth();
    const long double Minimum =
        Bits >= 64
            ? static_cast<long double>(std::numeric_limits<int64_t>::min())
            : -static_cast<long double>(uint64_t{1} << (Bits - 1U));
    const long double Maximum =
        Bits >= 64
            ? static_cast<long double>(std::numeric_limits<int64_t>::max())
            : static_cast<long double>((uint64_t{1} << (Bits - 1U)) - 1U);
    if (Value < Minimum || Value > Maximum)
      return nullptr;
    return makeIntegerResult(static_cast<int64_t>(FloatValue),
                             DestinationInteger);
  }
  case ZEXT: {
    if (!SourceInteger || !Operand->tryGetInt64Const(IntValue))
      return nullptr;
    if (!DestinationInteger)
      DestinationInteger = INTType::getInt32Ty();
    if (DestinationInteger->getBitWidth() <=
        SourceInteger->getBitWidth())
      return nullptr;
    const uint64_t Raw =
        getLowBits(IntValue, SourceInteger->getBitWidth());
    return makeIntegerResult(signedFromBits(Raw), DestinationInteger);
  }
  case SEXT: {
    if (!SourceInteger || !Operand->tryGetInt64Const(IntValue))
      return nullptr;
    if (!DestinationInteger)
      DestinationInteger = INTType::getInt32Ty();
    if (DestinationInteger->getBitWidth() <=
        SourceInteger->getBitWidth())
      return nullptr;
    const int64_t Extended = IRMath::normalizeSignedToWidth(
        IntValue, SourceInteger->getBitWidth());
    return makeIntegerResult(Extended, DestinationInteger);
  }
  case TRUNC: {
    if (!SourceInteger || !Operand->tryGetInt64Const(IntValue))
      return nullptr;
    if (!DestinationInteger)
      DestinationInteger = INTType::getBoolTy();
    if (DestinationInteger->getBitWidth() >=
        SourceInteger->getBitWidth())
      return nullptr;
    return makeIntegerResult(
        IRMath::normalizeSignedToWidth(
            IntValue, DestinationInteger->getBitWidth()),
        DestinationInteger);
  }
  default:
    return nullptr;
  }
}

RetInst::RetInst(Value* retVal)
    : Instruction(validateAndGetResultType(retVal)) {
    addUse(retVal);
    instType = InstType::RET;
}
RetInst::RetInst() : Instruction(VOIDType::NewVoid()) {
    instType = InstType::RET;
}
RetInst::RetInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::RET;}
Value *RetInst::getReturnValue() const {
  return getNumOperands() == 1 ? getOperand(0) : nullptr;
}
void RetInst::dumpIR(){
    std::cout << "ret ";
  for (auto &i : userList) {
    i->getValue()->getIRType()->toString();
    std::cout << " ";
    i->getValue()->dumpIR();
    std::cout << " ";
  }
  if (userList.empty())
    std::cout << "void";
     std::cout << '\n';
}


BrInst::BrInst(Value* cond,BasicBlock* truebb,BasicBlock* falsebb):Instruction(VOIDType::NewVoid(),InstType::BR){
    addUse(cond);
    addUse(truebb);
    addUse(falsebb);
    isCond = true;
}

BrInst::BrInst(BasicBlock* bb):Instruction(VOIDType::NewVoid(),InstType::BR){
    addUse(bb);
    isCond = false;
}
bool BrInst::isConditional() const {
  return isCond;
}

Value *BrInst::getCondition() const {
  return isConditional() && getNumOperands() == 3 ? getOperand(0) : nullptr;
}

unsigned BrInst::getSuccessorCount() const {
  if (isConditional() && getNumOperands() == 3) return 2;
  if (!isConditional() && getNumOperands() == 1) return 1;
  return 0;
}

BasicBlock *BrInst::getSuccessor(unsigned Index) const {
  if (Index >= getSuccessorCount()) return nullptr;
  size_t OperandIndex = isConditional() ? Index + 1 : Index;
  return dynamic_cast<BasicBlock *>(getOperand(OperandIndex));
}

bool BrInst::setSuccessor(unsigned Index, BasicBlock *Successor) {
  if (!Successor || Index >= getSuccessorCount()) return false;
  size_t OperandIndex = isConditional() ? Index + 1 : Index;
  return replaceOperand(OperandIndex, Successor);
}

BasicBlock *BrInst::getTrueSuccessor() const {
  return getSuccessor(0);
}

BasicBlock *BrInst::getFalseSuccessor() const {
  return isConditional() ? getSuccessor(1) : nullptr;
}

void BrInst::dumpIR(){
    std::cout << "br ";
    if(!isCond){
        for(auto& i:userList){
            std::cout << "label ";
            i->getValue()->dumpIR();
            std::cout << " ";
        }
        std::cout << '\n';
    }else{
        bool flag = 0;
        for (auto &i : userList) {
            if (flag == 0) {
            i->getValue()->getIRType()->toString();
            std::cout << " ";
            flag = 1;
            } else std::cout << "label ";
            i->getValue()->dumpIR();
            if (i.get() != userList.back().get())
            std::cout << ", ";
        }
        std::cout << '\n';
    }
}

AllocaInst::AllocaInst(std::shared_ptr<IRType> allocatedType)
    : Instruction(makeResultType(allocatedType)) {
    instType = InstType::ALLOCA;
}
AllocaInst::AllocaInst(
    std::string ident, std::shared_ptr<IRType> allocatedType)
    : Instruction(makeResultType(allocatedType)) {
    this->ident = std::move(ident);
    instType = InstType::ALLOCA;
}
void AllocaInst::dumpIR(){
    Value::dumpIR();
    std::cout << " = alloca ";
    std::dynamic_pointer_cast<POINTERType>(irtype)->getPointerType()->toString();
    std::cout << "\n";
}

std::shared_ptr<IRType> AllocaInst::getAllocatedType() const {
  return getIRType() ? getIRType()->getPointeeType() : nullptr;
}

bool AllocaInst::isArrayAllocation() const {
  return IRType::isArray(getAllocatedType());
}

std::shared_ptr<IRType> AllocaInst::getAllocatedType(const AllocaInst *AI) {
  return AI ? AI->getAllocatedType() : nullptr;
}

bool AllocaInst::isArrayAllocation(const AllocaInst *AI) {
  return AI && AI->isArrayAllocation();
}

StoreInst::StoreInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::STORE;}
StoreInst::StoreInst(Value* val,Value* ptr)
    : Instruction(VOIDType::NewVoid()) {
    validateOperands(val, ptr);
    addUse(val);
    addUse(ptr);
    ident = "StoreInst";
    instType = InstType::STORE;
}

Value* StoreInst::getValueOperand() const {
    return getOperand(0);
}

Value* StoreInst::getPointerOperand() const {
    return getOperand(1);
}

bool StoreInst::fullyCoversItsPointer() const {
    Value *Pointer = getPointerOperand();
    Value *StoredValue = getValueOperand();
    std::shared_ptr<IRType> Pointee =
        Pointer ? Pointer->getPointeeType() : nullptr;
    return Pointee && StoredValue && StoredValue->getIRType() &&
           Pointee->hasSameShape(*StoredValue->getIRType());
}

void StoreInst::dumpIR(){
  std::cout << "store ";
  for (auto &i : userList) {
    i->getValue()->getIRType()->toString();
    std::cout << " ";
    i->getValue()->dumpIR();
    if (i.get() != userList.back().get())
      std::cout << ", ";
  }
  std::cout << '\n';
}

LoadInst::LoadInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::LOAD;}
// 结果类型取自指针指向的类型，操作数关系在类型确定后建立。
LoadInst::LoadInst(Value* v)
    : Instruction(validateAndGetResultType(v)) {
    instType = InstType::LOAD;
    addUse(v);
}

Value* LoadInst::getPointerOperand() const {
  return getOperand(0);
}

void LoadInst::dumpIR(){
  Value::dumpIR();
  std::cout << " = load ";
  irtype->toString();
  std::cout << ", ";
  for (auto &i : userList) {
    i->getValue()->getIRType()->toString();
    std::cout << " ";
    i->getValue()->dumpIR();
    if (i.get() != userList.back().get())
      std::cout << ", ";
  }
  std::cout << '\n';
}

GetElementPtrInst::GetElementPtrInst(Value* v)
    : Instruction(requireResultType(v, {})) {
    addUse(v);
    instType = InstType::GEP;
}
GetElementPtrInst::GetElementPtrInst(
    Value* v, std::vector<Value*>& vv)
    : Instruction(requireResultType(v, vv)) {
    addUse(v);
    for (Value *Index : vv) addUse(Index);
    instType = InstType::GEP;
}
GetElementPtrInst::GetElementPtrInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::GEP;}
void GetElementPtrInst::dumpIR() {
    // 先输出结果值，再按操作数顺序输出基指针和全部索引。
    this->Value::dumpIR();
    std::cout << " = getelementptr inbounds ";

    Value* basePtr = userList[0]->getValue();
    auto baseType = basePtr->getIRType();

    // GEP 文本中的首个类型是基指针所指向的聚合或元素类型。
    std::shared_ptr<IRType> elementType;
    if (baseType->getTypeSystem() == IR_POINTER) {
        auto ptrType = std::dynamic_pointer_cast<POINTERType>(baseType);
        elementType = ptrType->getPointerType();
    } else {
        elementType = baseType;
    }

    elementType->toString();
    std::cout << ", ";
    basePtr->getIRType()->toString();
    std::cout << " ";
    basePtr->dumpIR();

    for (size_t i = 1; i < userList.size(); i++) {
        Value* index = userList[i]->getValue();
        std::cout << ", ";
        index->getIRType()->toString();
        std::cout << " ";
        index->dumpIR();
    }
    std::cout << "\n";
}

void GetElementPtrInst::updateType() {
    if (userList.empty()) return;

    Value *Base = userList[0]->getValue();
    std::vector<Value *> Indices;
    Indices.reserve(userList.size() - 1);
    for (size_t I = 1; I < userList.size(); ++I)
        Indices.push_back(userList[I]->getValue());

    std::shared_ptr<IRType> ResultType;
    if (tryDeriveResultType(Base, Indices, ResultType))
        this->irtype = std::move(ResultType);
}

Value *GetElementPtrInst::getBasePointer() const {
    return getOperand(0);
}

size_t GetElementPtrInst::getNumIndices() const {
    size_t NumOperands = getNumOperands();
    return NumOperands == 0 ? 0 : NumOperands - 1;
}

Value *GetElementPtrInst::getIndex(size_t Index) const {
    return Index < getNumIndices() ? getOperand(Index + 1) : nullptr;
}

std::vector<Value *> GetElementPtrInst::getIndices() const {
    std::vector<Value *> Indices;
    Indices.reserve(getNumIndices());
    for (size_t I = 0; I < getNumIndices(); ++I)
        Indices.push_back(getIndex(I));
    return Indices;
}

bool GetElementPtrInst::hasOnlyConstantIndices() const {
    if (!getBasePointer()) return false;
    for (size_t I = 0; I < getNumIndices(); ++I) {
        Value *Index = getIndex(I);
        if (!Index || !Index->isConst()) return false;
    }
    return true;
}

bool GetElementPtrInst::getBaseAndIndices(
    Value *&Base, std::vector<Value *> &Indices) const {
    Base = getBasePointer();
    Indices = getIndices();
    if (!Base) return false;
    for (Value *Index : Indices) {
        if (!Index) return false;
    }
    return true;
}

bool GetElementPtrInst::getBaseAndIndices(
    const GetElementPtrInst *GEP, Value *&Base,
    std::vector<Value *> &Indices) {
    Base = nullptr;
    Indices.clear();
    return GEP && GEP->getBaseAndIndices(Base, Indices);
}

bool GetElementPtrInst::reset(Value *Base,
                              const std::vector<Value *> &Indices) {
    std::shared_ptr<IRType> ResultType;
    if (!tryDeriveResultType(Base, Indices, ResultType))
        return false;

    clearOperands();
    addUse(Base);
    for (Value *Index : Indices) addUse(Index);
    this->irtype = std::move(ResultType);
    return true;
}

std::optional<int64_t> GetElementPtrInst::getLastIndexStride() {
    Value *Base = nullptr;
    std::vector<Value *> Indices;
    if (!getBaseAndIndices(Base, Indices) || Indices.empty())
        return std::nullopt;

    std::vector<int64_t> Strides;
    std::shared_ptr<IRType> ResultType;
    if (!tryDeriveResultType(Base, Indices, ResultType, &Strides) ||
        Strides.empty())
        return std::nullopt;
    return Strides.back();
}

bool GetElementPtrInst::flattenZeroBasedChain(Value *&Base,
                                              std::vector<Value *> &Indices) {
    return flattenZeroBasedChain(this, Base, Indices);
}

bool GetElementPtrInst::flattenZeroBasedChain(Value *V, Value *&Base,
                                              std::vector<Value *> &Indices) {
    auto *GEP = dynamic_cast<GetElementPtrInst *>(V);
    if (!GEP) {
        Base = V;
        return Base != nullptr;
    }

    if (GEP->getNumOperands() == 0) return false;

    Value *LocalBase = GEP->getOperand(0);
    if (!LocalBase) return false;
    auto *BaseGEP = dynamic_cast<GetElementPtrInst *>(LocalBase);
    if (BaseGEP) {
        // 只有内层 GEP 以零索引进入时，才能安全合并两段索引链。
        Value *FirstIndex = GEP->getOperand(1);
        if (!FirstIndex ||
            !FirstIndex->isConstIntZero()) {
            return false;
        }
        if (!flattenZeroBasedChain(BaseGEP, Base, Indices))
            return false;
        for (size_t I = 2; I < GEP->getNumOperands(); ++I) {
            Value *Index = GEP->getOperand(I);
            if (!Index) return false;
            Indices.push_back(Index);
        }
        return true;
    }

    Base = LocalBase;
    for (size_t I = 1; I < GEP->getNumOperands(); ++I) {
        Value *Index = GEP->getOperand(I);
        if (!Index) return false;
        Indices.push_back(Index);
    }
    return Base != nullptr;
}

bool GetElementPtrInst::getIndexStrides(Value *Base,
                                        const std::vector<Value *> &Indices,
                                        std::vector<int64_t> &Strides,
                                        int64_t &FinalPointeeStride) {
    Strides.clear();
    FinalPointeeStride = 0;
    if (Indices.empty()) return false;

    std::shared_ptr<IRType> ResultType;
    if (!tryDeriveResultType(Base, Indices, ResultType, &Strides) ||
        Strides.empty())
        return false;
    FinalPointeeStride = Strides.back();
    return true;
}

ExtractElementInst::ExtractElementInst(Value* vector, Value* index)
    : Instruction(validateAndGetResultType(vector, index)) {
    instType = InstType::EXTRACTELEMENT;
    addUse(vector);
    addUse(index);
}

Value* ExtractElementInst::getVectorOperand() const {
    return getOperand(0);
}

Value* ExtractElementInst::getIndexOperand() const {
    return getOperand(1);
}

void ExtractElementInst::dumpIR() {
    Value::dumpIR();
    std::cout << " = extractelement ";
    getVectorOperand()->getIRType()->toString();
    std::cout << " ";
    getVectorOperand()->dumpIR();
    std::cout << ", ";
    getIndexOperand()->getIRType()->toString();
    std::cout << " ";
    getIndexOperand()->dumpIR();
    std::cout << "\n";
}

InsertElementInst::InsertElementInst(
    Value* vector, Value* element, Value* index)
    : Instruction(
          validateAndGetResultType(vector, element, index)) {
    instType = InstType::INSERTELEMENT;
    addUse(vector);
    addUse(element);
    addUse(index);
}

Value* InsertElementInst::getVectorOperand() const {
    return getOperand(0);
}

Value* InsertElementInst::getElementOperand() const {
    return getOperand(1);
}

Value* InsertElementInst::getIndexOperand() const {
    return getOperand(2);
}

void InsertElementInst::dumpIR() {
    Value::dumpIR();
    std::cout << " = insertelement ";
    getVectorOperand()->getIRType()->toString();
    std::cout << " ";
    getVectorOperand()->dumpIR();
    std::cout << ", ";
    getElementOperand()->getIRType()->toString();
    std::cout << " ";
    getElementOperand()->dumpIR();
    std::cout << ", ";
    getIndexOperand()->getIRType()->toString();
    std::cout << " ";
    getIndexOperand()->dumpIR();
    std::cout << "\n";
}

ShuffleVectorInst::ShuffleVectorInst(
    Value* first, Value* second, std::vector<int> mask)
    : Instruction(
          validateAndGetResultType(first, second, mask)),
      mask(std::move(mask)) {
    instType = InstType::SHUFFLEVECTOR;
    addUse(first);
    addUse(second);
}

Value* ShuffleVectorInst::getFirstVectorOperand() const {
    return getOperand(0);
}

Value* ShuffleVectorInst::getSecondVectorOperand() const {
    return getOperand(1);
}

void ShuffleVectorInst::dumpIR() {
    Value::dumpIR();
    std::cout << " = shufflevector ";
    getFirstVectorOperand()->getIRType()->toString();
    std::cout << " ";
    getFirstVectorOperand()->dumpIR();
    std::cout << ", ";
    getSecondVectorOperand()->getIRType()->toString();
    std::cout << " ";
    getSecondVectorOperand()->dumpIR();
    std::cout << ", <" << mask.size() << " x i32> <";
    for (std::size_t I = 0; I < mask.size(); ++I) {
        if (I != 0)
            std::cout << ", ";
        std::cout << "i32 ";
        if (mask[I] < 0)
            std::cout << "poison";
        else
            std::cout << mask[I];
    }
    std::cout << ">\n";
}

BinaryInst::BinaryInst(std::shared_ptr<IRType> irtype):Instruction(irtype){
    instType = InstType::BinaryUnknown;
}

BinaryInst::BinaryInst(Value* x,Operation op,Value* y)
    : Instruction(x && x->getIRType()
                      ? x->getIRType()
                      : VOIDType::NewVoid()),
      op(op) {
    if (!x || !y || !x->getIRType() || !y->getIRType() ||
        !x->getIRType()->hasSameShape(*y->getIRType()))
      throw std::invalid_argument("二元指令要求两个操作数类型完全一致");
    const auto ScalarType =
        IRType::getScalarElementType(x->getIRType());
    const bool IsScalarOrVector =
        ScalarType && ScalarType->isScalar();
    const bool IsPointerCompare =
        x->getIRType()->isPointer() && BinaryInst::isCompareOp(op);
    if (!IsScalarOrVector && !IsPointerCompare)
      throw std::invalid_argument("二元指令的操作数类型不受支持");
    if (BinaryInst::isIntegerOnlyOp(op) &&
        (!ScalarType || !ScalarType->isInt()))
      throw std::invalid_argument("该二元操作只接受整数类型");
    if (BinaryInst::isCompareOp(op)) {
      if (auto VectorType = IRType::asVector(x->getIRType())) {
        this->irtype = VECTORType::NewVector(
            INTType::getBoolTy(), VectorType->getElementCount());
      } else {
        this->irtype = INTType::getBoolTy();
      }
    }
    this->instType =
        getInstType(op, ScalarType->getTypeSystem());
    this->addUse(x);
    this->addUse(y);
}

bool BinaryInst::getOperands(Value *&LHS, Value *&RHS) const {
    LHS = nullptr;
    RHS = nullptr;
    if (getNumOperands() < 2) return false;
    LHS = getOperand(0);
    RHS = getOperand(1);
    return LHS && RHS;
}

bool BinaryInst::getOperands(const BinaryInst *BI, Value *&LHS, Value *&RHS) {
    LHS = nullptr;
    RHS = nullptr;
    return BI && BI->getOperands(LHS, RHS);
}

bool BinaryInst::setOperands(Value *LHS, Value *RHS) {
    if (!LHS || !RHS || getNumOperands() < 2) return false;
    bool Changed = false;
    if (getOperand(0) != LHS) Changed |= replaceOperand(0, LHS);
    if (getOperand(1) != RHS) Changed |= replaceOperand(1, RHS);
    return Changed;
}

bool BinaryInst::isCompareOp(Operation Op) {
  switch (Op) {
  case E:
  case NE:
  case G:
  case GE:
  case L:
  case LE:
    return true;
  default:
    return false;
  }
}

bool BinaryInst::isAssociativeOp(Operation Op) {
  return Op == ADD || Op == MUL || Op == AND || Op == OR || Op == XOR;
}

bool BinaryInst::isCommutativeOp(Operation Op) {
  return isAssociativeOp(Op) || Op == E || Op == NE;
}

bool BinaryInst::isReassociateOp(Operation Op) {
  return Op == ADD || Op == SUB || Op == MUL ||
         Op == AND || Op == OR || Op == XOR;
}

BinaryInst::Operation BinaryInst::negatePredicate(Operation Op) {
  switch (Op) {
  case L:  return GE;
  case LE: return G;
  case G:  return LE;
  case GE: return L;
  case E:  return NE;
  case NE: return E;
  default: return Op;
  }
}

BinaryInst::Operation BinaryInst::reversePredicate(Operation Op) {
  switch (Op) {
  case L:  return G;
  case LE: return GE;
  case G:  return L;
  case GE: return LE;
  case E:
  case NE:
    return Op;
  default:
    return Op;
  }
}

BinaryInst *BinaryInst::createBooleanNot(Value *Operand) {
  return Operand
      ? new BinaryInst(Operand, XOR, ConstBool::newConstBool(true))
      : nullptr;
}

Value *BinaryInst::foldConstants(Operation Op, Value *LHS, Value *RHS,
                                 bool NormalizeIntegerWidth) {
  if (!LHS || !RHS) return nullptr;
  (void)NormalizeIntegerWidth;
  const auto Ty = LHS->getIRType();
  const auto RHSType = RHS->getIRType();
  if (!Ty || !RHSType || !Ty->hasSameScalarType(*RHSType))
    return nullptr;

  if (Ty->isFloat()) {
    float LV = 0.0f;
    float RV = 0.0f;
    if (!LHS->tryGetFloatConst(LV) || !RHS->tryGetFloatConst(RV))
      return nullptr;
    if (isCompareOp(Op)) {
      // SysY 使用有序浮点比较；任一操作数为 NaN 时仅“不等”成立。
      if (std::isnan(LV) || std::isnan(RV))
        return ConstBool::newConstBool(Op == NE);
      switch (Op) {
      case E:  return ConstBool::newConstBool(LV == RV);
      case NE: return ConstBool::newConstBool(LV != RV);
      case G:  return ConstBool::newConstBool(LV > RV);
      case GE: return ConstBool::newConstBool(LV >= RV);
      case L:  return ConstBool::newConstBool(LV < RV);
      case LE: return ConstBool::newConstBool(LV <= RV);
      default: return nullptr;
      }
    }
    switch (Op) {
    case ADD: return ConstFloat::newConstFloat(LV + RV);
    case SUB: return ConstFloat::newConstFloat(LV - RV);
    case MUL: return ConstFloat::newConstFloat(LV * RV);
    case DIV: return ConstFloat::newConstFloat(LV / RV);
    case MOD: return ConstFloat::newConstFloat(std::fmod(LV, RV));
    default: return nullptr;
    }
  }

  auto IntTy = std::dynamic_pointer_cast<INTType>(Ty);
  if (!IntTy)
    return nullptr;
  const unsigned Bits = IntTy->getBitWidth();
  if (Bits == 0 || Bits > 64)
    return nullptr;

  int64_t LV = 0;
  int64_t RV = 0;
  if (!LHS->tryGetInt64ConstAtWidth(Bits, LV) ||
      !RHS->tryGetInt64ConstAtWidth(Bits, RV))
    return nullptr;

  const uint64_t WidthMask =
      Bits == 64 ? std::numeric_limits<uint64_t>::max()
                 : (uint64_t{1} << Bits) - 1U;
  const uint64_t LRaw = static_cast<uint64_t>(LV) & WidthMask;
  const uint64_t RRaw = static_cast<uint64_t>(RV) & WidthMask;

  if (isCompareOp(Op)) {
    switch (Op) {
    case E:  return ConstBool::newConstBool(LRaw == RRaw);
    case NE: return ConstBool::newConstBool(LRaw != RRaw);
    case G:  return ConstBool::newConstBool(LV > RV);
    case GE: return ConstBool::newConstBool(LV >= RV);
    case L:  return ConstBool::newConstBool(LV < RV);
    case LE: return ConstBool::newConstBool(LV <= RV);
    default: return nullptr;
    }
  }

  if ((Op == DIV || Op == UDIV || Op == MOD || Op == UREM) &&
      RRaw == 0)
    return Value::makeUndefForType(Ty);

  const int64_t SignedMinimum =
      Bits == 64 ? std::numeric_limits<int64_t>::min()
                 : -static_cast<int64_t>(uint64_t{1} << (Bits - 1U));
  if ((Op == DIV || Op == MOD) && LV == SignedMinimum && RV == -1)
    return Value::makeUndefForType(Ty);

  uint64_t ResultRaw = 0;
  switch (Op) {
  case ADD: ResultRaw = LRaw + RRaw; break;
  case SUB: ResultRaw = LRaw - RRaw; break;
  case MUL: ResultRaw = LRaw * RRaw; break;
  case DIV: ResultRaw = static_cast<uint64_t>(LV / RV); break;
  case UDIV: ResultRaw = LRaw / RRaw; break;
  case MOD: ResultRaw = static_cast<uint64_t>(LV % RV); break;
  case UREM: ResultRaw = LRaw % RRaw; break;
  case AND: ResultRaw = LRaw & RRaw; break;
  case OR: ResultRaw = LRaw | RRaw; break;
  case XOR: ResultRaw = LRaw ^ RRaw; break;
  case SHL:
    if (RRaw >= Bits) return Value::makeUndefForType(Ty);
    ResultRaw = LRaw << RRaw;
    break;
  case LSHR:
    if (RRaw >= Bits) return Value::makeUndefForType(Ty);
    ResultRaw = LRaw >> RRaw;
    break;
  case ASHR:
    if (RRaw >= Bits) return Value::makeUndefForType(Ty);
    ResultRaw = LRaw >> RRaw;
    if (LV < 0 && RRaw != 0) {
      const unsigned RemainingBits = Bits - static_cast<unsigned>(RRaw);
      ResultRaw |= ~uint64_t{0} << RemainingBits;
    }
    break;
  default:
    return nullptr;
  }
  ResultRaw &= WidthMask;

  int64_t Result = 0;
  std::memcpy(&Result, &ResultRaw, sizeof(Result));
  Result = IRMath::normalizeSignedToWidth(Result, Bits);
  return Value::makeIntegerForType(Result, Ty);
}

void BinaryInst::dumpIR(){
  Value::dumpIR();
  auto OperandType = userList[0]->getValue()->getIRType();
  auto ScalarType = IRType::getScalarElementType(OperandType);
  TypeSystem tp = ScalarType->getTypeSystem();
  std::cout << " = ";
  switch (op) {
  case BinaryInst::ADD:
      if (tp == IR_INT)
        std::cout << "add ";
      else
        std::cout << "fadd ";
    break;
  case BinaryInst::SUB:
    if (tp == IR_INT)
      std::cout << "sub ";
    else
      std::cout << "fsub ";
    break;
  case BinaryInst::MUL:
    if (tp == IR_INT)
      std::cout << "mul ";
    else
      std::cout << "fmul ";
    break;
  case BinaryInst::DIV:
    if (tp == IR_INT)
      std::cout << "sdiv ";
    else
      std::cout << "fdiv ";
    break;
  case BinaryInst::UDIV:
    std::cout << "udiv ";
    break;
  case BinaryInst::MOD:
    if (tp == IR_INT)
      std::cout << "srem ";
    else
      std::cout << "frem ";
    break;
  case BinaryInst::UREM:
    std::cout << "urem ";
    break;
  case BinaryInst::AND:
    std::cout << "and ";
    break;
  case BinaryInst::OR:
    std::cout << "or ";
    break;
  case BinaryInst::XOR:
    std::cout << "xor ";
    break;
  case BinaryInst::SHL:
    std::cout << "shl ";
    break;
  case BinaryInst::LSHR:
    std::cout << "lshr ";
    break;
  case BinaryInst::ASHR:
    std::cout << "ashr ";
    break;
  case BinaryInst::E:
    if (tp == IR_FLOAT) std::cout << "fcmp oeq ";
    else                std::cout << "icmp eq ";
    break;
  case BinaryInst::NE:
    if (tp == IR_FLOAT) std::cout << "fcmp une ";
    else                std::cout << "icmp ne ";
    break;
  case BinaryInst::G:
    if (tp == IR_FLOAT) std::cout << "fcmp ogt ";
    else                std::cout << "icmp sgt ";
    break;
  case BinaryInst::GE:
    if (tp == IR_FLOAT) std::cout << "fcmp oge ";
    else                std::cout << "icmp sge ";
    break;
  case BinaryInst::L:
    if (tp == IR_FLOAT) std::cout << "fcmp olt ";
    else                std::cout << "icmp slt ";
    break;
  case BinaryInst::LE:
    if (tp == IR_FLOAT) std::cout << "fcmp ole ";
    else                std::cout << "icmp sle ";
    break;
  default:
    break;
  }
  userList[0]->getValue()->getIRType()->toString();
  std::cout << " ";
  userList[0]->getValue()->dumpIR();
  std::cout << ", ";
  userList[1]->getValue()->dumpIR();
  std::cout << "\n";

}


CallInst::CallInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::CALL;}
CallInst::CallInst(Value *func, std::vector<Value*> &args,
                   std::string appendix)
    : Instruction(validateAndGetResultType(func, args)) {
  ident += appendix;
  addUse(func);
  for (auto &i : args)
    addUse(i);
  instType = InstType::CALL;
}

void CallInst::dumpIR(){
  if (irtype != VOIDType::NewVoid()) {
    Value::dumpIR();
    std::cout << " = ";
  }
  std::cout << (tailCall ? "tail call " : "call ");
  for (auto &i : this->userList) {
    i->getValue()->getIRType()->toString();
    std::cout << " ";

    i->getValue()->dumpIR();
    if (i.get() == userList.front().get())
      std::cout << "(";
    else if (i.get() != userList.back().get())
      std::cout << ", ";
  }
  std::cout << ")\n";
}

Value *CallInst::getCallee() const {
  return getOperand(0);
}

Function *CallInst::getDirectCallee() const {
  return dynamic_cast<Function *>(getCallee());
}

BuildInFunction *CallInst::getBuiltinCallee() const {
  return dynamic_cast<BuildInFunction *>(getCallee());
}

std::string CallInst::getCalleeName() const {
  if (auto *F = getDirectCallee()) return F->getIdent();
  if (auto *BIF = getBuiltinCallee()) return BIF->getIdent();
  return {};
}

void CallInst::setTailCall(bool v) {
  tailCall = v;
}

Value *CallInst::getArg(unsigned Index) const {
  return getOperand(static_cast<size_t>(Index) + 1);
}

std::vector<Value *> CallInst::getArgs() const {
  std::vector<Value *> Args;
  Args.reserve(getArgCount());
  for (unsigned I = 0; I < getArgCount(); ++I) Args.push_back(getArg(I));
  return Args;
}

unsigned CallInst::getArgCount() const {
  size_t NumOperands = getNumOperands();
  return NumOperands == 0 ? 0 : static_cast<unsigned>(NumOperands - 1);
}

int CallInst::getArgIndex(const Use *U) const {
  int OperandIndex = getUserIndex(U);
  return OperandIndex > 0 ? OperandIndex - 1 : -1;
}

bool CallInst::isCalleeUse(const Use *U) const {
  return U && getUserIndex(U) == 0;
}

bool CallInst::eraseArg(unsigned Index) {
  return Index < getArgCount() && eraseOperand(static_cast<size_t>(Index) + 1);
}

bool CallInst::isMemcpyLike() const {
  return BuildInFunction::isMemcpyName(getCalleeName());
}

bool CallInst::isNonVolatileMemcpy() const {
  int32_t IsVolatile = 0;
  return isMemcpyLike() && getArgCount() >= 4 && getArg(3) &&
         getArg(3)->tryGetIntConst(IsVolatile) && IsVolatile == 0;
}

bool CallInst::isMemcpyLike(const CallInst *CI) {
  return CI && CI->isMemcpyLike();
}

bool CallInst::isNonVolatileMemcpy(const CallInst *CI) {
  return CI && CI->isNonVolatileMemcpy();
}

FPTOSIInst::FPTOSIInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::FPTOSI;}
FPTOSIInst::FPTOSIInst(Value* v)
    : FPTOSIInst(v, INTType::getInt32Ty()) {}
FPTOSIInst::FPTOSIInst(
    Value* v, std::shared_ptr<IRType> targetType)
    : Instruction(
          IRType::deriveLanePreservingType(
              v ? v->getIRType() : nullptr, targetType)) {
    auto SourceElement =
        IRType::getScalarElementType(v->getIRType());
    auto DestinationElement =
        IRType::getScalarIntegerType(getIRType());
    if (!SourceElement || !SourceElement->isFloat() ||
        !DestinationElement)
      throw std::invalid_argument("fptosi 要求浮点源值和整数目标类型");
    addUse(v);
    instType = InstType::FPTOSI;
}
void FPTOSIInst::dumpIR(){
  Value::dumpIR();
  std::cout << " = fptosi ";
  for (auto &i : userList) {
    i->getValue()->getIRType()->toString();
    std::cout << " ";
    i->getValue()->dumpIR();
    std::cout << " ";
  }
  std::cout << "to ";
  irtype->toString();
  std::cout << "\n";
}

void SITOFPInst::dumpIR(){
  Value::dumpIR();
  std::cout << " = sitofp ";
  for (auto &i : userList) {
    i->getValue()->getIRType()->toString();
    std::cout << " ";
    i->getValue()->dumpIR();
    std::cout << " ";
  }
  std::cout << "to ";
  irtype->toString();
  std::cout << '\n';
}

SITOFPInst::SITOFPInst(Value* v)
    : SITOFPInst(v, FLOATType::NewFloat()) {}

SITOFPInst::SITOFPInst(
    Value* v, std::shared_ptr<IRType> targetType)
    : Instruction(
          IRType::deriveLanePreservingType(
              v ? v->getIRType() : nullptr, targetType)) {
    auto SourceElement =
        IRType::getScalarIntegerType(v->getIRType());
    auto DestinationElement =
        IRType::getScalarElementType(getIRType());
    if (!SourceElement || !DestinationElement ||
        !DestinationElement->isFloat())
      throw std::invalid_argument("sitofp 要求整数源值和浮点目标类型");
    addUse(v);
    instType = InstType::SITOFP;
}

SITOFPInst::SITOFPInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::SITOFP;}


ZextInst::ZextInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::ZEXT;}
ZextInst::ZextInst(Value* v)
    : ZextInst(v, INTType::getInt32Ty()) {}
ZextInst::ZextInst(Value* v, std::shared_ptr<IRType> targetType)
    : Instruction(
          IRType::deriveLanePreservingType(
              v ? v->getIRType() : nullptr, targetType)) {
    auto sourceType =
        IRType::getScalarIntegerType(v->getIRType());
    auto destinationType =
        IRType::getScalarIntegerType(getIRType());
    if (!sourceType || !destinationType ||
        destinationType->getBitWidth() <= sourceType->getBitWidth())
      throw std::invalid_argument("zext 要求目标整数位宽大于源位宽");
    addUse(v);
    instType = InstType::ZEXT;
}
void ZextInst::dumpIR(){
    Value::dumpIR();
    std::cout << " = zext ";
    userList[0]->getValue()->getIRType()->toString();
    std::cout << " ";
    userList[0]->getValue()->dumpIR();
    std::cout << " to ";
    irtype->toString();
    std::cout << "\n";

}

TruncInst::TruncInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::TRUNC;}

TruncInst::TruncInst(Value* v)
    : TruncInst(v, INTType::getBoolTy()) {}
TruncInst::TruncInst(Value* v, std::shared_ptr<IRType> targetType)
    : Instruction(
          IRType::deriveLanePreservingType(
              v ? v->getIRType() : nullptr, targetType)) {
  auto sourceType =
      IRType::getScalarIntegerType(v->getIRType());
  auto destinationType =
      IRType::getScalarIntegerType(getIRType());
  if (!sourceType || !destinationType ||
      destinationType->getBitWidth() >= sourceType->getBitWidth())
    throw std::invalid_argument("trunc 要求目标整数位宽小于源位宽");
  addUse(v);
  instType = InstType::TRUNC;
}

void TruncInst::dumpIR(){
  Value::dumpIR();
  std::cout << " = trunc ";
  userList[0]->getValue()->getIRType()->toString();
  std::cout << " ";
  userList[0]->getValue()->dumpIR();
  std::cout << " to ";
  irtype->toString();
  std::cout << "\n";
}

SextInst::SextInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::SEXT;}

SextInst::SextInst(Value* v)
    : SextInst(v, INTType::getInt32Ty()) {}
SextInst::SextInst(Value* v, std::shared_ptr<IRType> targetType)
    : Instruction(
          IRType::deriveLanePreservingType(
              v ? v->getIRType() : nullptr, targetType)) {
  auto sourceType =
      IRType::getScalarIntegerType(v->getIRType());
  auto destinationType =
      IRType::getScalarIntegerType(getIRType());
  if (!sourceType || !destinationType ||
      destinationType->getBitWidth() <= sourceType->getBitWidth())
    throw std::invalid_argument("sext 要求目标整数位宽大于源位宽");
  addUse(v);
  instType = InstType::SEXT;
}

void SextInst::dumpIR(){
  Value::dumpIR();
  std::cout << " = sext ";
  userList[0]->getValue()->getIRType()->toString();
  std::cout << " ";
  userList[0]->getValue()->dumpIR();
  std::cout << " to ";
  irtype->toString();
  std::cout << "\n";
}

SelectInst::SelectInst(Value* cond,Value* isTrue,Value* isFalse)
  : Instruction(isTrue && isTrue->getIRType()
                    ? isTrue->getIRType()
                    : VOIDType::NewVoid()) {
  if (!isTrue || !isFalse || !isTrue->getIRType() ||
      !isFalse->getIRType() ||
      !isTrue->getIRType()->hasSameShape(*isFalse->getIRType()))
    throw std::invalid_argument("select 的两个候选值必须类型完全一致");
  const bool IsScalarCondition =
      cond && cond->getIRType() && cond->getIRType()->isBool();
  const bool IsVectorCondition =
      cond && cond->getIRType() &&
      cond->getIRType()->isBooleanVector();
  if (!IsScalarCondition && !IsVectorCondition)
    throw std::invalid_argument(
        "select 条件必须是 i1 或 <N x i1>");
  if (IsVectorCondition) {
    auto ConditionVector = IRType::asVector(cond->getIRType());
    auto ValueVector = IRType::asVector(isTrue->getIRType());
    if (!ValueVector ||
        ConditionVector->getElementCount() !=
            ValueVector->getElementCount())
      throw std::invalid_argument(
          "向量 select 的条件和值必须具有相同 lane 数");
  }
  instType = InstType::SELECT;
  addUse(cond);
  addUse(isTrue);
  addUse(isFalse);
}

Value *SelectInst::getCondition() const { return getOperand(0); }
Value *SelectInst::getTrueValue() const { return getOperand(1); }
Value *SelectInst::getFalseValue() const { return getOperand(2); }

void SelectInst::dumpIR() {
  Value::dumpIR();
  std::cout << " = select ";

  userList[0]->getValue()->getIRType()->toString();
  std::cout << " ";
  userList[0]->getValue()->dumpIR();
  std::cout << ", ";

  userList[1]->getValue()->getIRType()->toString();
  std::cout << " ";
  userList[1]->getValue()->dumpIR();
  std::cout << ", ";

  userList[2]->getValue()->getIRType()->toString();
  std::cout << " ";
  userList[2]->getValue()->dumpIR();
  std::cout << "\n";
}

Instruction* SelectInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
  if (userList.size() != 3) return nullptr;
  return new SelectInst(remapVal(userList[0]->getValue(), VMap),
              remapVal(userList[1]->getValue(), VMap),
              remapVal(userList[2]->getValue(), VMap));
}

Value* BasicBlock::genSelectInst(Value* cond, Value* isTrue, Value* isFalse) {
  if (!cond || !isTrue || !isFalse || !cond->getIRType() ||
      !isTrue->getIRType() || !isFalse->getIRType())
    return nullptr;
  // 标量条件统一转换为 i1；<N x i1> 直接作为逐 lane mask 使用。
  if (!cond->getIRType()->isBool() &&
      !cond->getIRType()->isBooleanVector()) {
    switch (cond->getIRType()->getTypeSystem()) {
    case IR_INT:
      cond = BasicBlock::genBinaryInst(
          this, cond, BinaryInst::NE,
          Value::makeZeroForType(cond->getIRType()));
      break;
    case IR_FLOAT:
      cond = genBinaryInst(cond, BinaryInst::NE, ConstFloat::newConstFloat(0.0f));
      break;
    case IR_POINTER:
      cond = genBinaryInst(cond, BinaryInst::NE, ConstPtr::newConstPtr(cond->getIRType()));
      break;
    default:
      assert(0 && "select 条件类型不受支持");
      return nullptr;
    }
  }
  if (!cond)
    return nullptr;

  // 两个候选值先做公共类型转换，再由构造器执行严格验证。
  if (!isTrue->getIRType()->hasSameShape(*isFalse->getIRType())) {
    auto tTy = isTrue->getIRType()->getTypeSystem();
    auto fTy = isFalse->getIRType()->getTypeSystem();
    if (tTy == IR_INT && fTy == IR_FLOAT) {
      isTrue = coerceValueToType(
          isTrue, FLOATType::NewFloat());
    } else if (tTy == IR_FLOAT && fTy == IR_INT) {
      isFalse = coerceValueToType(
          isFalse, FLOATType::NewFloat());
    } else if (tTy == IR_INT && fTy == IR_INT) {
      auto TrueInteger =
          std::dynamic_pointer_cast<INTType>(isTrue->getIRType());
      auto FalseInteger =
          std::dynamic_pointer_cast<INTType>(isFalse->getIRType());
      if (!TrueInteger || !FalseInteger)
        return nullptr;
      auto CommonType = INTType::get(
          std::max(TrueInteger->getBitWidth(),
                   FalseInteger->getBitWidth()));
      isTrue = coerceValueToType(isTrue, CommonType);
      isFalse = coerceValueToType(isFalse, CommonType);
    } else if (tTy == IR_POINTER && fTy == IR_POINTER) {
      isFalse =
          coerceValueToType(isFalse, isTrue->getIRType());
    } else {
      return nullptr;
    }
  }
  if (!isTrue || !isFalse)
    return nullptr;

  auto *inst = new SelectInst(cond, isTrue, isFalse);
  pushBack(inst);
  return inst;
}


Param::Param(std::shared_ptr<IRType> parameterType,
             std::string sourceName)
    : Value(std::move(parameterType), std::move(sourceName)) {}

bool Param::isParam() {
    return true;
}

bool Variable::isGlobal(){
    return true;
}

Variable::Variable(VarTag variableTag,
                   std::shared_ptr<IRType> objectType,
                   std::string sourceName)
    : User(POINTERType::NewPointer(std::move(objectType))),
      vartag(variableTag) {
    if (variableTag == GlobalVariable) {
        this->ident = ".G." + sourceName;
    } else {
        // 匿名常量保留 Value 已分配的唯一编号；具名常量随后由 IRGenerator 改名。
        this->ident = ".C." + this->ident;
    }
    ModuleIR->getGlobalVar().emplace_back(this);
}

bool Variable::isScalarGlobal() {
  if (vartag != GlobalVariable) return false;
  auto PtrTy = std::dynamic_pointer_cast<POINTERType>(getIRType());
  if (!PtrTy) return false;
  auto ElemTy = PtrTy->getPointerType();
  if (!ElemTy) return false;
  return ElemTy->isInt() || ElemTy->isFloat();
}

void Variable::dumpIR(){
    Value::dumpIR();
    if (vartag == GlobalVariable)
    std::cout << " = global ";
    else if (vartag == GlobalConstant)
    std::cout << " = constant ";
    auto type = std::dynamic_pointer_cast<POINTERType>(getIRType());
    type->getPointerType()->toString();
    std::cout<<" ";
    if (Value *init = getOperand(0)) {
        if(auto arrayInit = dynamic_cast<Initializer*>(init)) arrayInit->dumpIR();
        else init->dumpIR();
    }else std::cout<<"zeroinitializer";
    std::cout<<'\n';
}

 Value* Variable::getUsee(){
    return getOperand(0);
 }

Value *Variable::getConstantElement(const std::vector<int> &Path) {
  std::shared_ptr<IRType> Ty =
      getIRType() ? getIRType()->getPointeeType() : nullptr;
  if (!Ty) return nullptr;

  Value *Node = getUsee();
  for (int Index : Path) {
    auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(Ty);
    if (!ArrTy || Index < 0 || Index >= ArrTy->getElementCount())
      return nullptr;

    if (Node) {
      auto *Init = dynamic_cast<Initializer *>(Node);
      if (!Init) return nullptr;
      Node = static_cast<size_t>(Index) < Init->size()
                 ? (*Init)[static_cast<size_t>(Index)]
                 : nullptr;
    }
    Ty = ArrTy->getElementType();
  }

  if (!Ty || Ty->isArray()) return nullptr;
  if (!Node) return Value::makeZeroForType(Ty);
  return dynamic_cast<ConstantSystem *>(Node) ? Node : nullptr;
}

Value *Variable::getConstantElement(Variable *Source,
                                    const std::vector<int> &Path) {
  return Source ? Source->getConstantElement(Path) : nullptr;
}

Initializer::Initializer(std::shared_ptr<IRType> irtype):Value(irtype){}

bool Initializer::isAllZero(Value *value) {
    if (auto *init = dynamic_cast<Initializer *>(value)) {
        for (Value *element : *init) {
            if (!isAllZero(element)) return false;
        }
        return true;
    }
    if (auto *valueInt = dynamic_cast<ConstInt *>(value))
        return valueInt->getVal() == 0;
    if (auto *valueBool = dynamic_cast<ConstBool *>(value))
        return !valueBool->getVal();
    if (auto *valueFloat = dynamic_cast<ConstFloat *>(value)) {
        const float number = valueFloat->getVal();
        return number == 0.0f && !std::signbit(number);
    }
    return dynamic_cast<ConstPtr *>(value) != nullptr;
}

void Initializer::dumpIR(){
    if(isAllZero(this)){
        std::cout << "zeroinitializer";
        return;
    }
    auto arrType = std::dynamic_pointer_cast<ARRAYType>(irtype);
    if (!arrType) {
        // 非数组类型无法按聚合常量展开，退化为同类型的零初始化值。
        std::cout << "zeroinitializer";
        return;
    }
    std::cout << " [";
    int limi = arrType->getElementCount();
    for (int i = 0; i < limi; i++) {
    arrType->getElementType()->toString();
    if (i < this->size()) {
      std::cout << " ";
      if (auto inits = dynamic_cast<Initializer *>((*this)[i]))
        inits->dumpIR();
      else
        (*this)[i]->dumpIR();
    } else
      std::cout << " zeroinitializer";
    if (i != limi - 1)
      std::cout << ", ";
  }
  std::cout << "]";
}

// 常量对象工厂与驻留。
ConstantSystem::ConstantSystem(std::shared_ptr<IRType> irtype):Value(irtype){}

std::shared_ptr<VECTORType> ConstantVector::validateAndGetType(
    const std::shared_ptr<VECTORType> &Type,
    const std::vector<ConstantSystem *> &Elements) {
  if (!Type)
    throw std::invalid_argument("向量常量需要有效的向量类型");
  if (Elements.size() != Type->getElementCount())
    throw std::invalid_argument("向量常量的元素数量必须与 lane 数一致");

  const std::shared_ptr<IRType> ElementType = Type->getElementType();
  for (ConstantSystem *Element : Elements) {
    if (!Element || !Element->getIRType() ||
        !Element->getIRType()->hasSameShape(*ElementType))
      throw std::invalid_argument(
          "向量常量的所有元素必须与向量元素类型完全一致");
    if (!Element->getIRType()->isScalar())
      throw std::invalid_argument("向量常量的元素必须是标量常量");
  }
  return Type;
}

std::string ConstantVector::buildIdent(
    const std::vector<ConstantSystem *> &Elements) {
  std::stringstream Stream;
  Stream << "<";
  for (std::size_t Index = 0; Index < Elements.size(); ++Index) {
    if (Index != 0) Stream << ", ";
    const std::shared_ptr<IRType> ElementType = Elements[Index]->getIRType();
    if (auto IntegerType = std::dynamic_pointer_cast<INTType>(ElementType))
      Stream << "i" << IntegerType->getBitWidth();
    else
      Stream << "float";
    Stream << " " << Elements[Index]->getIdent();
  }
  Stream << ">";
  return Stream.str();
}

ConstantVector::ConstantVector(
    std::shared_ptr<VECTORType> Type,
    std::vector<ConstantSystem *> Elements)
    : ConstantSystem(validateAndGetType(Type, Elements)),
      elements(std::move(Elements)) {
  ident = buildIdent(elements);
}

ConstantVector *ConstantVector::get(
    const std::vector<ConstantSystem *> &Elements) {
  if (Elements.empty() || !Elements.front() ||
      !Elements.front()->getIRType())
    throw std::invalid_argument(
        "向量常量需要至少一个带类型的常量元素");
  return get(VECTORType::NewVector(Elements.front()->getIRType(),
                                   Elements.size()),
             Elements);
}

ConstantVector *ConstantVector::get(
    const std::shared_ptr<VECTORType> &Type,
    const std::vector<ConstantSystem *> &Elements) {
  validateAndGetType(Type, Elements);
  // 标量常量和类型均已驻留，因此元素指针序列可以作为稳定的唯一键。
  static std::map<std::vector<ConstantSystem *>, ConstantVector *> Constants;
  auto &Constant = Constants[Elements];
  if (!Constant)
    Constant = new ConstantVector(Type, Elements);
  return Constant;
}

ConstantVector *ConstantVector::getSplat(
    std::size_t ElementCount, ConstantSystem *Element) {
  if (!Element || !Element->getIRType())
    throw std::invalid_argument("splat 常量需要有效的标量常量");
  return getSplat(
      VECTORType::NewVector(Element->getIRType(), ElementCount), Element);
}

ConstantVector *ConstantVector::getSplat(
    const std::shared_ptr<VECTORType> &Type, ConstantSystem *Element) {
  if (!Type)
    throw std::invalid_argument("splat 常量需要有效的向量类型");
  std::vector<ConstantSystem *> Elements(Type->getElementCount(), Element);
  return get(Type, Elements);
}

ConstantSystem *ConstantVector::getElement(std::size_t Index) const {
  if (Index >= elements.size())
    throw std::out_of_range("向量常量的 lane 索引越界");
  return elements[Index];
}

bool ConstantVector::isSplat() const {
  if (elements.empty()) return false;
  for (std::size_t Index = 1; Index < elements.size(); ++Index)
    if (!Value::areEqualConstants(elements.front(), elements[Index]))
      return false;
  return true;
}

ConstantSystem *ConstantVector::getSplatValue() const {
  return isSplat() ? elements.front() : nullptr;
}


ConstBool::ConstBool(bool val):ConstantSystem(INTType::getBoolTy()),val(val){
    if (val)
    ident = "true";
    else
    ident = "false";
}

bool ConstBool::getVal() const {return val;}

ConstBool* ConstBool::newConstBool(bool val){
  static ConstBool trueConst(true);
  static ConstBool falseConst(false);
  if (val)
    return &trueConst;
  else
    return &falseConst;
}

ConstInt::ConstInt(int64_t val, std::shared_ptr<INTType> type)
    : ConstantSystem(std::move(type)), val(val) {
    ident = std::to_string(val);
}

int64_t ConstInt::getVal() const {return val;}

ConstInt* ConstInt::newConstInt(int64_t val, size_t size){
  if (size == 0 || size > static_cast<size_t>(UINT32_MAX / 8U))
    throw std::invalid_argument("invalid integer storage size");
  return newConstIntForType(
      val, INTType::get(static_cast<unsigned>(size * 8U)));
}

ConstInt* ConstInt::newConstIntForType(
    int64_t val, const std::shared_ptr<INTType> &type) {
  if (!type || type->getIsBool())
    throw std::invalid_argument("ConstInt 需要非 i1 整数类型");
  val = IRMath::normalizeSignedToWidth(val, type->getBitWidth());
  using Key = std::pair<int64_t, unsigned>;
  static std::map<Key, ConstInt*> intConstMap;
  const Key key{val, type->getBitWidth()};
  auto &constant = intConstMap[key];
  if (!constant)
    constant = new ConstInt(val, type);
  return constant;
}

ConstFloat::ConstFloat(float val):ConstantSystem(FLOATType::NewFloat()),val(val){
  double tmp = val;
  std::uint64_t Bits = 0;
  static_assert(sizeof(Bits) == sizeof(tmp), "double 位宽不符合预期");
  std::memcpy(&Bits, &tmp, sizeof(Bits));
  std::stringstream ss;
  ss << "0x" << std::hex << Bits;
  ident = ss.str();
}
ConstFloat* ConstFloat::newConstFloat(float val){
  // 浮点常量按位模式驻留，以区分正负零，并避免 NaN 破坏有序容器的严格弱序。
  std::uint32_t Bits = 0;
  static_assert(sizeof(Bits) == sizeof(val), "float 位宽不符合预期");
  std::memcpy(&Bits, &val, sizeof(Bits));
  static std::map<std::uint32_t, ConstFloat*> floatConstMap;
  auto It = floatConstMap.find(Bits);
  if (It == floatConstMap.end()) {
    It = floatConstMap.emplace(Bits, new ConstFloat(val)).first;
  }
  return It->second;
}


float ConstFloat::getVal() const {return val;}

 ConstPtr::ConstPtr(std::shared_ptr<IRType> irtype):ConstantSystem(irtype){}

ConstPtr* ConstPtr::newConstPtr(std::shared_ptr<IRType> irtype){
    if (!irtype || !irtype->isPointer())
      throw std::invalid_argument("空指针常量需要指针类型");
    static std::map<std::shared_ptr<IRType>, ConstPtr *> NullPointers;
    auto &Constant = NullPointers[irtype];
    if (!Constant)
      Constant = new ConstPtr(std::move(irtype));
    return Constant;
}

UndefValue::UndefValue(std::shared_ptr<IRType> irtype):ConstantSystem(irtype){ident="undef";}

UndefValue *UndefValue::NewUndefValue(std::shared_ptr<IRType> it) {
  static std::map<std::shared_ptr<IRType>, UndefValue*> Undefs;
  UndefValue *&UV = Undefs[it];
  if (!UV)
    UV = new UndefValue(it);
  return UV;
}

BitCastInst::BitCastInst(Value* v, std::shared_ptr<IRType> targetType)
    : Instruction(validateAndGetResultType(v, targetType)) {
    instType = InstType::BITCAST;
    addUse(v);
}

void BitCastInst::dumpIR() {
    Value::dumpIR();
    std::cout << " = bitcast ";
    userList[0]->getValue()->getIRType()->toString();
    std::cout << " ";
    userList[0]->getValue()->dumpIR();
    std::cout << " to ";
    irtype->toString();
    std::cout << "\n";
}

// 创建位转换指令并追加到当前基本块。
Value* BasicBlock::genBitCastInst(Value* v, std::shared_ptr<IRType> targetType) {
    auto inst = new BitCastInst(v, targetType);
    pushBack(inst);
    return inst;
}

PhiInst::PhiInst(std::shared_ptr<IRType> irtype) : Instruction(irtype) {
    instType = InstType::PHI;
}

void PhiInst::addIncoming(Value* val, BasicBlock* bb) {
    if (!val || !bb || !val->getIRType() || !getIRType() ||
        !val->getIRType()->hasSameShape(*getIRType()))
      throw std::invalid_argument("phi 输入值必须与结果类型完全一致");
    addUse(val);
    addUse(bb);
}

unsigned PhiInst::getNumIncomingValues() const {
    return static_cast<unsigned>(userList.size() / 2);
}

Value *PhiInst::getIncomingValue(unsigned i) const {
    return i < getNumIncomingValues() ? userList[i * 2]->getValue() : nullptr;
}

BasicBlock *PhiInst::getIncomingBlock(unsigned i) const {
    if (i >= getNumIncomingValues()) return nullptr;
    return dynamic_cast<BasicBlock *>(userList[i * 2 + 1]->getValue());
}

void PhiInst::setIncomingValue(unsigned i, Value* val) {
    if (!val || i >= getNumIncomingValues()) return;
    if (!val->getIRType() || !getIRType() ||
        !val->getIRType()->hasSameShape(*getIRType()))
      throw std::invalid_argument("phi 替换值必须与结果类型完全一致");
    replaceOperand(i * 2, val);
}

bool PhiInst::setIncomingBlock(unsigned i, BasicBlock *BB) {
    if (!BB || i >= getNumIncomingValues()) return false;
    return replaceOperand(i * 2 + 1, BB);
}

unsigned PhiInst::countIncomingFromPred(BasicBlock *Pred) const {
    if (!Pred) return 0;
    unsigned Count = 0;
    for (unsigned I = 0; I < getNumIncomingValues(); ++I) {
        if (getIncomingBlock(I) == Pred) ++Count;
    }
    return Count;
}

void PhiInst::dumpIR() {
    Value::dumpIR();
    std::cout << " = phi ";
    irtype->toString();
    std::cout << " ";
    for (unsigned i = 0; i < getNumIncomingValues(); i++) {
        if (i > 0) std::cout << ", ";
        std::cout << "[ ";
        getIncomingValue(i)->dumpIR();
        std::cout << ", ";
        std::cout << "%" << getIncomingBlock(i)->getIdent();
        std::cout << " ]";
    }
    std::cout << "\n";
}

  std::vector<PhiInst*> BasicBlock::collectPhiNodes() {
    std::vector<PhiInst*> phis;
    for (auto *inst : *this) {
      auto *phi = dynamic_cast<PhiInst*>(inst);
      if (!phi) break;
      phis.push_back(phi);
    }
    return phis;
  }

// 各指令副本先按映射替换操作数，映射区域外的引用保持指向原值。
Instruction* RetInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.empty()) return new RetInst();
    if (userList.size() == 1) {
        return new RetInst(remapVal(userList[0]->getValue(), VMap));
    }
    return nullptr;
}

Instruction* BrInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.size() == 1) {
        BasicBlock *Dst = remapBB(userList[0]->getValue(), VMap);
        if (!Dst) return nullptr;
        return new BrInst(Dst);
    }
    if (userList.size() == 3) {
        Value *Cond = remapVal(userList[0]->getValue(), VMap);
        BasicBlock *T = remapBB(userList[1]->getValue(), VMap);
        BasicBlock *F = remapBB(userList[2]->getValue(), VMap);
        if (!T || !F) return nullptr;
        return new BrInst(Cond, T, F);
    }
    return nullptr;
}

Instruction* AllocaInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    return new AllocaInst(getAllocatedType());
}

Instruction* StoreInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.size() != 2) return nullptr;
    return new StoreInst(remapVal(userList[0]->getValue(), VMap),
                         remapVal(userList[1]->getValue(), VMap));
}

Instruction* LoadInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.size() != 1) return nullptr;
    return new LoadInst(remapVal(userList[0]->getValue(), VMap));
}

Instruction* GetElementPtrInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.empty()) return nullptr;
    Value *Base = remapVal(getBasePointer(), VMap);
    std::vector<Value *> Indices;
    Indices.reserve(getNumIndices());
    for (size_t I = 0; I < getNumIndices(); ++I)
        Indices.push_back(remapVal(getIndex(I), VMap));
    return new GetElementPtrInst(Base, Indices);
}

Instruction* ExtractElementInst::clone(
    const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.size() != 2) return nullptr;
    return new ExtractElementInst(
        remapVal(getVectorOperand(), VMap),
        remapVal(getIndexOperand(), VMap));
}

Instruction* InsertElementInst::clone(
    const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.size() != 3) return nullptr;
    return new InsertElementInst(
        remapVal(getVectorOperand(), VMap),
        remapVal(getElementOperand(), VMap),
        remapVal(getIndexOperand(), VMap));
}

Instruction* ShuffleVectorInst::clone(
    const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.size() != 2) return nullptr;
    return new ShuffleVectorInst(
        remapVal(getFirstVectorOperand(), VMap),
        remapVal(getSecondVectorOperand(), VMap),
        mask);
}

Instruction* BinaryInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.size() != 2) return nullptr;
    return new BinaryInst(remapVal(userList[0]->getValue(), VMap),
                          this->getOp(),
                          remapVal(userList[1]->getValue(), VMap));
}

Instruction* CallInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.empty()) return nullptr;
    Value *Callee = remapVal(userList[0]->getValue(), VMap);
    std::vector<Value *> Args;
    for (size_t i = 1; i < userList.size(); ++i) {
        Args.push_back(remapVal(userList[i]->getValue(), VMap));
    }
    auto *NewCall = new CallInst(Callee, Args);
    NewCall->setTailCall(this->isTailCall());
    return NewCall;
}

Instruction* FPTOSIInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.size() != 1) return nullptr;
    return new FPTOSIInst(
        remapVal(userList[0]->getValue(), VMap),
        getIRType());
}

Instruction* SITOFPInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.size() != 1) return nullptr;
    return new SITOFPInst(
        remapVal(userList[0]->getValue(), VMap),
        getIRType());
}

Instruction* ZextInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.size() != 1) return nullptr;
    return new ZextInst(
        remapVal(userList[0]->getValue(), VMap),
        getIRType());
}

Instruction* TruncInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
  if (userList.size() != 1) return nullptr;
  return new TruncInst(
      remapVal(userList[0]->getValue(), VMap),
      getIRType());
}

Instruction* SextInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
  if (userList.size() != 1) return nullptr;
  return new SextInst(
      remapVal(userList[0]->getValue(), VMap),
      getIRType());
}

Instruction* BitCastInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.size() != 1) return nullptr;
    return new BitCastInst(remapVal(userList[0]->getValue(), VMap), this->getIRType());
}

Instruction* PhiInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    auto *NewPhi = new PhiInst(this->getIRType());
    for (unsigned i = 0; i < this->getNumIncomingValues(); ++i) {
        Value *InV = remapVal(this->getIncomingValue(i), VMap);
        BasicBlock *InB = remapBB(this->getIncomingBlock(i), VMap);
        if (!InB) {
            delete NewPhi;
            return nullptr;
        }
        NewPhi->addIncoming(InV, InB);
    }
    return NewPhi;
}

Value* Instruction::remapVal(Value* v, const std::unordered_map<Value*, Value*>& vmap) {
    if (!v) return nullptr;
    auto it = vmap.find(v);
    return it == vmap.end() ? v : it->second;
}

BasicBlock* Instruction::remapBB(Value* v, const std::unordered_map<Value*, Value*>& vmap) {
    return dynamic_cast<BasicBlock*>(remapVal(v, vmap));
}

BrInst *BasicBlock::getBranchTerminator() {
  if (this->size() == 0) return nullptr;
  return dynamic_cast<BrInst *>(this->back());
}

bool BasicBlock::isCondBr(BrInst *Br) {
  return Br && Br->isConditional() && Br->getSuccessorCount() == 2;
}

bool BasicBlock::isUncondBr(BrInst *Br) {
  return Br && !Br->isConditional() && Br->getSuccessorCount() == 1;
}
// 统计终结指令中从当前块指向目标块的边数，平行边分别计数。
int BasicBlock::countEdgesToSucc(BasicBlock *To) {
  BrInst *Br = getBranchTerminator();
  if (!Br || !To) return 0;

  int Cnt = 0;
  for (unsigned I = 0; I < Br->getSuccessorCount(); ++I)
    if (Br->getSuccessor(I) == To) ++Cnt;
  return Cnt;
}

std::vector<PhiInst::IncomingEdge> PhiInst::collectIncomingEdges() {
  std::vector<IncomingEdge> R;
  for (unsigned i = 0; i < getNumIncomingValues(); ++i) {
    R.push_back({getIncomingValue(i), getIncomingBlock(i)});
  }
  return R;
}

bool PhiInst::rebuildFromIncoming(PhiInst *Phi, const std::vector<IncomingEdge> &Incoming) {
  if (!Phi) return false;
  for (const auto &Entry : Incoming) {
    if (!Entry.V || !Entry.Pred || !Entry.V->getIRType() ||
        !Phi->getIRType() ||
        !Entry.V->getIRType()->hasSameShape(*Phi->getIRType()))
      return false;
  }

  // 无入边时折叠为未定义值，单入边时折叠为唯一输入，多入边时重建 PHI。
  if (Incoming.empty()) {
    Phi->replaceAllUsesWith(UndefValue::NewUndefValue(Phi->getIRType()));
    delete Phi;
    return true;
  }

  if (Incoming.size() == 1) {
    Phi->replaceAllUsesWith(Incoming.front().V);
    delete Phi;
    return true;
  }

  auto *NewPhi = new PhiInst(Phi->getIRType());
  for (const auto &In : Incoming) {
    NewPhi->addIncoming(In.V, In.Pred);
  }
  Phi->replaceAllUsesWith(NewPhi);
  Phi->replaceWith(NewPhi);
  delete Phi;
  return true;
}

bool PhiInst::removeIncomingByPred(PhiInst *Phi, BasicBlock *Pred, int RemoveCount) {
  if (!Phi) return false;
  auto In = Phi->collectIncomingEdges();
  if (In.empty()) return false;

  std::vector<IncomingEdge> Keep;
  Keep.reserve(In.size());

  int Removed = 0;
  for (const auto &E : In) {
    bool Match = (E.Pred == Pred);
    bool CanRemove = (RemoveCount < 0) || (Removed < RemoveCount);
    if (Match && CanRemove) {
      ++Removed;
      continue;
    }
    Keep.push_back(E);
  }

  if (Removed == 0) return false;
  return rebuildFromIncoming(Phi, Keep);
}

bool PhiInst::removeIncomingEdge(PhiInst *Phi, BasicBlock *Pred,
                                 unsigned Occurrence) {
  if (!Phi || !Pred) return false;
  auto Incoming = Phi->collectIncomingEdges();

  // 以同一前驱的出现次序定位平行边，只删除指定的一项。
  unsigned Seen = 0;
  for (auto It = Incoming.begin(); It != Incoming.end(); ++It) {
    if (It->Pred != Pred) continue;
    if (Seen++ != Occurrence) continue;
    Incoming.erase(It);
    return rebuildFromIncoming(Phi, Incoming);
  }
  return false;
}

bool PhiInst::replaceIncomingPred(PhiInst *Phi, BasicBlock *OldPred, BasicBlock *NewPred) {
  if (!Phi || !OldPred || !NewPred) return false;
  bool Changed = false;
  for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
    if (Phi->getIncomingBlock(I) != OldPred) continue;
    Changed |= Phi->setIncomingBlock(I, NewPred);
  }
  return Changed;
}

bool PhiInst::replaceIncomingPredEdge(PhiInst *Phi, BasicBlock *OldPred,
                                      BasicBlock *NewPred,
                                      unsigned Occurrence) {
  if (!Phi || !OldPred || !NewPred || OldPred == NewPred) return false;

  unsigned Seen = 0;
  for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
    if (Phi->getIncomingBlock(I) != OldPred) continue;
    if (Seen++ != Occurrence) continue;
    return Phi->setIncomingBlock(I, NewPred);
  }
  return false;
}

void BasicBlock::removeEdgeFromSuccPhis(BasicBlock *Succ, BasicBlock *From, int RemoveCount) {
  if (!Succ || !From || RemoveCount == 0) return;
  auto Phis = Succ->collectPhiNodes();
  for (PhiInst *Phi : Phis) {
    if (!Phi) continue;
    PhiInst::removeIncomingByPred(Phi, From, RemoveCount);
  }
}

void BasicBlock::replaceIncomingPredInSuccPhis(BasicBlock *Succ, BasicBlock *OldPred, BasicBlock *NewPred) {
  if (!Succ || !OldPred || !NewPred) return;
  auto Phis = Succ->collectPhiNodes();
  for (PhiInst *Phi : Phis) {
    if (!Phi) continue;
    PhiInst::replaceIncomingPred(Phi, OldPred, NewPred);
  }
}

bool BasicBlock::redirectBranchTarget(BasicBlock *Pred, BasicBlock *OldSucc, BasicBlock *NewSucc) {
  if (!Pred || !OldSucc || !NewSucc || OldSucc == NewSucc) return false;
  BrInst *Br = Pred->getBranchTerminator();
  if (!Br) return false;

  bool Changed = false;
  for (unsigned I = 0; I < Br->getSuccessorCount(); ++I)
    if (Br->getSuccessor(I) == OldSucc)
      Changed |= Br->setSuccessor(I, NewSucc);
  return Changed;
}

bool BasicBlock::replaceCondBrWithUncond(BasicBlock *BB, BasicBlock *Dst) {
  if (!BB || !Dst) return false;
  BrInst *OldBr = BB->getBranchTerminator();
  if (!OldBr) return false;

  auto *NewBr = new BrInst(Dst);
  OldBr->replaceWith(NewBr);
  delete OldBr;
  return true;
}

bool BasicBlock::replaceTerminator(Instruction *NewTerminator) {
  if (!NewTerminator) return false;
  Instruction *OldTerminator = getTerminator();
  if (!OldTerminator) {
    delete NewTerminator;
    return false;
  }
  OldTerminator->replaceWith(NewTerminator);
  delete OldTerminator;
  return true;
}

bool BasicBlock::replaceTerminator(BasicBlock *BB,
                                   Instruction *NewTerminator) {
  if (BB) return BB->replaceTerminator(NewTerminator);
  delete NewTerminator;
  return false;
}

bool BasicBlock::onlyPhiAndTerminator() {
  if (this->size() == 0) return false;
  Instruction *Term = this->back();
  for (Instruction *I : *this) {
    if (I == Term) continue;
    if (!dynamic_cast<PhiInst *>(I)) return false;
  }
  return true;
}

void BasicBlock::eraseBlockSafely() {
  // 删块前先用同类型未定义值承接外部引用，再清空指令和块对象。
  for (Instruction *I : *this) {
    if (!I) continue;
    if (!I->getIRType() || I->getIRType()->getTypeSystem() == IR_VOID) continue;
    if (I->hasUses()) {
      I->replaceAllUsesWith(UndefValue::NewUndefValue(I->getIRType()));
    }
  }
  this->clear();
  delete this;
}

Value *PhiInst::getIncomingFromPred(BasicBlock *Pred) {
  if (!Pred) return nullptr;
  for (unsigned i = 0; i < getNumIncomingValues(); ++i) {
    if (getIncomingBlock(i) == Pred) {
      return getIncomingValue(i);
    }
  }
  return nullptr;
}

bool PhiInst::areEquivalent(PhiInst *A, PhiInst *B) {
  if (!A || !B) return false;
  if (A->getIRType() != B->getIRType()) return false;
  if (A->getNumIncomingValues() != B->getNumIncomingValues()) return false;

  for (unsigned i = 0; i < A->getNumIncomingValues(); ++i) {
    if (A->getIncomingValue(i) != B->getIncomingValue(i)) return false;
    if (A->getIncomingBlock(i) != B->getIncomingBlock(i)) return false;
  }
  return true;
}
