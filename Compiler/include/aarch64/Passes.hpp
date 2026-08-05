#pragma once

#include "MIR.hpp"

namespace aarch64 {

// 机器层必经阶段的底层实现函数
// 旧的函数指针式 MachineFunctionPassManager 已废弃，
// 统一改用 opt/MachinePassManager.hpp 的基础设施，
// 这些函数由 opt/CodegenPasses.hpp 中对应的 pass 包装后进入流水线

void rebuildCFG(MachineFunction &mf);
void eliminatePhiNodes(MachineFunction &mf);
// 当 branchlessOnly 为 true 时，只下降能证明条件为 0/1 的整数 SELECT，
// 其余 SELECT 留给后续的分支式下降。
bool lowerSelectPseudo(MachineFunction &mf, bool branchlessOnly = false);

} // namespace aarch64
