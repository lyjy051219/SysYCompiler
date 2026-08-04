#include "lib/BaseManager.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

Use::Use(User *UserValue, Value *UsedValue)
    : user(UserValue), usee(UsedValue) {
    usee->getUseList().addUseNode(this);
}

Use::~Use() {
    removeFromUseList();
}

User *Use::getUser() const {
    return user;
}

Value *Use::getValue() const {
    return usee;
}

std::optional<std::size_t> Use::getOperandIndex() const {
    if (!user) return std::nullopt;
    int Index = user->getUserIndex(this);
    if (Index < 0) return std::nullopt;
    return static_cast<std::size_t>(Index);
}

bool Use::replaceUseeWith(Value *ValueToUse) {
    if (!ValueToUse || usee == ValueToUse) return false;
    // 先从旧值解链，再挂入新值
    removeFromUseList();
    usee = ValueToUse;
    ValueToUse->getUseList().addUseNode(this);
    return true;
}

void Use::removeFromUseList() {
    if (!usee || !prev) return;
    --usee->getUseList().size;
    *prev = next;
    if (next) next->prev = prev;
    usee = nullptr;
    next = nullptr;
    prev = nullptr;
}

void UseList::addUseNode(Use *NewNode) {
    if (!NewNode) return;
    ++size;
    NewNode->next = head;
    if (head) head->prev = &NewNode->next;
    NewNode->prev = &head;
    head = NewNode;
}

Use *&UseList::getHead() {
    return head;
}

Use *UseList::getHead() const {
    return head;
}

bool UseList::isEmpty() const {
    return head == nullptr;
}

UseList::iterator::iterator(Use *Ptr) : ptr(Ptr) {}

UseList::iterator &UseList::iterator::operator++() {
    ptr = ptr ? ptr->next : nullptr;
    return *this;
}

Use *UseList::iterator::operator*() const {
    return ptr;
}

bool UseList::iterator::operator==(const iterator &Other) const {
    return ptr == Other.ptr;
}

bool UseList::iterator::operator!=(const iterator &Other) const {
    return !(*this == Other);
}

UseList::iterator UseList::begin() const {
    return iterator(head);
}

UseList::iterator UseList::end() const {
    return iterator(nullptr);
}

int Value::generateNum(const std::string &Name) {
    static std::unordered_map<std::string, int> Counters;
    return Counters[Name]++;
}

Value::~Value() {
    while (!useList.isEmpty()) delete useList.getHead()->user;
}

Value::Value(std::shared_ptr<IRType> Type) : irtype(std::move(Type)) {
    ident = "." + std::to_string(generateNum("."));
}

Value::Value(std::shared_ptr<IRType> Type, std::string Ident)
    : irtype(std::move(Type)), ident(std::move(Ident)),
      num(generateNum(ident)) {}

bool Value::isConst() {
    return false;
}

bool Value::isGlobal() {
    return false;
}

bool Value::isParam() {
    return false;
}

std::shared_ptr<IRType> Value::getIRType() const {
    return irtype;
}

void Value::setIdent(std::string Ident) {
    ident = std::move(Ident);
}

std::string Value::getIdent() const {
    return ident;
}

UseList &Value::getUseList() {
    return useList;
}

const UseList &Value::getUseList() const {
    return useList;
}

void Value::replaceAllUsesWith(Value *ValueToUse) {
    if (!ValueToUse || ValueToUse == this) return;
    useList.size = 0;
    Use *Head = useList.getHead();
    useList.getHead() = nullptr;
    while (Head) {
        Head->usee = ValueToUse;
        Use *Next = Head->next;
        Head->next = nullptr;
        Head->prev = nullptr;
        ValueToUse->getUseList().addUseNode(Head);
        Head = Next;
    }
}

User::~User() = default;

User::User() : Value(VOIDType::NewVoid()) {}

User::User(std::shared_ptr<IRType> Type) : Value(std::move(Type)) {}

const std::vector<std::unique_ptr<Use>> &User::getUserList() const {
    return userList;
}

int User::getUserIndex(const Use *OperandUse) const {
    for (std::size_t Index = 0; Index < userList.size(); ++Index)
        if (userList[Index].get() == OperandUse)
            return static_cast<int>(Index);
    return -1;
}

void User::addUse(Value *Operand) {
    userList.push_back(std::make_unique<Use>(this, Operand));
}

bool Value::isConstZero() const {
    if (auto numVal = dynamic_cast<const ConstInt*>(this))
        return numVal->getVal() == 0;
    else if (auto numVal = dynamic_cast<const ConstFloat*>(this))
        return numVal->getVal() == 0.0f;
    else if (auto numVal = dynamic_cast<const ConstBool*>(this))
        return numVal->getVal() == false;
    else
        return false;
}

bool Value::isConstIntZero() const {
    auto *CI = dynamic_cast<const ConstInt *>(this);
    return CI && CI->getVal() == 0;
}

bool Value::isConstOne() const {
    if (auto *CI = dynamic_cast<const ConstInt *>(this)) return CI->getVal() == 1;
    if (auto *CF = dynamic_cast<const ConstFloat *>(this)) return CF->getVal() == 1.0f;
    if (auto *CB = dynamic_cast<const ConstBool *>(this)) return CB->getVal();
    return false;
}

bool Value::isConstMinusOne() const {
    if (auto *CI = dynamic_cast<const ConstInt *>(this)) return CI->getVal() == -1;
    if (auto *CF = dynamic_cast<const ConstFloat *>(this)) return CF->getVal() == -1.0f;
    return false;
}

bool Value::isUndef() const {
    return dynamic_cast<const UndefValue *>(this) != nullptr;
}

bool Value::tryGetBoolConst(bool &Out) const {
    if (auto *CB = dynamic_cast<const ConstBool *>(this)) {
        Out = CB->getVal();
        return true;
    }
    if (auto *CI = dynamic_cast<const ConstInt *>(this)) {
        Out = CI->getVal() != 0;
        return true;
    }
    if (auto *CF = dynamic_cast<const ConstFloat *>(this)) {
        Out = CF->getVal() != 0.0f;
        return true;
    }
    return false;
}

bool Value::tryGetIntConst(int32_t &Out) const {
    if (auto *CI = dynamic_cast<const ConstInt *>(this)) {
        const int64_t Value = CI->getVal();
        if (Value < std::numeric_limits<int32_t>::min() ||
            Value > std::numeric_limits<int32_t>::max())
            return false;
        Out = static_cast<int32_t>(Value);
        return true;
    }
    if (auto *CB = dynamic_cast<const ConstBool *>(this)) {
        Out = CB->getVal() ? 1 : 0;
        return true;
    }
    return false;
}

bool Value::tryGetInt64Const(int64_t &Out) const {
    if (auto *CI = dynamic_cast<const ConstInt *>(this)) {
        Out = CI->getVal();
        return true;
    }
    if (auto *CB = dynamic_cast<const ConstBool *>(this)) {
        Out = CB->getVal() ? 1 : 0;
        return true;
    }
    return false;
}

bool Value::tryGetInt64ConstAtWidth(unsigned Bits, int64_t &Out) const {
    int64_t Raw = 0;
    if (!tryGetInt64Const(Raw)) return false;
    Out = IRMath::normalizeSignedToWidth(Raw, Bits);
    return true;
}

bool Value::tryGetFloatConst(float &Out) const {
    if (auto *CF = dynamic_cast<const ConstFloat *>(this)) {
        Out = CF->getVal();
        return true;
    }
    return false;
}

bool Value::tryGetNumericFloatConst(float &Out) const {
    if (tryGetFloatConst(Out)) return true;
    int64_t IntValue = 0;
    if (!tryGetInt64Const(IntValue)) return false;
    Out = static_cast<float>(IntValue);
    return true;
}

Value *Value::stripBooleanExtension(Value *V,
                                    BooleanExtensionKind &Kind) {
    Kind = BooleanExtensionKind::None;
    if (!V) return nullptr;
    if (IRType::isBool(V->getIRType())) {
        Kind = BooleanExtensionKind::PlainBool;
        return V;
    }

    if (auto *ZI = dynamic_cast<ZextInst *>(V)) {
        Value *Operand = ZI->getUnaryOperand();
        if (Operand && IRType::isBool(Operand->getIRType())) {
            Kind = BooleanExtensionKind::ZeroExt;
            return Operand;
        }
    }
    if (auto *SI = dynamic_cast<SextInst *>(V)) {
        Value *Operand = SI->getUnaryOperand();
        if (Operand && IRType::isBool(Operand->getIRType())) {
            Kind = BooleanExtensionKind::SignExt;
            return Operand;
        }
    }
    return nullptr;
}

std::optional<int64_t> Value::getIntConstant() const {
    int64_t Result = 0;
    if (!tryGetInt64Const(Result)) return std::nullopt;
    return Result;
}

int Value::integerBitWidth(int DefaultBits) const {
    if (!getIRType()) return DefaultBits;
    return getIRType()->getIntegerBitWidth(DefaultBits);
}

int Value::countUses() const {
    return useList.size;
}

int Value::countInstructionUses(Instruction **SingleUser) const {
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

std::vector<Use *> Value::getUses() const {
    std::vector<Use *> Uses;
    Uses.reserve(static_cast<std::size_t>(useList.size));
    for (auto It = getUseList().begin(); It != getUseList().end(); ++It) {
        Use *U = *It;
        if (U && U->getValue() == this) Uses.push_back(U);
    }
    return Uses;
}

Use *Value::getSingleUse() const {
    Use *Single = nullptr;
    for (auto It = getUseList().begin(); It != getUseList().end(); ++It) {
        Use *U = *It;
        if (!U || U->getValue() != this) continue;
        if (Single) return nullptr;
        Single = U;
    }
    return Single;
}

std::vector<Instruction *> Value::getInstructionUsers() const {
    std::vector<Instruction *> Users;
    for (auto It = getUseList().begin(); It != getUseList().end(); ++It) {
        Use *U = *It;
        if (!U || U->getValue() != this) continue;
        if (auto *InstUser = dynamic_cast<Instruction *>(U->getUser()))
            Users.push_back(InstUser);
    }
    return Users;
}

bool Value::hasUses() const {
    return !useList.isEmpty();
}

bool Value::hasExactlyOneUse() const {
    return getSingleUse() != nullptr;
}

bool Value::isPointerValue() const {
    return getIRType() && getIRType()->isPointer();
}

std::shared_ptr<IRType> Value::getPointeeType() const {
    return getIRType() ? getIRType()->getPointeeType() : nullptr;
}

std::shared_ptr<IRType> Value::getPointeeType(const Value *V) {
    return V ? V->getPointeeType() : nullptr;
}

bool Value::isExternalVariable() const {
    auto *Var = dynamic_cast<const Variable *>(this);
    return (Var && Var->vartag == Variable::GlobalVariable) ||
           dynamic_cast<const Param *>(this);
}

bool Value::isConstZero(const Value *V) {
    return V && V->isConstZero();
}

bool Value::isConstOne(const Value *V) {
    return V && V->isConstOne();
}

bool Value::isConstMinusOne(const Value *V) {
    return V && V->isConstMinusOne();
}

bool Value::isUndef(const Value *V) {
    return V && V->isUndef();
}

bool Value::tryGetBoolConst(const Value *V, bool &Out) {
    return V && V->tryGetBoolConst(Out);
}

bool Value::tryGetIntConst(const Value *V, int32_t &Out) {
    return V && V->tryGetIntConst(Out);
}

bool Value::tryGetFloatConst(const Value *V, float &Out) {
    return V && V->tryGetFloatConst(Out);
}

bool Value::areEqualConstants(const Value *A, const Value *B) {
    if (A == B) return true;
    if (!A || !B || !A->getIRType() || !B->getIRType() ||
        !A->getIRType()->hasSameShape(*B->getIRType()))
        return false;
    if (auto *AI = dynamic_cast<const ConstInt *>(A)) {
        auto *BI = dynamic_cast<const ConstInt *>(B);
        return BI && AI->getVal() == BI->getVal();
    }
    if (auto *AF = dynamic_cast<const ConstFloat *>(A)) {
        auto *BF = dynamic_cast<const ConstFloat *>(B);
        if (!BF) return false;
        std::uint32_t ABits = 0;
        std::uint32_t BBits = 0;
        float AV = AF->getVal();
        float BV = BF->getVal();
        std::memcpy(&ABits, &AV, sizeof(ABits));
        std::memcpy(&BBits, &BV, sizeof(BBits));
        return ABits == BBits;
    }
    if (auto *AB = dynamic_cast<const ConstBool *>(A)) {
        auto *BB = dynamic_cast<const ConstBool *>(B);
        return BB && AB->getVal() == BB->getVal();
    }
    return false;
}

bool Value::tryGetIntIndex(const Value *V, int &Out) {
    int32_t Raw = 0;
    if (!tryGetIntConst(V, Raw)) return false;
    Out = static_cast<int>(Raw);
    return true;
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

Value *Value::makeZeroForType(const std::shared_ptr<IRType> &Ty) {
    if (!Ty) return ConstInt::newConstInt(0);
    if (Ty->isFloat()) return ConstFloat::newConstFloat(0.0f);
    if (Ty->isBool()) return ConstBool::newConstBool(false);
    if (Ty->isInt())
        return ConstInt::newConstIntForType(
            0, std::dynamic_pointer_cast<INTType>(Ty));
    return ConstInt::newConstInt(0);
}

Value *Value::makeOneForType(const std::shared_ptr<IRType> &Ty) {
    if (!Ty) return ConstInt::newConstInt(1);
    if (Ty->isFloat()) return ConstFloat::newConstFloat(1.0f);
    if (Ty->isBool()) return ConstBool::newConstBool(true);
    if (Ty->isInt())
        return ConstInt::newConstIntForType(
            1, std::dynamic_pointer_cast<INTType>(Ty));
    return ConstInt::newConstInt(1);
}

Value *Value::makeMinusOneForType(const std::shared_ptr<IRType> &Ty) {
    if (!Ty) return ConstInt::newConstInt(-1);
    if (Ty->isFloat()) return ConstFloat::newConstFloat(-1.0f);
    if (Ty->isBool()) return ConstBool::newConstBool(true);
    if (Ty->isInt())
        return ConstInt::newConstIntForType(
            -1, std::dynamic_pointer_cast<INTType>(Ty));
    return ConstInt::newConstInt(-1);
}

Value *Value::makeUndefForType(const std::shared_ptr<IRType> &Ty) {
    return UndefValue::NewUndefValue(Ty ? Ty : INTType::getInt32Ty());
}

Value *Value::makeIntegerForType(int64_t Val,
                                 const std::shared_ptr<IRType> &Ty) {
    if (!Ty || !Ty->isInt()) return nullptr;
    if (Ty->isBool()) {
        if (Val != 0 && Val != 1) return nullptr;
        return ConstBool::newConstBool(Val != 0);
    }
    return ConstInt::newConstIntForType(
        Val, std::dynamic_pointer_cast<INTType>(Ty));
}

Value *Value::makeIntegerLike(const Value *Model, int64_t Val) {
    return Model ? makeIntegerForType(Val, Model->getIRType()) : nullptr;
}

size_t User::getNumOperands() const {
    return userList.size();
}

Value *User::getOperand(size_t Index) const {
    if (Index >= userList.size() || !userList[Index]) return nullptr;
    return userList[Index]->getValue();
}

Use *User::getOperandUse(size_t Index) const {
    if (Index >= userList.size() || !userList[Index]) return nullptr;
    return userList[Index].get();
}

bool User::replaceOperand(size_t Index, Value *V) {
    if (Index >= userList.size() || !userList[Index] || !V) return false;
    return userList[Index]->replaceUseeWith(V);
}

bool User::eraseOperand(size_t Index) {
    if (Index >= userList.size()) return false;
    userList.erase(userList.begin() + static_cast<std::ptrdiff_t>(Index));
    return true;
}

void User::clearOperands() {
    userList.clear();
}

void Value::dumpIR() {
    if (isConst()) std::cout << getIdent();
    else if (isGlobal()) std::cout << "@" << getIdent();
    else if (auto tmp = dynamic_cast<Function*>(this)) std::cout << "@" << tmp->getIdent();
    else if (auto tmp = dynamic_cast<BuildInFunction*>(this)) std::cout << "@" << tmp->getIdent();
    else if (getIdent() == "undef") std::cout << getIdent();
    else std::cout << "%" << getIdent();
}
