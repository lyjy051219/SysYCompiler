#pragma once

#include "riscv/opt/MachinePassManager.hpp"

// 代码生成必经阶段的 pass 封装
// 把 Passes.hpp RegAlloc.hpp Frame.hpp 里的既有阶段包装成统一的 pass 形态，
// 由 MachinePassBuilder 组装进流水线。新增变换 pass 时仿照这里的写法：
// 定义一个带 run(MachineFunction 引用, MachineFunctionAnalysisManager 引用)
// 的结构体，按是否修改了 IR 返回对应的 MachinePreservationStatus。

namespace riscv {

// 根据跳转指令重建各块的前驱后继
struct RebuildCFGPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 机器层控制流清理：删除不可达块、空跳板块、线性合并和冗余跳转
struct CleanupCFGPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 把 SELECT 伪指令下降为无分支整数序列或分支和拷贝。
struct LowerSelectPass {
  bool branchlessOnly = false;

  explicit LowerSelectPass(bool branchlessOnly = false)
      : branchlessOnly(branchlessOnly) {}

  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 消除 PHI 结点，改写为前驱块末尾的并行拷贝
struct EliminatePhiPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 寄存器分配
struct RegisterAllocationPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 栈帧布局分配并展开 FrameIndex 操作数
struct FrameLoweringPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 分配前折叠 compare + branch 指令链
struct BranchCompareFoldingPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 分配前复用函数内重复出现的全局地址
struct PreRAGlobalAddressReusePass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 分配前保守拷贝传播
struct PreRACopyPropagationPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 分配前基本块内局部公共子表达式删除
struct PreRALocalCSEPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 分配后窥孔优化
struct PostRAPeepholePass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 分配后消除相邻块中的条件跳转加无条件跳转双跳。
struct BlockPlacementPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 校验 pass：检查指令里没有非法的虚拟寄存器引用，出错时抛 BackendError
// 只读不修改，适合在调试时插到任意两个阶段之间
struct MachineVerifyPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 调试打印 pass：设置环境变量 BACKEND_DUMP_MIR 后把当前 MIR 打到标准错误
// tag 用来区分打印位置
struct MachineDumpPass {
  const char *tag;
  explicit MachineDumpPass(const char *tag) : tag(tag) {}
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

} // namespace riscv
