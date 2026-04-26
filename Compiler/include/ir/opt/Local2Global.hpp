#pragma once

#include "ir/opt/NewPassManager.hpp"

#include <cstddef>

// 将超大局部栈分配(alloca)提升为全局变量，避免超大栈对象。
class Local2GlobalPass : public PassDescriptor<Local2GlobalPass> {
public:
	PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
	static constexpr size_t RISCVThreshold = 1024;
};
