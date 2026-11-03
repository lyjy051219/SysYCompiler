// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "MIR.hpp"

#include <cstdint>
#include <memory>

class IRType;

namespace riscv {

int64_t typeSizeBytes(const std::shared_ptr<IRType> &type);
int64_t typeAlignBytes(const std::shared_ptr<IRType> &type);

class FrameLowering {
public:
  void assignFrameLayout(MachineFunction &mf);
  void expandFrameIndices(MachineFunction &mf);
};

} // namespace riscv
 