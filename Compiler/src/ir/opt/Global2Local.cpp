#include "ir/opt/Global2Local.hpp"

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "lib/CFG.hpp"
#include "lib/TypeSystem.hpp"

#include <algorithm>
#include <queue>
#include <utility>

namespace {

struct CandidateInfo {
	Variable *GV = nullptr;
	std::shared_ptr<IRType> ElemTy;
	std::vector<Use *> MainUses;
	bool HasNonEntryStore = false;
};

static Instruction *insertAllocaAfterEntryPhis(Function *F, AllocaInst *AI) {
	if (!F || !AI) return nullptr;
	BasicBlock *Entry = F->front();
	if (!Entry) return nullptr;

	Instruction *lastPhi = nullptr;
	for (Instruction *I : *Entry) {
		if (dynamic_cast<PhiInst *>(I)) {
			lastPhi = I;
		} else {
			break;
		}
	}

	if (!Entry->front()) {
		Entry->push_back(AI);
		return AI;
	}

	if (!lastPhi) {
		auto it = Entry->begin();
		it.insertBefore(AI);
		return AI;
	}

	for (auto it = Entry->begin(); it != Entry->end(); ++it) {
		if (*it != lastPhi) continue;
		it.insertAfter(AI);
		return AI;
	}

	Entry->push_front(AI);
	return AI;
}

static Instruction *insertAllocaAfter(Function *F, AllocaInst *AI, Instruction *AfterI) {
	if (!F || !AI || !AfterI) return nullptr;
	BasicBlock *Entry = F->front();
	if (!Entry) return nullptr;
	for (auto it = Entry->begin(); it != Entry->end(); ++it) {
		if (*it != AfterI) continue;
		it.insertAfter(AI);
		return AI;
	}
	return nullptr;
}

static void insertInstAfter(BasicBlock *BB, Instruction *Anchor, Instruction *NewI) {
	if (!BB || !Anchor || !NewI) return;
	for (auto it = BB->begin(); it != BB->end(); ++it) {
		if (*it != Anchor) continue;
		it.insertAfter(NewI);
		return;
	}
	BB->push_back(NewI);
}

// For scalar globals, only allow very simple load/store users in main.
// Complex pointer/address-taken patterns are deferred to avoid downstream IR issues.
static bool isSafeScalarUseInMain(Use *U, Variable *GV) {
	if (!U || !GV) return false;
	auto *UserI = dynamic_cast<Instruction *>(U->getUser());
	if (!UserI) return false;

	if (auto *LI = dynamic_cast<LoadInst *>(UserI)) {
		return LI->getUserIndex(U) == 0;
	}

	if (auto *SI = dynamic_cast<StoreInst *>(UserI)) {
		// Only allow direct stores where GV is the destination pointer.
		return SI->getUserIndex(U) == 1;
	}

	return false;
}

} // namespace

bool Global2LocalPass::isMemcpyLikeCall(CallInst *CI) {
	if (!CI) return false;
	auto &UL = CI->getUserList();
	if (UL.empty() || !UL[0]) return false;

	Value *Callee = UL[0]->getValue();
	if (!Callee) return false;

	if (auto *BIF = dynamic_cast<BuildInFunction *>(Callee)) {
		const std::string Name = BIF->getIdent();
		return Name.find("memcpy") != std::string::npos;
	}

	if (auto *F = dynamic_cast<Function *>(Callee)) {
		const std::string Name = F->getIdent();
		return Name.find("memcpy") != std::string::npos;
	}

	return false;
}

bool Global2LocalPass::isMemcpyArgUse(Use *U) {
	if (!U) return false;
	auto *CI = dynamic_cast<CallInst *>(U->getUser());
	if (!CI || !isMemcpyLikeCall(CI)) return false;
	int OpIdx = CI->getUserIndex(U);
	return OpIdx >= 1;
}

std::unordered_map<Function *, std::vector<Function *>>
Global2LocalPass::buildCallGraph(Module &M) {
	std::unordered_map<Function *, std::vector<Function *>> G;

	for (auto &FuncPtr : M.getFunctions()) {
		Function *F = FuncPtr.get();
		if (!F) continue;

		auto &Adj = G[F];
		for (BasicBlock *BB : *F) {
			if (!BB) continue;
			for (Instruction *I : *BB) {
				auto *CI = dynamic_cast<CallInst *>(I);
				if (!CI) continue;

				auto &UL = CI->getUserList();
				if (UL.empty() || !UL[0]) continue;

				auto *Callee = dynamic_cast<Function *>(UL[0]->getValue());
				if (!Callee) continue;
				Adj.push_back(Callee);
			}
		}
	}

	return G;
}

std::unordered_set<Function *> Global2LocalPass::computeReachableFrom(
	Function *Entry,
	const std::unordered_map<Function *, std::vector<Function *>> &CallGraph) {
	std::unordered_set<Function *> Reachable;
	if (!Entry) return Reachable;

	std::queue<Function *> Q;
	Reachable.insert(Entry);
	Q.push(Entry);

	while (!Q.empty()) {
		Function *Cur = Q.front();
		Q.pop();

		auto It = CallGraph.find(Cur);
		if (It == CallGraph.end()) continue;

		for (Function *Nxt : It->second) {
			if (!Nxt) continue;
			if (Reachable.insert(Nxt).second) {
				Q.push(Nxt);
			}
		}
	}

	return Reachable;
}

PreservationStatus Global2LocalPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	Module *M = Module::getActiveModule();
	if (!M) return PreservationStatus::all();

	Function *Main = M->getMainFunction();
	if (!Main || Main != &F || !Main->front()) return PreservationStatus::all();
	auto &LI = FAM.getResult<LoopAnalysis>(F);

	auto CallGraph = buildCallGraph(*M);
	auto ReachableFromMain = computeReachableFrom(Main, CallGraph);

	std::vector<CandidateInfo> WorkList;

	for (auto &GVPtr : M->getGlobalVar()) {
		auto *GV = GVPtr.get();
		if (!GV) continue;
		if (GV->vartag != Variable::GlobalVariable) continue;
		if (GV->getUseList().is_empty()) continue;

		auto PtrTy = std::dynamic_pointer_cast<POINTERType>(GV->getIRType());
		if (!PtrTy) continue;

		auto ElemTy = PtrTy->getPointerType();
		if (!ElemTy) continue;
		if (ElemTy->getSize() > RISCVThreshold) continue;
		const bool IsArray = ElemTy->getEnum() == IR_ARRAY;

		bool CanPromote = true;
		std::vector<Use *> MainUses;
		bool ScalarHasStore = false;
		bool ScalarHasLoopMemUse = false;
		bool ScalarAllUsesInEntry = true;
		bool ScalarHasNonEntryStore = false;
		BasicBlock *MainEntry = Main->front();

		for (auto it = GV->getUseList().begin(); it != GV->getUseList().end(); ++it) {
			Use *U = *it;
			if (!U) continue;

			Function *UF = Instruction::getUserFunction(U);
			if (!UF) {
				CanPromote = false;
				break;
			}

			if (UF != Main) {
				if (ReachableFromMain.count(UF)) {
					CanPromote = false;
					break;
				}
				continue;
			}

			if (isMemcpyArgUse(U)) {
				CanPromote = false;
				break;
			}

			if (!IsArray) {
				if (!isSafeScalarUseInMain(U, GV)) {
					CanPromote = false;
					break;
				}

				auto *UserI = dynamic_cast<Instruction *>(U->getUser());
				auto *SI = dynamic_cast<StoreInst *>(UserI);
				if (RelaxScalarUses) {
					if (SI && SI->getUserIndex(U) == 1) {
						BasicBlock *SBB = SI->getParent();
						if (SBB && SBB != MainEntry) {
							ScalarHasNonEntryStore = true;
						}
					}
				} else {
					BasicBlock *UBB = UserI ? UserI->getParent() : nullptr;
					if (!UBB || UBB != MainEntry) {
						ScalarAllUsesInEntry = false;
					}
					if (UBB && LI.getLoopFor(UBB) != nullptr) {
						if (dynamic_cast<LoadInst *>(UserI) ||
							dynamic_cast<StoreInst *>(UserI)) {
							ScalarHasLoopMemUse = true;
						}
					}

					if (SI && SI->getUserIndex(U) == 1) {
						ScalarHasStore = true;
						BasicBlock *SBB = SI->getParent();
						if (!SBB || LI.getLoopFor(SBB) != nullptr) {
							CanPromote = false;
							break;
						}
					}
				}
			}

			MainUses.push_back(U);
		}

		if (!RelaxScalarUses && !IsArray && ScalarHasStore &&
			ScalarHasLoopMemUse) {
			CanPromote = false;
		}
		if (!RelaxScalarUses && !IsArray && ScalarHasStore &&
			!ScalarAllUsesInEntry) {
			CanPromote = false;
		}

		if (!CanPromote || MainUses.empty()) continue;

		WorkList.push_back(
			CandidateInfo{
				GV, ElemTy, std::move(MainUses), ScalarHasNonEntryStore});
	}

	if (RelaxScalarUses) {
		const size_t NonEntryStoredScalarCount = static_cast<size_t>(std::count_if(
			WorkList.begin(), WorkList.end(), [](const CandidateInfo &C) {
				return C.HasNonEntryStore;
			}));

		// The current late CFG cleanup cannot yet safely converge several
		// independently promoted loop-carried globals in deeply nested loops
		// (functional/28_while_test3 is the minimal suite witness).  Continue
		// promoting entry-written/loop-read globals, but defer all loop-written
		// scalars when more than one such state variable exists.
		if (NonEntryStoredScalarCount > 1) {
			WorkList.erase(
				std::remove_if(WorkList.begin(), WorkList.end(),
					[](const CandidateInfo &C) {
						return C.HasNonEntryStore;
					}),
				WorkList.end());
		}
	}

	if (WorkList.empty()) return PreservationStatus::all();

	bool Changed = false;
	BasicBlock *Entry = Main->front();
	Instruction *AllocaInsertTail = nullptr;

	for (auto &C : WorkList) {
		auto *Alloca = new AllocaInst(POINTERType::NewPointer(C.ElemTy));
		Alloca->setIdent(C.GV->getIdent() + ".l2g");

		Instruction *InsertPos = nullptr;
		if (AllocaInsertTail) {
			InsertPos = insertAllocaAfter(Main, Alloca, AllocaInsertTail);
		}
		if (!InsertPos) {
			InsertPos = insertAllocaAfterEntryPhis(Main, Alloca);
		}
		if (!InsertPos) {
			delete Alloca;
			continue;
		}
		AllocaInsertTail = InsertPos;

		Value *Init = C.GV->getUsee();
		Instruction *LastInitInst = InsertPos;

		if (C.ElemTy->getEnum() == IR_ARRAY) {
			const int ByteSize = static_cast<int>(C.ElemTy->getSize());
			std::vector<Value *> Args;
			Args.push_back(Alloca);
			// 直接从原全局变量拷贝：有初始化值时复制初始化数据，无初始化值时复制 zeroinitializer。
			Args.push_back(C.GV);
			Args.push_back(ConstInt::newConstInt(ByteSize));
			Args.push_back(ConstBool::newConstBool(false));

			auto *MemcpyCall =
				new CallInst(BuildInFunction::genBuildInFunction("llvm.memcpy.p0.p0.i32"), Args);
			insertInstAfter(Entry, LastInitInst, MemcpyCall);
			LastInitInst = MemcpyCall;
		} else {
			if (!Init) {
				if (C.ElemTy->getEnum() == IR_INT) {
					Init = ConstInt::newConstInt();
				} else if (C.ElemTy->getEnum() == IR_FLOAT) {
					Init = ConstFloat::newConstFloat();
				} else {
					Init = UndefValue::NewUndefValue(C.ElemTy);
				}
			}

			auto *SI = new StoreInst(Init, Alloca);
			insertInstAfter(Entry, LastInitInst, SI);
			LastInitInst = SI;
		}

		for (Use *U : C.MainUses) {
			if (!U) continue;
			U->replaceUseeWith(Alloca);
		}

		Changed = true;
	}

	if (!Changed) return PreservationStatus::all();

	PreservationStatus PA = PreservationStatus::none();
	PA.keep<LoopAnalysis>();
	PA.keep<DominantAnalysis>();
	PA.keep<PostDominantAnalysis>();
	return PA;
}
