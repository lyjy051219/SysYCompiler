#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <set>
#include <variant>
class BasicAAResult;
class LoopInfo;
class MemorySSAResult;

// SCCP 使用单调三态格：Top 表示尚无可执行路径提供信息，Constant 表示所有
// 已知可行路径给出同一常量，Bottom 表示过定义。合并只能沿
// Top -> Constant -> Bottom 下降，因此工作队列必然收敛。
class LatticeValue {
public:
    enum State { Top, Constant, Bottom };

private:
    State state;
    Value* constVal;

public:
    LatticeValue();
    static LatticeValue getTop();
    static LatticeValue getBottom();
    static LatticeValue getConstant(Value* v);

    bool isTop() const;
    bool isBottom() const;
    bool isConstant() const;
    State getState() const;

    Value* getConstantValue() const;

    // 合并来自新可行路径的信息；只有格值下降时返回 true。
    bool mergeIn(const LatticeValue& other);

};

// 求解阶段同时传播“边是否可行”和“SSA 值处于哪个格状态”：
// 1. 新可执行块进入块队列，新可行边使目标 PHI 重新求值；
// 2. 值格下降时把其指令使用者压入指令队列；
// 3. 两个队列迭代至不动点后，统一提交常量替换和 CFG 清理。
class SCCPSolver {
public:
    SCCPSolver();
    SCCPSolver(Function &F, MemorySSAResult &MSSA, BasicAAResult &AA,
               const LoopInfo &LI, FunctionAnalysisPassManager &FAM);

    void markBlockExecutable(BasicBlock* BB);

    bool markEdgeFeasible(BasicBlock* Source, BasicBlock* Dest);

    void solve();

    bool isBlockExecutable(BasicBlock* BB) const;
    bool isEdgeFeasible(BasicBlock* From, BasicBlock* To) const;
    LatticeValue getLatticeValue(Value* V) const;

    // 服务步骤 3：先替换可达块常量，再修复并删除不可达块，最后折叠已知
    // 条件分支；CfgChanged 区分纯 SSA 改写与 CFG 改写。
    bool rewriteConstants(Function& F, bool &CfgChanged);

    bool mergeInValue(Value* V, const LatticeValue& NewVal);

private:
    std::unordered_map<Value*, LatticeValue> ValueState;

    std::unordered_set<BasicBlock*> ExecutableBlocks;
    std::set<std::pair<BasicBlock*, BasicBlock*>> FeasibleEdges;

    std::vector<BasicBlock*> BBWorkList;
    std::vector<Instruction*> InstWorkList;

    // Load 只有在每条MemorySSA路径的首个写均为“同址、完整覆盖、同一SSA值”
    // 的Store时才进入稀疏常量内存模型；反向表负责在写入值退化时重访Load。
    std::unordered_map<LoadInst *, Value *> LoadStoredValues;
    std::unordered_map<StoreInst *, std::vector<LoadInst *>>
        StoreLoadDependents;

    LatticeValue& getValueState(Value* V);

    void pushUsersToWorkList(Value* V);
    void initializeMemoryDependencies(
        Function &F, MemorySSAResult &MSSA, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM);

    // 服务求解步骤 1—2：各访问器只根据当前可行边和操作数格值产生更低的
    // 格值或新可行边，不直接修改 IR。
    void visitInstruction(Instruction* I);
    void visitPhiNode(PhiInst* Phi);
    void visitBinaryInst(BinaryInst* BI);
    void visitBranchInst(BrInst* Br);
    void visitRetInst(RetInst* Ret);
    void visitLoadInst(LoadInst* LI);
    void visitStoreInst(StoreInst* SI);
    void visitCallInst(CallInst* CI);
    void visitCastInst(Instruction* I);
    void visitGEPInst(GetElementPtrInst* GEP);

    void getFeasibleSuccessors(BrInst* Br,
                               std::vector<bool>& Succs);

    void visitSelectInst(Instruction* I);
};

// Pass 入口负责初始化边界条件、运行求解器，并按 CFG 是否变化声明分析失效。
class SCCPPass : public PassDescriptor<SCCPPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
