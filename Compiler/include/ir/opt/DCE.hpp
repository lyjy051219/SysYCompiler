#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <unordered_map>

 
 
// 流程：1. 递归证明无副作用调用同时必然返回；2. 筛出无用户且无可观察语义的指令；
// 3. 删除后把失去最后用户的操作数定义加入 FIFO，沿 def-use 链清理到闭包。
class DCEPass : public PassDescriptor<DCEPass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);
private:
	 
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

	 
	friend bool dceCallHasSideEffect(
		CallInst *CI, FunctionAnalysisPassManager &FAM);
};

 
// 以下共享合法性查询供 DCE、IfConversion、LICM 和地址类变换复用；它们都采用
// “未知即保守”的策略，分别处理调用行为、有限返回、移位、undef 与陷阱/poison。
bool dceCallHasSideEffect(CallInst* CI, FunctionAnalysisPassManager& FAM);

 
 
 
bool hasAcyclicReturningCFG(Function &F);

 
 
 
bool hasDefinedConstantShiftAmount(const Instruction *I);

 
 
bool valueMayDependOnUndef(
	const Value *V, std::size_t MaxVisitedValues = 256);

 
 
 
bool dceInstructionMustBePreserved(Instruction *I);

 
bool instructionMayFaultOrProducePoison(const Instruction *I);
