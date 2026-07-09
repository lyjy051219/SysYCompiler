#include "../../include/lib/CFG.hpp"
#include "../../include/lib/BaseManager.hpp"
#include "CFG.hpp"
#include <cmath>
#include <sstream>
#include <map>
#include <unordered_set>
#include <variant>
#include <limits>

extern std::unique_ptr<Module> ModuleIR;

bool checkBinaryBoolean(BinaryInst::Operation op) {
  return BinaryInst::isCompareOp(op);
}

static bool isIntOnlyBinaryOp(BinaryInst::Operation op) {
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

static Instruction::InstType mapBinaryOpToInstType(BinaryInst::Operation op,
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

bool Value::isConstZero() {
    if (auto num_val = dynamic_cast<ConstInt*>(this))
        return num_val->getVal() == 0;
    else if (auto num_val = dynamic_cast<ConstFloat*>(this))
        return num_val->getVal() == 0.0f;
    else if (auto num_val = dynamic_cast<ConstBool*>(this))
        return num_val->getVal() == false;
    else
        return false;
}

bool Value::isConstIntZero() {
    auto *CI = dynamic_cast<ConstInt *>(this);
    return CI && CI->getVal() == 0;
}

bool Value::isConstOne() {
    if (auto *CI = dynamic_cast<ConstInt *>(this)) return CI->getVal() == 1;
    if (auto *CF = dynamic_cast<ConstFloat *>(this)) return CF->getVal() == 1.0f;
    if (auto *CB = dynamic_cast<ConstBool *>(this)) return CB->getVal();
    return false;
}

bool Value::isConstMinusOne() {
    if (auto *CI = dynamic_cast<ConstInt *>(this)) return CI->getVal() == -1;
    if (auto *CF = dynamic_cast<ConstFloat *>(this)) return CF->getVal() == -1.0f;
    return false;
}

bool Value::tryGetBoolConst(bool &Out) {
    if (auto *CB = dynamic_cast<ConstBool *>(this)) {
        Out = CB->getVal();
        return true;
    }
    if (auto *CI = dynamic_cast<ConstInt *>(this)) {
        Out = CI->getVal() != 0;
        return true;
    }
    if (auto *CF = dynamic_cast<ConstFloat *>(this)) {
        Out = CF->getVal() != 0.0f;
        return true;
    }
    return false;
}

bool Value::tryGetIntConst(int32_t &Out) {
    if (auto *CI = dynamic_cast<ConstInt *>(this)) {
        Out = CI->getVal();
        return true;
    }
    if (auto *CB = dynamic_cast<ConstBool *>(this)) {
        Out = CB->getVal() ? 1 : 0;
        return true;
    }
    return false;
}

bool Value::tryGetFloatConst(float &Out) {
    if (auto *CF = dynamic_cast<ConstFloat *>(this)) {
        Out = CF->getVal();
        return true;
    }
    return false;
}

int Value::integerBitWidth(int DefaultBits) {
    if (!getIRType()) return DefaultBits;
    return getIRType()->getIntegerBitWidth(DefaultBits);
}

int Value::countUses() {
    int Count = 0;
    for (auto It = getUseList().begin(); It != getUseList().end(); ++It) {
        Use *U = *It;
        if (!U || U->getValue() != this) continue;
        ++Count;
    }
    return Count;
}

int Value::countInstructionUses(Instruction **SingleUser) {
    if (SingleUser) *SingleUser = nullptr;

    int Count = 0;
    Instruction *OnlyInstUser = nullptr;
    for (auto It = getUseList().begin(); It != getUseList().end(); ++It) {
        Use *U = *It;
        if (!U || U->getValue() != this) continue;
        auto *InstUser = dynamic_cast<Instruction *>(U->getUser());
        if (!InstUser) continue;
        ++Count;
        if (Count == 1) {
            OnlyInstUser = InstUser;
        } else {
            OnlyInstUser = nullptr;
        }
    }

    if (SingleUser && Count == 1) *SingleUser = OnlyInstUser;
    return Count;
}

bool Value::hasUses() {
    return countUses() > 0;
}

bool Value::hasExactlyOneUse() {
    int Count = 0;
    for (auto It = getUseList().begin(); It != getUseList().end(); ++It) {
        Use *U = *It;
        if (!U || U->getValue() != this) continue;
        ++Count;
        if (Count > 1) return false;
    }
    return Count == 1;
}

bool Value::isPointerValue() {
    return getIRType() && getIRType()->isPointer();
}

bool Value::isExternalVariable() {
    auto *Var = dynamic_cast<Variable *>(this);
    return Var && (Var->isGlobal() || Var->isParam());
}

Value *Value::stripPointerCastsAndGEP(Value *V) {
    std::unordered_set<Value *> Visited;
    while (V && Visited.insert(V).second) {
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(V)) {
            V = GEP->getOperand(0);
            continue;
        }
        if (auto *BC = dynamic_cast<BitCastInst *>(V)) {
            V = BC->getOperand(0);
            continue;
        }
        break;
    }
    return V;
}

bool Value::isLocalStackPointer(Value *V) {
    return dynamic_cast<AllocaInst *>(stripPointerCastsAndGEP(V)) != nullptr;
}

Value *Value::makeZeroForType(const std::shared_ptr<IRType> &Ty) {
    if (!Ty) return ConstInt::newConstInt(0);
    if (Ty->isFloat()) return ConstFloat::newConstFloat(0.0f);
    if (Ty->isBool()) return ConstBool::newConstBool(false);
    if (Ty->isInt()) return ConstInt::newConstInt(0, Ty->getSize());
    return ConstInt::newConstInt(0);
}

Value *Value::makeOneForType(const std::shared_ptr<IRType> &Ty) {
    if (!Ty) return ConstInt::newConstInt(1);
    if (Ty->isFloat()) return ConstFloat::newConstFloat(1.0f);
    if (Ty->isBool()) return ConstBool::newConstBool(true);
    if (Ty->isInt()) return ConstInt::newConstInt(1, Ty->getSize());
    return ConstInt::newConstInt(1);
}

Value *Value::makeMinusOneForType(const std::shared_ptr<IRType> &Ty) {
    if (!Ty) return ConstInt::newConstInt(-1);
    if (Ty->isFloat()) return ConstFloat::newConstFloat(-1.0f);
    if (Ty->isBool()) return ConstBool::newConstBool(true);
    if (Ty->isInt()) return ConstInt::newConstInt(-1, Ty->getSize());
    return ConstInt::newConstInt(-1);
}

Value *Value::makeUndefForType(const std::shared_ptr<IRType> &Ty) {
    return UndefValue::NewUndefValue(Ty ? Ty : INTType::NewInt(4));
}

size_t User::getNumOperands() const {
    return userList.size();
}

Value *User::getOperand(size_t Index) const {
    if (Index >= userList.size() || !userList[Index]) return nullptr;
    return userList[Index]->getValue();
}

bool User::getOperand(size_t Index, Value *&Out) const {
    Out = getOperand(Index);
    return Out != nullptr;
}

bool User::replaceOperand(size_t Index, Value *V) {
    if (Index >= userList.size() || !userList[Index] || !V) return false;
    return userList[Index]->replaceUseeWith(V);
}

void Value::dumpIR() {
    if (isConst()) std::cout << getIdent();
    else if (isGlobal()) std::cout << "@" << getIdent();
    else if (auto tmp = dynamic_cast<Function*>(this)) std::cout << "@" << tmp->getIdent();
    else if (auto tmp = dynamic_cast<BuildInFunction*>(this)) std::cout << "@" << tmp->getIdent();
    else if (getIdent() == "undef") std::cout << getIdent();
    else std::cout << "%" << getIdent();
}


Function& Module::newFunction(TypeSystem ts,std::string ident){
    auto func = new Function(ts,ident);
    this->getSymbolTable().register_symbolTable(ident,func);
    this->functions.push_back(std::unique_ptr<Function>(func));
    return *(this->functions.back());
}

void Module::pushFunction(Function* func){
    this->symbolTable.register_symbolTable(func->getIdent(),func);
    functions.push_back(std::unique_ptr<Function>(func));
}


std::vector<std::unique_ptr<Function>>& Module::getFunctions(){
    return functions;
}

void Module::deleteFunction(Function*ptr){
    for(auto it = functions.begin();it != functions.end();it++){
        auto func = (*it).get();
        if(func == ptr){
            functions.erase(it);
            break;
        }
    }
}

Function* Module::getMainFunction(){
    for(auto& i: functions){
        if(i->getIdent() == "main") return i.get();
    }
    return nullptr;
}

Module* Module::getActiveModule() {
  return ModuleIR.get();
}

void Module::pushGlobalVar(Variable* ptr){
    globalvars.emplace_back(ptr);
}


std::vector<std::unique_ptr<Variable>>& Module::getGlobalVar(){return globalvars;}

SymbolTable& Module::getSymbolTable(){return symbolTable;}

void Module::dumpIR() {
    for(auto& i : globalvars) i->dumpIR();
    for(auto& i : functions) i->dumpIR();
}
std::string Module::genFuncNameNum(std::string ident){
for (int i = 0;; i++) {
    bool HaveSameName = false;
    for (auto &func : functions) {
      if (func->getIdent() == (ident + "_" + std::to_string(i))) {
        HaveSameName = true;
        break;
      }
    }
    if (!HaveSameName)
      return (ident + "_" + std::to_string(i));
  }
}

Function::Function(TypeSystem ts,std::string ident) : Value(IRType::NewTypeByTypeSystem(ts),ident){
    //创建的时候就会在侵入式链表里面加入一个BasicBlock
    push_back(new BasicBlock());
}



void Function::pushBlock(BasicBlock* bb){
    push_back(bb);
}



void Function::insertBlock(BasicBlock* curr,BasicBlock* insert){
    insert->genUnCondInst(curr);
    insert->num = this->blockNum++;
    this->pushBlock(insert);
}

void Function::pushParam(Variable* v){ params.emplace_back(v); }

void Function::pushParam(std::string ident,Variable* v){
    //不仅vector中需要写入，而且其创建的inst也要加入侵入式链表之中
    auto alloca = new AllocaInst(POINTERType::NewPointer(v->getIRType()));
    auto store = new StoreInst(v,alloca);
    front()->push_front(alloca);
    front()->push_back(store);
    ModuleIR->getSymbolTable().register_symbolTable(ident,alloca);
    params.emplace_back(std::unique_ptr<Variable>(v));
}

std::vector<std::unique_ptr<Value>>& Function::getParams(){return params;}

void Function::InsertAlloca(AllocaInst* ptr){front()->push_back(ptr);}

bool Function::isReachable(BasicBlock* BB) {
  if (!BB) return false;

  std::unordered_set<BasicBlock *> Reachable;
  collectReachableBlocks(Reachable);
  return Reachable.count(BB) != 0;
}

void Function::collectReachableBlocks(
    std::unordered_set<BasicBlock *> &Reachable) {
  Reachable.clear();
  if (this->Size() == 0 || !front()) return;

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

std::vector<BasicBlock *> Function::getReachableBlocks() {
  std::unordered_set<BasicBlock *> ReachableSet;
  collectReachableBlocks(ReachableSet);

  std::vector<BasicBlock *> Blocks;
  Blocks.reserve(ReachableSet.size());
  for (BasicBlock *BB : *this) {
    if (BB && ReachableSet.count(BB)) Blocks.push_back(BB);
  }
  return Blocks;
}

void Function::dropAllReferences() {
  for (BasicBlock *BB : *this) {
    if (!BB) continue;
    for (Instruction *I : *BB) {
      if (!I) continue;
      I->getUserList().clear();
    }
  }
}

void Function::dumpIR(){
  std::cout << "define ";
  this->getIRType()->to_string();
  std::cout << " @" << ident << "(";
  for (auto &i : params) {
    i->getIRType()->to_string();
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

BuildInFunction *BuildInFunction::genBuildInFunction(std::string ident) {
  static std::map<std::string, BuildInFunction *> mp;
  auto gt = [&ident]() -> std::shared_ptr<IRType> {
    if (ident == "getint")
      return INTType::NewInt(4);
    if (ident == "getfloat")
      return FLOATType::NewFloat();
    if (ident == "getch")
      return INTType::NewInt(4);
    if (ident == "getarray")
      return INTType::NewInt(4);
    if (ident == "getfarray")
      return INTType::NewInt(4);
    if (ident == "putint")
      return VOIDType::NewVoid();
    if (ident == "putch")
      return VOIDType::NewVoid();
    if (ident == "putarray")
      return VOIDType::NewVoid();
    if (ident == "putfloat")
      return VOIDType::NewVoid();
    if (ident == "putfarray")
     return VOIDType::NewVoid();
    if (ident == "starttime")
      return VOIDType::NewVoid();
    if (ident == "stoptime")
      return VOIDType::NewVoid();
    if (ident == "putf")
      return VOIDType::NewVoid();
    if (ident == "llvm.memcpy.p0i8.p0i8.i32" || ident == "llvm.memcpy.p0.p0.i32")
        return VOIDType::NewVoid();
    if (ident == "memcpy@plt")
      return VOIDType::NewVoid();
    if (ident == "buildin_NotifyWorkerLE")
      return VOIDType::NewVoid();
    if (ident == "buildin_NotifyWorkerLT")
      return VOIDType::NewVoid();
    if (ident == "buildin_FenceArgLoaded")
      return VOIDType::NewVoid();
    if (ident == "buildin_AtomicF32add")
      return VOIDType::NewVoid();
    if(ident == "CacheLookUp")
     return VOIDType::NewVoid();
    if(ident == "CacheLookUp4")
      return VOIDType::NewVoid();
    assert(0);
    return nullptr;
  };
  if (mp.find(ident) == mp.end()) {
    mp[ident] = new BuildInFunction(gt(), ident);
  }
  return mp[ident];
}

bool BuildInFunction::isKnownRuntimeName(const std::string &Name) {
  static const std::unordered_set<std::string> SysyLibNames = {
      "getint", "getch", "getfloat", "getarray", "getfarray",
      "putint", "putch", "putfloat", "putarray", "putfarray", "putf",
      "starttime", "stoptime", "_sysy_starttime", "_sysy_stoptime"};

  if (SysyLibNames.count(Name)) return true;
  if (Name.find("_sysy_") != std::string::npos) return true;
  if (Name.find("memcpy") != std::string::npos) return true;
  if (Name.find("llvm.") == 0) return true;
  return false;
}



BasicBlock::BasicBlock():Value(VOIDType::NewVoid()){};

void BasicBlock::deleteBlock(){
    this->~BasicBlock();
}

void BasicBlock::dumpIR(){
  std::cout << this->getIdent() << ":\n";
  for (auto i : (*this)) {
    std::cout << "  ";
    i->dumpIR();
  }

}

// CFG.cpp

std::vector<BasicBlock*> BasicBlock::getSuccessors() {
    std::vector<BasicBlock*> succs;
    // 1. 如果块为空，没有后继
    if (this->Size() == 0) return succs;
    // 2. 获取最后一条指令（List 提供的 back() 接口）
    Instruction* term = this->back();

    // 3. 只有 BR 指令才有 CFG 图意义上的后继
    // (RetInst 表示函数结束，没有后继块)
    if (term->instType == Instruction::BR) {
        // 获取操作数列表 (Instruction 继承自 User)
        auto& uses = term->getUserList();
        // 4. 根据 userList 的大小判断是条件跳转还是无条件跳转
        if (uses.size() == 1) {
            // === 无条件跳转: br label %dest ===
            // 构造时：addUse(bb) -> index 0
            // 获取 Value* 并转为 BasicBlock*
            Value* op = uses[0]->getValue();
            if (BasicBlock* bb = dynamic_cast<BasicBlock*>(op)) {
                succs.push_back(bb);
            }
        }
        else if (uses.size() == 3) {
            // === 条件跳转: br i1 %cond, label %true, label %false ===
            // 构造时：addUse(cond)[0], addUse(true)[1], addUse(false)[2]
            // 获取 True 分支
            Value* trueOp = uses[1]->getValue();
            if (BasicBlock* trueBB = dynamic_cast<BasicBlock*>(trueOp)) {
                succs.push_back(trueBB);
            }
            // 获取 False 分支
            Value* falseOp = uses[2]->getValue();
            if (BasicBlock* falseBB = dynamic_cast<BasicBlock*>(falseOp)) {
                succs.push_back(falseBB);
            }
        }
    }
    return succs;
}

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

void BasicBlock::genRetInst(Value* retVal){
    if(getParent()->getIRType()->getEnum() != retVal->getIRType()->getEnum()){
        if(retVal->getIRType()->getEnum() == IR_INT) retVal = genSITOFPInst(retVal);
        else retVal = genFPTOSIInst(retVal);
    }
    auto inst = new RetInst(retVal);
    push_back(inst);
}

void BasicBlock::genRetInst(){
    auto inst = new RetInst();
    push_back(inst);
}

void BasicBlock::genCondInst(Value* cond,BasicBlock* isTrue,BasicBlock* isFalse){
   auto inst = new BrInst(cond,isTrue,isFalse);
   push_back(inst);
}

void BasicBlock::genUnCondInst(BasicBlock *block) {
  auto inst = new BrInst(block);
  push_back(inst);
}

AllocaInst* BasicBlock::genAllocaInst(std::shared_ptr<IRType> irtype,std::string ident){
    auto tp = POINTERType::NewPointer(irtype);
    auto alloca = new AllocaInst(tp);
    ModuleIR->getSymbolTable().register_symbolTable(ident,alloca);
    getParent()->front()->push_front(alloca);
    return alloca;
}

Value* BasicBlock::genLoadInst(Value* v){
    auto inst = new LoadInst(v);
    push_back(inst);
    return inst->getOprand();
}

void BasicBlock::genStoreInst(Value* val,Value* ptr){
    auto point = std::dynamic_pointer_cast<POINTERType>(ptr->getIRType());
    assert(point && "StoreInst pointer operand is not a pointer type!"); // 安全检查
    if(point->getPointerType()->getEnum() != val->getIRType()->getEnum()){
        if(point->getPointerType()->getEnum()  == IR_INT) val = this->genFPTOSIInst(val);
        else val = this->genSITOFPInst(val);
    }
    auto inst = new StoreInst(val,ptr);
    this->push_back(inst);
}

Value* BasicBlock::genGepInst(Value* ptr){
    auto inst = new GetElementPtrInst(ptr);
    push_back(inst);
    return inst->getOprand();
}

Value* BasicBlock::genBinaryInst(Value* A, BinaryInst::Operation op, Value* B){
  bool a = (A->getIRType()->getEnum() == IR_INT);
  bool b = (B->getIRType()->getEnum() == IR_INT);
    BinaryInst* inst;

  if (isIntOnlyBinaryOp(op)) {
    if (!a) {
      A = genFPTOSIInst(A);
      a = true;
    }
    if (!b) {
      B = genFPTOSIInst(B);
      b = true;
    }

    bool aa = A->getIRType() == INTType::NewInt(4);
    bool bb = B->getIRType() == INTType::NewInt(4);
    if (aa ^ bb) {
      if (!aa) A = genZextInst(A);
      else B = genZextInst(B);
    }

    inst = new BinaryInst(A, op, B);
    push_back(inst);
    return inst->getOprand();
  }

    if(a ^ b) {
        if(a) {
            A = genSITOFPInst(A);
        } else {
            B = genSITOFPInst(B);
        }
        inst = new BinaryInst(A, op, B);
    }
    else {
        if(a){
            bool aa = A->getIRType() == INTType::NewInt(4);
            bool bb = B->getIRType() == INTType::NewInt(4);
            if(aa ^ bb){
                if(!aa) A = genZextInst(A); // i1 -> i32
                else B = genZextInst(B);    // i1 -> i32
            }
        }
        inst = new BinaryInst(A, op, B);
    }
    push_back(inst);
    return inst->getOprand();
}

Value* BasicBlock::genBinaryInst(BasicBlock* bb ,Value* _A,BinaryInst::Operation op,Value* _B){
  if (_A->isConst() && _B->isConst()) {
    if ((op == BinaryInst::DIV || op == BinaryInst::MOD ||
         op == BinaryInst::UDIV || op == BinaryInst::UREM) &&
        _B->isConstZero()) {
      if (_A->getIRType() == INTType::NewInt(4) &&
          _B->getIRType() == INTType::NewInt(4))
        return UndefValue::NewUndefValue(INTType::NewInt(4));
      else
        return UndefValue::NewUndefValue(FLOATType::NewFloat());
    }
    auto calc = [](auto a, BinaryInst::Operation op,
                   auto b) -> std::variant<float, int> {
      switch (op) {
      case BinaryInst::ADD:
        return a + b;
      case BinaryInst::SUB:
        return a - b;
      case BinaryInst::MUL:
        return a * b;
      case BinaryInst::DIV:
        return a / b;
      case BinaryInst::UDIV:
        return static_cast<int>(static_cast<unsigned int>(a) /
                                static_cast<unsigned int>(b));
      case BinaryInst::AND:
        return (a != 0) && (b != 0);
      case BinaryInst::OR:
        return (a != 0) || (b != 0);
      case BinaryInst::MOD:
        return (int)a % (int)b;
      case BinaryInst::UREM:
        return static_cast<int>(static_cast<unsigned int>(a) %
                                static_cast<unsigned int>(b));
      case BinaryInst::SHL:
        return static_cast<int>(a) << static_cast<int>(b);
      case BinaryInst::LSHR:
        return static_cast<int>(static_cast<unsigned int>(a) >> static_cast<int>(b));
      case BinaryInst::ASHR:
        return static_cast<int>(a) >> static_cast<int>(b);
      case BinaryInst::E:
        return a == b;
      case BinaryInst::NE:
        return a != b;
      case BinaryInst::G:
        return a > b;
      case BinaryInst::GE:
        return a >= b;
      case BinaryInst::L:
        return a < b;
      case BinaryInst::LE:
        return a <= b;
      default:
        assert(0);
        return 0;
      }
    };
    std::variant<float, int> fuc;
    if (auto A = dynamic_cast<ConstInt *>(_A)) {
      if (auto B = dynamic_cast<ConstInt *>(_B))
        fuc = calc(A->getVal(), op, B->getVal());
      else if (auto B = dynamic_cast<ConstFloat *>(_B))
        fuc = calc(A->getVal(), op, B->getVal());
      else if (auto B = dynamic_cast<ConstBool *>(_B))
        fuc = calc(A->getVal(), op, B->getVal());
      else
        assert(0);
    } else if (auto A = dynamic_cast<ConstFloat *>(_A)) {
      if (auto B = dynamic_cast<ConstInt *>(_B))
        fuc = calc(A->getVal(), op, B->getVal());
      else if (auto B = dynamic_cast<ConstFloat *>(_B))
        fuc = calc(A->getVal(), op, B->getVal());
      else if (auto B = dynamic_cast<ConstBool *>(_B))
        fuc = calc(A->getVal(), op, B->getVal());
      else
        assert(0);
    } else if (auto A = dynamic_cast<ConstBool *>(_A)) {
      if (auto B = dynamic_cast<ConstInt *>(_B))
        fuc = calc(A->getVal(), op, B->getVal());
      else if (auto B = dynamic_cast<ConstFloat *>(_B))
        fuc = calc(A->getVal(), op, B->getVal());
      else if (auto B = dynamic_cast<ConstBool *>(_B))
        fuc = calc(A->getVal(), op, B->getVal());
      else
        assert(0);
    } else
      assert(0);
    if (checkBinaryBoolean(op))
      return ConstBool::newConstBool(std::get<int>(fuc));
    else if (std::holds_alternative<int>(fuc))
      return ConstInt::newConstInt(std::get<int>(fuc));
    else
      return ConstFloat::newConstFloat(std::get<float>(fuc));
  } else {
    assert(bb != nullptr);
    return bb->genBinaryInst(_A, op, _B);
  }
}

Value* BasicBlock::genSITOFPInst(Value* x){
    auto inst = new SITOFPInst(x);
    push_back(inst);
    return inst->getOprand();
}

Value* BasicBlock::genFPTOSIInst(Value* x){
    auto inst = new FPTOSIInst(x);
    push_back(inst);
    return inst->getOprand();
}

Value* BasicBlock::genZextInst(Value* x){
    auto inst = new ZextInst(x);
    push_back(inst);
    return inst->getOprand();
}

Value* BasicBlock::genTruncInst(Value* x){
  auto inst = new TruncInst(x);
  push_back(inst);
  return inst->getOprand();
}

Value* BasicBlock::genSextInst(Value* x){
  auto inst = new SextInst(x);
  push_back(inst);
  return inst->getOprand();
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

// 在 BasicBlock::genCallInst 函数中修改类型检查逻辑
Value* BasicBlock::genCallInst(std::string ident, std::vector<Value*> args) {
  auto check_builtin = [](std::string _id) {
    if (_id == "getint") return true;
    if (_id == "getfloat") return true;
    if (_id == "getch") return true;
    if (_id == "getarray") return true;
    if (_id == "getfarray") return true;
    if (_id == "putint") return true;
    if (_id == "putch") return true;
    if (_id == "putarray") return true;
    if (_id == "putfloat") return true;
    if (_id == "putfarray") return true;
    if (_id == "starttime") return true;
    if (_id == "stoptime") return true;
    if (_id == "putf") return true;
    if (_id == "llvm.memcpy.p0i8.p0i8.i32") return true;
    if (_id == "llvm.memcpy.p0.p0.i32") return true;
    return false;
  };

  if (check_builtin(ident)) {
    ModuleIR->registerBuiltin(ident);
    if (ident == "putint" || ident == "putch" || ident == "putarray" ||
        ident == "putfarray") {
      if (args[0]->getIRType()->getEnum() == IR_FLOAT)
        args[0] = genFPTOSIInst(args[0]);
    }
    if (ident == "putfloat") {
      if (args[0]->getIRType()->getEnum() == IR_INT)
        args[0] = genSITOFPInst(args[0]);
    }
    auto tmp = new CallInst(BuildInFunction::genBuildInFunction(ident), args);
    push_back(tmp);
    return tmp->getOprand();
  }

  if (auto func = dynamic_cast<Function*>(ModuleIR->getSymbolTable().getValueByName(ident))) {
    auto &params = func->getParams();
    assert(args.size() == params.size());
    auto i = args.begin();
    for (auto j = params.begin(); j != params.end(); j++, i++) {
      auto &ii = *i;
      auto jj = j->get();
      if (jj->getIRType() != ii->getIRType()) {
        auto a = ii->getIRType()->getEnum(), b = jj->getIRType()->getEnum();

        // ============ 修复：增加对实参类型的检查 ============
        // 如果形参或实参是指针/数组类型，跳过基本类型转换
        if (b == IR_POINTER || a == IR_POINTER || a == IR_ARRAY) {
          continue;
        }
        // ===================================================

        // 只对基本类型（INT/FLOAT）进行转换
        assert(a == IR_INT || a == IR_FLOAT);
        assert(b == IR_INT || b == IR_FLOAT);
        if (b == IR_FLOAT)
          ii = genSITOFPInst(ii);
        else
          ii = genFPTOSIInst(ii);
      }
    }
    auto inst = new CallInst(func, args);
    push_back(inst);
    return inst->getOprand();
  } else {
    std::cerr << "No Such Function!\n";
    assert(0);
    return nullptr;
  }
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
  push_back(I);
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
  push_back(I);
  return true;
}

void BasicBlock::clear(){
    for(auto inst : *this) assert(inst->getUseList().size == 0);
    ChainList<BasicBlock,Instruction>::clear();
}

Value* Instruction::getOprand(){return dynamic_cast<Value*>(this);}

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

RetInst::RetInst(Value* retVal){
    addUse(retVal);
    instType = InstType::RET;
}
RetInst::RetInst(){instType = InstType::RET;}
RetInst::RetInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::RET;}
Value* RetInst::getOprand(){return nullptr;}
void RetInst::dumpIR(){
    std::cout << "ret ";
  for (auto &i : userList) {
    i->getValue()->getIRType()->to_string();
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
BrInst::BrInst(std::shared_ptr<IRType> irtype,bool is_Cond):Instruction(irtype){instType = InstType::BR; isCond = is_Cond;}

Value* BrInst::getOprand(){
    return nullptr;
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
            i->getValue()->getIRType()->to_string();
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

AllocaInst::AllocaInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::ALLOCA;}
AllocaInst::AllocaInst(std::string ident,std::shared_ptr<IRType> irtype):Instruction(POINTERType::NewPointer(irtype)){
    instType = InstType::ALLOCA;
    //ident的相关映射没有去写，等到后面再补吧
}
void AllocaInst::dumpIR(){
    Value::dumpIR();
    std::cout << " = alloca ";
    std::dynamic_pointer_cast<POINTERType>(irtype)->getPointerType()->to_string();
    std::cout << "\n";
}

StoreInst::StoreInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::STORE;}
StoreInst::StoreInst(Value* val,Value* ptr){
    addUse(val);
    addUse(ptr);
    ident = "StoreInst";
    instType = InstType::STORE;
}

Value* getOprand(){return nullptr;}

Value* StoreInst::getValueOperand() const {
    return getOperand(0);
}

Value* StoreInst::getPointerOperand() const {
    return getOperand(1);
}

void StoreInst::dumpIR(){
  std::cout << "store ";
  for (auto &i : userList) {
    i->getValue()->getIRType()->to_string();
    std::cout << " ";
    i->getValue()->dumpIR();
    if (i.get() != userList.back().get())
      std::cout << ", ";
  }
  std::cout << '\n';
}

LoadInst::LoadInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::LOAD;}
LoadInst::LoadInst(Value* v) : Instruction(VOIDType::NewVoid()) { // 先传一个安全类型防止崩溃
    auto ptrType = std::dynamic_pointer_cast<POINTERType>(v->getIRType());
    assert(ptrType && "LoadInst operand is not a pointer type!"); // 断言检查

    // 手动更新父类 Instruction 的 irtype 成员
    this->irtype = ptrType->getPointerType();

    addUse(v);
    instType = InstType::LOAD;
}

Value* LoadInst::getPointerOperand() const {
  return getOperand(0);
}

void LoadInst::dumpIR(){
  Value::dumpIR();
  std::cout << " = load ";
  irtype->to_string();
  std::cout << ", ";
  for (auto &i : userList) {
    i->getValue()->getIRType()->to_string();
    std::cout << " ";
    i->getValue()->dumpIR();
    if (i.get() != userList.back().get())
      std::cout << ", ";
  }
  std::cout << '\n';
}

GetElementPtrInst::GetElementPtrInst(Value* v) : Instruction(v->getIRType()) {
    addUse(v);
    instType = InstType::GEP;
}
GetElementPtrInst::GetElementPtrInst(Value* v, std::vector<Value*>& vv) : Instruction(v->getIRType()) {
    addUse(v);
    for(auto &&i : vv) addUse(i);
    instType = InstType::GEP;
    // 注意：如果有索引列表，这里其实应该立即重新计算类型，见第二步
}
GetElementPtrInst::GetElementPtrInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::GEP;}
void GetElementPtrInst::dumpIR() {
    // 打印结果变量
    this->Value::dumpIR();  // 这会打印出类似 "%1" 的标识符
    std::cout << " = getelementptr inbounds ";

    // 获取基指针类型（第一个操作数）
    Value* basePtr = userList[0]->getValue();
    auto baseType = basePtr->getIRType();

    // 处理指针类型，获取元素类型
    std::shared_ptr<IRType> elementType;
    if (baseType->getEnum() == IR_POINTER) {
        auto ptrType = std::dynamic_pointer_cast<POINTERType>(baseType);
        elementType = ptrType->getPointerType();
    } else {
        // 如果不是指针类型，可能有问题，但先假设是指针
        elementType = baseType;
    }

    // 打印元素类型
    elementType->to_string();
    std::cout << ", ";
    // 打印基指针类型和值
    basePtr->getIRType()->to_string();
    std::cout << " ";
    basePtr->dumpIR();

    // 打印索引
    for (size_t i = 1; i < userList.size(); i++) {
        Value* index = userList[i]->getValue();
        std::cout << ", ";
        index->getIRType()->to_string();
        std::cout << " ";
        index->dumpIR();
    }
    std::cout << "\n";
}

void GetElementPtrInst::updateType() {
    if (userList.empty()) return;

    // 获取基地址的类型 (第一个操作数)
    Value* basePtr = userList[0]->getValue();
    auto currentType = basePtr->getIRType();

    // 从第二个操作数开始遍历（这些是索引）
    for (size_t i = 1; i < userList.size(); i++) {
        // 如果当前是指针，解引用一层
        if (auto ptr = std::dynamic_pointer_cast<POINTERType>(currentType)) {
            currentType = ptr->getPointerType();
        }
        // 如果当前是数组，剥离一层维度
        else if (auto arr = std::dynamic_pointer_cast<ARRAYType>(currentType)) {
            currentType = arr->getElementType();
        }
        else {
            // 错误处理：索引过多或类型不匹配
            assert(0 && "GEP index out of bound or invalid type");
        }
    }

    // GEP 的结果永远是指向最终元素的指针
    this->irtype = POINTERType::NewPointer(currentType);
}

std::optional<int64_t> GetElementPtrInst::getLastIndexStride() {
    auto &Operands = getUserList();
    if (Operands.size() < 2 || !Operands.front()) return std::nullopt;

    Value *Base = Operands.front()->getValue();
    if (!Base || !Base->getIRType()) return std::nullopt;

    std::shared_ptr<IRType> CurrentTy = Base->getIRType();
    int64_t LastStride = 0;
    for (size_t I = 1; I < Operands.size(); ++I) {
        std::shared_ptr<IRType> ElementTy;
        if (auto PtrTy = std::dynamic_pointer_cast<POINTERType>(CurrentTy)) {
            ElementTy = PtrTy->getPointerType();
        } else if (auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(CurrentTy)) {
            ElementTy = ArrTy->getElementType();
        } else {
            return std::nullopt;
        }
        if (!ElementTy ||
            ElementTy->getSize() >
                static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
            return std::nullopt;
        }
        LastStride = static_cast<int64_t>(ElementTy->getSize());
        CurrentTy = ElementTy;
    }
    return LastStride;
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

    auto &Operands = GEP->getUserList();
    if (Operands.empty() || !Operands.front()) return false;

    Value *LocalBase = Operands.front()->getValue();
    auto *BaseGEP = dynamic_cast<GetElementPtrInst *>(LocalBase);
    if (BaseGEP) {
        Value *FirstIndex = nullptr;
        if (Operands.size() < 2 || !Operands[1] ||
            !(FirstIndex = Operands[1]->getValue()) ||
            !FirstIndex->isConstIntZero()) {
            return false;
        }
        if (!flattenZeroBasedChain(BaseGEP, Base, Indices))
            return false;
        for (size_t I = 2; I < Operands.size(); ++I) {
            if (!Operands[I] || !Operands[I]->getValue()) return false;
            Indices.push_back(Operands[I]->getValue());
        }
        return true;
    }

    Base = LocalBase;
    for (size_t I = 1; I < Operands.size(); ++I) {
        if (!Operands[I] || !Operands[I]->getValue()) return false;
        Indices.push_back(Operands[I]->getValue());
    }
    return Base != nullptr;
}

bool GetElementPtrInst::getIndexStrides(Value *Base,
                                        const std::vector<Value *> &Indices,
                                        std::vector<int64_t> &Strides,
                                        int64_t &FinalPointeeStride) {
    Strides.clear();
    FinalPointeeStride = 0;
    if (!Base || !Base->getIRType()) return false;

    std::shared_ptr<IRType> CurrentTy = Base->getIRType();
    for (Value *Idx : Indices) {
        (void)Idx;
        std::shared_ptr<IRType> ElementTy;
        if (auto PtrTy = std::dynamic_pointer_cast<POINTERType>(CurrentTy)) {
            ElementTy = PtrTy->getPointerType();
        } else if (auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(CurrentTy)) {
            ElementTy = ArrTy->getElementType();
        } else {
            return false;
        }
        if (!ElementTy ||
            ElementTy->getSize() >
                static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
            return false;
        }
        int64_t Stride = static_cast<int64_t>(ElementTy->getSize());
        Strides.push_back(Stride);
        FinalPointeeStride = Stride;
        CurrentTy = ElementTy;
    }
    return FinalPointeeStride > 0 && Strides.size() == Indices.size();
}

BinaryInst::BinaryInst(std::shared_ptr<IRType> irtype):Instruction(irtype){
    instType = InstType::BinaryUnknown;
}

BinaryInst::BinaryInst(Value* x,Operation op,Value* y)
    :Instruction(x->getIRType()),op(op){
    if (checkBinaryBoolean(op)) {
      this->irtype = INTType::NewBool();
    }
    this->instType = mapBinaryOpToInstType(op, x->getIRType()->getEnum());
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

void BinaryInst::dumpIR(){
  Value::dumpIR();
  TypeSystem tp = userList[0]->getValue()->getIRType()->getEnum();
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
      std::cout << "frem "; ///应该不存在
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
    if (tp == IR_FLOAT) std::cout << "fcmp ueq ";
    else                std::cout << "icmp eq ";
    break;
  case BinaryInst::NE:
    if (tp == IR_FLOAT) std::cout << "fcmp une ";
    else                std::cout << "icmp ne ";
    break;
  case BinaryInst::G:
    if (tp == IR_FLOAT) std::cout << "fcmp ugt ";
    else                std::cout << "icmp sgt ";
    break;
  case BinaryInst::GE:
    if (tp == IR_FLOAT) std::cout << "fcmp uge ";
    else                std::cout << "icmp sge ";
    break;
  case BinaryInst::L:
    if (tp == IR_FLOAT) std::cout << "fcmp ult ";
    else                std::cout << "icmp slt ";
    break;
  case BinaryInst::LE:
    if (tp == IR_FLOAT) std::cout << "fcmp ule ";
    else                std::cout << "icmp sle ";
    break;
  default:
    break;
  }
  userList[0]->getValue()->getIRType()->to_string();
  std::cout << " ";
  userList[0]->getValue()->dumpIR();
  std::cout << ", ";
  //userList[1]->getValue()->getIRType()->to_string();
 // std::cout << " ";
  userList[1]->getValue()->dumpIR();
  std::cout << "\n";

}


CallInst::CallInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::CALL;}
CallInst::CallInst(Value *func, std::vector<Value*> &_args,
                   std::string appendix)
    : Instruction(func->getIRType()) {
  ident += appendix;
  addUse(func);
  for (auto &i : _args)
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
    i->getValue()->getIRType()->to_string();
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

Value *CallInst::getArg(unsigned Index) const {
  return getOperand(static_cast<size_t>(Index) + 1);
}

unsigned CallInst::getArgCount() const {
  size_t NumOperands = getNumOperands();
  return NumOperands == 0 ? 0 : static_cast<unsigned>(NumOperands - 1);
}

FPTOSIInst::FPTOSIInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::FPTOSI;}
FPTOSIInst::FPTOSIInst(Value* v):Instruction(INTType::NewInt(4)){
    addUse(v);
    instType = InstType::FPTOSI;
}
void FPTOSIInst::dumpIR(){
  Value::dumpIR();
  std::cout << " = fptosi ";
  for (auto &i : userList) {
    i->getValue()->getIRType()->to_string();
    std::cout << " ";
    i->getValue()->dumpIR();
    std::cout << " ";
  }
  std::cout << "to ";
  irtype->to_string();
  std::cout << "\n";
}

void SITOFPInst::dumpIR(){
  Value::dumpIR();
  std::cout << " = sitofp ";
  for (auto &i : userList) {
    i->getValue()->getIRType()->to_string();
    std::cout << " ";
    i->getValue()->dumpIR();
    std::cout << " ";
  }
  std::cout << "to ";
  irtype->to_string();
  std::cout << '\n';
}

SITOFPInst::SITOFPInst(Value* v):Instruction(FLOATType::NewFloat()){
    addUse(v);
    instType = InstType::SITOFP;
}

SITOFPInst::SITOFPInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::SITOFP;}


ZextInst::ZextInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::ZEXT;}
ZextInst::ZextInst(Value* v):Instruction(INTType::NewInt(4)){
    addUse(v);
    instType = InstType::ZEXT;
}
void ZextInst::dumpIR(){
    Value::dumpIR();
    std::cout << " = zext i1 ";
    userList[0]->getValue()->dumpIR();
    std::cout << " to i32\n";

}

TruncInst::TruncInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::TRUNC;}

TruncInst::TruncInst(Value* v):Instruction(INTType::NewBool()){
  addUse(v);
  instType = InstType::TRUNC;
}

void TruncInst::dumpIR(){
  Value::dumpIR();
  std::cout << " = trunc ";
  userList[0]->getValue()->getIRType()->to_string();
  std::cout << " ";
  userList[0]->getValue()->dumpIR();
  std::cout << " to ";
  irtype->to_string();
  std::cout << "\n";
}

SextInst::SextInst(std::shared_ptr<IRType> irtype):Instruction(irtype){instType = InstType::SEXT;}

SextInst::SextInst(Value* v):Instruction(INTType::NewInt(4)){
  addUse(v);
  instType = InstType::SEXT;
}

void SextInst::dumpIR(){
  Value::dumpIR();
  std::cout << " = sext ";
  userList[0]->getValue()->getIRType()->to_string();
  std::cout << " ";
  userList[0]->getValue()->dumpIR();
  std::cout << " to ";
  irtype->to_string();
  std::cout << "\n";
}

SelectInst::SelectInst(Value* cond,Value* isTrue,Value* isFalse)
  : Instruction(isTrue->getIRType()) {
  assert(isTrue->getIRType() == isFalse->getIRType());
  instType = InstType::SELECT;
  addUse(cond);
  addUse(isTrue);
  addUse(isFalse);
}

void SelectInst::dumpIR() {
  Value::dumpIR();
  std::cout << " = select ";

  userList[0]->getValue()->getIRType()->to_string();
  std::cout << " ";
  userList[0]->getValue()->dumpIR();
  std::cout << ", ";

  userList[1]->getValue()->getIRType()->to_string();
  std::cout << " ";
  userList[1]->getValue()->dumpIR();
  std::cout << ", ";

  userList[2]->getValue()->getIRType()->to_string();
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
  if (cond->getIRType()->getEnum() != IR_INT || cond->getIRType() != INTType::NewBool()) {
    switch (cond->getIRType()->getEnum()) {
    case IR_INT:
      cond = genBinaryInst(cond, BinaryInst::NE, ConstInt::newConstInt(0));
      break;
    case IR_FLOAT:
      cond = genBinaryInst(cond, BinaryInst::NE, ConstFloat::newConstFloat(0.0f));
      break;
    case IR_POINTER:
      cond = genBinaryInst(cond, BinaryInst::NE, ConstPtr::newConstPtr(cond->getIRType()));
      break;
    default:
      assert(0 && "select condition type is unsupported");
    }
  }

  if (isTrue->getIRType() != isFalse->getIRType()) {
    auto tTy = isTrue->getIRType()->getEnum();
    auto fTy = isFalse->getIRType()->getEnum();
    if (tTy == IR_INT && fTy == IR_FLOAT) {
      isTrue = genSITOFPInst(isTrue);
    } else if (tTy == IR_FLOAT && fTy == IR_INT) {
      isFalse = genSITOFPInst(isFalse);
    } else {
      assert(0 && "select arms type mismatch");
    }
  }

  auto *inst = new SelectInst(cond, isTrue, isFalse);
  push_back(inst);
  return inst->getOprand();
}


bool Variable::isGlobal(){
    if(vartag == Param) return false;
    else return true;
}

Variable::Variable(VarTag vt,std::shared_ptr<IRType> irtype,std::string ident):User(vt == Param ? irtype : POINTERType::NewPointer(irtype)),vartag(vt){
    //形参直接调用，直接存储值，所以不用创建指针类型
    //而全局变量和常量需要创建指针类型进行存储
    //在创建Variable的时候就需要完成加入Module vector和加入符号表之中
    if(vt == Param) return;
    else if(vt == GlobalVariable){
        this->ident =  ".G." + ident;
        ModuleIR->getSymbolTable().register_symbolTable(ident,this);
    }
    else{
        this->ident = ".C."+ this->ident;
    }
    ModuleIR->getGlobalVar().emplace_back(std::unique_ptr<Variable>(this));
}

bool Variable::isParam(){
    if(vartag == Param) return true;
    else return false;
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
    else if (vartag == Constant)
    std::cout << " = constant ";
    else /* if(usage==Param) */
    return;
    auto type = std::dynamic_pointer_cast<POINTERType>(getIRType());
    type->getPointerType()->to_string();
    std::cout<<" ";
    if(userList.size() != 0){
        Value* init = userList[0]->usee;
        if(auto arrayInit = dynamic_cast<Initializer*>(init)) arrayInit->dumpIR();
        else init->dumpIR();
    }else std::cout<<"zeroinitializer";
    std::cout<<'\n';
}

 Value* Variable::getUsee(){
    if(userList.empty()) return nullptr;
    return userList[0]->usee;
 }

Initializer::Initializer(std::shared_ptr<IRType> irtype):Value(irtype){}

void Initializer::Var2Store(BasicBlock *bb, const std::string &name, std::vector<int> &gep_data) {
    for (int i = 0; i < this->size(); i++) {
        auto &handle = (*this)[i];
        gep_data.push_back(i);

        if (auto inits = dynamic_cast<Initializer *>(handle)) {
            inits->Var2Store(bb, name, gep_data);
        } else {
            // 如果初始值不是常量（即使用了变量进行初始化，如 a[3][0]）
            if (!handle->isConst()) {
                // 1. 生成 GEP 指令获取基地址
                auto gep = dynamic_cast<GetElementPtrInst *>(
                    bb->genGepInst(ModuleIR->getSymbolTable().getValueByName(name)));

                // 2. 添加索引 0 (解引用数组指针)
                gep->addUse(ConstInt::newConstInt(0));

                // 3. 添加后续维度的索引
                for (auto &j : gep_data)
                    gep->addUse(ConstInt::newConstInt(j));

                // ==========================================
                // 【修复核心】：添加完索引后，必须更新GEP的类型！
                // 否则 gep 仍被视为指向整个数组的指针，导致 Store 时类型误判
                // ==========================================
                gep->updateType();

                // 4. 生成 Store 指令
                // 现在 gep 类型已更正为 i32*，genStoreInst 会发现类型匹配(INT==INT)，不会生成 sitofp
                bb->genStoreInst(handle, gep);

                // 恢复 handle 为默认值（避免重复使用）
                if (handle->getIRType()->getEnum() == IR_INT)
                    handle = ConstInt::newConstInt();
                else
                    handle = ConstFloat::newConstFloat();
            }
        }
        gep_data.pop_back();
    }
}

static bool isAllZeroInitializer(Value *value) {
    if (auto *init = dynamic_cast<Initializer *>(value)) {
        for (Value *element : *init) {
            if (!isAllZeroInitializer(element)) return false;
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
    if(isAllZeroInitializer(this)){
        std::cout << "zeroinitializer";
        return;
    }
    std::cout << " [";
    auto arrType = std::dynamic_pointer_cast<ARRAYType>(irtype);
    if (!arrType) {
        // 如果不是数组，直接打印自身的值（防御性编程）
        // 或者根据逻辑直接 return
        std::cout << "Invalid Initializer Type";
        return;
    }
    int limi = arrType->getElementCount();
    for (int i = 0; i < limi; i++) {
    std::dynamic_pointer_cast<ARRAYType>(irtype)-> getElementType()->to_string();
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

//常量系统的相关实现
ConstantSystem::ConstantSystem(std::shared_ptr<IRType> irtype):Value(irtype){}


ConstBool::ConstBool(bool val):ConstantSystem(INTType::NewBool()),val(val){
    if (val)
    ident = "true";
    else
    ident = "false";
}

bool ConstBool::getVal(){return val;}

ConstBool* ConstBool::newConstBool(bool val){
  static ConstBool true_const(true);
  static ConstBool false_const(false);
  if (val)
    return &true_const;
  else
    return &false_const;
}

ConstInt::ConstInt(int val):ConstantSystem(INTType::NewInt(4)),val(val){
    ident = std::to_string(val);
}
ConstInt::ConstInt(int val, size_t size):ConstantSystem(INTType::NewInt(size)),val(val){
    ident = std::to_string(val);
}

int ConstInt::getVal(){return val;}

ConstInt* ConstInt::newConstInt(int val, size_t size){
  static std::map<std::pair<int, size_t>, ConstInt*> int_const_map;
  auto key = std::make_pair(val, size);
  if (int_const_map.find(key) == int_const_map.end())
    int_const_map[key] = (size == 4) ? new ConstInt(val) : new ConstInt(val, size);
  return int_const_map[key];
}

ConstFloat::ConstFloat(float val):ConstantSystem(FLOATType::NewFloat()),val(val){
  double tmp = val;
  std::stringstream ss;
  ss << "0x" << std::hex << *(reinterpret_cast<long long *>(&tmp));
  ident = ss.str();
}
ConstFloat* ConstFloat::newConstFloat(float val){
     static std::map<float, ConstFloat*> float_const_map;
  if (float_const_map.find(val) == float_const_map.end())
    float_const_map[val] = new ConstFloat(val);
  return float_const_map[val];
}


float ConstFloat::getVal(){return val;}

 ConstPtr::ConstPtr(std::shared_ptr<IRType> irtype):ConstantSystem(irtype){}

ConstPtr* ConstPtr::newConstPtr(std::shared_ptr<IRType> irtype){
    static ConstPtr *const_ptr = new ConstPtr(irtype);
    return const_ptr;
}

UndefValue::UndefValue(std::shared_ptr<IRType> irtype):ConstantSystem(irtype){ident="undef";}

UndefValue *UndefValue::NewUndefValue(std::shared_ptr<IRType> it) {
  static std::map<std::shared_ptr<IRType>, UndefValue*> Undefs;
  UndefValue *&UV = Undefs[it];
  if (!UV)
    UV = new UndefValue(it);
  return UV;
}

Value* StoreInst::getOprand(){
    return nullptr;
}

// BitCastInst 实现
BitCastInst::BitCastInst(Value* v, std::shared_ptr<IRType> targetType)
    : Instruction(targetType) {
    instType = InstType::BITCAST; // 确保你在 InstType 枚举里添加了 BITCAST
    addUse(v);
}

void BitCastInst::dumpIR() {
    Value::dumpIR();
    std::cout << " = bitcast ";
    userList[0]->getValue()->getIRType()->to_string();
    std::cout << " ";
    userList[0]->getValue()->dumpIR();
    std::cout << " to ";
    irtype->to_string();
    std::cout << "\n";
}

// BasicBlock 中的 helper 函数
Value* BasicBlock::genBitCastInst(Value* v, std::shared_ptr<IRType> targetType) {
    auto inst = new BitCastInst(v, targetType);
    push_back(inst);
    return inst->getOprand();
}

PhiInst::PhiInst(std::shared_ptr<IRType> irtype) : Instruction(irtype) {
    instType = InstType::PHI;
}

void PhiInst::addIncoming(Value* val, BasicBlock* bb) {
    addUse(val);
    addUse(bb);
}

void PhiInst::setIncomingValue(unsigned i, Value* val) {
    // 移除旧的 Use，创建新的
    // 简单做法：直接修改 usee 指针
    Use* use = userList[i * 2].get();
    // 从旧 Value 的 UseList 中移除
    use->removeFromUseList(use->getUser());
    // 设置新的 usee
    use->usee = val;
    // 加入新 Value 的 UseList
    val->getUseList().addUseNode(use);
}

void PhiInst::dumpIR() {
    Value::dumpIR();
    std::cout << " = phi ";
    irtype->to_string();
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

  bool BasicBlock::simplifySinglePredPhis() {
    auto preds = getPredecessors();
    if (preds.size() != 1) return false;

    bool changed = false;
    auto phis = collectPhiNodes();
    for (PhiInst *phi : phis) {
      if (!phi) continue;
      if (phi->getNumIncomingValues() != 1) continue;
      phi->replaceAllUsesWith(phi->getIncomingValue(0));
      delete phi;
      changed = true;
    }
    return changed;
  }

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
    return new AllocaInst(this->getIRType());
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
    auto *NewGEP = new GetElementPtrInst(this->getIRType());
    for (auto &U : userList) {
        NewGEP->addUse(remapVal(U->getValue(), VMap));
    }
    NewGEP->updateType();
    return NewGEP;
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
    return new FPTOSIInst(remapVal(userList[0]->getValue(), VMap));
}

Instruction* SITOFPInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.size() != 1) return nullptr;
    return new SITOFPInst(remapVal(userList[0]->getValue(), VMap));
}

Instruction* ZextInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
    if (userList.size() != 1) return nullptr;
    return new ZextInst(remapVal(userList[0]->getValue(), VMap));
}

Instruction* TruncInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
  if (userList.size() != 1) return nullptr;
  auto *NewTrunc = new TruncInst(this->getIRType());
  NewTrunc->addUse(remapVal(userList[0]->getValue(), VMap));
  return NewTrunc;
}

Instruction* SextInst::clone(const std::unordered_map<Value*, Value*>& VMap) {
  if (userList.size() != 1) return nullptr;
  auto *NewSext = new SextInst(this->getIRType());
  NewSext->addUse(remapVal(userList[0]->getValue(), VMap));
  return NewSext;
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
  if (this->Size() == 0) return nullptr;
  return dynamic_cast<BrInst *>(this->back());
}

bool BasicBlock::isCondBr(BrInst *Br) {
  return Br && Br->getUserList().size() == 3;
}

bool BasicBlock::isUncondBr(BrInst *Br) {
  return Br && Br->getUserList().size() == 1;
}
//计算基本块从From到To的边数
int BasicBlock::countEdgesToSucc(BasicBlock *To) {
  BrInst *Br = getBranchTerminator();
  if (!Br || !To) return 0;

  int Cnt = 0;
  auto &UL = Br->getUserList();
  if (UL.size() == 1) {
    if (UL[0] && UL[0]->getValue() == To) Cnt = 1;
  } else if (UL.size() == 3) {
    if (UL[1] && UL[1]->getValue() == To) ++Cnt;
    if (UL[2] && UL[2]->getValue() == To) ++Cnt;
  }
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

bool PhiInst::replaceIncomingPred(PhiInst *Phi, BasicBlock *OldPred, BasicBlock *NewPred) {
  if (!Phi) return false;
  auto In = Phi->collectIncomingEdges();
  if (In.empty()) return false;

  bool Changed = false;
  for (auto &E : In) {
    if (E.Pred == OldPred) {
      E.Pred = NewPred;
      Changed = true;
    }
  }
  if (!Changed) return false;
  return rebuildFromIncoming(Phi, In);
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
  for (auto &UsePtr : Br->getUserList()) {
    Use *U = UsePtr.get();
    if (!U) continue;
    if (U->getValue() != OldSucc) continue;
    U->replaceUseeWith(NewSucc);
    Changed = true;
  }
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

bool BasicBlock::onlyPhiAndTerminator() {
  if (this->Size() == 0) return false;
  Instruction *Term = this->back();
  for (Instruction *I : *this) {
    if (I == Term) continue;
    if (!dynamic_cast<PhiInst *>(I)) return false;
  }
  return true;
}

bool BasicBlock::hasOnlyBlockLocalInstructionUsers(Instruction *I, BasicBlock *BB) {
  if (!I || !BB) return false;
  for (auto It = I->getUseList().begin(); It != I->getUseList().end(); ++It) {
    Use *U = *It;
    if (!U || U->getValue() != I) continue;
    auto *UserI = dynamic_cast<Instruction *>(U->getUser());
    if (!UserI) return false;
    if (UserI->getParent() != BB) return false;
  }
  return true;
}

void BasicBlock::eraseBlockSafely() {
  for (Instruction *I : *this) {
    if (!I) continue;
    if (!I->getIRType() || I->getIRType()->getEnum() == IR_VOID) continue;
    if (!I->getUseList().is_empty()) {
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
