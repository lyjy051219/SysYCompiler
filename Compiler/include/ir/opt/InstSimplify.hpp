#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// InstSimplifyPass:
// 1) 以工作队列方式在函数内做局部指令化简
// 2) 支持 Binary / Select / PHI 的常见等式和常量折叠
// 3) 通过 RAUW 传播化简结果，并级联触发其用户重试
class InstSimplifyPass : public PassDescriptor<InstSimplifyPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
