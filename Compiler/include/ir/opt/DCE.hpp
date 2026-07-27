#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <unordered_map>

// 死代码消除 (Dead Code Elimination)
//函数内级联删除死代码
class DCEPass : public PassDescriptor<DCEPass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);
private:
	// 递归函数可删除性证明的三色状态。
	enum class ReturnProofState {
		Visiting,
		Proven,
		Failed,
	};

	static bool isGuaranteedRemovableFunction(
		Function &F, FunctionAnalysisPassManager &FAM,
		std::unordered_map<Function *, ReturnProofState> &Memo);
	static bool isDeletableInstruction(
		Instruction *I, FunctionAnalysisPassManager &FAM);

	// 公共调用副作用查询复用上述私有证明过程。
	friend bool dceCallHasSideEffect(
		CallInst *CI, FunctionAnalysisPassManager &FAM);
};

// 可复用的调用可观察副作用判定。
bool dceCallHasSideEffect(CallInst* CI, FunctionAnalysisPassManager& FAM);

// 共享的“无环且所有 CFG 路径都返回”结构证明。
// 该接口只回答控制流一定到达 ret，不隐含无副作用或无 fault；
// 调用方仍需分别检查指令与内存行为。
bool hasAcyclicReturningCFG(Function &F);

// 仅当 I 是整数移位且常量移位量位于 [0, bitwidth(lhs)) 时返回 true。
// 动态、类型错误或越界移位不能作为可投机执行或“值有定义”的证据。
// DCE、IfConversion 与 LICM 共用该判定，避免在非 i32 类型上规则不一致。
bool hasDefinedConstantShiftAmount(const Instruction *I);

// 在有界 def-use 图中检查值是否显式或传递依赖 undef。该接口只供确实
// 需要“值稳定”证明的变换使用；普通折叠、GVN 与死代码删除不得全局套用。
bool valueMayDependOnUndef(
	const Value *V, std::size_t MaxVisitedValues = 256);

// 返回 true 表示即使结果无人使用，也必须保留该指令的执行。
// 普通 undef/poison 仍按本 IR 的通用语义处理；这里只有显式可观察行为
// 或未知指令种类需要强制保留。
bool dceInstructionMustBePreserved(Instruction *I);

// 用于移动或投机执行的统一危险指令判定；不用于普通死代码删除。
bool instructionMayFaultOrProducePoison(const Instruction *I);
