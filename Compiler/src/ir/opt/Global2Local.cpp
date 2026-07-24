#include "ir/opt/Global2Local.hpp"

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include "lib/TypeSystem.hpp"

#include <algorithm>
#include <utility>

Global2LocalPass::Global2LocalPass(bool RelaxScalarUses)
	: RelaxScalarUses(RelaxScalarUses) {}

bool Global2LocalPass::isRecursiveFrom(
	Function *Root, const FunctionCallGraph &CallGraph) {
	if (!Root) return false;

	auto RootIt = CallGraph.find(Root);
	if (RootIt == CallGraph.end()) return false;
	return isReachableFromRootsCustomSucc<Function>(
		RootIt->second, Root,
		[&CallGraph](Function *Current) -> std::vector<Function *> {
			auto It = CallGraph.find(Current);
			return It == CallGraph.end() ? std::vector<Function *>{}
			                            : It->second;
		});
}

// For scalar globals, only allow very simple load/store users in main.
// Complex pointer/address-taken patterns are deferred to avoid downstream IR issues.
bool Global2LocalPass::isSafeScalarUseInMain(Use *U, Variable *GV) {
	if (!U || !GV) return false;
	auto *UserI = dynamic_cast<Instruction *>(U->getUser());
	if (!UserI) return false;

	if (auto *LI = dynamic_cast<LoadInst *>(UserI)) {
		return LI->getPointerOperand() == GV;
	}

	if (auto *SI = dynamic_cast<StoreInst *>(UserI)) {
		// Only allow direct stores where GV is the destination pointer.
		return SI->getPointerOperand() == GV;
	}

	return false;
}

PreservationStatus Global2LocalPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	Module *M = Module::getActiveModule();
	if (!M) return PreservationStatus::all();

	Function *Main = M->getMainFunction();
	if (!Main || Main != &F || !Main->front()) return PreservationStatus::all();

	auto CallGraph = M->buildDirectCallGraph();
	// A global has one shared instance across recursive invocations of main;
	// replacing it with an alloca would create one instance per stack frame.
	if (isRecursiveFrom(Main, CallGraph))
		return PreservationStatus::all();

	auto &LI = FAM.getResult<LoopAnalysis>(F);
	auto ReachableFromMain =
		Module::collectReachableFunctions(Main, CallGraph);

	std::vector<Global2LocalCandidateInfo> WorkList;

	for (auto &GVPtr : M->getGlobalVar()) {
		auto *GV = GVPtr.get();
		if (!GV) continue;
		if (GV->vartag != Variable::GlobalVariable) continue;
		if (!GV->hasUses()) continue;

		auto ElemTy = IRType::getPointeeType(GV->getIRType());
		if (!ElemTy) continue;
		if (ElemTy->getSize() > RISCVThreshold) continue;
		const bool IsArray = ElemTy->isArray();

		bool CanPromote = true;
		std::vector<Use *> MainUses;
		bool ScalarHasStore = false;
		bool ScalarHasLoopMemUse = false;
		bool ScalarAllUsesInEntry = true;
		BasicBlock *MainEntry = Main->front();

		for (Use *U : GV->getUses()) {

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

			auto *MemcpyCall = dynamic_cast<CallInst *>(U ? U->getUser() : nullptr);
			if (MemcpyCall && MemcpyCall->isMemcpyLike() &&
				MemcpyCall->getArgIndex(U) >= 0) {
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
				if (!RelaxScalarUses) {
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

					if (SI && SI->getPointerOperand() == GV) {
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
			Global2LocalCandidateInfo{
				GV, ElemTy, std::move(MainUses)});
	}

	if (WorkList.empty()) return PreservationStatus::all();

	bool Changed = false;
	Instruction *AllocaInsertTail = nullptr;

	for (auto &C : WorkList) {
		auto *Alloca = new AllocaInst(C.ElemTy);
		Alloca->setIdent(C.GV->getIdent() + ".l2g");

		Instruction *InsertPos = nullptr;
		BasicBlock *Entry = Main->front();
		if (AllocaInsertTail && Entry && AllocaInsertTail->getParent() == Entry &&
			AllocaInsertTail->insertAfter(Alloca)) {
			InsertPos = Alloca;
		}
		if (!InsertPos && Entry && Entry->insertAfterPhiNodes(Alloca)) {
			InsertPos = Alloca;
		}
		if (!InsertPos) {
			delete Alloca;
			continue;
		}
		AllocaInsertTail = InsertPos;

		Value *Init = C.GV->getUsee();
		Instruction *LastInitInst = InsertPos;

		if (C.ElemTy->isArray()) {
			const int ByteSize = static_cast<int>(C.ElemTy->getSize());
			std::vector<Value *> Args;
			Args.push_back(Alloca);
			// 直接从原全局变量拷贝：有初始化值时复制初始化数据，无初始化值时复制 zeroinitializer。
			Args.push_back(C.GV);
			Args.push_back(ConstInt::newConstInt(ByteSize));
			Args.push_back(ConstBool::newConstBool(false));

			auto *MemcpyCall = new CallInst(
				BuildInFunction::genBuildInFunction(
					BuildInFunction::getDefaultMemcpyName()),
				Args);
			LastInitInst->insertAfter(MemcpyCall);
			LastInitInst = MemcpyCall;
		} else {
			if (!Init) {
				if (C.ElemTy->isScalar()) {
					Init = Value::makeZeroForType(C.ElemTy);
				} else {
					Init = UndefValue::NewUndefValue(C.ElemTy);
				}
			}

			auto *SI = new StoreInst(Init, Alloca);
			LastInitInst->insertAfter(SI);
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
