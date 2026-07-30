#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

//候选快照保存目标、自递归调用点和所有返回点，使模块扫描与 CFG 提交阶段分离。
struct FunctionMemoizationCandidate {
    //记录通过全部纯度和签名检查的递归目标函数。
    Function *Target = nullptr;
    //快照目标函数中的全部直接自递归调用点。
    std::vector<CallInst *> RecursiveCalls;
    //快照目标函数中需要发布缓存结果的全部返回点。
    std::vector<RetInst *> Returns;
};

//流程：1. 验证纯标量、NoModRef、多分支递归函数；2. 在模块缓存预算内创建键/结果数组；
//3. 插入 lookup/hit 入口 CFG；4. 在每个原返回点按发布顺序写入直接映射缓存。
class FunctionMemoizationPass
    : public PassDescriptor<FunctionMemoizationPass> {
public:
    //绑定函数级分析管理器并规范化直接映射缓存容量。
    explicit FunctionMemoizationPass(
        FunctionAnalysisPassManager &FAM,
        unsigned CacheEntries = 4096);

    //扫描模块候选并在总缓存预算内为递归纯函数插入记忆化缓存。
    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
    //限制缓存键允许包含的标量参数数量。
    static constexpr unsigned MaxScalarParameters = 4;
    //给出非法配置回退采用的默认缓存槽数量。
    static constexpr unsigned DefaultCacheEntries = 4096;
    //限制单个模块为所有记忆化函数创建的全局缓存总字节数。
    static constexpr std::size_t MaxModuleCacheBytes = 256 * 1024;

    //判断无符号整数是否为非零二次幂。
    static bool isPowerOfTwo(unsigned Value);
    //判断类型是否为缓存键和值支持的32位整数或单精度浮点标量。
    static bool isSupportedScalarType(
        const std::shared_ptr<IRType> &Type);
    //检查指令是否包含会破坏缓存确定性的未定义或不安全运算。
    static bool hasUnsafeOrUndefinedOperation(Instruction *Inst);
    //验证函数的纯度、递归形态和标量语义并建立候选快照。
    static bool collectCandidate(
        Function &F, FunctionAnalysisPassManager &FAM,
        FunctionMemoizationCandidate &Candidate);
    //生成同时避开模块函数与全局变量名称的唯一缓存符号。
    static std::string makeUniqueGlobalName(
        Module &M, const std::string &Stem);
    //在基本块末尾生成指向缓存数组指定槽位的GEP。
    static GetElementPtrInst *appendSlotPointer(
        BasicBlock &Block, Variable *Cache, Value *Index);
    //把整数或浮点参数规范化为32位缓存键比特。
    static Value *appendKeyBits(BasicBlock &Block, Value *Argument);
    //把一个参数键混入当前32位哈希值。
    static Value *appendHashMix(
        BasicBlock &Block, Value *Hash, Value *KeyBits);
    //为单个候选创建缓存全局量、命中入口和返回点发布逻辑。
    bool transformCandidate(
        Module &M, const FunctionMemoizationCandidate &Candidate,
        unsigned CacheOrdinal) const;

    //引用候选纯度查询所需的函数级分析管理器。
    FunctionAnalysisPassManager &FAM;
    //记录规范化后的直接映射缓存槽数量。
    unsigned CacheEntries;
};
