// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "riscv/MIR.hpp"

#include <cassert>
#include <map>
#include <memory>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

// 后端 MachinePassManager
// 仿照中端 ir/opt/NewPassManager.hpp 实现，结构和用法保持一致：
// CRTP 描述符提供唯一 PassID，类型擦除装入统一容器，分析结果按需计算并缓存，
// 变换 pass 通过返回 PreservationStatus 手动声明哪些分析仍然有效。
// 与中端的区别：IR 单元是 MachineModule 和 MachineFunction。
// 注意：分析缓存以 IR 单元的地址为键，MachineFunction 按值存放在
// MachineModule::functions 里，流水线运行期间不要增删函数。

namespace riscv {

// 每个分析类型的唯一标识，取静态成员的地址作为 ID
struct alignas(8) MachinePassID {};

// 记录一个变换 pass 运行后哪些分析结果仍然可用
class MachinePreservationStatus {
private:
  bool PreservedAll = false;
  std::set<MachinePassID *> PreservedIDs;
  std::set<MachinePassID *> AbandonedIDs;

public:
  // 什么都不保留，代表 IR 被修改过，是最保守的返回值
  static MachinePreservationStatus none() { return MachinePreservationStatus(); }

  // 全部保留，代表 pass 没有修改 IR
  static MachinePreservationStatus all() {
    MachinePreservationStatus PA;
    PA.PreservedAll = true;
    return PA;
  }

  template <typename AnalysisT> void keep() { keep(AnalysisT::ID()); }

  template <typename AnalysisT> void discard() { discard(AnalysisT::ID()); }

  void keep(MachinePassID *ID) {
    AbandonedIDs.erase(ID);
    if (!PreservedAll) {
      PreservedIDs.insert(ID);
    }
  }

  void discard(MachinePassID *ID) {
    PreservedIDs.erase(ID);
    AbandonedIDs.insert(ID);
  }

  bool areAllKept() const { return PreservedAll && AbandonedIDs.empty(); }

  bool isKept(MachinePassID *ID) const {
    if (AbandonedIDs.count(ID)) {
      return false;
    }
    return PreservedAll || PreservedIDs.count(ID);
  }

  // 合并另一个 pass 的保留集合，取两者的交集
  void merge(const MachinePreservationStatus &Arg) {
    if (Arg.areAllKept()) {
      return;
    } 
    if (PreservedAll) {
      *this = Arg;
      return;
    }
    for (auto *id : Arg.AbandonedIDs) {
      PreservedIDs.erase(id);
      AbandonedIDs.insert(id);
    }
    for (auto it = PreservedIDs.begin(); it != PreservedIDs.end();) {
      if (!Arg.PreservedIDs.count(*it)) {
        it = PreservedIDs.erase(it);
      } else {
        ++it;
      }
    }
  }
};

// 分析 pass 的 CRTP 基类，派生类需要定义 static MachinePassID Key 和 Result 类型
template <typename DerivedT> struct MachineAnalysisDescriptor {
  static MachinePassID *ID() { return &DerivedT::Key; }
};

template <typename UnitT> class MachineAnalysisManager;

// 分析结果的类型擦除接口
template <typename UnitT> struct MachineAnalysisResultInterface {
  virtual ~MachineAnalysisResultInterface() = default;
};

template <typename UnitT, typename PassT, typename ResultT>
struct MachineAnalysisResultWrapper : MachineAnalysisResultInterface<UnitT> {
  explicit MachineAnalysisResultWrapper(ResultT Result)
      : Result(std::move(Result)) {}
  ResultT Result;
};

// 分析 pass 的类型擦除接口
template <typename UnitT> struct MachineAnalysisPassInterface {
  virtual ~MachineAnalysisPassInterface() = default;
  virtual std::unique_ptr<MachineAnalysisResultInterface<UnitT>>
  run(UnitT &IR, MachineAnalysisManager<UnitT> &AM) const = 0;
};

template <typename UnitT, typename PassT>
struct MachineAnalysisPassWrapper : MachineAnalysisPassInterface<UnitT> {
  explicit MachineAnalysisPassWrapper(PassT Pass) : Pass(std::move(Pass)) {}
  std::unique_ptr<MachineAnalysisResultInterface<UnitT>>
  run(UnitT &IR, MachineAnalysisManager<UnitT> &AM) const override {
    using ResultWrapperT =
        MachineAnalysisResultWrapper<UnitT, PassT, typename PassT::Result>;
    return std::make_unique<ResultWrapperT>(Pass.run(IR, AM));
  }
  PassT Pass;
};

// 变换 pass 的类型擦除接口
template <typename UnitT> struct MachinePassInterface {
  virtual ~MachinePassInterface() = default;
  virtual MachinePreservationStatus run(UnitT &IR,
                                        MachineAnalysisManager<UnitT> &AM) = 0;
};

template <typename UnitT, typename PassT>
struct MachinePassWrapper : MachinePassInterface<UnitT> {
  explicit MachinePassWrapper(PassT Pass) : Pass(std::move(Pass)) {}
  MachinePreservationStatus run(UnitT &IR,
                                MachineAnalysisManager<UnitT> &AM) override {
    return Pass.run(IR, AM);
  }
  PassT Pass;
};

// 分析管理器：注册分析 pass，按需计算结果并缓存，配合失效机制复用结果
template <typename UnitT> class MachineAnalysisManager {
private:
  using ResultPtr = std::unique_ptr<MachineAnalysisResultInterface<UnitT>>;
  using PassPtr = std::unique_ptr<MachineAnalysisPassInterface<UnitT>>;
  using CacheKey = std::pair<MachinePassID *, UnitT *>;

  std::map<MachinePassID *, PassPtr> AnalysisPasses;
  std::map<CacheKey, ResultPtr> ResultCache;

public:
  MachineAnalysisManager() = default;
  MachineAnalysisManager(MachineAnalysisManager &&) = default;
  MachineAnalysisManager &operator=(MachineAnalysisManager &&) = default;

  void clear() { ResultCache.clear(); }

  // 取分析结果，没有缓存时现算
  template <typename PassT> typename PassT::Result &getResult(UnitT &IR) {
    assert(AnalysisPasses.count(PassT::ID()) && "Pass not registered");
    auto Key = CacheKey{PassT::ID(), &IR};
    auto it = ResultCache.find(Key);
    using WrapperT =
        MachineAnalysisResultWrapper<UnitT, PassT, typename PassT::Result>;
    if (it != ResultCache.end()) {
      return static_cast<WrapperT &>(*it->second).Result;
    }
    auto &Pass = *AnalysisPasses.at(PassT::ID());
    ResultCache[Key] = Pass.run(IR, *this);
    return static_cast<WrapperT &>(*ResultCache[Key]).Result;
  }

  // 只取缓存，没有时返回空指针，适合可选的分析消费者
  template <typename PassT> typename PassT::Result *getCachedResult(UnitT &IR) {
    auto Key = CacheKey{PassT::ID(), &IR};
    auto it = ResultCache.find(Key);
    if (it == ResultCache.end()) {
      return nullptr;
    }
    using WrapperT =
        MachineAnalysisResultWrapper<UnitT, PassT, typename PassT::Result>;
    return &static_cast<WrapperT &>(*it->second).Result;
  }

  // 注册分析 pass，传入一个能构造 pass 的可调用对象
  template <typename PassBuilderT> bool registerPass(PassBuilderT &&PassBuilder) {
    using PassT = std::remove_reference_t<decltype(PassBuilder())>;
    using PassWrapperT = MachineAnalysisPassWrapper<UnitT, PassT>;
    auto &PassPtr = AnalysisPasses[PassT::ID()];
    if (PassPtr) {
      return false;
    }
    PassPtr.reset(new PassWrapperT(std::forward<PassBuilderT>(PassBuilder)()));
    return true;
  }

  // 根据保留集合清除失效的缓存
  void invalidate(UnitT &IR, const MachinePreservationStatus &PA) {
    if (PA.areAllKept()) {
      return;
    }
    for (auto it = ResultCache.begin(); it != ResultCache.end();) {
      if (it->first.second == &IR && !PA.isKept(it->first.first)) {
        it = ResultCache.erase(it);
      } else {
        ++it;
      }
    }
  }
};

using MachineFunctionAnalysisManager = MachineAnalysisManager<MachineFunction>;
using MachineModuleAnalysisManager = MachineAnalysisManager<MachineModule>;

// 变换 pass 管理器：按加入顺序执行，串联失效逻辑
template <typename UnitT> class MachinePassManager {
protected:
  using PassInterfaceT = MachinePassInterface<UnitT>;
  std::vector<std::unique_ptr<PassInterfaceT>> Passes;

public:
  MachinePassManager() = default;
  MachinePassManager(MachinePassManager &&Arg) : Passes(std::move(Arg.Passes)) {}
  MachinePassManager &operator=(MachinePassManager &&RHS) {
    Passes = std::move(RHS.Passes);
    return *this;
  }

  MachinePreservationStatus run(UnitT &IR, MachineAnalysisManager<UnitT> &AM) {
    MachinePreservationStatus PA = MachinePreservationStatus::all();
    for (auto &Pass : Passes) {
      MachinePreservationStatus PassPA = Pass->run(IR, AM);
      AM.invalidate(IR, PassPA);
      PA.merge(PassPA);
    }
    return PA;
  }

  template <typename PassT>
  std::enable_if_t<!std::is_same_v<std::decay_t<PassT>, MachinePassManager>, void>
  addPass(PassT &&Pass) {
    using PassWrapperT = MachinePassWrapper<UnitT, std::decay_t<PassT>>;
    Passes.push_back(std::unique_ptr<PassInterfaceT>(
        new PassWrapperT(std::forward<PassT>(Pass))));
  }

  // 嵌套子管理器，用来给流水线分段
  void addPass(MachinePassManager &&Inner) {
    using PassWrapperT = MachinePassWrapper<UnitT, MachinePassManager>;
    Passes.push_back(
        std::unique_ptr<PassInterfaceT>(new PassWrapperT(std::move(Inner))));
  }

  bool empty() const { return Passes.empty(); }
};

using MachineFunctionPassManager = MachinePassManager<MachineFunction>;
using MachineModulePassManager = MachinePassManager<MachineModule>;

// 固定点包装器：反复执行内部 pass 直到一轮内没有任何修改，
// 或达到最大迭代次数，防止互相抵消的改写造成死循环
template <typename UnitT> class MachineFixedPointPass {
private:
  using PassInterfaceT = MachinePassInterface<UnitT>;
  std::unique_ptr<PassInterfaceT> InnerPass;
  int MaxIterations;

public:
  explicit MachineFixedPointPass(std::unique_ptr<PassInterfaceT> Inner,
                                 int MaxIters = 8)
      : InnerPass(std::move(Inner)), MaxIterations(MaxIters) {}

  MachinePreservationStatus run(UnitT &IR, MachineAnalysisManager<UnitT> &AM) {
    MachinePreservationStatus OverallPA = MachinePreservationStatus::all();
    for (int Iter = 0; Iter < MaxIterations; ++Iter) {
      MachinePreservationStatus IterPA = InnerPass->run(IR, AM);
      AM.invalidate(IR, IterPA);
      OverallPA.merge(IterPA);
      // 一轮下来全部分析都保留，说明没有实质修改，达到固定点
      if (IterPA.areAllKept()) {
        break;
      }
    }
    return OverallPA;
  }
};

template <typename UnitT, typename PassT>
MachineFixedPointPass<UnitT> createMachineFixedPointPass(PassT &&Pass,
                                                         int MaxIters = 8) {
  using PassWrapperT = MachinePassWrapper<UnitT, std::decay_t<PassT>>;
  return MachineFixedPointPass<UnitT>(
      std::unique_ptr<MachinePassInterface<UnitT>>(
          new PassWrapperT(std::forward<PassT>(Pass))),
      MaxIters);
}

// 模块到函数的桥接：把函数级流水线依次作用到模块内每个函数上
class MachineModuleFunctionBridge {
public:
  using FunctionPassInterfaceT = MachinePassInterface<MachineFunction>;

  MachineModuleFunctionBridge(std::unique_ptr<FunctionPassInterfaceT> Pass,
                              MachineFunctionAnalysisManager &MFAM)
      : Pass(std::move(Pass)), FAM(MFAM) {}

  MachinePreservationStatus run(MachineModule &M,
                                MachineModuleAnalysisManager &) {
    MachinePreservationStatus PA = MachinePreservationStatus::all();
    for (MachineFunction &F : M.functions) {
      MachinePreservationStatus FuncPA = Pass->run(F, FAM);
      FAM.invalidate(F, FuncPA);
      if (!FuncPA.areAllKept()) {
        PA = MachinePreservationStatus::none();
      }
    }
    return PA;
  }

private:
  std::unique_ptr<FunctionPassInterfaceT> Pass;
  MachineFunctionAnalysisManager &FAM;
};

template <typename FunctionPassT>
MachineModuleFunctionBridge
createMachineModuleFunctionBridge(FunctionPassT &&Pass,
                                  MachineFunctionAnalysisManager &MFAM) {
  using PassWrapperT =
      MachinePassWrapper<MachineFunction, std::decay_t<FunctionPassT>>;
  return MachineModuleFunctionBridge(
      std::unique_ptr<MachineModuleFunctionBridge::FunctionPassInterfaceT>(
          new PassWrapperT(std::forward<FunctionPassT>(Pass))),
      MFAM);
}

// 把普通函数包装成变换 pass，兼容现有的两类写法：
// 返回 void 的视为总是修改 IR，返回 bool 的按返回值决定是否保留分析
template <typename UnitT, typename FnT> class MachineCallbackPass {
private:
  FnT Fn;

public:
  explicit MachineCallbackPass(FnT Fn) : Fn(std::move(Fn)) {}

  MachinePreservationStatus run(UnitT &IR, MachineAnalysisManager<UnitT> &) {
    if constexpr (std::is_convertible_v<decltype(std::declval<FnT &>()(
                      std::declval<UnitT &>())), bool>) {
      return Fn(IR) ? MachinePreservationStatus::none()
                    : MachinePreservationStatus::all();
    } else {
      Fn(IR);
      return MachinePreservationStatus::none();
    }
  }
};

template <typename UnitT, typename FnT>
MachineCallbackPass<UnitT, std::decay_t<FnT>> createMachineCallbackPass(FnT &&Fn) {
  return MachineCallbackPass<UnitT, std::decay_t<FnT>>(std::forward<FnT>(Fn));
}

} // namespace riscv
