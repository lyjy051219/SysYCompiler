#pragma once

#include "aarch64/MIR.hpp"
#include "aarch64/opt/MachinePassManager.hpp"

#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

// 后端分析 pass 集合
// 每个分析继承 MachineAnalysisDescriptor，声明 Result 类型和静态 Key，
// 通过 MachineFunctionAnalysisManager::getResult 按需计算并缓存。
// 新增分析时在此文件添加类型，并在 MachinePassBuilder 里注册。

namespace aarch64 {

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

// 函数级虚拟寄存器活跃性分析：按基本块给出 use/def/live-in/live-out
// 结果只保存块下标和虚拟寄存器编号，不持有 MachineBasicBlock 指针或引用。
class MachineLivenessAnalysis
    : public MachineAnalysisDescriptor<MachineLivenessAnalysis> {
public:
  static MachinePassID Key;

  using RegSet = std::set<uint32_t>;

  struct Result {
    std::vector<RegSet> uses;
    std::vector<RegSet> defs;
    std::vector<RegSet> liveIn;
    std::vector<RegSet> liveOut;
  };

  Result run(MachineFunction &mf, MachineFunctionAnalysisManager &am) const;
};

// 自然循环分析：复用寄存器分配原有的支配、回边和反向可达算法。
// 结果只含旧布局下标；任何块顺序变换都会使其失效。
class MachineLoopAnalysis
    : public MachineAnalysisDescriptor<MachineLoopAnalysis> {
public:
  static MachinePassID Key;

  struct Result {
    std::vector<int> depth;
    std::set<std::pair<int, int>> backedges;
    // 按 header 排序的自然循环成员块下标集合（含 header 自身）。
    // 增量字段：既有消费方（RegAlloc/BlockPlacement/GuardedLoopPreheader）
    // 只读 depth/backedges，不受影响。供 machineLICM 等按 header 枚举循环。
    std::map<int, std::set<int>> membersByHeader;
  };

  Result run(MachineFunction &mf, MachineFunctionAnalysisManager &am) const;
};

// 确定性的静态块频率和边概率。概率使用定点数，避免浮点排序差异。
class MachineBlockFrequencyAnalysis
    : public MachineAnalysisDescriptor<MachineBlockFrequencyAnalysis> {
public:
  static MachinePassID Key;
  static constexpr uint32_t ProbabilityScale = 1024;

  struct Result {
    std::vector<uint64_t> blockFrequency;
    std::vector<std::vector<int>> successors;
    std::vector<std::vector<uint32_t>> edgeProbability;
    std::vector<std::vector<uint64_t>> edgeWeight;
    // converged 仅诊断有限迭代是否达到精确定点。reliable 表示该有界估计
    // 可安全用于布局：入口无入边、无可达闭合循环 SCC，且传播未饱和。
    bool converged = false;
    bool reliable = false;
  };

  Result run(MachineFunction &mf, MachineFunctionAnalysisManager &am) const;
};

} // namespace aarch64
