#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <set>
#include <variant>
//BasicAAResult提供稀疏内存常量传播所需的别名证明
class BasicAAResult;
//LoopInfo提供带索引范围的内存位置判定所需的循环信息
class LoopInfo;
//MemorySSAResult提供每条控制流路径上的首个内存写定义
class MemorySSAResult;

//LatticeValue表示SCCP沿Top、Constant、Bottom单调下降的值格
class LatticeValue {
public:
    //State枚举SCCP值格的三种状态
    enum State {
        //Top表示尚无可执行路径为该值提供信息
        Top,
        //Constant表示全部已知可行路径给出同一常量
        Constant,
        //Bottom表示该值已经过定义而不能继续常量化
        Bottom
    };

private:
    //state记录当前值格状态
    State state;
    //constVal在Constant状态下指向对应IR常量
    Value* constVal;

public:
    //构造尚无信息的Top格值
    LatticeValue();
    //创建Top格值
    static LatticeValue getTop();
    //创建Bottom格值
    static LatticeValue getBottom();
    //创建保存给定IR常量的Constant格值
    static LatticeValue getConstant(Value* v);

    //判断当前格值是否为Top
    bool isTop() const;
    //判断当前格值是否为Bottom
    bool isBottom() const;
    //判断当前格值是否为Constant
    bool isConstant() const;
    //返回当前格值状态
    State getState() const;

    //返回Constant状态保存的IR常量
    Value* getConstantValue() const;

    //合并新可行路径的格值并在当前格值下降时返回真
    bool mergeIn(const LatticeValue& other);
};

//SCCPSolver联合求解可行CFG边、可执行块和SSA值格
class SCCPSolver {
public:
    //构造尚未绑定函数和内存分析的空SCCP求解器
    SCCPSolver();
    //构造求解器并预建可证明的Store到Load稀疏内存依赖
    SCCPSolver(Function &F, MemorySSAResult &MSSA, BasicAAResult &AA,
               const LoopInfo &LI, FunctionAnalysisPassManager &FAM);

    //首次发现基本块可执行时把它加入块工作队列
    void markBlockExecutable(BasicBlock* BB);

    //记录一条新可行CFG边并调度目标块或其PHI
    bool markEdgeFeasible(BasicBlock* Source, BasicBlock* Dest);

    //交替处理块和指令工作队列直到稀疏传播收敛
    void solve();

    //判断基本块是否已经证明可执行
    bool isBlockExecutable(BasicBlock* BB) const;
    //判断给定CFG边是否已经证明可行
    bool isEdgeFeasible(BasicBlock* From, BasicBlock* To) const;
    //查询IR值当前的SCCP格值
    LatticeValue getLatticeValue(Value* V) const;

    //提交常量替换、不可达块清理和已知条件分支折叠
    bool rewriteConstants(Function& F, bool &CfgChanged);

    //把新格值合并到IR值并在下降时调度全部指令用户
    bool mergeInValue(Value* V, const LatticeValue& NewVal);

private:
    //ValueState把IR值映射到当前单调求解格值
    std::unordered_map<Value*, LatticeValue> ValueState;

    //ExecutableBlocks保存已经证明可执行的基本块
    std::unordered_set<BasicBlock*> ExecutableBlocks;
    //FeasibleEdges保存已经证明可行的CFG有向边
    std::set<std::pair<BasicBlock*, BasicBlock*>> FeasibleEdges;

    //BBWorkList保存首次变为可执行且等待扫描的基本块
    std::vector<BasicBlock*> BBWorkList;
    //InstWorkList保存因格值或入边变化而需要重访的指令
    std::vector<Instruction*> InstWorkList;

    //LoadStoredValues记录经MemorySSA和AA证明的Load共同到达写入值
    std::unordered_map<LoadInst *, Value *> LoadStoredValues;
    //StoreLoadDependents记录Store写入值变化时需要重访的Load
    std::unordered_map<StoreInst *, std::vector<LoadInst *>>
        StoreLoadDependents;

    //按需取得IR值的可变格状态并初始化常量或全局值边界
    LatticeValue& getValueState(Value* V);

    //把给定IR值的全部指令用户加入重访队列
    void pushUsersToWorkList(Value* V);
    //预建满足同址完整覆盖和共同写入值证明的Load内存依赖
    void initializeMemoryDependencies(
        Function &F, MemorySSAResult &MSSA, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM);

    //按照具体指令种类分发一次稀疏传播访问
    void visitInstruction(Instruction* I);
    //合并PHI全部可行入边对应的值格
    void visitPhiNode(PhiInst* Phi);
    //传播二元运算的恒等式和常量折叠结果
    void visitBinaryInst(BinaryInst* BI);
    //根据条件格值标记分支的可行后继边
    void visitBranchInst(BrInst* Br);
    //处理不产生可传播结果的返回指令
    void visitRetInst(RetInst* Ret);
    //传播经稀疏内存模型证明的Load共同写入值
    void visitLoadInst(LoadInst* LI);
    //在Store写入值变化后重新调度依赖Load
    void visitStoreInst(StoreInst* SI);
    //把有返回值但未建模的调用结果保守降为Bottom
    void visitCallInst(CallInst* CI);
    //传播支持的常量转换或保守降为Bottom
    void visitCastInst(Instruction* I);
    //把当前未建模的GEP结果保守降为Bottom
    void visitGEPInst(GetElementPtrInst* GEP);

    //根据分支条件格值填写两个后继是否可行
    void getFeasibleSuccessors(BrInst* Br,
                               std::vector<bool>& Succs);

    //根据条件格值选择或合并select两臂的值格
    void visitSelectInst(Instruction* I);
};

//SCCPPass运行稀疏条件常量传播并清理不可达控制流
class SCCPPass : public PassDescriptor<SCCPPass> {
public:
    //运行SCCP并按是否改写CFG返回分析保留状态
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
