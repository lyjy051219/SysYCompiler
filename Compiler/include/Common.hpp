#pragma once

// 公共配置头（占位/锚点）。
//
// 评测平台不使用 CMake，而是直接 clang++ 编译所有 .cpp，并按
// “目录内直接包含头文件”的规则自动收集 -I 搜索路径。此前 include/ 根目录
// 没有任何头文件，导致平台遗漏 -I <repo>/include，所有带
// "ir/..."/"lib/..."/"aarch64/..."/"riscv/..."/"yacc/..." 前缀的包含语句
// 无法解析（提交 CE）。本文件的存在让平台把 include/ 根加入搜索路径。

// 评测平台直接编译（不定义 ENABLE_*_BACKEND 宏）时的默认目标后端。
// - ARM 赛道提交：保持下面这行被注释（默认 AArch64）；
// - RISC-V 赛道提交：取消注释（默认 RISC-V）。
// #define DEFAULT_BACKEND_ARCH_RISCV 1
