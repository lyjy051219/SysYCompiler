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
	// 1.1 若 main 的调用闭包能回到 main，把全局状态改成单次栈对象会改变重入语义。
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

bool Global2LocalPass::isSafeScalarUseInMain(Use *U, Variable *GV) {
	// 2.1 标量全局只允许作为直接 load/store 地址，不能把地址本身存出或参与其他运算。
	if (!U || !GV) return false;
	auto *UserI = dynamic_cast<Instruction *>(U->getUser());
	if (!UserI) return false;

	if (auto *LI = dynamic_cast<LoadInst *>(UserI)) {
		return LI->getPointerOperand() == GV;
	}

	if (auto *SI = dynamic_cast<StoreInst *>(UserI)) {

		return SI->getPointerOperand() == GV;
	}

	return false;
}

PreservationStatus Global2LocalPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	// 1. 该函数 pass 仅在活动模块的 main 上执行，并先用直接调用图排除递归重入。
	Module *M = Module::getActiveModule();
	if (!M) return PreservationStatus::all();

	Function *Main = M->getMainFunction();
	if (!Main || Main != &F || !Main->front()) return PreservationStatus::all();

	auto CallGraph = M->buildDirectCallGraph();

	if (isRecursiveFrom(Main, CallGraph))
		return PreservationStatus::all();

	auto &LI = FAM.getResult<LoopAnalysis>(F);
	auto ReachableFromMain =
		Module::collectReachableFunctions(Main, CallGraph);

	std::vector<Global2LocalCandidateInfo> WorkList;

	// 2. 扫描小型可变全局：main 可达的其他函数不得引用候选；数组地址不得进入
	// memcpy；标量 use 必须通过直接访问检查并遵守保守/宽松模式的循环限制。
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

		// 2.2 局部化只改变对象存储表示；timer 不读取 SysY 程序对象的地址，
		// 因此不把计时边界作为候选过滤条件。

		if (!CanPromote || MainUses.empty()) continue;

		WorkList.push_back(
			Global2LocalCandidateInfo{
				GV, ElemTy, std::move(MainUses)});
	}

	if (WorkList.empty()) return PreservationStatus::all();

	bool Changed = false;
	Instruction *AllocaInsertTail = nullptr;

	// 3. 为候选在 main 入口 PHI 区后连续插入 alloca，并紧随其后建立初始化。
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

		// 3.1 数组用全尺寸 memcpy 从原全局初值复制；标量用显式初值或类型零值 store。
		if (C.ElemTy->isArray()) {
			const int ByteSize = static_cast<int>(C.ElemTy->getSize());
			std::vector<Value *> Args;
			Args.push_back(Alloca);

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

		// 3.2 仅替换收集阶段确认的 main use；其他不可达函数中的引用仍指向原全局。
		for (Use *U : C.MainUses) {
			if (!U) continue;
			U->replaceUseeWith(Alloca);
		}

		Changed = true;
	}

	if (!Changed) return PreservationStatus::all();

	// 4. CFG 未改，保留支配/后支配/循环；地址空间和内存访问变化使其余分析失效。
	PreservationStatus PA = PreservationStatus::none();
	PA.keep<LoopAnalysis>();
	PA.keep<DominantAnalysis>();
	PA.keep<PostDominantAnalysis>();
	return PA;
}
