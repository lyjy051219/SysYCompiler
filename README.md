# Compiler

曾获2026全国大学生计算机系统能力大赛-编译系统设计赛-编译系统实现赛 国家级二等奖。

本项目使用 C++17 实现，支持 SysY 程序及 tensor 扩展，经过自定义 IR 和中端优化后生成 RISC-V 64 位汇编。当前仓库实现的目标后端为 RISC-V，运行示例使用 RV64GC / LP64D。

## 分工

- 刘烨嘉一（[@lyjy051219](https://github.com/lyjy051219)）：编译器前端、中端
- 宋科宏（[@Koki-Niwa](https://github.com/Koki-Niwa)）：编译器后端

## 主要功能

- **前端**：Flex/Bison 词法与语法分析、AST、符号表、类型系统及 IR 生成。
- **语言扩展**：tensor 整数与浮点运算、逐元素运算、标量广播、整体赋值、参数与返回值，以及点积和矩阵乘法。
- **中端分析**：支配关系、循环分析、别名分析、MemorySSA、标量演化（SCEV）、范围分析、循环依赖及访存分析。
- **中端优化**：Mem2Reg、常量传播、GVN/PRE、死代码与死存储消除、函数内联、尾递归消除、循环交换/分布/融合/展开、不变量外提和强度削减等。
- **后端**：IR 到 MIR 的指令选择、Phi 与伪指令消除、寄存器分配、栈帧处理，以及机器层公共表达式消除、LICM、窥孔优化、基本块布局和指令调度。

编译流程：

```text
SysY / tensor 源程序
    → Flex / Bison → AST
    → IRGenerator → 自定义 IR
    → 中端分析与优化
    → RISC-V 指令选择 → MIR
    → 机器优化、寄存器分配与栈帧处理
    → RISC-V 汇编
```

## 环境与构建

### 依赖

构建编译器需要 CMake 3.13 或更新版本、支持 C++17 的 GCC/Clang，以及 Make 或 Ninja。仓库已包含生成的词法器与语法解析器，常规构建不需要重新运行 Flex/Bison。

运行生成的 RISC-V 程序和测试脚本还需要：

- RISC-V Linux 交叉工具链，如 `riscv64-linux-gnu-gcc`。
- `qemu-riscv64`，或可运行目标程序的 RISC-V Linux 环境。
- Python 3，用于测试和基准脚本。

如需修改并重新生成词法器、语法解析器，现有生成文件对应 Flex 2.6.4 和 Bison 3.8.2。

### 构建完整编译器

本 README 位于仓库根目录。先执行一次 `cd Compiler` 进入编译器工程；后续构建、编译、运行与测试命令均在该目录执行，命令中的相对路径也以该目录为基准：

```bash
# 从仓库根目录进入编译器工程
cd Compiler

# 配置完整 RISC-V Release 编译器
cmake -S . -B build-riscv-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_MIDDLE_END=OFF \
  -DBUILD_MIDDLE_END_RELEASE=OFF \
  -DBUILD_RISCV_BACKEND_RELEASE=ON

cmake --build build-riscv-release --target compiler_riscv_release -j 4
```

生成的可执行文件为 `build-riscv-release/compiler`。CMake 目标名是 `compiler_riscv_release`，输出文件名仍是 `compiler`。

调试构建：

```bash
cmake -S . -B build-riscv \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_MIDDLE_END=OFF \
  -DBUILD_MIDDLE_END_RELEASE=OFF \
  -DBUILD_RISCV_BACKEND_RELEASE=OFF

cmake --build build-riscv --target compiler -j 4
```

`BUILD_MIDDLE_END` 默认开启，且其分支优先于 RISC-V Release 分支，因此构建完整编译器时必须显式关闭。现有中端专用目标未链接 Backend，但共享的 `main.cpp` 会调用后端，不应将这些目标作为当前版本的独立 IR 输出入口。

## Command 格式

### 生成汇编

```bash
# 功能测试调用格式
./build-riscv-release/compiler testcase.sy -S -o testcase.s

# 性能测试调用格式
./build-riscv-release/compiler testcase.sy -S -o testcase.s -O1

# 兼容评测平台的参数顺序
./build-riscv-release/compiler -S -o testcase.s testcase.sy -O1
```

输入文件也可以使用 `.sysy` 后缀。输出目录需事先存在；编译器生成汇编，汇编和链接由交叉工具链完成。

**当前参数行为：**入口接受 `-O0`、`-O1` 和 `-O2`，但未将这些参数映射到优化配置；中端固定使用 O1，后端默认使用 O1。因此上述功能与性能调用目前使用相同的优化配置，`-O0` 不会关闭优化。CMake 的 Release 配置控制编译器自身的构建，与输入程序的优化等级不同。

当前版本不额外输出 `.ll` 文件，入口中的 IR 调试输出代码处于注释状态；也没有 `--emit-llvm` 接口。

### 汇编、链接与执行

```bash
riscv64-linux-gnu-gcc -march=rv64gc -mabi=lp64d -static \
  testcase.s runtime/sylib.c -lm -o testcase

qemu-riscv64 ./testcase

# 对需要标准输入的程序
qemu-riscv64 ./testcase < testcase.in
```

静态链接需要交叉工具链提供对应的静态运行库。

## 测试与调试

### Tensor 回归与性能基准

仓库自带 8 个 tensor 用例，覆盖逐元素运算、广播、复制与返回、点积、矩阵乘向量、向量乘矩阵、矩阵乘法及批量广播矩阵乘法。

```bash
# 快速回归：每个用例运行一次，不预热
python3 tensor-benchmark/run.py \
  --compiler build-riscv-release/compiler \
  --warmup 0 --repeat 1 --label smoke

# 性能采样：预热一次，正式运行五次
python3 tensor-benchmark/run.py \
  --compiler build-riscv-release/compiler \
  --warmup 1 --repeat 5 --label baseline
```

脚本检查程序输出与返回码，生成 JSON/CSV 报告；默认工作目录为 `tensor-benchmark/_work/`，报告目录为 `tensor-benchmark/results/`。更多选项见 [tensor 基准说明](Compiler/tensor-benchmark/README.md)。QEMU 计时适合同环境下的相对比较，不代表比赛目标板的实际性能。

`backendtest.py`、`backendtest.ipynb` 和 `performance_score.py` 保留了后端测试与评分流程，依赖本地工具链及额外测试集，并非克隆仓库后即可完整复现比赛评测。`performance_score.py` 的 LLVM IR 基线还依赖 IR 输出；当前主程序不生成 `.ll`，使用前需适配这一接口及测试集路径。它们的结果不等同于官方比赛成绩。

### 后端调试开关

```bash
# 在关键后端阶段检查 MIR
BACKEND_VERIFY_MIR=1 ./build-riscv-release/compiler testcase.sy -S -o testcase.s

# 打印各阶段 MIR
BACKEND_DUMP_MIR=1 ./build-riscv-release/compiler testcase.sy -S -o testcase.s

# 暂时关闭指定的可选机器优化 Pass
BACKEND_DISABLE_PASSES=post-ra-peephole \
  ./build-riscv-release/compiler testcase.sy -S -o testcase.s
```

`BACKEND_DISABLE_PASSES` 支持逗号、分号或空格分隔的名称；使用 `all` 关闭全部可选机器 Pass，必需的代码生成阶段仍执行。Pass 名称以 `src/riscv/opt/MachinePassBuilder.cpp` 为准。上述校验和打印开关通过环境变量是否存在来判断，关闭时应取消设置。

## 项目架构

下图从仓库根目录展开，`include/` 与 `src/` 分别组织接口和实现：

```text
仓库根目录/
├── README.md                        # 项目说明（本文件）
├── LICENSE                          # MIT 协议全文
├── .gitignore
└── Compiler/                        # CMake 工程与源码目录
    ├── CMakeLists.txt
    ├── main.cpp                     # 命令行入口与完整编译流程
    ├── include/
    │   ├── Common.hpp
    │   ├── yacc/                    # 生成的词法器、语法解析器接口
    │   ├── lib/                     # AST、类型系统、IR 与 CFG 基础设施
    │   ├── ir/
    │   │   ├── analysis/            # 中端分析接口
    │   │   └── opt/                 # Pass 管理与中端变换接口
    │   └── riscv/                   # MIR、指令选择、寄存器与栈帧接口
    │       └── opt/                 # 机器分析、优化与代码生成 Pass
    ├── src/
    │   ├── yacc/                    # sysy.l、sysy.y 及生成的解析代码
    │   ├── lib/                     # IR 生成与基础设施实现
    │   ├── ir/
    │   │   ├── analysis/            # 中端分析实现
    │   │   └── opt/                 # 优化实现及 PassBuilder 流水线
    │   └── riscv/                   # RISC-V 后端实现
    │       └── opt/                 # 机器层优化与流水线实现
    ├── runtime/                     # SysY 运行库与 runtime.ll
    ├── tensor-benchmark/            # 8 个用例、运行脚本及基准结果
    ├── backendtest.py               # 后端测试脚本
    ├── backendtest.ipynb            # 后端测试 Notebook
    ├── performance_score.py         # 本地性能评分与基线对比脚本
    ├── doc/                         # 设计文档、语言定义、PDF 及插图
    │   ├── language/                # SysY 语言参考文档
    │   │   ├── SysY2022语言定义-V1.pdf
    │   │   └── SYSY2026.pdf          # 包含 tensor 类型的语法与语言说明
    │   ├── DOCS.md                  # 前中端设计文档
    │   ├── BACKEND.md               # 后端设计文档
    │   └── DOCS.pdf                 # 设计文档 PDF
    └── docs/images/                 # 补充文档图片
```

## 语言参考与设计文档

### 语言参考

`Compiler/doc/language/` 收录语言参考资料，便于对照前端语法和 tensor 扩展：

- [SysY 2022 语言定义（V1）](Compiler/doc/language/SysY2022语言定义-V1.pdf)：基础类型、数组、函数、语句及表达式等语言规则。
- [SysY 2026 语言参考](Compiler/doc/language/SYSY2026.pdf)：包含 `tensor int`、`tensor float` 及 tensor 函数参数与返回类型的语法和语言说明。

这些文档用于查阅语言规则，不表示当前编译器已通过全部规范符合性验证；具体实现可对照 `Compiler/src/yacc/sysy.l`、`Compiler/src/yacc/sysy.y` 和 `Compiler/src/lib/IRGenerator.cpp`。

### 设计与测试文档

- [前中端设计文档](Compiler/doc/DOCS.md)
- [后端设计文档](Compiler/doc/BACKEND.md)
- [设计文档 PDF](Compiler/doc/DOCS.pdf)
- [Tensor 基准说明](Compiler/tensor-benchmark/README.md)

设计文档及历史基准记录可能描述开发阶段的实现；当前行为以源码和构建配置为准。

## 开源协议

本项目自有代码采用 [MIT License](LICENSE)，源码中的 `SPDX-License-Identifier: MIT` 与根目录协议对应。生成代码及外部来源文件保留其原有版权、许可声明和适用例外。
