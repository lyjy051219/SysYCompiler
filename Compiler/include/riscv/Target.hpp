// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "../lib/CFG.hpp"
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace riscv {

// 后端优化等级，O0 只跑必经的代码生成阶段
enum class CodegenOptLevel {
  O0 = 0,
  O1 = 1,
};

struct BackendOptions {
  bool emitMetrics = false;
  CodegenOptLevel optLevel = CodegenOptLevel::O1;
  // 可选机器层 pass 禁用列表
  //名字由 MachinePassBuilder 定义
  // 如 cleanup-cfg post-ra-peephole
  //填 all 可禁用全部可选 pass。
  std::vector<std::string> disabledMachinePasses;
};

class BackendError {
  std::string message_;
 
public:
  explicit BackendError(std::string message) : message_(std::move(message)) {}
  const std::string &message() const { return message_; }
};

void emitAssembly(Module &module, std::ostream &out,
                  const BackendOptions &options = BackendOptions{});

} // namespace riscv
