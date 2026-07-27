#include "ir/opt/InstSimplify.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"

#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <vector>

bool InstSimplifyPass::getBinaryOperandsForOp(
	Value *V, BinaryInst::Operation Op, Value *&L, Value *&R) {
	auto *BI = dynamic_cast<BinaryInst *>(V);
	if (!BI || BI->getOp() != Op) return false;
	return BI->getOperands(L, R);
}

bool InstSimplifyPass::matchNegation(Value *V, Value *&Y) {
	// `0 - x` is not an exact floating-point negation: for example,
	// `+0.0 - +0.0` is +0.0 rather than -0.0.  This helper is used by
	// algebraic identities, so keep it in the modular-integer domain.
	if (!V || !IRType::isInt(V->getIRType())) return false;

	Value *L = nullptr;
	Value *R = nullptr;
	if (!InstSimplifyPass::getBinaryOperandsForOp(V, BinaryInst::SUB, L, R)) return false;
	if (!Value::isConstZero(L)) return false;
	Y = R;
	return Y != nullptr;
}

bool InstSimplifyPass::matchSub(Value *V, Value *&LHS, Value *&RHS) {
	auto *BI = dynamic_cast<BinaryInst *>(V);
	if (!BI || BI->getOp() != BinaryInst::SUB) return false;
	return BI->getOperands(LHS, RHS);
}

bool InstSimplifyPass::isNegationPair(Value *A, Value *B) {
	if (!A || !B) return false;
	if (!IRType::isInt(A->getIRType()) || !IRType::isInt(B->getIRType())) return false;

	int32_t AI = 0, BI = 0;
	if (Value::tryGetIntConst(A, AI) && Value::tryGetIntConst(B, BI)) {
		// Widen before negating so INT_MIN does not trigger signed host UB.
		return static_cast<int64_t>(AI) == -static_cast<int64_t>(BI);
	}

	Value *SL = nullptr, *SR = nullptr;
	if (InstSimplifyPass::matchSub(B, SL, SR) && Value::isConstZero(SL) && SR == A) return true;
	if (InstSimplifyPass::matchSub(A, SL, SR) && Value::isConstZero(SL) && SR == B) return true;

	return false;
}

bool InstSimplifyPass::matchYMinusX(Value *MaybeSub, Value *X, Value *&Y) {
	Value *SubL = nullptr;
	Value *SubR = nullptr;
	if (!InstSimplifyPass::matchSub(MaybeSub, SubL, SubR)) return false;
	if (SubR != X) return false;
	Y = SubL;
	return Y != nullptr;
}

InstSimplifier::InstSimplifier(
	DominatorTree *DT, Function *F, FunctionAnalysisPassManager *FAM,
	BasicAAResult *AA, LoopInfo *LI)
	: DT(DT), F(F), FAM(FAM), AA(AA), LI(LI) {}

bool InstSimplifier::run(Function &F) {
	bool Changed = false;

	UniqueWorkList<Instruction *> Q;
	std::unordered_set<Instruction *> Erased;

	auto enqueue = [&](Instruction *I) {
		if (!I) return;
		if (Erased.count(I)) return;
		if (!I->getParent()) return;
		Q.push(I);
	};

	for (Instruction *I : collectInstructions(F))
		if (I->hasUses()) enqueue(I);

	while (!Q.empty()) {
		Instruction *I = Q.pop();

		if (!I || Erased.count(I) || !I->getParent()) continue;

		Value *V = simplifyInstruction(I, Changed);
		if (!V || V == I) continue;

		std::vector<Instruction *> Users = I->getInstructionUsers();

		if (auto *NewI = dynamic_cast<Instruction *>(V)) {
			if (NewI->getParent() == nullptr) {
				I->replaceWith(NewI);
			}
		}

		I->replaceAllUsesWith(V);
		delete I;
		Erased.insert(I);
		Changed = true;

		for (Instruction *U : Users) enqueue(U);
		if (auto *NewI = dynamic_cast<Instruction *>(V)) enqueue(NewI);
	}

	return Changed;
}

bool InstSimplifier::isLoadSuitableForSinking(LoadInst *L) const {
	if (!L || !FAM || !AA || !LI) return false;

	Value *Ptr = L->getPointerOperand();
	if (!Ptr) return false;
	BasicBlock *BB = L->getParent();
	if (!BB) return false;

	bool SeenLoad = false;
	for (auto *I : *BB) {
		if (!SeenLoad) {
			SeenLoad = (I == L);
			continue;
		}
		ModRefInfo MRI = getInstModRefInfo(
			MemoryLocation::get(L), I, *AA, *LI, *FAM);
		if (isModSet(MRI)) return false;
	}

	if (auto *Alloca = dynamic_cast<AllocaInst *>(Ptr)) {
		bool AddressTaken = false;
		for (Use *U : Alloca->getUses()) {
			auto *UserI = dynamic_cast<Instruction *>(U->getUser());
			if (!UserI) {
				AddressTaken = true;
				break;
			}
			if (dynamic_cast<LoadInst *>(UserI)) continue;
			if (auto *S = dynamic_cast<StoreInst *>(UserI)) {
				if (S->getPointerOperand() == Alloca) continue;
			}
			AddressTaken = true;
			break;
		}
		if (!AddressTaken) return false;
	}

	if (auto *GEP = dynamic_cast<GetElementPtrInst *>(Ptr)) {
		if (GEP->hasOnlyConstantIndices()) return false;
	}

	return true;
}

Value *InstSimplifier::foldPhiOfBinary(PhiInst *Phi) {
	if (!Phi) return nullptr;
	const unsigned N = Phi->getNumIncomingValues();
	if (N < 2) return nullptr;

	auto *First = dynamic_cast<BinaryInst *>(Phi->getIncomingValue(0));
	if (!First) return nullptr;

	Value *FL = nullptr;
	Value *FR = nullptr;
	if (!BinaryInst::getOperands(First, FL, FR)) return nullptr;
	BinaryInst::Operation Op = First->getOp();

	Value *CommonL = FL;
	Value *CommonR = FR;
	for (unsigned i = 0; i < N; ++i) {
		auto *Bin = dynamic_cast<BinaryInst *>(Phi->getIncomingValue(i));
		if (!Bin || Bin->getOp() != Op) return nullptr;
		Value *L = nullptr;
		Value *R = nullptr;
		if (!BinaryInst::getOperands(Bin, L, R)) return nullptr;
		if (CommonL && CommonL != L) CommonL = nullptr;
		if (CommonR && CommonR != R) CommonR = nullptr;
		if (!CommonL && !CommonR) return nullptr;
	}

	// Every incoming computes the exact same binary expression in its own
	// predecessor.  None of those predecessor-local instructions necessarily
	// dominates the merge, so reusing `First` would be invalid SSA.  Rebuild the
	// expression after the PHIs instead; its common operands must be available
	// on every path into the block.
	if (CommonL && CommonR) {
		if (!DT || !DT->dominatesValue(CommonL, Phi) ||
			!DT->dominatesValue(CommonR, Phi)) {
			return nullptr;
		}

		auto *NewBin = new BinaryInst(CommonL, Op, CommonR);
		if (!BasicBlock::insertAfterPhiNodes(Phi->getParent(), NewBin)) {
			delete NewBin;
			return nullptr;
		}
		return NewBin;
	}

	Value *SampleUncommon = nullptr;
	if (CommonL) {
		Value *TmpL = nullptr;
		Value *TmpR = nullptr;
		if (!BinaryInst::getOperands(First, TmpL, TmpR)) return nullptr;
		SampleUncommon = TmpR;
	} else {
		Value *TmpL = nullptr;
		Value *TmpR = nullptr;
		if (!BinaryInst::getOperands(First, TmpL, TmpR)) return nullptr;
		SampleUncommon = TmpL;
	}
	if (!SampleUncommon) return nullptr;

	auto *UncommonPhi = new PhiInst(SampleUncommon->getIRType());
	for (unsigned i = 0; i < N; ++i) {
		auto *Bin = dynamic_cast<BinaryInst *>(Phi->getIncomingValue(i));
		Value *L = nullptr;
		Value *R = nullptr;
		if (!Bin || !BinaryInst::getOperands(Bin, L, R)) {
			delete UncommonPhi;
			return nullptr;
		}
		Value *U = CommonL ? R : L;
		UncommonPhi->addIncoming(U, Phi->getIncomingBlock(i));
	}

	if (!Instruction::insertBefore(Phi, UncommonPhi)) {
		delete UncommonPhi;
		return nullptr;
	}

	auto *NewBin = CommonL ? new BinaryInst(CommonL, Op, UncommonPhi)
						  : new BinaryInst(UncommonPhi, Op, CommonR);
	if (!BasicBlock::insertAfterPhiNodes(Phi->getParent(), NewBin)) {
		delete NewBin;
		return nullptr;
	}

	return NewBin;
}

Value *InstSimplifier::foldPhiOfGEP(PhiInst *Phi) {
	if (!Phi) return nullptr;
	const unsigned N = Phi->getNumIncomingValues();
	if (N < 2) return nullptr;

	auto *First = dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(0));
	if (!First || First->hasOnlyConstantIndices()) return nullptr;

	Value *FirstPtr = nullptr;
	std::vector<Value *> FirstIdxs;
	if (!GetElementPtrInst::getBaseAndIndices(First, FirstPtr, FirstIdxs)) return nullptr;
	if (dynamic_cast<AllocaInst *>(FirstPtr)) return nullptr;

	std::vector<Value *> Commons = FirstIdxs;
	Commons.push_back(FirstPtr);

	size_t UncommonCnt = 0;
	for (unsigned i = 0; i < N; ++i) {
		auto *GEP = dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(i));
		if (!GEP || GEP->hasOnlyConstantIndices()) return nullptr;

		Value *Ptr = nullptr;
		std::vector<Value *> Idxs;
		if (!GetElementPtrInst::getBaseAndIndices(GEP, Ptr, Idxs)) return nullptr;
		if (Idxs.size() != Commons.size() - 1) return nullptr;
		if (dynamic_cast<AllocaInst *>(Ptr)) return nullptr;

		if (Commons.back() && Commons.back() != Ptr) {
			Commons.back() = nullptr;
			++UncommonCnt;
		}

		for (size_t k = 0; k < Idxs.size(); ++k) {
			if (!Commons[k] || Commons[k] == Idxs[k]) continue;
			if (Commons[k]->isConst() || Idxs[k]->isConst()) return nullptr;
			Commons[k] = nullptr;
			++UncommonCnt;
		}

		if (UncommonCnt > 1) return nullptr;
	}

	if (UncommonCnt == 0) return nullptr;

	size_t UncommonIndex = 0;
	while (UncommonIndex < Commons.size() && Commons[UncommonIndex] != nullptr) ++UncommonIndex;
	if (UncommonIndex >= Commons.size()) return nullptr;

	Value *Sample = nullptr;
	if (UncommonIndex == Commons.size() - 1) {
		Value *Ptr = nullptr;
		std::vector<Value *> Idxs;
		if (!GetElementPtrInst::getBaseAndIndices(dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(0)), Ptr, Idxs)) return nullptr;
		Sample = Ptr;
	} else {
		Value *Ptr = nullptr;
		std::vector<Value *> Idxs;
		if (!GetElementPtrInst::getBaseAndIndices(dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(0)), Ptr, Idxs)) return nullptr;
		Sample = Idxs[UncommonIndex];
	}
	if (!Sample) return nullptr;

	auto *UncommonPhi = new PhiInst(Sample->getIRType());
	for (unsigned i = 0; i < N; ++i) {
		auto *GEP = dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(i));
		Value *Ptr = nullptr;
		std::vector<Value *> Idxs;
		if (!GEP || !GetElementPtrInst::getBaseAndIndices(GEP, Ptr, Idxs)) {
			delete UncommonPhi;
			return nullptr;
		}
		if (UncommonIndex == Commons.size() - 1) {
			UncommonPhi->addIncoming(Ptr, Phi->getIncomingBlock(i));
		} else {
			UncommonPhi->addIncoming(Idxs[UncommonIndex], Phi->getIncomingBlock(i));
		}
	}

	if (!Instruction::insertBefore(Phi, UncommonPhi)) {
		delete UncommonPhi;
		return nullptr;
	}

	Instruction *NewGEP = nullptr;
	if (UncommonIndex == Commons.size() - 1) {
		std::vector<Value *> Idxs = Commons;
		Idxs.pop_back();
		NewGEP = new GetElementPtrInst(UncommonPhi, Idxs);
		dynamic_cast<GetElementPtrInst *>(NewGEP)->updateType();
	} else {
		Value *Ptr = Commons.back();
		std::vector<Value *> Idxs = Commons;
		Idxs.pop_back();
		Idxs[UncommonIndex] = UncommonPhi;
		NewGEP = new GetElementPtrInst(Ptr, Idxs);
		dynamic_cast<GetElementPtrInst *>(NewGEP)->updateType();
	}

	if (!BasicBlock::insertAfterPhiNodes(Phi->getParent(), NewGEP)) {
		delete NewGEP;
		return nullptr;
	}
	return NewGEP;
}

Value *InstSimplifier::foldPhiOfLoad(PhiInst *Phi) {
	if (!Phi) return nullptr;
	const unsigned N = Phi->getNumIncomingValues();
	if (N < 2) return nullptr;

	auto *First = dynamic_cast<LoadInst *>(Phi->getIncomingValue(0));
	if (!First) return nullptr;
	Value *FirstPtr = First->getPointerOperand();
	if (!FirstPtr) return nullptr;

	for (unsigned i = 0; i < N; ++i) {
		auto *L = dynamic_cast<LoadInst *>(Phi->getIncomingValue(i));
		if (!L) return nullptr;
		if (!L->hasExactlyOneUse()) return nullptr;
		if (L->getParent() != Phi->getIncomingBlock(i)) return nullptr;
		if (!isLoadSuitableForSinking(L)) return nullptr;
	}

	auto *MergedPtr = new PhiInst(FirstPtr->getIRType());
	for (unsigned i = 0; i < N; ++i) {
		auto *L = dynamic_cast<LoadInst *>(Phi->getIncomingValue(i));
		Value *Ptr = L ? L->getPointerOperand() : nullptr;
		if (!L || !Ptr) {
			delete MergedPtr;
			return nullptr;
		}
		MergedPtr->addIncoming(Ptr, Phi->getIncomingBlock(i));
	}

	if (!Instruction::insertBefore(Phi, MergedPtr)) {
		delete MergedPtr;
		return nullptr;
	}

	auto *MergedLoad = new LoadInst(MergedPtr);
	if (!BasicBlock::insertAfterPhiNodes(Phi->getParent(), MergedLoad)) {
		delete MergedLoad;
		return nullptr;
	}
	return MergedLoad;
}

Value *InstSimplifier::simplifyInstruction(Instruction *I, bool &Changed) {
	if (!I) return nullptr;

	if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
		return simplifyBinary(BI, Changed);
	}
	if (auto *SI = dynamic_cast<SelectInst *>(I)) {
		return simplifySelect(SI);
	}
	if (auto *PI = dynamic_cast<PhiInst *>(I)) {
		return simplifyPhi(PI);
	}
	return nullptr;
}

bool InstSimplifier::maybeSwapCommutative(
	BinaryInst *BI, Value *&LHS, Value *&RHS, bool &Changed) {
	if (!BI || !LHS || !RHS) return false;
	if (!LHS->isConst() || RHS->isConst()) return false;

	switch (BI->getOp()) {
	case BinaryInst::ADD:
	case BinaryInst::MUL:
	case BinaryInst::AND:
	case BinaryInst::OR:
	case BinaryInst::XOR: {
		BI->setOperands(RHS, LHS);
		std::swap(LHS, RHS);
		Changed = true;
		return true;
	}
	default:
		return false;
	}
}

Value *InstSimplifier::simplifyAdd(
	BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed) {
	const bool IsIntArith = IRType::isInt(BI->getIRType()) && !IRType::isBool(BI->getIRType());

	if ((LHS->isConst() && RHS->isConst())) {
		return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
	}

	maybeSwapCommutative(BI, LHS, RHS, Changed);

	if (Value::isUndef(LHS) || Value::isUndef(RHS)) {
		return Value::makeUndefForType(BI->getIRType());
	}

	// `fadd x, +0.0` is not an IEEE identity for x == -0.0.  Adding -0.0
	// is an exact identity, however, so retain that strictly-safe case.
	float FloatZero = 0.0f;
	const bool IsNegativeFloatZero = IRType::isFloat(BI->getIRType()) &&
		Value::tryGetFloatConst(RHS, FloatZero) && FloatZero == 0.0f &&
		std::signbit(FloatZero);
	if ((IsIntArith || IsNegativeFloatZero) && Value::isConstZero(RHS)) {
		return LHS;
	}

	if (IsIntArith && InstSimplifyPass::isNegationPair(LHS, RHS)) {
		return Value::makeZeroForType(BI->getIRType());
	}

	// x + c1 + c2 -> x + (c1 + c2)
	if (IsIntArith && RHS->isConst()) {
		Value *AL = nullptr;
		Value *AR = nullptr;
		if (InstSimplifyPass::getBinaryOperandsForOp(LHS, BinaryInst::ADD, AL, AR) && AR && AR->isConst()) {
			Value *SumC = BinaryInst::foldConstants(BinaryInst::ADD, AR, RHS);
			if (SumC) {
				return new BinaryInst(AL, BinaryInst::ADD, SumC);
			}
		}
	}

	Value *Y = nullptr;
	if (IsIntArith && InstSimplifyPass::matchYMinusX(RHS, LHS, Y)) return Y;
	if (IsIntArith && InstSimplifyPass::matchYMinusX(LHS, RHS, Y)) return Y;

	return nullptr;
}

Value *InstSimplifier::simplifySub(
	BinaryInst *BI, Value *LHS, Value *RHS) {
	const bool IsIntArith = IRType::isInt(BI->getIRType()) && !IRType::isBool(BI->getIRType());

	if (LHS->isConst() && RHS->isConst()) {
		return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
	}

	if (Value::isUndef(LHS) || Value::isUndef(RHS)) {
		return Value::makeUndefForType(BI->getIRType());
	}

	// `fsub x, +0.0` is an exact IEEE identity; `fsub x, -0.0` is not
	// (for example, -0.0 - -0.0 is +0.0).  Preserve only the safe sign.
	float FloatZero = 0.0f;
	const bool IsPositiveFloatZero = IRType::isFloat(BI->getIRType()) &&
		Value::tryGetFloatConst(RHS, FloatZero) && FloatZero == 0.0f &&
		!std::signbit(FloatZero);
	if ((IsIntArith || IsPositiveFloatZero) && Value::isConstZero(RHS)) {
		return LHS;
	}

	// `fsub x, x` is NaN for NaN/Inf and therefore is not always +0.0.
	if (IsIntArith && LHS == RHS) {
		return Value::makeZeroForType(BI->getIRType());
	}

	// x - (x + y) -> -y
	if (IsIntArith) {
		Value *AL = nullptr;
		Value *AR = nullptr;
		if (InstSimplifyPass::getBinaryOperandsForOp(RHS, BinaryInst::ADD, AL, AR)) {
			if (AL == LHS) return new BinaryInst(Value::makeZeroForType(BI->getIRType()), BinaryInst::SUB, AR);
			if (AR == LHS) return new BinaryInst(Value::makeZeroForType(BI->getIRType()), BinaryInst::SUB, AL);
		}
	}

	// x - (-y) -> x + y
	if (IsIntArith) {
		Value *Y = nullptr;
		if (InstSimplifyPass::matchNegation(RHS, Y)) {
			return new BinaryInst(LHS, BinaryInst::ADD, Y);
		}
	}

	// x*y - x*z -> x*(y-z)
	if (IsIntArith) {
		Value *L0 = nullptr;
		Value *L1 = nullptr;
		Value *R0 = nullptr;
		Value *R1 = nullptr;
		if (InstSimplifyPass::getBinaryOperandsForOp(LHS, BinaryInst::MUL, L0, L1) &&
			InstSimplifyPass::getBinaryOperandsForOp(RHS, BinaryInst::MUL, R0, R1)) {
			Value *Common = nullptr;
			Value *Y = nullptr;
			Value *Z = nullptr;
			if (L0 == R0) { Common = L0; Y = L1; Z = R1; }
			else if (L0 == R1) { Common = L0; Y = L1; Z = R0; }
			else if (L1 == R0) { Common = L1; Y = L0; Z = R1; }
			else if (L1 == R1) { Common = L1; Y = L0; Z = R0; }

			if (Common && Y && Z) {
				auto *YZ = new BinaryInst(Y, BinaryInst::SUB, Z);
				if (Instruction::insertBefore(BI, YZ)) {
					return new BinaryInst(Common, BinaryInst::MUL, YZ);
				}
				delete YZ;
			}
		}
	}

	// (x*x) - (y*y) -> (x+y) * (x-y)
	if (IsIntArith) {
		Value *L0 = nullptr;
		Value *L1 = nullptr;
		Value *R0 = nullptr;
		Value *R1 = nullptr;
		if (InstSimplifyPass::getBinaryOperandsForOp(LHS, BinaryInst::MUL, L0, L1) &&
			InstSimplifyPass::getBinaryOperandsForOp(RHS, BinaryInst::MUL, R0, R1) &&
			L0 == L1 && R0 == R1) {
			auto *Plus = new BinaryInst(L0, BinaryInst::ADD, R0);
			auto *Minus = new BinaryInst(L0, BinaryInst::SUB, R0);
			if (Instruction::insertBefore(BI, Plus) && Instruction::insertBefore(BI, Minus)) {
				return new BinaryInst(Plus, BinaryInst::MUL, Minus);
			}
			delete Plus;
			delete Minus;
		}
	}

	return nullptr;
}

Value *InstSimplifier::simplifyMul(
	BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed) {
	const bool IsIntArith = IRType::isInt(BI->getIRType()) && !IRType::isBool(BI->getIRType());

	if (LHS->isConst() && RHS->isConst()) {
		return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
	}

	maybeSwapCommutative(BI, LHS, RHS, Changed);

	if (Value::isUndef(LHS) || Value::isUndef(RHS)) {
		// Choosing integer undef as zero is valid under the IR's undef
		// semantics.  It is not valid for strict FP when the other operand
		// may be Inf (0.0 * Inf is NaN).
		return IsIntArith ? Value::makeZeroForType(BI->getIRType())
		                  : Value::makeUndefForType(BI->getIRType());
	}

	// `fmul x, 0.0` is NaN for NaN/Inf and also carries signed-zero rules.
	if (IsIntArith && Value::isConstZero(RHS)) {
		return Value::makeZeroForType(BI->getIRType());
	}

	if (Value::isConstOne(RHS)) {
		return LHS;
	}

	if (IRType::isInt(BI->getIRType()) && !IRType::isBool(BI->getIRType()) &&
		Value::isConstMinusOne(RHS)) {
		return new BinaryInst(Value::makeZeroForType(BI->getIRType()), BinaryInst::SUB, LHS);
	}

	// x * 2^n -> x << n (integer domain)
	if (IRType::isInt(BI->getIRType())) {
		int32_t C = 0;
		if (Value::tryGetIntConst(RHS, C) && C > 0) {
			uint32_t U = static_cast<uint32_t>(C);
			if (IRMath::isUnsignedPowerOf2(U)) {
				return new BinaryInst(LHS, BinaryInst::SHL,
					Value::makeIntegerLike(
						LHS, IRMath::log2u(U)));
			}
		}
	}

	return nullptr;
}

Value *InstSimplifier::simplifyDiv(
	BinaryInst *BI, Value *LHS, Value *RHS) {
	const auto Op = BI->getOp();
	const auto Ty = BI->getIRType();
	const bool IsFloat = IRType::isFloat(Ty);
	const bool IsIntArith = IRType::isInt(Ty) && !IRType::isBool(Ty);

	// Do not fold x / (0 - x) to -1 without a range/no-wrap proof:
	// for signed INT_MIN, 0 - x wraps back to x and the quotient is 1.

	if (LHS->isConst() && RHS->isConst()) {
		// foldConstants distinguishes integer zero-divisor undef from IEEE
		// fdiv Inf/NaN and is safe for fully constant operands.
		return BinaryInst::foldConstants(Op, LHS, RHS);
	}

	if (Value::isUndef(RHS)) {
		return Value::makeUndefForType(Ty);
	}

	if (Value::isUndef(LHS)) {
		if (IsFloat) return Value::makeUndefForType(Ty);
		if (Value::isConstZero(RHS) || Value::isUndef(RHS)) {
			return Value::makeUndefForType(Ty);
		}
		return Value::makeZeroForType(Ty);
	}

	if (Value::isConstZero(RHS)) {
		return IsFloat ? nullptr : Value::makeUndefForType(Ty);
	}

	// Strict FP 0/x is not constant: x may be zero, NaN, Inf or signed.
	if (IsIntArith && Value::isConstZero(LHS)) {
		return Value::makeZeroForType(Ty);
	}

	// Strict FP x/x is NaN for zero, Inf and NaN.
	if (IsIntArith && LHS == RHS) {
		return Value::makeOneForType(Ty);
	}

	if (Value::isConstOne(RHS)) {
		return LHS;
	}

	// fdiv x, -1.0 and 0.0 - x disagree for signed zero.
	if (IsIntArith && Op == BinaryInst::DIV && Value::isConstMinusOne(RHS)) {
		return new BinaryInst(Value::makeZeroForType(Ty), BinaryInst::SUB, LHS);
	}

	return nullptr;
}

Value *InstSimplifier::simplifyRem(
	BinaryInst *BI, Value *LHS, Value *RHS) {
	const auto Ty = BI->getIRType();
	const bool IsFloat = IRType::isFloat(Ty);
	const bool IsIntArith = IRType::isInt(Ty) && !IRType::isBool(Ty);

	if (IsIntArith && InstSimplifyPass::isNegationPair(LHS, RHS)) {
		return Value::makeZeroForType(Ty);
	}

	if (LHS->isConst() && RHS->isConst()) {
		// Constant IEEE frem, including a zero divisor, is handled as NaN by
		// foldConstants rather than integer-style undef.
		return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
	}

	if (Value::isUndef(RHS)) return Value::makeUndefForType(Ty);
	if (Value::isUndef(LHS)) {
		return IsFloat ? Value::makeUndefForType(Ty)
		               : Value::makeZeroForType(Ty);
	}

	if (IsIntArith && Value::isConstZero(LHS)) return Value::makeZeroForType(Ty);
	if (Value::isConstZero(RHS)) {
		return IsFloat ? nullptr : Value::makeUndefForType(Ty);
	}
	if (IsIntArith && Value::isConstOne(RHS)) return Value::makeZeroForType(Ty);
	if (IsIntArith && BI->getOp() == BinaryInst::MOD && Value::isConstMinusOne(RHS))
		return Value::makeZeroForType(Ty);
	if (IsIntArith && LHS == RHS) return Value::makeZeroForType(Ty);

	return nullptr;
}

Value *InstSimplifier::simplifyAnd(
	BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed) {
	if (LHS->isConst() && RHS->isConst()) {
		return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
	}

	maybeSwapCommutative(BI, LHS, RHS, Changed);

	if (Value::isUndef(LHS) || Value::isUndef(RHS)) {
		return Value::makeZeroForType(BI->getIRType());
	}

	if (LHS == RHS) return LHS;
	if (Value::isConstZero(RHS)) return Value::makeZeroForType(BI->getIRType());
	if (IRType::isBool(BI->getIRType()) && Value::isConstOne(RHS))
		return LHS;
	if (Value::isConstMinusOne(RHS)) return LHS;

	// Boolean complements are represented canonically as xor i1 X, true.
	if (IRType::isBool(BI->getIRType())) {
		auto isNotOf = [](Value *MaybeNot, Value *X) {
			auto *Not = dynamic_cast<BinaryInst *>(MaybeNot);
			Value *NL = nullptr;
			Value *NR = nullptr;
			return Not && Not->getOp() == BinaryInst::XOR &&
				BinaryInst::getOperands(Not, NL, NR) && NL == X &&
				Value::isConstOne(NR);
		};
		if (isNotOf(LHS, RHS) || isNotOf(RHS, LHS))
			return ConstBool::newConstBool(false);
	}

	// x & (x | y) == x.  This is valid for both booleans and integer
	// bitmasks and often collapses short-circuit CFGs after if-conversion.
	auto absorbsOr = [](Value *X, Value *MaybeOr) {
		auto *Or = dynamic_cast<BinaryInst *>(MaybeOr);
		Value *OL = nullptr;
		Value *OR = nullptr;
		return Or && Or->getOp() == BinaryInst::OR &&
			BinaryInst::getOperands(Or, OL, OR) && (OL == X || OR == X);
	};
	if (absorbsOr(LHS, RHS)) return LHS;
	if (absorbsOr(RHS, LHS)) return RHS;

	return nullptr;
}

Value *InstSimplifier::simplifyOr(
	BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed) {
	if (LHS->isConst() && RHS->isConst()) {
		return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
	}

	maybeSwapCommutative(BI, LHS, RHS, Changed);

	if (Value::isUndef(LHS) || Value::isUndef(RHS)) {
		return Value::makeMinusOneForType(BI->getIRType());
	}

	if (Value::isConstMinusOne(RHS)) return Value::makeMinusOneForType(BI->getIRType());
	if (LHS == RHS) return LHS;
	if (Value::isConstZero(RHS)) return LHS;
	if (IRType::isBool(BI->getIRType()) && Value::isConstOne(RHS))
		return ConstBool::newConstBool(true);

	if (IRType::isBool(BI->getIRType())) {
		auto isNotOf = [](Value *MaybeNot, Value *X) {
			auto *Not = dynamic_cast<BinaryInst *>(MaybeNot);
			Value *NL = nullptr;
			Value *NR = nullptr;
			return Not && Not->getOp() == BinaryInst::XOR &&
				BinaryInst::getOperands(Not, NL, NR) && NL == X &&
				Value::isConstOne(NR);
		};
		if (isNotOf(LHS, RHS) || isNotOf(RHS, LHS))
			return ConstBool::newConstBool(true);
	}

	// x | (x & y) == x.
	auto absorbsAnd = [](Value *X, Value *MaybeAnd) {
		auto *And = dynamic_cast<BinaryInst *>(MaybeAnd);
		Value *AL = nullptr;
		Value *AR = nullptr;
		return And && And->getOp() == BinaryInst::AND &&
			BinaryInst::getOperands(And, AL, AR) && (AL == X || AR == X);
	};
	if (absorbsAnd(LHS, RHS)) return LHS;
	if (absorbsAnd(RHS, LHS)) return RHS;

	// (x > a) || (x < b) -> true, when a < b
	if (IRType::isBool(BI->getIRType())) {
		Value *GL = nullptr;
		Value *GR = nullptr;
		Value *LL = nullptr;
		Value *LR = nullptr;
		if (InstSimplifyPass::getBinaryOperandsForOp(LHS, BinaryInst::G, GL, GR) &&
			InstSimplifyPass::getBinaryOperandsForOp(RHS, BinaryInst::L, LL, LR) &&
			GL == LL && GR && LR && GR->isConst() && LR->isConst() &&
			IRType::isInt(GL->getIRType()) && !IRType::isFloat(GL->getIRType())) {
			Value *Cmp = BinaryInst::foldConstants(BinaryInst::L, GR, LR);
			bool TrueCond = false;
			if (Cmp && Value::tryGetBoolConst(Cmp, TrueCond) && TrueCond) {
				return ConstBool::newConstBool(true);
			}
		}

		if (InstSimplifyPass::getBinaryOperandsForOp(LHS, BinaryInst::L, GL, GR) &&
			InstSimplifyPass::getBinaryOperandsForOp(RHS, BinaryInst::G, LL, LR) &&
			GL == LL && GR && LR && GR->isConst() && LR->isConst() &&
			IRType::isInt(GL->getIRType()) && !IRType::isFloat(GL->getIRType())) {
			Value *Cmp = BinaryInst::foldConstants(BinaryInst::L, LR, GR);
			bool TrueCond = false;
			if (Cmp && Value::tryGetBoolConst(Cmp, TrueCond) && TrueCond) {
				return ConstBool::newConstBool(true);
			}
		}
	}

	return nullptr;
}

Value *InstSimplifier::simplifyXor(
	BinaryInst *BI, Value *LHS, Value *RHS, bool &Changed) {
	if (LHS->isConst() && RHS->isConst()) {
		return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
	}

	maybeSwapCommutative(BI, LHS, RHS, Changed);

	if (Value::isUndef(LHS) || Value::isUndef(RHS)) {
		return Value::makeUndefForType(BI->getIRType());
	}

	if (Value::isConstZero(RHS)) return LHS;
	if (LHS == RHS) return Value::makeZeroForType(BI->getIRType());

	if (IRType::isBool(BI->getIRType()) && Value::isConstOne(RHS)) {
		// !!x -> x, while a single xor true remains the canonical boolean
		// negation used by CFGSimplify.
		auto *Inner = dynamic_cast<BinaryInst *>(LHS);
		Value *IL = nullptr;
		Value *IR = nullptr;
		if (Inner && Inner->getOp() == BinaryInst::XOR &&
			BinaryInst::getOperands(Inner, IL, IR) &&
			Value::isConstOne(IR)) {
			return IL;
		}
	}

	return nullptr;
}

Value *InstSimplifier::simplifyCmp(
	BinaryInst *BI, Value *LHS, Value *RHS) {
	if (LHS->isConst() && RHS->isConst()) {
			// Strict SysY/C comparisons are ordered except !=.  Keep this
			// explicit so every constant-folding entry point agrees with the
			// textual IR and the RISC-V lowering.
		if (IRType::isFloat(LHS->getIRType())) {
			float LV = 0.0f;
			float RV = 0.0f;
			if (Value::tryGetFloatConst(LHS, LV) &&
				Value::tryGetFloatConst(RHS, RV) &&
				(std::isnan(LV) || std::isnan(RV))) {
				return ConstBool::newConstBool(
					BI->getOp() == BinaryInst::NE);
			}
		}
		return BinaryInst::foldConstants(BI->getOp(), LHS, RHS);
	}

	if (BI->getOp() == BinaryInst::E || BI->getOp() == BinaryInst::NE) {
		auto tryBoolCmpConst = [&](Value *A, Value *B) -> Value * {
			Value::BooleanExtensionKind Kind =
				Value::BooleanExtensionKind::None;
			Value *Cond = Value::stripBooleanExtension(A, Kind);
			if (!Cond || !B) return nullptr;

			int32_t Raw = 0;
			if (auto *CB = dynamic_cast<ConstBool *>(B)) {
				Raw = CB->getVal() ? 1 : 0;
			} else if (auto *CI = dynamic_cast<ConstInt *>(B)) {
				Raw = CI->getVal();
			} else {
				return nullptr;
			}

			int32_t TrueValue =
				(Kind == Value::BooleanExtensionKind::SignExt) ? -1 : 1;
			if (Raw == TrueValue) {
				return BI->getOp() == BinaryInst::E
					? Cond : BinaryInst::createBooleanNot(Cond);
			}
			if (Raw == 0) {
				return BI->getOp() == BinaryInst::E
					? BinaryInst::createBooleanNot(Cond) : Cond;
			}
			return ConstBool::newConstBool(BI->getOp() == BinaryInst::NE);
		};

		if (Value *V = tryBoolCmpConst(LHS, RHS)) return V;
		if (Value *V = tryBoolCmpConst(RHS, LHS)) return V;
	}

	// Recognize an integer min/max select and compare it with either source
	// operand.  Only order-theoretic facts are folded, so this needs no
	// overflow assumption:
	//   max(a,b) <  a/b  -> false    max(a,b) >= a/b -> true
	//   min(a,b) >  a/b  -> false    min(a,b) <= a/b -> true
	auto matchExtremum = [](Value *V, Value *Bound) {
		auto *Sel = dynamic_cast<SelectInst *>(V);
		if (!Sel || !Bound || !IRType::isInt(Sel->getIRType()) ||
			IRType::isBool(Sel->getIRType())) {
			return ExtremumKind::None;
		}
		Value *TV = Sel->getTrueValue();
		Value *FV = Sel->getFalseValue();
		if (!TV || !FV || (Bound != TV && Bound != FV) ||
			Value::isUndef(TV) || Value::isUndef(FV)) {
			return ExtremumKind::None;
		}
		auto *Cmp = dynamic_cast<BinaryInst *>(Sel->getCondition());
		Value *CL = nullptr;
		Value *CR = nullptr;
		if (!Cmp || !BinaryInst::isCompareOp(Cmp->getOp()) ||
			!BinaryInst::getOperands(Cmp, CL, CR) || !CL || !CR ||
			IRType::isFloat(CL->getIRType())) {
			return ExtremumKind::None;
		}

		auto Pred = Cmp->getOp();
		if (CL == FV && CR == TV) {
			Pred = BinaryInst::reversePredicate(Pred);
		} else if (CL != TV || CR != FV) {
			return ExtremumKind::None;
		}

		if (Pred == BinaryInst::G || Pred == BinaryInst::GE)
			return ExtremumKind::Maximum;
		if (Pred == BinaryInst::L || Pred == BinaryInst::LE)
			return ExtremumKind::Minimum;
		return ExtremumKind::None;
	};

	if (ExtremumKind Kind = matchExtremum(LHS, RHS);
		Kind != ExtremumKind::None) {
		if (Kind == ExtremumKind::Maximum && BI->getOp() == BinaryInst::L)
			return ConstBool::newConstBool(false);
		if (Kind == ExtremumKind::Maximum && BI->getOp() == BinaryInst::GE)
			return ConstBool::newConstBool(true);
		if (Kind == ExtremumKind::Minimum && BI->getOp() == BinaryInst::G)
			return ConstBool::newConstBool(false);
		if (Kind == ExtremumKind::Minimum && BI->getOp() == BinaryInst::LE)
			return ConstBool::newConstBool(true);
	}
	if (ExtremumKind Kind = matchExtremum(RHS, LHS);
		Kind != ExtremumKind::None) {
		if (Kind == ExtremumKind::Maximum && BI->getOp() == BinaryInst::G)
			return ConstBool::newConstBool(false);
		if (Kind == ExtremumKind::Maximum && BI->getOp() == BinaryInst::LE)
			return ConstBool::newConstBool(true);
		if (Kind == ExtremumKind::Minimum && BI->getOp() == BinaryInst::L)
			return ConstBool::newConstBool(false);
		if (Kind == ExtremumKind::Minimum && BI->getOp() == BinaryInst::GE)
			return ConstBool::newConstBool(true);
	}

	// x % (2^n) == t  -> (x & (2^n - 1)) == t
	// 约束：
	// 1) 仅处理等值比较
	// 2) UREM: t in [0, 2^n-1]
	// 3) MOD: 仅 t==0 保守安全
	if (BI->getOp() == BinaryInst::E) {
		auto tryModEqPow2 = [&](Value *A, Value *B) -> Value * {
			Value *X = nullptr;
			Value *D = nullptr;
			auto *MB = dynamic_cast<BinaryInst *>(A);
			if (!MB) return nullptr;
			if (MB->getOp() != BinaryInst::MOD && MB->getOp() != BinaryInst::UREM) return nullptr;
			if (!InstSimplifyPass::getBinaryOperandsForOp(MB, MB->getOp(), X, D)) return nullptr;
			if (!B || !B->isConst()) return nullptr;

			int32_t Div = 0;
			int32_t T = 0;
			if (!Value::tryGetIntConst(D, Div) || !Value::tryGetIntConst(B, T)) return nullptr;
			if (Div <= 0) return nullptr;

			const uint32_t UDiv = static_cast<uint32_t>(Div);
			if (!IRMath::isUnsignedPowerOf2(UDiv)) return nullptr;

			const int32_t Mask = static_cast<int32_t>(UDiv - 1u);
			bool Safe = false;
			if (MB->getOp() == BinaryInst::UREM) {
				Safe = (T >= 0 && static_cast<uint32_t>(T) <= static_cast<uint32_t>(Mask));
			} else {
				Safe = (T == 0);
			}
			if (!Safe) return nullptr;

			auto *And = new BinaryInst(
				X, BinaryInst::AND,
				Value::makeIntegerLike(X, Mask));
			if (!Instruction::insertBefore(BI, And)) {
				delete And;
				return nullptr;
			}
			return new BinaryInst(And, BinaryInst::E, B);
		};

		if (Value *R = tryModEqPow2(LHS, RHS)) return R;
		if (Value *R = tryModEqPow2(RHS, LHS)) return R;
	}

	// With strict FP, self-comparisons depend on NaN.  Even where one
	// unordered predicate happens to be constant, keeping the fcmp here
	// avoids silently assuming nnan in this generic integer/FP path.
	if (LHS == RHS && !IRType::isFloat(LHS->getIRType())) {
		switch (BI->getOp()) {
		case BinaryInst::E:
		case BinaryInst::GE:
		case BinaryInst::LE:
			return ConstBool::newConstBool(true);
		case BinaryInst::NE:
		case BinaryInst::G:
		case BinaryInst::L:
			return ConstBool::newConstBool(false);
		default:
			return nullptr;
		}
	}

	return nullptr;
}

Value *InstSimplifier::simplifyBinary(BinaryInst *BI, bool &Changed) {
	Value *LHS = nullptr;
	Value *RHS = nullptr;
	if (!BinaryInst::getOperands(BI, LHS, RHS)) return nullptr;

	switch (BI->getOp()) {
	case BinaryInst::ADD:
		return simplifyAdd(BI, LHS, RHS, Changed);
	case BinaryInst::SUB:
		return simplifySub(BI, LHS, RHS);
	case BinaryInst::MUL:
		return simplifyMul(BI, LHS, RHS, Changed);
	case BinaryInst::DIV:
	case BinaryInst::UDIV:
		return simplifyDiv(BI, LHS, RHS);
	case BinaryInst::MOD:
	case BinaryInst::UREM:
		return simplifyRem(BI, LHS, RHS);
	case BinaryInst::AND:
		return simplifyAnd(BI, LHS, RHS, Changed);
	case BinaryInst::OR:
		return simplifyOr(BI, LHS, RHS, Changed);
	case BinaryInst::XOR:
		return simplifyXor(BI, LHS, RHS, Changed);
	case BinaryInst::E:
	case BinaryInst::NE:
	case BinaryInst::G:
	case BinaryInst::GE:
	case BinaryInst::L:
	case BinaryInst::LE:
		return simplifyCmp(BI, LHS, RHS);
	default:
		return nullptr;
	}
}

Value *InstSimplifier::simplifySelect(SelectInst *SI) {
	Value *Cond = SI->getCondition();
	Value *TV = SI->getTrueValue();
	Value *FV = SI->getFalseValue();
	if (!Cond || !TV || !FV) return nullptr;

	if (TV == FV) return TV;

	if (Value::isUndef(TV)) return FV;
	if (Value::isUndef(FV)) return TV;

	if (Value::isUndef(Cond)) {
		if (TV->isConst() && !FV->isConst()) return TV;
		if (FV->isConst() && !TV->isConst()) return FV;
		return TV;
	}

	bool CV = false;
	if (Value::tryGetBoolConst(Cond, CV)) {
		return CV ? TV : FV;
	}

	if (Cond->isConst() && TV->isConst() && FV->isConst()) {
		// 三个操作数均为常量时，按条件常量进行折叠；
		// 若条件常量形态不可判真值，则保守不折叠。
		bool ConstCond = false;
		if (Value::tryGetBoolConst(Cond, ConstCond)) {
			return ConstCond ? TV : FV;
		}
		return nullptr;
	}

	const auto ResTy = SI->getIRType();
	if (IRType::isBool(ResTy)) {
		if (Value::isConstZero(FV)) {
			return new BinaryInst(Cond, BinaryInst::AND, TV);
		}
		if (Value::isConstOne(TV)) {
			return new BinaryInst(Cond, BinaryInst::OR, FV);
		}

		if (IRType::isBool(Cond->getIRType()) && Value::isConstOne(FV)) {
			auto *NotX = BinaryInst::createBooleanNot(Cond);
			if (Instruction::insertBefore(SI, NotX)) {
				return new BinaryInst(NotX, BinaryInst::OR, TV);
			}
			delete NotX;
		}

		if (IRType::isBool(Cond->getIRType()) && Value::isConstZero(TV)) {
			auto *NotX = BinaryInst::createBooleanNot(Cond);
			if (Instruction::insertBefore(SI, NotX)) {
				return new BinaryInst(NotX, BinaryInst::AND, FV);
			}
			delete NotX;
		}
	}

	if (IRType::isBool(Cond->getIRType()) && IRType::isInt(ResTy) && !IRType::isBool(ResTy) &&
		Value::isConstOne(TV) && Value::isConstZero(FV)) {
		auto *ZI = new ZextInst(
			Cond, std::dynamic_pointer_cast<INTType>(ResTy));
		return ZI;
	}

	if (auto *Cmp = dynamic_cast<BinaryInst *>(Cond)) {
		Value *CmpL = nullptr;
		Value *CmpR = nullptr;
		if (BinaryInst::getOperands(Cmp, CmpL, CmpR) && CmpL && CmpR) {
			const bool IsFloatCmp = IRType::isFloat(CmpL->getIRType());

			if (!IsFloatCmp && Cmp->getOp() == BinaryInst::E &&
				((CmpL == TV && CmpR == FV) || (CmpL == FV && CmpR == TV))) {
				// select(x == y, x, y) is y on the false edge and x == y
				// on the true edge, hence it is exactly the false value.
				return FV;
			}

			if (!IsFloatCmp && Cmp->getOp() == BinaryInst::NE &&
				((CmpL == TV && CmpR == FV) || (CmpL == FV && CmpR == TV))) {
				// Conversely, select(x != y, x, y) is the true value.
				return TV;
			}

			if (Cmp->getOp() == BinaryInst::E && Value::isConstZero(CmpR)) {
				if (auto *OrI = dynamic_cast<BinaryInst *>(CmpL)) {
					Value *OrL = nullptr;
					Value *OrR = nullptr;
					if (OrI->getOp() == BinaryInst::OR && BinaryInst::getOperands(OrI, OrL, OrR) &&
						(TV == OrL || TV == OrR) && Value::isConstZero(FV)) {
						return Value::makeZeroForType(TV->getIRType());
					}
				}
			}

			if (Cmp->getOp() == BinaryInst::E && Value::isConstMinusOne(CmpR)) {
				if (auto *AndI = dynamic_cast<BinaryInst *>(CmpL)) {
					Value *AndL = nullptr;
					Value *AndR = nullptr;
					if (AndI->getOp() == BinaryInst::AND && BinaryInst::getOperands(AndI, AndL, AndR) &&
						(TV == AndL || TV == AndR) && Value::isConstMinusOne(FV)) {
						return Value::makeMinusOneForType(TV->getIRType());
					}
				}
			}
		}
	}

	return nullptr;
}

Value *InstSimplifier::simplifyPhi(PhiInst *Phi) {
	if (!Phi) return nullptr;
	if (Phi->getIdent().find(".lcssa") != std::string::npos) return nullptr;

	if (Value *V = foldPhiOfBinary(Phi)) return V;
	if (Value *V = foldPhiOfGEP(Phi)) return V;
	if (Value *V = foldPhiOfLoad(Phi)) return V;

	Value *Common = nullptr;
	bool HasUndef = false;

	bool Compatible = forEachPhiIncoming(
		Phi, [&](unsigned, Value *InV, BasicBlock *) {
			if (!InV || InV == Phi) return true;
			if (Value::isUndef(InV)) {
				HasUndef = true;
				return true;
			}
			if (!Common) {
				Common = InV;
				return true;
			}
			return Common == InV;
		});
	if (!Compatible) return nullptr;

	if (!Common) return nullptr;

	if (HasUndef) {
		// 无法确认支配关系时(例如没有DT)必须保守退出，避免错误化简。
		if (!DT) return nullptr;
		if (!DT->dominatesValue(Common, Phi)) return nullptr;
	}

	return Common;
}

PreservationStatus InstSimplifyPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	DominatorTree *DT = nullptr;
	DT = &FAM.getResult<DominantAnalysis>(F);
	BasicAAResult *AA = &FAM.getResult<BasicAliasAnalysis>(F);
	LoopInfo *LI = &FAM.getResult<LoopAnalysis>(F);

	InstSimplifier Simplifier(DT, &F, &FAM, AA, LI);
	bool Changed = Simplifier.run(F);

	if (!Changed) return PreservationStatus::all();

	PreservationStatus PA;
	PA.keep<DominantAnalysis>();
	PA.keep<PostDominantAnalysis>();
	PA.keep<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
