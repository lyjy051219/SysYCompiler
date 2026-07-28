#include "ir/opt/GlobalInitializerFold.hpp"

#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

bool GlobalInitializerFoldPass::isRecursiveMain(Module &M, Function *Main) {
    // 1.1 main 一旦可能被再次调用，运行期拷贝就不再只执行一次，不能提升为静态初值。
    if (!Main) return true;
    for (CallInst *CI : collectInstructions<CallInst>(M))
        if (CI->getDirectCallee() == Main) return true;
    return false;
}

Variable *GlobalInitializerFoldPass::getWholeObjectGlobal(Value *Pointer) {
    // 2.1 只剥离 bitcast 和全零 GEP；任一非零索引都意味着只指向子对象而非完整全局量。
    std::unordered_set<Value *> Seen;
    while (Pointer && Seen.insert(Pointer).second) {
        if (auto *BC = dynamic_cast<BitCastInst *>(Pointer)) {
            Pointer = BC->getUnaryOperand();
            continue;
        }
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(Pointer)) {
            for (Value *Index : GEP->getIndices()) {
                int ConstantIndex = 0;
                if (!Value::tryGetIntIndex(Index, ConstantIndex) ||
                    ConstantIndex != 0)
                    return nullptr;
            }
            Pointer = GEP->getBasePointer();
            continue;
        }
        break;
    }
    return dynamic_cast<Variable *>(Pointer);
}

bool GlobalInitializerFoldPass::isFullGlobalCopy(
    CallInst *CI, Variable *&Destination, Variable *&Source) {
    // 2.2 将 memcpy 约束为两个不同的同型全局对象，并要求常量长度精确覆盖整个对象。
    Destination = nullptr;
    Source = nullptr;
    if (!CI || !CI->isNonVolatileMemcpy() || CI->getArgCount() < 4)
        return false;

    Destination = getWholeObjectGlobal(CI->getArg(0));
    Source = getWholeObjectGlobal(CI->getArg(1));
    auto *ByteCount = dynamic_cast<ConstInt *>(CI->getArg(2));
    if (!Destination || !Source || !ByteCount || Destination == Source)
        return false;
    if (Destination->vartag != Variable::GlobalVariable)
        return false;
    if (Source->vartag != Variable::GlobalVariable &&
        Source->vartag != Variable::GlobalConstant)
        return false;

    auto DestTy = Destination->getPointeeType();
    auto SourceTy = Source->getPointeeType();
    if (!DestTy || !SourceTy || !DestTy->hasSameShape(*SourceTy))
        return false;
    if (DestTy->getSize() >
        static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        return false;
    return ByteCount->getVal() >= 0 &&
           static_cast<size_t>(ByteCount->getVal()) == DestTy->getSize();
}

void GlobalInitializerFoldPass::copyInitializer(Variable *Destination,
                                                 Variable *Source) {
    // 3.1 把源对象的静态初值转移到目标；无显式源初值对应目标的零初始化表示。
    if (!Destination || !Source) return;
    Value *SourceInit = Source->getUsee();
    if (!SourceInit) {
         
        Destination->clearOperands();
        return;
    }
    if (Destination->getNumOperands() == 0)
        Destination->addUse(SourceInit);
    else
        Destination->replaceOperand(0, SourceInit);
}

PreservationStatus GlobalInitializerFoldPass::run(
    Module &M, ModuleAnalysisPassManager &MAM) {
    (void)MAM;
    // 1. 仅处理非递归 main 的入口块，保证被折叠的初始化动作在程序启动时执行一次。
    Function *Main = M.getMainFunction();
    if (!Main || Main->size() == 0 || isRecursiveMain(M, Main))
        return PreservationStatus::all();

    BasicBlock *Entry = Main->front();
    if (!Entry) return PreservationStatus::all();

     
     
     
    // 2. 顺序扫描入口前缀：在任何可能观察或改变内存的普通 load/store/call 之前，
    // 识别完整全局对象 memcpy；首个不匹配的内存操作之后不再折叠，保持执行顺序语义。
    bool SafePrefix = true;
    std::vector<CallInst *> FoldedCalls;
    for (Instruction *I : *Entry) {
        if (!I) continue;
        if (auto *CI = dynamic_cast<CallInst *>(I)) {
            Variable *Destination = nullptr;
            Variable *Source = nullptr;
            if (SafePrefix &&
                isFullGlobalCopy(CI, Destination, Source)) {
                copyInitializer(Destination, Source);
                FoldedCalls.push_back(CI);
                continue;
            }
            SafePrefix = false;
            continue;
        }
        if (dynamic_cast<LoadInst *>(I) || dynamic_cast<StoreInst *>(I))
            SafePrefix = false;
    }

    // 3. 候选在扫描时只更新初始化器，最后统一删除对应调用，避免遍历期间迭代器失效。
    if (FoldedCalls.empty()) return PreservationStatus::all();
    for (CallInst *CI : FoldedCalls) delete CI;
    return PreservationStatus::none();
}
