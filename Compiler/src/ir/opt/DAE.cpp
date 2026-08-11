#include "ir/opt/DAE.hpp"
#include "lib/IRUtils.hpp"
#include "lib/Target.hpp"
#include <algorithm>
#include <map>
#include <stdexcept>

bool DAEPass::collectPointerRecipe(
	Value *V, SpecializationValue &Result, unsigned Depth) {
	if (!V || !V->getIRType() || !V->getIRType()->isPointer() ||
		Depth > MaxPointerRecipeDepth) {
		return false;
	}

	if (auto *Global = dynamic_cast<Variable *>(V)) {
		if (!Global->isGlobal()) return false;
		Result.pointerBase = Global;
		Result.key += "global:" + Global->getIdent() + ":" +
			IRKey::type(Global->getIRType());
		return true;
	}

	if (auto *GEP = dynamic_cast<GetElementPtrInst *>(V)) {
		if (!GEP->hasOnlyConstantIndices() ||
			!collectPointerRecipe(GEP->getBasePointer(), Result, Depth + 1)) {
			return false;
		}
		PointerRecipeStep Step;
		Step.kind = PointerRecipeStep::Kind::GEP;
		Step.indices = GEP->getIndices();
		Result.key += "/gep";
		for (Value *Index : Step.indices) {
			if (!Index || Index->isUndef()) return false;
			Result.key += ":" + IRKey::type(Index->getIRType()) + "=" +
				Index->getIdent();
		}
		Result.key += ":" + IRKey::type(GEP->getIRType());
		Result.pointerSteps.push_back(std::move(Step));
		return true;
	}

	if (auto *Cast = dynamic_cast<BitCastInst *>(V)) {
		Value *Source = Cast->getUnaryOperand();
		if (!Source || !Source->getIRType() ||
			!Source->getIRType()->isPointer() ||
			!collectPointerRecipe(Source, Result, Depth + 1)) {
			return false;
		}
		PointerRecipeStep Step;
		Step.kind = PointerRecipeStep::Kind::BitCast;
		Step.targetType = Cast->getIRType();
		Result.key += "/bitcast:" + IRKey::type(Step.targetType);
		Result.pointerSteps.push_back(std::move(Step));
		return true;
	}

	return false;
}

std::optional<DAEPass::SpecializationValue>
DAEPass::describeSpecializationValue(
	Value *Argument, Value *Parameter) {
	if (!Argument || !Parameter || !Argument->getIRType() ||
		!Parameter->getIRType() ||
		Argument->isUndef() ||
		!Argument->getIRType()->hasSameShape(*Parameter->getIRType())) {
		return std::nullopt;
	}

	SpecializationValue Result;
	if (Argument->isConst()) {
		Result.directValue = Argument;
		Result.key = "literal:" + IRKey::type(Argument->getIRType()) +
			"=" + Argument->getIdent();
		return Result;
	}

	if (!Parameter->getIRType()->isPointer() ||
		!collectPointerRecipe(Argument, Result)) {
		return std::nullopt;
	}
	return Result;
}

Value *DAEPass::materializeSpecializationValue(
	const SpecializationValue &Spec, BasicBlock *InsertionBlock) {
	if (Spec.directValue) return Spec.directValue;
	if (!Spec.pointerBase || !InsertionBlock) return nullptr;

	Value *Current = Spec.pointerBase;
	std::vector<Instruction *> Created;
	Created.reserve(Spec.pointerSteps.size());
	auto Rollback = [&Created]() {
		for (Instruction *I : Created)
			if (I) I->clearOperands();
		for (Instruction *I : Created)
			delete I;
		Created.clear();
	};
	try {
		for (const PointerRecipeStep &Step : Spec.pointerSteps) {
			Instruction *NewInstruction = nullptr;
			if (Step.kind == PointerRecipeStep::Kind::GEP) {
				auto Indices = Step.indices;
				NewInstruction = new GetElementPtrInst(Current, Indices);
			} else {
				if (!Step.targetType) {
					Rollback();
					return nullptr;
				}
				NewInstruction = new BitCastInst(Current, Step.targetType);
			}
			InsertionBlock->pushBack(NewInstruction);
			Created.push_back(NewInstruction);
			Current = NewInstruction;
		}
	} catch (const std::invalid_argument &) {
		Rollback();
		return nullptr;
	} catch (...) {
		Rollback();
		throw;
	}
	return Current;
}

void DAEPass::moveMaterializedValuesToEntry(
	BasicBlock *Staging, BasicBlock *Entry) {
	if (!Staging || !Entry || Staging == Entry) return;
	Instruction *Anchor = Entry->front();
	while (Instruction *I = Staging->front()) {
		I->eraseFromParent();
		if (Anchor)
			Anchor->insertBefore(I);
		else
			Entry->pushBack(I);
	}
}

bool DAEPass::hasFullyReachableBody(Function &F) {
	std::unordered_set<BasicBlock *> Reachable;
	F.collectReachableBlocks(Reachable);
	return Reachable.size() == static_cast<std::size_t>(F.size());
}

std::size_t DAEPass::countInstructions(Function &F) {
	std::size_t Count = 0;
	for (BasicBlock *BB : F)
		if (BB) Count += static_cast<std::size_t>(BB->size());
	return Count;
}

std::string DAEPass::makeUniqueSpecializedName(
	Module &M, Function &F, std::size_t &Ordinal) {
	for (;;) {
		std::string Name = F.getIdent() + ".spec." +
			std::to_string(Ordinal++);
		if (!M.findFunctionByName(Name)) return Name;
	}
}

std::string DAEPass::makeGroupKey(
	const std::vector<std::pair<unsigned, SpecializationValue>> &Args,
	const std::vector<ParameterEquality> &EqualArguments) {
	std::string Key;
	for (const auto &Entry : Args) {
		Key += "#v" + std::to_string(Entry.first) + "=" + Entry.second.key;
	}
	for (const ParameterEquality &Equality : EqualArguments) {
		Key += "#e" + std::to_string(Equality.redundantIndex) + "->" +
			std::to_string(Equality.representativeIndex);
	}
	return Key;
}

std::unique_ptr<Function> DAEPass::cloneSpecializedFunction(
	Function &Source, const std::string &Name,
	const std::vector<std::pair<unsigned, SpecializationValue>> &Specialized,
	const std::vector<ParameterEquality> &EqualArguments) {
	auto Clone = std::make_unique<Function>(Source.getIRType(), Name);
	BasicBlock *Staging = Clone->front();
	if (!Staging) return nullptr;
	auto Fail = [&Clone]() -> std::unique_ptr<Function> {
		Clone->dropAllReferences();
		return nullptr;
	};

	const unsigned ParamCount = Source.getParamCount();
	std::vector<unsigned> RepresentativeByIndex(ParamCount);
	for (unsigned Index = 0; Index < ParamCount; ++Index)
		RepresentativeByIndex[Index] = Index;
	for (const ParameterEquality &Equality : EqualArguments) {
		const unsigned Representative = Equality.representativeIndex;
		const unsigned Redundant = Equality.redundantIndex;
		if (Representative >= ParamCount || Redundant >= ParamCount ||
			Representative >= Redundant ||
			RepresentativeByIndex[Representative] != Representative ||
			RepresentativeByIndex[Redundant] != Redundant) {
			return Fail();
		}
		Param *RepresentativeParam = Source.getParam(Representative);
		Param *RedundantParam = Source.getParam(Redundant);
		if (!RepresentativeParam || !RedundantParam ||
			!RepresentativeParam->getIRType() ||
			!RedundantParam->getIRType() ||
			!RepresentativeParam->getIRType()->hasSameShape(
				*RedundantParam->getIRType())) {
			return Fail();
		}
		RepresentativeByIndex[Redundant] = Representative;
	}

	std::map<unsigned, const SpecializationValue *> SpecializedByIndex;
	for (const auto &Entry : Specialized) {
		if (Entry.first >= ParamCount ||
			RepresentativeByIndex[Entry.first] != Entry.first ||
			!SpecializedByIndex.emplace(Entry.first, &Entry.second).second) {
			return Fail();
		}
	}

	ValueToValueMap InitialMap;
	// First create or materialize every canonical representative.  Aliases are
	// mapped in a second phase so the mapping never depends on relation order.
	for (unsigned Index = 0; Index < ParamCount; ++Index) {
		if (RepresentativeByIndex[Index] != Index) continue;
		Param *OldParam = Source.getParam(Index);
		if (!OldParam) return Fail();
		auto It = SpecializedByIndex.find(Index);
		if (It != SpecializedByIndex.end()) {
			Value *Replacement = materializeSpecializationValue(
				*It->second, Staging);
			if (!Replacement || !Replacement->getIRType()->hasSameShape(
								  *OldParam->getIRType())) {
				return Fail();
			}
			InitialMap[OldParam] = Replacement;
			continue;
		}

		auto NewParam = std::make_unique<Param>(OldParam->getIRType(),
			OldParam->getIdent());
		Param *NewParamValue = NewParam.get();
		Clone->getParams().push_back(std::move(NewParam));
		InitialMap[OldParam] = NewParamValue;
	}
	for (unsigned Index = 0; Index < ParamCount; ++Index) {
		const unsigned Representative = RepresentativeByIndex[Index];
		if (Representative == Index) continue;
		Param *OldParam = Source.getParam(Index);
		Param *RepresentativeParam = Source.getParam(Representative);
		if (!OldParam || !RepresentativeParam) return Fail();
		auto It = InitialMap.find(RepresentativeParam);
		if (It == InitialMap.end() || !It->second ||
			!It->second->getIRType()->hasSameShape(*OldParam->getIRType())) {
			return Fail();
		}
		InitialMap[OldParam] = It->second;
	}

	std::vector<BasicBlock *> SourceBlocks;
	SourceBlocks.reserve(static_cast<std::size_t>(Source.size()));
	for (BasicBlock *BB : Source)
		if (BB) SourceBlocks.push_back(BB);
	if (SourceBlocks.empty()) return Fail();

	auto Region = cloneBasicBlockRegion(*Clone, SourceBlocks, InitialMap);
	if (!Region) return Fail();
	BasicBlock *ClonedEntry = Region->getClonedBlock(Source.front());
	if (!ClonedEntry) return Fail();

	moveMaterializedValuesToEntry(Staging, ClonedEntry);
	delete Staging;
	return Clone;
}
//优化1：常量形参特化
/*
define i32 @foo(i32 %x) {
entry:
  %v = add i32 %x, 1
  ret i32 %v
}

call i32 @foo(i32 10)

->

define i32 @foo(i32 %x) {
entry:
  %v = add i32 10, 1
  ret i32 %v
}
*/
bool DAEPass::tryConstantSpecialization(
	Function *F, const std::vector<CallInst *> &CallSites) {
	if (!F || !F->front() || !F->hasOnlyDirectCallUses()) return false;
	if (F->getParamCount() == 0) return false;
	if (CallSites.empty()) return false;

	bool Changed = false;
	const bool FullyReachable = hasFullyReachableBody(*F);

	for (unsigned i = 0; i < F->getParamCount(); ++i) {
        //遍历该函数中每一个形参
		Value *P = F->getParam(i);
		if (!P) continue;

		bool AllSameValue = true;
		std::optional<SpecializationValue> CommonValue;
        //遍历调用该函数的每一个Call指令
		for (CallInst *CI : CallSites) {
			if (!CI || CI->getArgCount() != F->getParamCount()) {
				AllSameValue = false;
				break;
			}

            //获取该Call指令的第i个操作数，即传入该形参的实参
			Value *Arg = CI->getArg(static_cast<unsigned>(i));
			if (!Arg) {
				AllSameValue = false;
				break;
			}

			// A direct recursive edge that forwards the same formal parameter does
			// not introduce a new value.  Ignore it so an invariant supplied by
			// every real entry call can still be propagated through the recursion.
			if (CI->getParentFunction() == F && Arg == P) continue;

			auto Candidate = describeSpecializationValue(Arg, P);
			if (!Candidate ||
				(!FullyReachable && !Candidate->pointerSteps.empty())) {
				AllSameValue = false;
				break;
			}
			if (!CommonValue) {
				CommonValue = std::move(Candidate);
			} else if (CommonValue->key != Candidate->key) {
				AllSameValue = false;
				break;
			}
		}
		// Constants can be shared directly.  Caller-local pointer expressions
		// are rebuilt at the callee entry from their global-rooted recipe.
		if (AllSameValue && CommonValue) {
			BasicBlock Staging;
			Value *Replacement = materializeSpecializationValue(
				*CommonValue, &Staging);
			if (!Replacement || !Replacement->getIRType()->hasSameShape(
								  *P->getIRType())) {
				continue;
			}
			moveMaterializedValuesToEntry(&Staging, F->front());
			P->replaceAllUsesWith(Replacement);
			Changed = true;
		}
	}

	return Changed;
}

bool DAEPass::tryGroupedSpecialization(Module &M, Function *F) {
	if (!F || F->hasReservedSignature() || !F->hasOnlyDirectCallUses() ||
		F->getParamCount() == 0 ||
		F->getIdent().find(".spec.") != std::string::npos) {
		return false;
	}

	const std::size_t FunctionSize = countInstructions(*F);
	if (FunctionSize == 0 ||
		FunctionSize > MaxSpecializableFunctionInstructions) {
		return false;
	}

	const std::string ExistingPrefix = F->getIdent() + ".spec.";
	const bool FullyReachable = hasFullyReachableBody(*F);
	std::map<std::string, SpecializationGroup> Groups;
	for (CallInst *CI : F->getDirectCallSites()) {
		if (!CI || CI->getArgCount() != F->getParamCount()) continue;
		// Recursive calls describe the general transition of the function, not
		// an entry context.  Entry specializations clone that transition and
		// keep it targeting the original function, avoiding clone explosions.
		Function *Caller = CI->getParentFunction();
		if (Caller == F ||
			(Caller && Caller->getIdent().rfind(ExistingPrefix, 0) == 0)) {
			continue;
		}

		const unsigned ParamCount = F->getParamCount();
		std::vector<unsigned> RepresentativeByIndex(ParamCount);
		for (unsigned Index = 0; Index < ParamCount; ++Index)
			RepresentativeByIndex[Index] = Index;

		// Build caller-local equality classes by exact SSA identity.  The first
		// parameter in each class is the stable representative.  Undef is not an
		// equality fact because its different uses may denote different values.
		for (unsigned Index = 0; Index < ParamCount; ++Index) {
			Value *Argument = CI->getArg(Index);
			Param *Parameter = F->getParam(Index);
			if (!Argument || Argument->isUndef() || !Parameter ||
				!Parameter->getIRType()) {
				continue;
			}
			for (unsigned Prior = 0; Prior < Index; ++Prior) {
				if (CI->getArg(Prior) != Argument) continue;
				const unsigned Representative = RepresentativeByIndex[Prior];
				Param *RepresentativeParam = F->getParam(Representative);
				if (!RepresentativeParam || !RepresentativeParam->getIRType() ||
					!RepresentativeParam->getIRType()->hasSameShape(
						*Parameter->getIRType())) {
					continue;
				}
				RepresentativeByIndex[Index] = Representative;
				break;
			}
		}

		std::vector<ParameterEquality> EqualArguments;
		for (unsigned Index = 0; Index < ParamCount; ++Index) {
			if (RepresentativeByIndex[Index] == Index) continue;
			EqualArguments.push_back(
				{RepresentativeByIndex[Index], Index});
		}

		std::vector<std::pair<unsigned, SpecializationValue>> SpecializedArgs;
		for (unsigned Index = 0; Index < F->getParamCount(); ++Index) {
			// A specialized representative also supplies every equal member of its
			// class, so describe the value only once.
			if (RepresentativeByIndex[Index] != Index) continue;
			Param *P = F->getParam(Index);
			auto Candidate = describeSpecializationValue(CI->getArg(Index), P);
			if (Candidate &&
				(FullyReachable || Candidate->pointerSteps.empty())) {
				SpecializedArgs.emplace_back(Index, std::move(*Candidate));
			}
		}
		if (SpecializedArgs.empty() && EqualArguments.empty()) continue;

		std::string Key = makeGroupKey(SpecializedArgs, EqualArguments);
		auto &Group = Groups[Key];
		if (Group.calls.empty()) {
			Group.specializedArgs = std::move(SpecializedArgs);
			Group.equalArguments = std::move(EqualArguments);
		}
		Group.calls.push_back(CI);
	}
	if (Groups.empty()) return false;

	bool Changed = false;
	std::size_t CreatedCount = 0;
	std::size_t ClonedInstructions = 0;
	std::size_t NameOrdinal = 0;
	for (const auto &FunctionPtr : M.getFunctions()) {
		Function *Existing = FunctionPtr.get();
		if (!Existing || Existing == F ||
			Existing->getIdent().rfind(ExistingPrefix, 0) != 0) {
			continue;
		}
		++CreatedCount;
		ClonedInstructions += countInstructions(*Existing);
	}

	std::vector<std::pair<std::string, SpecializationGroup *>> OrderedGroups;
	OrderedGroups.reserve(Groups.size());
	for (auto &Entry : Groups)
		OrderedGroups.emplace_back(Entry.first, &Entry.second);
	std::sort(OrderedGroups.begin(), OrderedGroups.end(),
		[](const auto &Left, const auto &Right) {
			const SpecializationGroup &L = *Left.second;
			const SpecializationGroup &R = *Right.second;
			const std::size_t LRemoved =
				L.specializedArgs.size() + L.equalArguments.size();
			const std::size_t RRemoved =
				R.specializedArgs.size() + R.equalArguments.size();
			const std::size_t LScore = L.calls.size() * LRemoved;
			const std::size_t RScore = R.calls.size() * RRemoved;
			if (LScore != RScore) return LScore > RScore;
			if (L.calls.size() != R.calls.size())
				return L.calls.size() > R.calls.size();
			if (LRemoved != RRemoved) return LRemoved > RRemoved;
			return Left.first < Right.first;
		});

	for (auto &Entry : OrderedGroups) {
		SpecializationGroup &Group = *Entry.second;
		std::size_t CloneCost = FunctionSize;
		for (const auto &Specialized : Group.specializedArgs)
			CloneCost += Specialized.second.pointerSteps.size();
		if (CreatedCount >= MaxSpecializationsPerFunction) break;
		if (ClonedInstructions + CloneCost >
			MaxClonedInstructionsPerFunction) continue;

		// A one-off clone must either be small or remove multiple parameters.
		// Equality-only contexts get a wider bounded window: unlike a literal,
		// they cannot be exposed by ordinary constant propagation.
		const std::size_t RemovedParamCount =
			Group.specializedArgs.size() + Group.equalArguments.size();
		const std::size_t SingleCallLimit = Group.equalArguments.empty()
			? MaxSingleCallCloneInstructions
			: MaxSingleCallEqualityCloneInstructions;
		if (Group.calls.size() == 1 &&
			FunctionSize > SingleCallLimit && RemovedParamCount < 2) {
			continue;
		}

		std::string Name = makeUniqueSpecializedName(M, *F, NameOrdinal);
		auto Clone = cloneSpecializedFunction(
			*F, Name, Group.specializedArgs, Group.equalArguments);
		if (!Clone) continue;
		Function *CloneValue = Clone.get();

		std::vector<std::pair<CallInst *, std::unique_ptr<CallInst>>>
			ReplacementCalls;
		bool CallsAreValid = true;
		for (CallInst *CI : Group.calls) {
			if (!CI || !CI->getParent() || CI->getDirectCallee() != F ||
				CI->getArgCount() != F->getParamCount()) {
				CallsAreValid = false;
				break;
			}

			std::vector<bool> RemovedArgs(CI->getArgCount(), false);
			for (const auto &Specialized : Group.specializedArgs) {
				const unsigned Index = Specialized.first;
				if (Index >= RemovedArgs.size() || RemovedArgs[Index]) {
					CallsAreValid = false;
					break;
				}
				Param *Parameter = F->getParam(Index);
				auto Current = describeSpecializationValue(
					CI->getArg(Index), Parameter);
				if (!Current || Current->key != Specialized.second.key) {
					CallsAreValid = false;
					break;
				}
				RemovedArgs[Index] = true;
			}
			if (!CallsAreValid) break;
			for (const ParameterEquality &Equality : Group.equalArguments) {
				const unsigned Representative = Equality.representativeIndex;
				const unsigned Redundant = Equality.redundantIndex;
				if (Representative >= CI->getArgCount() ||
					Redundant >= CI->getArgCount() ||
					Representative >= Redundant || RemovedArgs[Redundant]) {
					CallsAreValid = false;
					break;
				}
				Value *RepresentativeArg = CI->getArg(Representative);
				Value *RedundantArg = CI->getArg(Redundant);
				if (!RepresentativeArg || RepresentativeArg->isUndef() ||
					RepresentativeArg != RedundantArg) {
					CallsAreValid = false;
					break;
				}
				RemovedArgs[Redundant] = true;
			}
			if (!CallsAreValid) break;

			std::vector<Value *> RemainingArgs;
			RemainingArgs.reserve(CloneValue->getParamCount());
			for (unsigned Index = 0; Index < CI->getArgCount(); ++Index) {
				if (RemovedArgs[Index]) continue;
				RemainingArgs.push_back(CI->getArg(Index));
			}
			if (RemainingArgs.size() != CloneValue->getParamCount()) {
				CallsAreValid = false;
				break;
			}

			try {
				auto NewCall = std::make_unique<CallInst>(
					CloneValue, RemainingArgs);
				NewCall->setIdent(CI->getIdent());
				NewCall->setTailCall(CI->isTailCall());
				ReplacementCalls.emplace_back(CI, std::move(NewCall));
			} catch (const std::invalid_argument &) {
				CallsAreValid = false;
				break;
			}
		}
		if (!CallsAreValid || ReplacementCalls.size() != Group.calls.size()) {
			ReplacementCalls.clear();
			Clone->dropAllReferences();
			continue;
		}

		bool CallsInserted = true;
		std::vector<CallInst *> InsertedCalls;
		InsertedCalls.reserve(ReplacementCalls.size());
		for (auto &Replacement : ReplacementCalls) {
			CallInst *OldCall = Replacement.first;
			CallInst *NewCall = Replacement.second.get();
			if (!Instruction::insertBefore(OldCall, NewCall)) {
				CallsInserted = false;
				break;
			}
			InsertedCalls.push_back(NewCall);
		}
		if (!CallsInserted) {
			for (CallInst *Inserted : InsertedCalls)
				if (Inserted && Inserted->getParent()) Inserted->eraseFromParent();
			ReplacementCalls.clear();
			Clone->dropAllReferences();
			continue;
		}

		M.getFunctions().push_back(std::move(Clone));
		for (auto &Replacement : ReplacementCalls) {
			CallInst *OldCall = Replacement.first;
			CallInst *NewCall = Replacement.second.release();
			OldCall->replaceAllUsesWith(NewCall);
			delete OldCall;
		}

		++CreatedCount;
		ClonedInstructions += CloneCost;
		Changed = true;
	}
	return Changed;
}

//优化2：冗余参数合并
/*
define i32 @foo(i32 %a, i32 %b) {
entry:
  %x = add i32 %a, %b
  ret i32 %x
}
call i32 @foo(i32 %v1, i32 %v1)
call i32 @foo(i32 %v2, i32 %v2)

->

define i32 @foo(i32 %a, i32 %b) {
entry:
  %x = add i32 %a, %a
  ret i32 %x
}
*/
bool DAEPass::tryRedundantArgMerge(
	Function *F, const std::vector<CallInst *> &CallSites) {
	if (!F || !F->hasOnlyDirectCallUses()) return false;
	if (F->getParamCount() < 2) return false;
	if (CallSites.empty()) return false;

	bool Changed = false;

	for (unsigned i = 0; i < F->getParamCount(); ++i) {
        //遍历该函数中每一个形参Pi
		Value *Pi = F->getParam(i);
		if (!Pi) continue;

		for (unsigned j = i + 1; j < F->getParamCount(); ++j) {
            //遍历该函数中每一个形参Pj，且j>i，避免重复比较
			Value *Pj = F->getParam(j);
			if (!Pj || !Pi->getIRType() || !Pj->getIRType() ||
				!Pi->getIRType()->hasSameShape(*Pj->getIRType())) continue;

			bool AlwaysEqual = true;
            //遍历调用该函数的每一个Call指令，检查传入Pi和Pj的实参是否总是相等
			for (CallInst *CI : CallSites) {
				if (!CI || CI->getArgCount() != F->getParamCount()) {
					AlwaysEqual = false;
					break;
				}
				Value *IArg = CI->getArg(static_cast<unsigned>(i));
				Value *JArg = CI->getArg(static_cast<unsigned>(j));
				if (!IArg || !JArg || IArg->isUndef() || JArg->isUndef()) {
					AlwaysEqual = false;
					break;
				}
				if (IArg != JArg) {
					AlwaysEqual = false;
					break;
				}
			}
            //如果Pi和Pj总是传入相同的实参，则将Pj在函数体内的使用替换为Pi，从而合并冗余参数
			if (AlwaysEqual) {
				Pj->replaceAllUsesWith(Pi);
				Changed = true;
			}
		}
	}

	return Changed;
}

const std::vector<bool> *DAEPass::getLiveMask(const ParamMaskMap &Masks, Function *F) {
	auto It = Masks.find(F);
	if (It == Masks.end()) return nullptr;
	return &It->second;
}

bool DAEPass::isParamLive(
	Function *Owner,
	Value *Param,
	std::unordered_map<Value *, ParamLiveness> &Memo,
	std::unordered_set<Value *> &Visiting,
	const ParamMaskMap &ExistingMasks) {
	if (!Owner || !Param) return true;

	if (const std::vector<bool> *KnownMask = getLiveMask(ExistingMasks, Owner)) {
			const int Idx = Owner->getParamIndex(Param);
		if (Idx >= 0 && static_cast<size_t>(Idx) < KnownMask->size()) {
			return (*KnownMask)[static_cast<size_t>(Idx)];
		}
	}

	auto It = Memo.find(Param);
	if (It != Memo.end()) {
		if (It->second == ParamLiveness::Live) return true;
		if (It->second == ParamLiveness::Dead) return false;
		if (It->second == ParamLiveness::InProgress) return false;
	}

	ScopedActiveMark VisitingMark(Visiting, Param);
	if (!VisitingMark.inserted()) {
		Memo[Param] = ParamLiveness::Dead;
		return false;
	}
	Memo[Param] = ParamLiveness::InProgress;

	bool Live = false;
	if (Param->hasUses()) {
		for (Use *U : Param->getUses()) {

			User *UseUser = U->getUser();
			if (!UseUser) {
				Live = true;
				break;
			}

			auto *CI = dynamic_cast<CallInst *>(UseUser);
			if (!CI) {
				Live = true;
				break;
			}

			int ArgIdx = CI->getArgIndex(U);
			if (ArgIdx < 0) {
				Live = true;
				break;
			}

			Function *Callee = CI->getDirectCallee();
			if (!Callee || Callee->hasReservedSignature()) {
				Live = true;
				break;
			}

			auto &CalleeParams = Callee->getParams();
			const size_t CalleeParamIdx = static_cast<size_t>(ArgIdx);
			if (CalleeParamIdx >= CalleeParams.size()) {
				Live = true;
				break;
			}

			Value *CalleeParam = CalleeParams[CalleeParamIdx].get();
			if (!CalleeParam) {
				Live = true;
				break;
			}

			if (isParamLive(Callee, CalleeParam, Memo, Visiting, ExistingMasks)) {
				Live = true;
				break;
			}
		}
	}

	Memo[Param] = Live ? ParamLiveness::Live : ParamLiveness::Dead;
	return Live;
}

bool DAEPass::rewriteFunctionWithDeadArgs(Module &M,
										  Function *OldF,
										  const std::vector<bool> &LiveMask) {
	(void)M;
	if (!OldF || !OldF->hasOnlyDirectCallUses()) return false;
	auto &OldParams = OldF->getParams();
	if (OldParams.empty()) return false;

	std::vector<size_t> DeadParamIdx;
	DeadParamIdx.reserve(OldParams.size());
	for (size_t i = 0; i < OldParams.size(); ++i) {
		const bool IsLive = (i < LiveMask.size() ? LiveMask[i] : false);
		if (!IsLive) DeadParamIdx.push_back(i);
	}
	if (DeadParamIdx.empty()) return false;

	// 先把死参在函数体内的使用替换成 undef，避免后续 erase 触发危险析构链
	for (size_t Idx : DeadParamIdx) {
		if (Idx >= OldParams.size() || !OldParams[Idx]) continue;
		Value *OldParam = OldParams[Idx].get();
		OldParam->replaceAllUsesWith(UndefValue::NewUndefValue(OldParam->getIRType()));
	}

	std::vector<CallInst *> CallSites = OldF->getDirectCallSites();

	for (CallInst *OldCI : CallSites) {
		if (!OldCI) continue;

		// 倒序删除，避免实参索引偏移。
		for (size_t k = DeadParamIdx.size(); k > 0; --k) {
			const size_t ParamIdx = DeadParamIdx[k - 1];
			OldCI->eraseArg(static_cast<unsigned>(ParamIdx));
		}
	}

	// 原地收缩函数形参表（倒序删除）
	for (size_t k = DeadParamIdx.size(); k > 0; --k) {
		const size_t ParamIdx = DeadParamIdx[k - 1];
		OldF->eraseParam(ParamIdx);
	}

	return true;
}

bool DAEPass::eliminateDeadArguments(Module &M) {
	std::vector<Function *> Snapshot;
	for (auto &FPtr : M.getFunctions()) {
		if (!FPtr) continue;
		Snapshot.push_back(FPtr.get());
	}

	ParamMaskMap DeadArgMaskByFunc;
	for (Function *F : Snapshot) {
		if (!F || F->hasReservedSignature() ||
			!F->hasOnlyDirectCallUses()) {
			continue;
		}
		auto &Params = F->getParams();
		if (Params.empty()) continue;

		std::unordered_map<Value *, ParamLiveness> Memo;
		std::unordered_set<Value *> Visiting;
		std::vector<bool> LiveMask(Params.size(), true);

		bool HasDeadArg = false;
		for (size_t i = 0; i < Params.size(); ++i) {
			Value *P = Params[i].get();
			const bool IsLive = isParamLive(F, P, Memo, Visiting, DeadArgMaskByFunc);
			LiveMask[i] = IsLive;
			if (!IsLive) HasDeadArg = true;
		}

		if (HasDeadArg) {
			DeadArgMaskByFunc[F] = std::move(LiveMask);
		}
	}

	bool Changed = false;
	std::vector<std::pair<Function *, std::vector<bool>>> RewritePlan;
	RewritePlan.reserve(DeadArgMaskByFunc.size());
	for (auto &KV : DeadArgMaskByFunc) {
		RewritePlan.push_back({KV.first, KV.second});
	}

	for (auto &Entry : RewritePlan) {
		Function *OldF = Entry.first;
		if (!OldF || OldF->hasReservedSignature() ||
			!OldF->hasOnlyDirectCallUses()) {
			continue;
		}
		const std::vector<bool> &LiveMask = Entry.second;
		if (rewriteFunctionWithDeadArgs(M, OldF, LiveMask)) {
			Changed = true;
		}
	}
	return Changed;
}

bool DAEPass::DeadReturnTraversalBudget::tryVisit(unsigned Depth) {
	if (Depth > MaxDepth || VisitedNodes >= MaxNodes) return false;
	++VisitedNodes;
	return true;
}

bool DAEPass::collectDeadReturnFlow(
	Value *Root, Function *Owner,
	std::unordered_set<Instruction *> &DeadFlow,
	std::unordered_set<Value *> &Visiting) {
	if (!Root || !Owner) return false;

	struct WorkItem {
		Value *FlowValue = nullptr;
		unsigned Depth = 0;
	};
	DeadReturnTraversalBudget Budget;
	if (!Budget.tryVisit(0)) return false;
	std::vector<WorkItem> WorkList{{Root, 0}};
	Visiting.clear();
	Visiting.insert(Root);

	// 透明返回值图可能包含PHI环和长链。显式工作表既消除递归栈风险，
	// 又让汇合节点只检查一次；预算耗尽时整个dead-return证明保守失败。
	while (!WorkList.empty()) {
		WorkItem Current = WorkList.back();
		WorkList.pop_back();
		if (!Current.FlowValue) return false;

		for (Use *UseSite : Current.FlowValue->getUses()) {
			auto *UserInstruction = dynamic_cast<Instruction *>(
				UseSite ? UseSite->getUser() : nullptr);
			if (!UserInstruction ||
				UserInstruction->getParentFunction() != Owner)
				return false;
			if (auto *Return = dynamic_cast<RetInst *>(UserInstruction)) {
				if (Return->getReturnValue() != Current.FlowValue ||
					!Budget.tryVisit(Current.Depth + 1))
					return false;
				continue;
			}

			bool IsTransparentFlow = dynamic_cast<PhiInst *>(UserInstruction);
			if (auto *Select = dynamic_cast<SelectInst *>(UserInstruction)) {
				const int OperandIndex = Select->getUserIndex(UseSite);
				IsTransparentFlow = OperandIndex == 1 || OperandIndex == 2;
			}
			if (UserInstruction->instType == Instruction::BITCAST ||
				UserInstruction->instType == Instruction::ZEXT ||
				UserInstruction->instType == Instruction::SEXT ||
				UserInstruction->instType == Instruction::TRUNC) {
				IsTransparentFlow =
					UserInstruction->getUnaryOperand() == Current.FlowValue;
			}
			if (!IsTransparentFlow) return false;
			DeadFlow.insert(UserInstruction);
			if (!Visiting.insert(UserInstruction).second) continue;
			if (Current.Depth >= DeadReturnTraversalBudget::MaxDepth ||
				!Budget.tryVisit(Current.Depth + 1))
				return false;
			WorkList.push_back({UserInstruction, Current.Depth + 1});
		}
	}
	return true;
}

std::unique_ptr<Function> DAEPass::cloneFunctionWithVoidReturn(
	Function &Source,
	const std::unordered_set<Instruction *> &DeadReturnFlow) {
	auto Clone = std::make_unique<Function>(
		VOIDType::NewVoid(), Source.getIdent());
	BasicBlock *Staging = Clone->front();
	if (!Staging) return nullptr;
	auto Fail = [&Clone]() -> std::unique_ptr<Function> {
		Clone->dropAllReferences();
		return nullptr;
	};

	ValueToValueMap InitialMap;
	for (const auto &OldParameterOwner : Source.getParams()) {
		Param *OldParameter = OldParameterOwner.get();
		if (!OldParameter || !OldParameter->getIRType()) return Fail();
		auto NewParameter = std::make_unique<Param>(
			OldParameter->getIRType(), OldParameter->getIdent());
		Param *NewParameterValue = NewParameter.get();
		Clone->getParams().push_back(std::move(NewParameter));
		InitialMap[OldParameter] = NewParameterValue;
	}

	std::vector<BasicBlock *> SourceBlocks;
	for (BasicBlock *Block : Source)
		if (Block) SourceBlocks.push_back(Block);
	if (SourceBlocks.empty()) return Fail();
	auto Region = cloneBasicBlockRegion(*Clone, SourceBlocks, InitialMap);
	if (!Region || !Region->getClonedBlock(Source.front())) return Fail();
	delete Staging;

	// 先让通用区域克隆保持旧自递归call的返回类型，避免返回专用PHI在构造
	// 阶段接收到void。块和值映射完成后，再整体删掉返回专用数据流。
	std::vector<RetInst *> Returns;
	for (BasicBlock *Block : *Clone)
		for (Instruction *I : *Block)
			if (auto *Return = dynamic_cast<RetInst *>(I))
				Returns.push_back(Return);
	for (RetInst *Return : Returns) {
		auto *VoidReturn = new RetInst();
		if (!Return->insertBefore(VoidReturn)) {
			delete VoidReturn;
			return Fail();
		}
		Return->eraseFromParent();
		delete Return;
	}

	std::vector<Instruction *> ClonedDeadFlow;
	for (Instruction *OldInstruction : DeadReturnFlow) {
		Instruction *NewInstruction =
			Region->getClonedInstruction(OldInstruction);
		if (!NewInstruction) return Fail();
		ClonedDeadFlow.push_back(NewInstruction);
	}
	// PHI可能构成环，批量断开操作数后再删除，不能依赖某个拓扑顺序。
	for (Instruction *InstructionToDelete : ClonedDeadFlow)
		InstructionToDelete->clearOperands();
	for (Instruction *InstructionToDelete : ClonedDeadFlow) {
		if (InstructionToDelete->hasUses()) return Fail();
		InstructionToDelete->eraseFromParent();
		delete InstructionToDelete;
	}

	std::vector<CallInst *> SourceSelfCalls;
	for (CallInst *Call : Source.getDirectCallSites())
		if (Call && Call->getParentFunction() == &Source)
			SourceSelfCalls.push_back(Call);
	try {
		for (CallInst *OldSelfCall : SourceSelfCalls) {
			auto *ClonedOldCall = dynamic_cast<CallInst *>(
				Region->getClonedInstruction(OldSelfCall));
			if (!ClonedOldCall || ClonedOldCall->hasUses()) return Fail();
			std::vector<Value *> Arguments = ClonedOldCall->getArgs();
			auto *NewSelfCall = new CallInst(Clone.get(), Arguments);
			NewSelfCall->setTailCall(ClonedOldCall->isTailCall());
			if (!ClonedOldCall->insertBefore(NewSelfCall)) {
				delete NewSelfCall;
				return Fail();
			}
			ClonedOldCall->eraseFromParent();
			delete ClonedOldCall;
		}
	} catch (const std::invalid_argument &) {
		return Fail();
	}
	return Clone;
}

bool DAEPass::rewriteFunctionWithDeadReturn(
	Module &M, std::size_t FunctionIndex) {
	auto &Functions = M.getFunctions();
	if (FunctionIndex >= Functions.size() || !Functions[FunctionIndex])
		return false;
	Function *OldFunction = Functions[FunctionIndex].get();
	if (!OldFunction || OldFunction->hasReservedSignature() ||
		!OldFunction->getIRType() || OldFunction->getIRType()->isVoid() ||
		!OldFunction->hasOnlyDirectCallUses())
		return false;

	std::vector<CallInst *> Calls = OldFunction->getDirectCallSites();
	if (Calls.empty()) return false;
	std::unordered_set<Instruction *> DeadReturnFlow;
	for (CallInst *Call : Calls) {
		if (!Call || !Call->getParent() ||
			Call->getDirectCallee() != OldFunction)
			return false;
		if (Call->getParentFunction() != OldFunction) {
			// 跨函数返回值传递属于多节点SCC问题，首版只接受外部调用完全忽略结果。
			if (Call->hasUses()) return false;
			continue;
		}
		std::unordered_set<Value *> Visiting;
		if (!collectDeadReturnFlow(
				Call, OldFunction, DeadReturnFlow, Visiting))
			return false;
	}

	std::unique_ptr<Function> NewFunctionOwner =
		cloneFunctionWithVoidReturn(*OldFunction, DeadReturnFlow);
	if (!NewFunctionOwner) return false;
	Function *NewFunction = NewFunctionOwner.get();

	struct ReplacementCall {
		CallInst *Old = nullptr;
		std::unique_ptr<CallInst> New;
	};
	std::vector<ReplacementCall> Replacements;
	try {
		for (CallInst *Call : Calls) {
			// 原函数体内的自递归调用已经由克隆映射指向新函数，旧调用随旧函数销毁。
			if (Call->getParentFunction() == OldFunction) continue;
			std::vector<Value *> Arguments = Call->getArgs();
			auto NewCall = std::make_unique<CallInst>(
				NewFunction, Arguments);
			NewCall->setTailCall(Call->isTailCall());
			Replacements.push_back({Call, std::move(NewCall)});
		}
	} catch (const std::invalid_argument &) {
		return false;
	}

	std::vector<CallInst *> InsertedCalls;
	for (ReplacementCall &Replacement : Replacements) {
		Instruction *NewCall = Replacement.New.release();
		if (!Replacement.Old->insertBefore(NewCall)) {
			delete NewCall;
			for (CallInst *Inserted : InsertedCalls) {
				Inserted->eraseFromParent();
				delete Inserted;
			}
			return false;
		}
		InsertedCalls.push_back(static_cast<CallInst *>(NewCall));
	}
	for (ReplacementCall &Replacement : Replacements) {
		Replacement.Old->eraseFromParent();
		delete Replacement.Old;
	}
	Functions[FunctionIndex] = std::move(NewFunctionOwner);
	return true;
}

bool DAEPass::eliminateDeadReturns(Module &M) {
	bool Changed = false;
	// 每个候选在提交前完整构造新函数和全部外部call；一次改写失败不会改变签名。
	for (std::size_t Index = 0; Index < M.getFunctions().size(); ++Index)
		Changed |= rewriteFunctionWithDeadReturn(M, Index);
	return Changed;
}

PreservationStatus DAEPass::run(Module &M, ModuleAnalysisPassManager &MAM) {
	(void)MAM;
	bool Changed = false;
	const bool EnableRISCVDeadReturn =
		TargetInfo::get().getArch() == TargetArch::RISCV64;
	std::vector<Function *> Snapshot;
	for (auto &FPtr : M.getFunctions()) {
		if (FPtr) Snapshot.push_back(FPtr.get());
	}

	// 先处理所有调用点都共享的值。这一步不克隆函数，随后由死参数
	// 消除统一收缩原函数及全部直接调用点的签名。
	for (Function *F : Snapshot) {
		if (!F || F->hasReservedSignature()) continue;
		std::vector<CallInst *> CallSites = F->getDirectCallSites();
		if (CallSites.empty()) continue;
		Changed |= tryConstantSpecialization(F, CallSites);
		Changed |= tryRedundantArgMerge(F, CallSites);
	}
	Changed |= eliminateDeadArguments(M);
	if (EnableRISCVDeadReturn) Changed |= eliminateDeadReturns(M);

	// 对剩余调用按局部常量/指针事实和实参等价类分组。等价关系仅在
	// 对应副本中成立，不会错误地改写仍需通用语义的原函数。
	// 只遍历此刻已有函数的快照，避免同一轮继续特化刚创建的副本。
	std::vector<Function *> GroupCandidates;
	for (auto &FPtr : M.getFunctions()) {
		if (FPtr) GroupCandidates.push_back(FPtr.get());
	}
	for (Function *F : GroupCandidates) {
		Changed |= tryGroupedSpecialization(M, F);
	}

	// 副本已直接省略特化形参；再做一次传递闭包，可继续删除因常量
	// 替换或跨函数纯转发而变死的其他形参。
	Changed |= eliminateDeadArguments(M);
	if (EnableRISCVDeadReturn) Changed |= eliminateDeadReturns(M);

	return Changed ? PreservationStatus::none() : PreservationStatus::all();
}
