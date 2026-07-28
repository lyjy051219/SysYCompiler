#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "lib/CFG.hpp"
#include <cstddef>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <vector>

class Loop;

// ModRefInfo 是查询结果格，两个低位分别表示可能写和可能读。
enum class ModRefInfo : unsigned {
    NoModRef = 0,
    Ref      = 1,
    Mod      = 2,
    ModRef   = 3
};
bool isModSet(ModRefInfo MRI);
bool isRefSet(ModRefInfo MRI);
bool isNoModRef(ModRefInfo MRI);
ModRefInfo unionModRef(ModRefInfo A, ModRefInfo B);

// FuncAttr 对内置函数的内存行为分类，未知类别必须按读写屏障处理。
enum class FuncAttr : unsigned {
    None         = 0,
    Sylib        = 1u << 0,
    NoModRef     = 1u << 1,
    MemReadOnly  = 1u << 2,
    MemWriteOnly = 1u << 3,
    MemReadWrite = (1u << 2) | (1u << 3),
    Untracked    = 1u << 4,
};

enum class IntrinsicID {
    None   = 0,
    Memcpy = 1,
    Memset = 2,
    Memmove = 3,
};

// 指针来源摘要保存所有可能基址；untracked 表示还可能来自集合之外。
struct BaseAddressInfo {
    std::unordered_set<Value*> bases;
    bool untracked = false;

    bool merge(const BaseAddressInfo& Other);
    bool isEmpty() const;
    void collectExternalBases(std::unordered_set<Value*> &Dest) const;
};

using BaseAddressInfoMap = std::unordered_map<Value*, BaseAddressInfo>;

// 函数摘要记录传递闭包上的外部读写集合、保守屏障标志和本地指针来源。
struct SideEffectInfo {
    std::unordered_set<Value*> ModSet;
    std::unordered_set<Value*> RefSet;
    bool hasBuiltin = false;

    BaseAddressInfoMap ptrMap;

    bool invalidate(Function &F, const PreservationStatus &PA);
};

// 固定点状态把本地摘要与尚待映射的直接调用边分开保存。
struct SideEffectPendingCall {
    Function *Callee = nullptr;
    std::vector<Value *> Args;
};

struct SideEffectFunctionState {
    SideEffectInfo Info;
    std::vector<SideEffectPendingCall> Calls;
};

// 分析入口先构建可达调用图的局部摘要，再迭代传播被调函数效果并统一缓存。
class SideEffectAnalysis : public AnalysisDescriptor<SideEffectAnalysis> {
public:
    using Result = SideEffectInfo;

    static constexpr bool InvalidateAcrossIRUnits = true;

    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

    static MemoryLocation getUnknownSizeMemoryLocation(Value *Pointer);
    static std::optional<std::size_t> getConstantMemoryIntrinsicLength(
        const std::vector<Value *> &Args);

private:
    friend AnalysisDescriptor<SideEffectAnalysis>;
    static PassID Key;
    static void ensureInitialized(Value *V, BaseAddressInfoMap &Map);
    static BaseAddressInfoMap buildBaseAddressInfoMap(Function &F);
    static void processBuiltinCall(
        BuildInFunction *BIF, const std::vector<Value*> &Args,
        const BaseAddressInfoMap &CallerPtrMap, SideEffectInfo &Info);
};

FuncAttr    classifyBuiltin(BuildInFunction* BIF);
IntrinsicID getIntrinsicID(BuildInFunction* BIF);

// 指令级查询将目标内存区间与 Load、Store 或 Call 的影响求交。
ModRefInfo getInstModRefInfo(const MemoryLocation &Location, Instruction *I,
                             BasicAAResult &AA, const LoopInfo &LI,
                             FunctionAnalysisPassManager &FAM);
ModRefInfo getInstModRefInfo(Value* V, Instruction* I,
                             BasicAAResult& AA, const LoopInfo& LI,
                             FunctionAnalysisPassManager& FAM);

// 聚合查询分别把函数、基本块和循环摘要转换成变换可消费的保守结论。
ModRefInfo getFunctionModRefInfo(const SideEffectInfo& SEI);

// 返回调用对 SysY 程序内存的整体影响；可观察性和计时边界由 CFG 接口独立维护。
ModRefInfo getCallProgramModRefInfo(
    CallInst *CI, FunctionAnalysisPassManager &FAM);

bool BasicBlockSideEffect(BasicBlock* BB,
                               const Loop& L,
                               BasicAAResult& AA,
                               const LoopInfo& LI,
                               FunctionAnalysisPassManager& FAM);

bool LoopSideEffect(const Loop& L,
                         BasicAAResult& AA,
                         const LoopInfo& LI,
                         FunctionAnalysisPassManager& FAM);
