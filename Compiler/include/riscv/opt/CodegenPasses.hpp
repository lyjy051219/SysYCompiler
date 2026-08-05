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

// 分配前把局部 i32 加减/左移 DAG 归一化为更短的仿射表达式
struct PreRAAffineCombinePass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 分配前折叠 common-base 地址临时和 load/store 偏移
struct PreRAAddressCombinePass {
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

// 分配前在零次内循环入口前跳过只供循环体使用的昂贵纯计算
struct PreRAGuardedLoopPreheaderPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 分配前机器层死代码删除
struct PreRAMachineDCEPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 分配前机器层循环不变量外提：把循环内不变且可安全推测的纯计算/可证安全 load
// 移到 preheader，每轮循环只执行一次。
struct PreRAMachineLICMPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 分配后窥孔优化
struct PostRAPeepholePass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 分配后将单块循环的回边直接改为条件回 body，跳过重复 header 判断。
struct PostRALoopBackedgeBypassPass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 分配后块内 list scheduling
struct PostRAListSchedulePass {
  MachinePreservationStatus run(MachineFunction &mf,
                                MachineFunctionAnalysisManager &am);
};

// 分配后按静态频率重排机器块，并规范化 fallthrough 分支。
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
