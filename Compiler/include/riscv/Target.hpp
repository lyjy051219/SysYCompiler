#pragma once

#include "../lib/CFG.hpp"
#include <iosfwd>
#include <string>
#include <utility>

namespace riscv {

struct BackendOptions {
  bool emitMetrics = false;
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