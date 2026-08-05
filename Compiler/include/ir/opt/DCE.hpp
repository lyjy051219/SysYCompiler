#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <unordered_map>

//流程：1. 递归证明无副作用调用同时必然返回；2. 筛出无用户且无可观察语义的指令；
//3. 删除后把失去最后用户的操作数定义加入 FIFO，沿 def-use 链清理到闭包。
class DCEPass : public PassDescriptor<DCEPass> {
public:
    //沿def-use链删除函数中无用户且无可观察语义的指令。
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);
private:

	//ReturnProofState记录递归函数证明的三色状态。
	enum class ReturnProofState {
		//表示当前函数正在递归证明栈中。
		Visiting,
		//表示函数已通过当前递归证明。
		Proven,
		//表示函数不满足当前递归证明条件。
		Failed,
	};

	//递归证明函数自身CFG和全部直接用户函数调用均必然有限返回。
	static bool isGuaranteedReturningFunction(
		Function &F,
		std::unordered_map<Function *, ReturnProofState> &Memo);
	//递归证明函数无可观察副作用、调用闭包安全且CFG必然有限返回。
	static bool isGuaranteedRemovableFunction(
		Function &F, FunctionAnalysisPassManager &FAM,
		std::unordered_map<Function *, ReturnProofState> &Memo);
	//判断指令是否没有用户且不包含必须保留的可观察语义。
	static bool isDeletableInstruction(
		Instruction *I, FunctionAnalysisPassManager &FAM);

	//允许共享调用副作用查询访问私有的递归返回证明状态。
	friend bool dceCallHasSideEffect(
		CallInst *CI, FunctionAnalysisPassManager &FAM);
	friend bool dceCallIsGuaranteedToReturn(CallInst *CI);
};

//以下共享合法性查询供 DCE、IfConversion、LICM 和地址类变换复用；它们都采用
//“未知即保守”的策略，分别处理调用行为、有限返回、移位、undef 与陷阱/poison。
//判断调用是否具有可观察副作用或可能不返回。
bool dceCallHasSideEffect(CallInst* CI, FunctionAnalysisPassManager& FAM);

//递归证明调用及其直接调用闭包的入口可达CFG均有限并最终返回。
bool dceCallIsGuaranteedToReturn(CallInst *CI);

//证明函数入口可达CFG无环且每条路径最终以ret结束。
bool hasAcyclicReturningCFG(Function &F);

//检查移位指令是否使用位宽范围内的已定义常量移位量。
bool hasDefinedConstantShiftAmount(const Instruction *I);

//有界反向检查值是否可能依赖undef或不完整操作数。
bool valueMayDependOnUndef(
	const Value *V, std::size_t MaxVisitedValues = 256);

//判断指令是否具有未知或不可丢弃的语义。
bool dceInstructionMustBePreserved(Instruction *I);

//判断无用户指令在执行时是否仍可能陷阱或产生poison。
bool instructionMayFaultOrProducePoison(const Instruction *I);
