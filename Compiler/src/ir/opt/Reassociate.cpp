// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/opt/Reassociate.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
//构造Pass并选择是否保留参与地址计算的整数表达式 
ReassociatePass::ReassociatePass(bool PreserveAddressArithmetic)
	: PreserveAddressArithmetic(PreserveAddressArithmetic) {}
//清理判定服务主流程扫描：无使用的纯计算无需再规范化；终结、PHI、store、
//有副作用调用必须保留，交由相应清理 pass 处理。
bool ReassociatePass::isDeadInstruction(
	Instruction *I, FunctionAnalysisPassManager &FAM) {
	if (!I) return false;
	if (I->countInstructionUses() > 0) return false;
	if (I->instType == Instruction::RET || I->instType == Instruction::BR ||
		I->instType == Instruction::PHI || I->instType == Instruction::STORE) {
		return false;
	}
	if (auto *CI = dynamic_cast<CallInst *>(I)) {
		if (dceCallHasSideEffect(CI, FAM)) return false;
	}
	return true;
}
//步骤 3.3：检查当前根是否已按规范操作数顺序构成同一棵链，避免等价重建
//在不动点迭代中反复产生新节点。
bool ReassociatePass::isAlreadyBuilt(
	Instruction *I, BinaryInst::Operation Op,
	const std::vector<Value *> &Ops) {
	if (!I) return false;
	if (Ops.size() < 2) return false;
	Value *Cur = I;
	for (size_t i = 0; i < Ops.size() - 1; ++i) {
		auto *BI = dynamic_cast<BinaryInst *>(Cur);
		if (!BI || BI->getOp() != Op) return false;
		Value *L = nullptr;
		Value *R = nullptr;
		if (!BI->getOperands(L, R)) return false;
		if (i == Ops.size() - 2) {
			return (L == Ops[i] && R == Ops[i + 1]) || (L == Ops[i + 1] && R == Ops[i]);
		}
		if (L != Ops[i]) return false;
		Cur = R;
	}
	return true;
}
//使用目标函数、分析管理器和地址保护策略构造求解器
ReassociateEngine::ReassociateEngine(
	Function &F, FunctionAnalysisPassManager &FAM,
	bool PreserveAddressArithmetic)
	: F(F), FAM(FAM),
	  PreserveAddressArithmetic(PreserveAddressArithmetic) {}
//计时调用既是内存顺序屏障，也是表达式求值顺序屏障。跨越该边界重结合会
//改变被计时区间实际执行的工作，即使最终数值完全相同也不能接受。
bool ReassociateEngine::crossesOrderingBarrier(
	Value *Operand, Instruction *Use) const {
	auto *Definition = dynamic_cast<Instruction *>(Operand);
	if (!Definition || !Definition->getParent()) return false;
	return hasOrderingBarrierOnPath(F, Definition, Use, FAM);
}
//主求解循环：先建立稳定排名，再按 RPO 扫描旧指令并处理新建指令待办项；
//任一局部变化都会触发下一轮，直到表达式形状与代数化简同时稳定。
bool ReassociateEngine::run() {
	initRankMap();
	bool AnyChanged = false;
	bool LocalChanged = false;
	do {
		LocalChanged = false;
		std::vector<BasicBlock *> Roots;
		if (F.front()) Roots.push_back(F.front());
		auto RPO = buildRPO<BasicBlock>(Roots, F);
		for (BasicBlock *BB : RPO) {
			if (!BB) continue;
			auto Insts = collectInstructions(*BB);
			for (Instruction *I : Insts) {
				if (!I || !I->getParent()) continue;
				if (ReassociatePass::isDeadInstruction(I, FAM)) {
					continue;
				}
				LocalChanged |= optimizeInst(I);
			}
		}
		LocalChanged |= processTodo();
		AnyChanged |= LocalChanged;
	} while (LocalChanged);
	return AnyChanged;
}
//步骤 1：按参数顺序和 CFG 的 RPO/块内顺序编号。可重结合节点不固定自身
//排名，使其展开后由叶子排名决定；其他定义保留稳定的程序次序。
void ReassociateEngine::initRankMap() {
	Ranks.clear();
	StableOrder.clear();
	NextStableOrder = 0;
	BlockBase.clear();
	BlockSeq.clear();
	Todo.clear();
	uint32_t Rank = 2;
	for (auto &P : F.getParams()) {
		Value *V = P.get();
		if (!V) continue;
		StableOrder[V] = ++NextStableOrder;
		Ranks[V] = ++Rank;
	}
	std::vector<BasicBlock *> Roots;
	if (F.front()) Roots.push_back(F.front());
	auto RPO = buildRPO<BasicBlock>(Roots, F);
	for (BasicBlock *BB : RPO) {
		if (!BB) continue;
		uint32_t Base = (++Rank) << 16;
		BlockBase[BB] = Base;
		uint32_t Seq = 0;
		forEachInstruction(*BB, [&](Instruction *I) {
			StableOrder[I] = ++NextStableOrder;
			bool ShouldSkip = false;
			if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
				BinaryInst::Operation Op = BI->getOp();
				ShouldSkip = I->getIRType() && I->getIRType()->isInt() &&
					BinaryInst::isReassociateOp(Op);
			}
			if (ShouldSkip) return;
			Ranks[I] = Base + (++Seq);
		});
		BlockSeq[BB] = Seq;
	}
}
//步骤 1.1：常量保持最低秩；除余结果视为最高延迟叶子，重建时靠近根部，
//避免长归约链把下一迭代依赖的高延迟操作埋入树底。
uint32_t ReassociateEngine::getRank(Value *V) const {
	if (!V) return 0;
	if (V && V->isConst()) return 1;
	if (auto *I = dynamic_cast<Instruction *>(V)) {
		switch (I->instType) {
			case Instruction::SDIV:
			case Instruction::UDIV:
			case Instruction::SREM:
			case Instruction::UREM:
				return UINT32_MAX;
			default:
				break;
		}
	}
	auto It = Ranks.find(V);
	if (It != Ranks.end()) return It->second;
	return 2;
}
//步骤 1.2：先比较语义排名，再用确定的 SSA 创建顺序打破平局；不比较对象
//地址，保证不同进程中的编译结果一致。
bool ReassociateEngine::rankComesBefore(Value *A, Value *B) const {
	uint32_t RA = getRank(A);
	uint32_t RB = getRank(B);
	if (RA != RB) return RA > RB;
	auto AI = StableOrder.find(A);
	auto BI = StableOrder.find(B);
	if (AI != StableOrder.end() && BI != StableOrder.end())
		return AI->second > BI->second;
	if (AI != StableOrder.end()) return true;
	if (BI != StableOrder.end()) return false;
	return false;
}
//步骤 1.3：为重建中新产生的定义延续稳定次序；重结合节点仍保持中性排名，
//防止下一轮排序因自身新编号而反向振荡。
void ReassociateEngine::stampRank(Instruction *I) {
	if (!I) return;
	if (!StableOrder.count(I))
		StableOrder[I] = ++NextStableOrder;
	if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
		if (I->getIRType() && I->getIRType()->isInt() &&
			BinaryInst::isReassociateOp(BI->getOp())) {
			Ranks.erase(I);
			return;
		}
	}
	BasicBlock *BB = I->getParent();
	if (!BB) return;
	uint32_t Base = 0;
	auto BI = BlockBase.find(BB);
	if (BI != BlockBase.end()) Base = BI->second;
	else {
		Base = 3u << 16;
		BlockBase[BB] = Base;
		BlockSeq[BB] = 0;
	}
	Ranks[I] = Base + (++BlockSeq[BB]);
}
//步骤 3.2：统一重访本轮新建或失去使用的节点，让局部改写在函数扫描之间
//也能及时发现后续机会。
void ReassociateEngine::pushTodo(Instruction *I) {
	if (!I || !I->getParent()) return;
	Todo.push(I);
}
//待办项按 LIFO 消费；已经删除或变死的节点跳过，其余重新走完整候选流程。
bool ReassociateEngine::processTodo() {
	bool Changed = false;
	while (!Todo.empty()) {
		Instruction *I = Todo.pop();
		if (!I || !I->getParent()) continue;
		if (ReassociatePass::isDeadInstruction(I, FAM)) continue;
		Changed |= optimizeInst(I);
	}
	return Changed;
}
//地址保护模式的合法性检查：沿整数宽度转换追踪使用链；只要最终进入 GEP，
//该表达式就留给 GEPCombine/LSR，避免重复规范化地址递推而扩大活跃区间。
bool ReassociateEngine::feedsAddressComputation(Instruction *Root) const {
	if (!Root) return false;
	std::vector<Instruction *> WorkList{Root};
	std::unordered_set<Instruction *> Visited;
	while (!WorkList.empty()) {
		Instruction *I = WorkList.back();
		WorkList.pop_back();
		if (!I || !Visited.insert(I).second) continue;
		for (Instruction *UserI : I->getInstructionUsers()) {
			if (dynamic_cast<GetElementPtrInst *>(UserI)) return true;
			//只穿过保持整数地址含义的宽度转换，不跨入任意算术表达式。
			if (UserI->instType == Instruction::ZEXT ||
				UserI->instType == Instruction::SEXT ||
				UserI->instType == Instruction::TRUNC) {
				WorkList.push_back(UserI);
			}
		}
	}
	return false;
}
//步骤 3.1：在旧根之前创建节点，以 O(1) 插入保持支配性，并立即加入排名和
//重访体系；所有树重建都经由此入口。
Instruction *ReassociateEngine::createBinaryBefore(
	Instruction *Pos, BinaryInst::Operation Op, Value *LHS, Value *RHS) {
	if (!Pos || !LHS || !RHS) return nullptr;
	BasicBlock *BB = Pos->getParent();
	if (!BB) return nullptr;
	auto *NI = new BinaryInst(LHS, Op, RHS);
	Pos->insertBefore(NI);
	stampRank(NI);
	pushTodo(NI);
	return NI;
}
//步骤 2 的乘法辅助：仅展开单用途乘法节点，防止复制共享子表达式。
bool ReassociateEngine::flattenMulFactors(
	Value *V, std::vector<Value *> &Factors) {
	auto *BI = dynamic_cast<BinaryInst *>(V);
	if (!BI || BI->getOp() != BinaryInst::MUL ||
		BI->countInstructionUses() != 1) {
		Factors.push_back(V);
		return false;
	}
	Value *L = nullptr;
	Value *R = nullptr;
	if (!BI->getOperands(L, R)) {
		Factors.push_back(V);
		return false;
	}
	flattenMulFactors(L, Factors);
	flattenMulFactors(R, Factors);
	return true;
}
//步骤 2 的构造辅助：按给定次序重建乘积，所有节点仍位于原根之前。
bool ReassociateEngine::buildBalancedProduct(
	Instruction *Root, std::vector<Value *> Terms, Value *&Out) {
	Out = nullptr;
	if (Terms.empty()) return false;
	if (Terms.size() == 1) {
		Out = Terms[0];
		return true;
	}
	Value *Acc = Terms.back();
	Terms.pop_back();
	while (!Terms.empty()) {
		Value *Next = Terms.back();
		Terms.pop_back();
		Instruction *NI = createBinaryBefore(Root, BinaryInst::MUL, Next, Acc);
		if (!NI) return false;
		Acc = NI;
	}
	Out = Acc;
	return true;
}
//步骤 2.5：把重复乘因子按指数分桶，并递归执行指数折半；相同指数的基先
//合并，再通过平方复用减少乘法链长度。
Value *ReassociateEngine::buildMulPowProduct(
	Instruction *Root,
	const std::vector<std::pair<Value *, int>> &PowList,
	bool &Changed) {
	if (PowList.empty()) return nullptr;
	std::unordered_map<int, std::vector<Value *>> ByPow;
	for (const auto &KV : PowList) {
		if (KV.second <= 0 || !KV.first) continue;
		ByPow[KV.second].push_back(KV.first);
	}
	std::vector<int> Powers;
	Powers.reserve(ByPow.size());
	for (const auto &KV : ByPow) Powers.push_back(KV.first);
	std::sort(Powers.begin(), Powers.end());
	std::vector<std::pair<Value *, int>> Normalized;
	for (int P : Powers) {
		auto &Bases = ByPow[P];
		if (Bases.size() == 1) {
			Normalized.push_back({Bases[0], P});
			continue;
		}
		Value *Merged = nullptr;
		if (!buildBalancedProduct(Root, Bases, Merged)) return nullptr;
		if (!Merged) return nullptr;
		Normalized.push_back({Merged, P});
		Changed = true;
	}
	std::vector<Value *> OuterProduct;
	std::vector<std::pair<Value *, int>> Halved;
	int MaxHalfPow = 0;
	for (auto &KV : Normalized) {
		Value *Base = KV.first;
		int Pow = KV.second;
		if (!Base || Pow <= 0) continue;
		if (Pow & 1) OuterProduct.push_back(Base);
		Pow >>= 1;
		if (Pow > 0) {
			Halved.push_back({Base, Pow});
			MaxHalfPow = std::max(MaxHalfPow, Pow);
		}
	}
	if (MaxHalfPow > 0) {
		Value *SquareRoot = buildMulPowProduct(Root, Halved, Changed);
		if (!SquareRoot) return nullptr;
		OuterProduct.push_back(SquareRoot);
		OuterProduct.push_back(SquareRoot);
	}
	if (OuterProduct.empty())
		return Value::makeOneForType(
			Root ? Root->getIRType() : nullptr);
	Value *Out = nullptr;
	if (!buildBalancedProduct(Root, OuterProduct, Out)) return nullptr;
	return Out;
}
//步骤 2.3 的匹配辅助：识别以整数 -1 相乘表示的负项，用于与正项成对消去。
bool ReassociateEngine::matchMulByMinusOne(Value *V, Value *&X) {
	X = nullptr;
	auto *BI = dynamic_cast<BinaryInst *>(V);
	if (!BI || BI->getOp() != BinaryInst::MUL) return false;
	Value *L = nullptr;
	Value *R = nullptr;
	if (!BI->getOperands(L, R)) return false;
	if (L->isConstMinusOne()) {
		X = R;
		return true;
	}
	if (R->isConstMinusOne()) {
		X = L;
		return true;
	}
	return false;
}
//步骤 2 的构造辅助：按给定次序重建加法项，供公因式提取生成内部和式。
bool ReassociateEngine::buildAddFromTerms(
	Instruction *Root, std::vector<Value *> Terms, Value *&Out) {
	Out = nullptr;
	if (Terms.empty()) return false;
	if (Terms.size() == 1) {
		Out = Terms[0];
		return true;
	}
	Value *Acc = Terms.back();
	Terms.pop_back();
	while (!Terms.empty()) {
		Value *Next = Terms.back();
		Terms.pop_back();
		auto *NI = createBinaryBefore(Root, BinaryInst::ADD, Next, Acc);
		if (!NI) return false;
		Acc = NI;
	}
	Out = Acc;
	return true;
}
//步骤 2：对一个展平操作数列依次执行常量归并、加法项化简和乘法幂压缩；
//每项改写都只使用严格整数代数等价关系。
Value *ReassociateEngine::optExp(
	Instruction *Root, BinaryInst::Operation Op,
	std::vector<Value *> &Ops, bool &Changed) {
	if (Ops.empty()) return nullptr;
	std::vector<Value *> NonConst;
	std::vector<Value *> Consts;
	NonConst.reserve(Ops.size());
	Consts.reserve(Ops.size());
	for (Value *V : Ops) {
		if (V && V->isConst()) Consts.push_back(V);
		else NonConst.push_back(V);
	}
	//2.1 折叠所有常量，并消除加零、乘一；乘零可直接替换整棵整数乘法树。
	Value *TotalConst = nullptr;
	for (auto It = Consts.rbegin(); It != Consts.rend(); ++It) {
		Value *C = *It;
		if (!TotalConst) {
			TotalConst = C;
			continue;
		}
		Value *Folded = BinaryInst::foldConstants(Op, TotalConst, C);
		if (Folded) {
			TotalConst = Folded;
			Changed = true;
		}
	}
	Ops.swap(NonConst);
	if (Ops.empty()) return TotalConst;
	if (TotalConst) {
		if (Op == BinaryInst::MUL && TotalConst->isConstZero()) return TotalConst;
		if (!((Op == BinaryInst::ADD && TotalConst->isConstZero()) ||
			(Op == BinaryInst::MUL && TotalConst->isConstOne()))) {
			Ops.push_back(TotalConst);
		}
	}
	if (Ops.size() == 1) return Ops[0];
	if (Op == BinaryInst::ADD) {
		//2.2 将重复的 X 项合并为 K * X，减少加法节点数。
		std::unordered_map<Value *, int> Cnt;
		Cnt.reserve(Ops.size());
		for (Value *V : Ops) ++Cnt[V];
		if (!Cnt.empty()) {
			std::vector<Value *> NewOps;
			std::unordered_set<Value *> Seen;
			NewOps.reserve(Ops.size());
			Seen.reserve(Ops.size());
			for (Value *V : Ops) {
				if (!Seen.insert(V).second) continue;
				int K = Cnt[V];
				if (K <= 1) {
					NewOps.push_back(V);
					continue;
				}
				Instruction *MulI = createBinaryBefore(
					Root, BinaryInst::MUL,
					Value::makeIntegerLike(V, K), V);
				if (!MulI) {
					NewOps.push_back(V);
					continue;
				}
				NewOps.push_back(MulI);
				Changed = true;
			}
			Ops.swap(NewOps);
		}
		if (Ops.size() >= 2) {
			//2.3 分桶后严格一对一消去 X 与 (-1) * X，保留未配对的重复项。
			std::vector<char> Removed(Ops.size(), 0);
			bool CanceledAny = false;
			std::unordered_map<Value *, std::vector<size_t>> PosIdx;
			std::unordered_map<Value *, std::vector<size_t>> NegIdx;
			PosIdx.reserve(Ops.size());
			NegIdx.reserve(Ops.size());
			for (size_t i = 0; i < Ops.size(); ++i) {
				Value *X = nullptr;
				if (matchMulByMinusOne(Ops[i], X) && X) {
					NegIdx[X].push_back(i);
				} else {
					PosIdx[Ops[i]].push_back(i);
				}
			}
			for (auto &KV : PosIdx) {
				Value *X = KV.first;
				auto NI = NegIdx.find(X);
				if (NI == NegIdx.end()) continue;
				auto &P = KV.second;
				auto &N = NI->second;
				size_t PairCnt = std::min(P.size(), N.size());
				if (PairCnt == 0) continue;
				for (size_t k = 0; k < PairCnt; ++k) {
					Removed[P[k]] = 1;
					Removed[N[k]] = 1;
				}
				CanceledAny = true;
				Changed = true;
			}
			if (CanceledAny) {
				std::vector<Value *> NewOps;
				NewOps.reserve(Ops.size());
				for (size_t i = 0; i < Ops.size(); ++i) {
					if (!Removed[i]) NewOps.push_back(Ops[i]);
				}
				Ops.swap(NewOps);
				if (Ops.empty()) return Value::makeZeroForType(Root->getIRType());
				if (Ops.size() == 1) return Ops[0];
			}
		}
		if (Ops.size() >= 2) {
			//2.4 展开每个加法项的单用途乘因子，选择覆盖项最多的公因式，重写为
			//F * (A + B + ...) 与未覆盖项之和。
			std::unordered_map<Value *, int> FactorFreq;
			std::vector<std::vector<Value *>> TermFactors(Ops.size());
			for (size_t i = 0; i < Ops.size(); ++i) {
				flattenMulFactors(Ops[i], TermFactors[i]);
				std::unordered_set<Value *> Once;
				for (Value *F : TermFactors[i]) {
					if (Once.insert(F).second) ++FactorFreq[F];
				}
			}
			Value *BestFactor = nullptr;
			int BestCount = 1;
			for (auto &KV : FactorFreq) {
				if (KV.second > BestCount ||
					(KV.second == BestCount && BestFactor &&
					 rankComesBefore(KV.first, BestFactor))) {
					BestFactor = KV.first;
					BestCount = KV.second;
				}
			}
			if (BestFactor && BestCount > 1) {
				std::vector<Value *> RemainingTerms;
				std::vector<Value *> OutsideTerms;
				for (size_t i = 0; i < Ops.size(); ++i) {
					auto Factors = TermFactors[i];
					auto Fit = std::find(Factors.begin(), Factors.end(), BestFactor);
					if (Fit == Factors.end()) {
						OutsideTerms.push_back(Ops[i]);
						continue;
					}
					Factors.erase(Fit);
					if (Factors.empty()) {
						RemainingTerms.push_back(
							Value::makeOneForType(
								Ops[i]->getIRType()));
					} else if (Factors.size() == 1) {
						RemainingTerms.push_back(Factors[0]);
					} else {
						Value *TermMul = nullptr;
						if (!buildBalancedProduct(Root, Factors, TermMul) || !TermMul) {
							RemainingTerms.push_back(Ops[i]);
						} else {
							RemainingTerms.push_back(TermMul);
						}
					}
				}
				if (RemainingTerms.size() >= 2) {
					Value *InsideAdd = nullptr;
					if (buildAddFromTerms(Root, RemainingTerms, InsideAdd) && InsideAdd) {
						Instruction *Factored = createBinaryBefore(Root, BinaryInst::MUL, BestFactor, InsideAdd);
						if (Factored) {
							std::vector<Value *> NewOps = OutsideTerms;
							NewOps.push_back(Factored);
							Ops.swap(NewOps);
							Changed = true;
						}
					}
				}
			}
		}
	}
	if (Op == BinaryInst::MUL && Ops.size() >= 4) {
		//2.5 足够长的乘积含重复因子时，用指数折半结构压缩重复乘法。
		std::unordered_map<Value *, int> PowCnt;
		for (Value *V : Ops) ++PowCnt[V];
		bool HasDup = false;
		for (auto &KV : PowCnt) {
			if (KV.second > 1) {
				HasDup = true;
				break;
			}
		}
		if (HasDup) {
			std::vector<std::pair<Value *, int>> PowList;
			PowList.reserve(PowCnt.size());
			std::unordered_set<Value *> Seen;
			for (Value *V : Ops) {
				if (Seen.insert(V).second)
					PowList.push_back({V, PowCnt[V]});
			}
			bool LocalPowChanged = false;
			Value *PowRebuild = buildMulPowProduct(Root, PowList, LocalPowChanged);
			if (PowRebuild) {
				Ops.clear();
				Ops.push_back(PowRebuild);
				Changed = Changed || LocalPowChanged || true;
			}
		}
	}
	if (Ops.size() == 1) return Ops[0];
	return nullptr;
}
//单根优化流程：筛选合法整数根，规范 SUB，展开并排序同操作树，执行代数
//化简，最后仅在形状确有变化时重建。
bool ReassociateEngine::optimizeInst(Instruction *I) {
	if (!I || !I->getParent()) return false;
	auto *BI = dynamic_cast<BinaryInst *>(I);
	if (!BI) return false;
	if (!I->getIRType() || !I->getIRType()->isInt()) return false;
	BinaryInst::Operation Op = BI->getOp();
	//1.候选发现只接受整数 ADD/MUL/AND/OR/XOR/SUB 等明确可重结合操作；
	//地址保护模式还要排除最终供 GEP 使用的表达式。
	if (!BinaryInst::isReassociateOp(Op)) return false;
	if (PreserveAddressArithmetic && feedsAddressComputation(I)) return false;
	Value *LHS = nullptr;
	Value *RHS = nullptr;
	if (!BI->getOperands(LHS, RHS)) return false;
	if (!IRType::isInt(LHS ? LHS->getIRType() : nullptr) ||
		!IRType::isInt(RHS ? RHS->getIRType() : nullptr)) {
		return false;
	}
	//2.SUB 先规范为 LHS + RHS * (-1)，新节点经 Todo 在后续继续重结合。
	if (Op == BinaryInst::SUB) {
		Instruction *Neg = createBinaryBefore(
			I, BinaryInst::MUL, RHS,
			Value::makeIntegerForType(-1, RHS->getIRType()));
		if (!Neg) return false;
		Instruction *Add = createBinaryBefore(I, BinaryInst::ADD, LHS, Neg);
		if (!Add) return false;
		I->replaceAllUsesWith(Add);
		pushTodo(I);
		return true;
	}
	if (!BinaryInst::isAssociativeOp(Op)) return false;
	//4.单用途且被同操作父节点消费的内部节点不单独重建，只从最外层根统一
	//展开，避免先后处理同一表达式造成重复工作。
	Instruction *OnlyUser = nullptr;
	if (I->countInstructionUses(&OnlyUser) == 1) {
		if (OnlyUser && OnlyUser != I && OnlyUser->getParent() == I->getParent()) {
			auto *UBI = dynamic_cast<BinaryInst *>(OnlyUser);
			if (UBI && UBI->getOp() == Op) {
				return false;
			}
		}
	}
	std::vector<Value *> Ops;
	std::vector<Value *> WorkList;
	std::unordered_set<Value *> Visited;
	Ops.reserve(8);
	WorkList.reserve(8);
	Visited.reserve(16);
	WorkList.push_back(LHS);
	WorkList.push_back(RHS);
	//5.展开单用途同操作子树；共享节点保留为叶子，确保不复制计算。
	while (!WorkList.empty()) {
		Value *Cur = WorkList.back();
		WorkList.pop_back();
		if (!Visited.insert(Cur).second) {
			Ops.push_back(Cur);
			continue;
		}
		auto *CurBI = dynamic_cast<BinaryInst *>(Cur);
		if (!CurBI || CurBI->getOp() != Op ||
			Cur->countInstructionUses() != 1 ||
			crossesOrderingBarrier(Cur, I)) {
			Ops.push_back(Cur);
			continue;
		}
		Value *A = nullptr;
		Value *B = nullptr;
		if (!CurBI->getOperands(A, B)) {
			Ops.push_back(Cur);
			continue;
		}
		WorkList.push_back(A);
		WorkList.push_back(B);
	}
	if (Ops.size() <= 1) {
		if (Ops.size() == 1 && Ops[0] != I) {
			I->replaceAllUsesWith(Ops[0]);
			pushTodo(I);
			return true;
		}
		return false;
	}
	//6.使用函数级确定秩建立全序，随后反复调用 optExp，直到操作数列不再
	//收缩或直接得到整棵树的替换值。
	std::sort(Ops.begin(), Ops.end(), [&](Value *A, Value *B) {
		return rankComesBefore(A, B);
	});
	bool Changed = false;
	while (true) {
		size_t OldSize = Ops.size();
		Value *Folded = optExp(I, Op, Ops, Changed);
		if (Folded && Folded != I) {
			I->replaceAllUsesWith(Folded);
			pushTodo(I);
			return true;
		}
		if (Ops.size() == OldSize) break;
	}
	if (Ops.size() == 1 && Ops[0] != I) {
		I->replaceAllUsesWith(Ops[0]);
		pushTodo(I);
		return true;
	}
	if (Ops.size() < 2) return Changed;
	//7.已是规范链则保留原节点；否则在旧根前重建有序链、替换全部 use，
	//并把旧根加入待办队列供后续清理判断。
	if (ReassociatePass::isAlreadyBuilt(I, Op, Ops)) return Changed;
	Value *R = Ops.back(); Ops.pop_back();
	Value *L = Ops.back(); Ops.pop_back();
	Instruction *Result = createBinaryBefore(I, Op, L, R);
	if (!Result) return Changed;
	while (!Ops.empty()) {
		Value *Next = Ops.back();
		Ops.pop_back();
		Instruction *NewInst = createBinaryBefore(I, Op, Next, Result);
		if (!NewInst) break;
		Result = NewInst;
	}
	I->replaceAllUsesWith(Result);
	pushTodo(I);
	return true;
}
//Pass 入口只委托函数级引擎求解；重结合不改 CFG，但可能改变地址表达式和
//调用参数的 SSA 关系，因此保留支配/循环分析并失效别名与副作用分析。
PreservationStatus ReassociatePass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	ReassociateEngine Engine(F, FAM, PreserveAddressArithmetic);
	bool Changed = Engine.run();
	if (!Changed) return PreservationStatus::all();
	PreservationStatus PA;
	PA.keep<DominantAnalysis>();
	PA.keep<PostDominantAnalysis>();
	PA.keep<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
