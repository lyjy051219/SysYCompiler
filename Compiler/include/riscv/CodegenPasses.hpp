#pragma once

#include "MachinePassManager.hpp"

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

// 机器层控制流清理，当前为空位，留给之后的块合并和死块删除
struct CleanupCFGPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 把 SELECT 伪指令下降为分支和拷贝
struct LowerSelectPass {
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

// 分配后窥孔优化
struct PostRAPeepholePass {
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
