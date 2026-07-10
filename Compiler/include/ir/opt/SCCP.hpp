#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <set>
#include <variant>

//少了位宽的区分，向量优化，基础代数化简
// 格值 (Lattice Value)--三态格模型
// Top(未知/未定义) → Constant(常量) → Bottom(过定义/非常量)
class LatticeValue {
public:
    enum State { Top, Constant, Bottom };

private:
    State state;
    Value* constVal; // 仅当 state == Constant 时有效

public:
    LatticeValue() : state(Top), constVal(nullptr) {}
    //创建三个状态的静态函数
    static LatticeValue getTop() { return LatticeValue(); }
    static LatticeValue getBottom() {
        LatticeValue lv;
        lv.state = Bottom;
        return lv;
    }
    static LatticeValue getConstant(Value* v) {
        LatticeValue lv;
        lv.state = Constant;
        lv.constVal = v;
        return lv;
    }

    bool isTop() const { return state == Top; }
    bool isBottom() const { return state == Bottom; }
    bool isConstant() const { return state == Constant; }
    State getState() const { return state; }

    Value* getConstantValue() const { return constVal; }

    // 格合并
    // Top ∧ x = x
    // Bottom ∧ x = Bottom
    // Const(a) ∧ Const(a) = Const(a)
    // Const(a) ∧ Const(b) = Bottom (a != b)
    bool mergeIn(const LatticeValue& other) {
        if (other.isTop()) return false;        // Top 不改变任何值
        if (isBottom()) return false;            // 已经是 Bottom，不会再变
        if (other.isBottom()) {
            state = Bottom;
            constVal = nullptr;
            return true;
        }
        // other 是 Constant
        if (isTop()) {
            state = Constant;
            constVal = other.constVal;
            return true;
        }
        // 都是 Constant，看值是否相同
        if (constVal == other.constVal) return false;

        // 两个不同的常量 → 值比较（ConstInt/ConstFloat/ConstBool 可能是不同指针但相同值）
        if (Value::areEqualConstants(constVal, other.constVal)) return false;

        state = Bottom;
        constVal = nullptr;
        return true;
    }

};

// SCCP 求解器 (Solver)
class SCCPSolver {
public:
    SCCPSolver() = default;

    // 标记一个基本块为可执行
    void markBlockExecutable(BasicBlock* BB);

    // 标记一条边 (Source → Dest) 为可达
    bool markEdgeFeasible(BasicBlock* Source, BasicBlock* Dest);

    // 核心求解循环
    void solve();

    // 查询接口
    bool isBlockExecutable(BasicBlock* BB) const;
    bool isEdgeFeasible(BasicBlock* From, BasicBlock* To) const;
    LatticeValue getLatticeValue(Value* V) const;

    // 应用变换：将常量替换到 IR 中，返回是否有改动
    bool rewriteConstants(Function& F, bool &CfgChanged);

    // 格值合并，如果发生变化则通知使用者
    bool mergeInValue(Value* V, const LatticeValue& NewVal);

private:
    // 格值映射
    std::unordered_map<Value*, LatticeValue> ValueState;

    // 可达基本块集合
    std::unordered_set<BasicBlock*> ExecutableBlocks;
    // 可达边集合
    std::set<std::pair<BasicBlock*, BasicBlock*>> FeasibleEdges;

    // 工作队列
    std::vector<BasicBlock*> BBWorkList;
    std::vector<Instruction*> InstWorkList;

    // 获取/更新格值
    LatticeValue& getValueState(Value* V);

    // 将值的使用者压入 InstWorkList
    void pushUsersToWorkList(Value* V);



    // 指令访问者
    void visitInstruction(Instruction* I);
    void visitPhiNode(PhiInst* Phi);
    void visitBinaryInst(BinaryInst* BI);
    void visitBranchInst(BrInst* Br);
    void visitRetInst(RetInst* Ret);
    void visitLoadInst(LoadInst* LI);
    void visitStoreInst(StoreInst* SI);
    void visitCallInst(CallInst* CI);
    void visitCastInst(Instruction* I); // SITOFP, FPTOSI, Zext
    void visitGEPInst(GetElementPtrInst* GEP);

    // 获取可行的后继块
    void getFeasibleSuccessors(BrInst* Br,
                               std::vector<bool>& Succs);

    // Select 指令访问者
    void visitSelectInst(Instruction* I);
};

// SCCP Pass
class SCCPPass : public PassDescriptor<SCCPPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
