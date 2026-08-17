#pragma once

#include "MIR.hpp"

namespace riscv {

// 通用向量 MIR 与最终机器指令之间的唯一目标接口。当前实现仅校验并
// fail-closed；决赛指令文档公布后，在这里完成 G_V* 合法化和目标选指令。
void legalizeAndSelectGenericVectors(MachineModule &module);

} // namespace riscv
