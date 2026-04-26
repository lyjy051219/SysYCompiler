#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// 将简单 if/if-else 控制流改写为 select，减少分支跳转。
class IfConversionPass : public PassDescriptor<IfConversionPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	bool tryConvertOne(Function &F, BasicBlock *TrueBB);
};
