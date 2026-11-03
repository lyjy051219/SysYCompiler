// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "MIR.hpp"

namespace riscv {

void rebuildCFG(MachineFunction &mf);
void eliminatePhiNodes(MachineFunction &mf);
// 当 branchlessOnly 为 true 时，只lower能证明条件为 0/1 的整数 SELECT，
// 其余 SELECT 留给后续的分支式lower
bool lowerSelectPseudo(MachineFunction &mf, bool branchlessOnly = false);

} // namespace riscv
  