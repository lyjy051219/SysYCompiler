#pragma once

#include "../lib/CFG.hpp"
#include <iosfwd>
#include <string>
#include <utility>

namespace riscv {

// 后端优化等级，O0 只跑必经的代码生成阶段
enum class CodegenOptLevel {
  O0 = 0,
  O1 = 1,
};

struct BackendOptions {
  bool emitMetrics = false;
  CodegenOptLevel optLevel = CodegenOptLevel::O1;
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