#pragma once

#include "MIR.hpp"
#include "MachinePassManager.hpp"

#include <cstddef>

// 后端分析 pass 集合
// 每个分析继承 MachineAnalysisDescriptor，声明 Result 类型和静态 Key，
// 通过 MachineFunctionAnalysisManager::getResult 按需计算并缓存。
// 新增分析时在此文件添加类型，并在 MachinePassBuilder 里注册。

namespace riscv {

// 函数级统计分析：块数、指令数、各类计数
// 作为分析框架的样例，也可用于优化前后的规模对比
class MachineInstrStatsAnalysis
    : public MachineAnalysisDescriptor<MachineInstrStatsAnalysis> {
public:
  static MachinePassID Key;

  struct Result {
    std::size_t blockCount = 0;
    std::size_t instrCount = 0;
    std::size_t copyCount = 0;
    std::size_t callCount = 0;
    std::size_t phiCount = 0;
  };

  Result run(MachineFunction &mf, MachineFunctionAnalysisManager &am) const;
};

// 函数级 CFG 视图分析：以块下标形式给出前驱后继
// 与 MachineBasicBlock 内嵌的 preds succs 不同，这里的结果由分析管理器缓存，
// 修改控制流的 pass 需要在返回值里 discard 它
class MachineCFGAnalysis
    : public MachineAnalysisDescriptor<MachineCFGAnalysis> {
public:
  static MachinePassID Key;

  struct Result {
    // 下标与 MachineFunction::blocks 对应
    std::vector<std::vector<int>> preds;
    std::vector<std::vector<int>> succs;
  };

  Result run(MachineFunction &mf, MachineFunctionAnalysisManager &am) const;
};

} // namespace riscv
