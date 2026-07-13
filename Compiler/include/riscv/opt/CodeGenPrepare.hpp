#pragma once

#include "lib/CFG.hpp"

namespace riscv {

// 后端私有的 IR 级代码生成准备阶段。
// 只在 RISC-V backend 入口调用，不接入通用中端流水线。
bool runCodeGenPrepare(Module &module);

} // namespace riscv
