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
  // 可选机器层 pass 禁用列表。名字由 MachinePassBuilder 统一定义，
  // 例如 cleanup-cfg、post-ra-peephole；填 all 可禁用全部可选 pass。
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

// 调试/适配入口：只生成并转储目标无关的 MIR，不进入物理寄存器分配或
// AsmPrinter。最终 SIMD 文档发布前可用它验证 IR -> G_V* lowering。
void emitGenericMachineIR(Module &module, std::ostream &out);

} // namespace riscv
