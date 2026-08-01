#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <cstddef>
#include <memory>
#include <vector>

//候选保留全局对象、元素类型以及扫描阶段已验证的 main 内 use，提交阶段只改这些边。
struct Global2LocalCandidateInfo {
	//记录待转换为main局部对象的全局变量。
	Variable *GV = nullptr;
	//记录全局指针指向的元素或完整对象类型。
	std::shared_ptr<IRType> ElemTy;
	//快照main中需要重连到新alloca的全部Use对象。
	std::vector<Use *> MainUses;
};

//流程：1. 仅在非递归 main 执行；2. 筛选小型且无跨函数可达引用的全局；
//3. 在入口创建 alloca 和标量 store/数组 memcpy 初始化；4. 重连 main use。
//timer 不读取对象存储位置，因此不限制全局与局部表示转换。
class Global2LocalPass : public PassDescriptor<Global2LocalPass> {
public:
	//构造Pass并选择是否允许受限标量load/store使用。
	explicit Global2LocalPass(bool RelaxScalarUses = false);

	//在非递归main入口把安全的小型全局对象转换为局部栈对象。
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	//限定适合转换为栈对象的最大目标字节数。
	static constexpr size_t RISCVThreshold = 32;
	//记录是否放宽标量全局的直接load/store使用限制。
	bool RelaxScalarUses = false;
	//检查标量全局在main中的使用是否为安全直接访存。
	static bool isSafeScalarUseInMain(Use *U, Variable *GV);
};
