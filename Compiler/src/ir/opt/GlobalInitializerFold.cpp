#include "ir/opt/GlobalInitializerFold.hpp"
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>
//剥离bitcast和全零GEP，得到完整全局对象指针
Variable *GlobalInitializerFoldPass::getWholeObjectGlobal(Value *Pointer) {
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
//识别初始化memcpy（长度覆盖整个对象）
bool GlobalInitializerFoldPass::isFullGlobalCopy(
    CallInst *CI, Variable *&Destination, Variable *&Source) {
    Destination = nullptr;
    Source = nullptr;
    //事实上我们的编译器也不会出现volatile的memcpy，这里只是为了保险起见
    if (!CI || !CI->isNonVolatileMemcpy() || CI->getArgCount() < 4)
        return false;
    //剥离bitcast和全零GEP，得到完整全局对象指针
    Destination = getWholeObjectGlobal(CI->getArg(0));
    Source = getWholeObjectGlobal(CI->getArg(1));
    auto *ByteCount = dynamic_cast<ConstInt *>(CI->getArg(2));
    if (!Destination || !Source || !ByteCount || Destination == Source)
        return false;
    //目标必须是全局变量，源可以是全局变量或全局常量，符合memcpy的语义
    if (Destination->vartag != Variable::GlobalVariable)
        return false;
    if (Source->vartag != Variable::GlobalVariable &&
        Source->vartag != Variable::GlobalConstant)
        return false;
    auto DestTy = Destination->getPointeeType();
    auto SourceTy = Source->getPointeeType();
    if (!DestTy || !SourceTy || !DestTy->hasSameShape(*SourceTy))
        return false;
    //目标对象大小不能超过int32_t的最大值，避免memcpy参数溢出
    if (DestTy->getSize() >
        static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        return false;
    //长度必须覆盖整个对象，才能保证memcpy的语义是完整初始化
    return ByteCount->getVal() >= 0 &&
           static_cast<size_t>(ByteCount->getVal()) == DestTy->getSize();
}
//把源全局的显式或零初始化内容复制到目标全局初始化器
void GlobalInitializerFoldPass::copyInitializer(Variable *Destination,
                                                 Variable *Source) {
    if (!Destination || !Source) return;
    //获得初始化值
    Value *SourceInit = Source->getUsee();
    if (!SourceInit) {
        //如果没有初始化值，为0初始化
        Destination->clearOperands();
        return;
    }
    //其他初始化
    if (Destination->getNumOperands() == 0)
        Destination->addUse(SourceInit);
    else
        //0初始化
        Destination->replaceOperand(0, SourceInit);
}
PreservationStatus GlobalInitializerFoldPass::run(Module &M, ModuleAnalysisPassManager &MAM) {
    (void)MAM;
    //1.仅处理非递归 main 的入口块，保证被折叠的初始化动作在程序启动时执行一次
    Function *Main = M.getMainFunction();
    if (!Main || Main->size() == 0)
        return PreservationStatus::all();
    FunctionCallGraph CallGraph = M.buildDirectCallGraph();
    if (Module::isFunctionRecursive(Main, CallGraph))
        return PreservationStatus::all();
    BasicBlock *Entry = Main->front();
    if (!Entry) return PreservationStatus::all();
    //2.扫描入口块，识别可折叠的全局到全局memcpy调用，然后把初始值移动到目标全局对象
    //SafePrefix：在当前扫描位置之前没有遇到可能Mod/Ref的load/store/不能折叠的call
    bool SafePrefix = true;
    //FoldedCalls：记录所有可折叠的全局到全局memcpy调用
    std::vector<CallInst *> FoldedCalls;
    for (Instruction *I : *Entry) {
        if (!I) continue;
        if (auto *CI = dynamic_cast<CallInst *>(I)) {
            Variable *Destination = nullptr;
            Variable *Source = nullptr;
            if (SafePrefix && isFullGlobalCopy(CI, Destination, Source)) {
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
    if (FoldedCalls.empty()) return PreservationStatus::all();
    //3.删除memcpy
    for (CallInst *CI : FoldedCalls) delete CI;
    return PreservationStatus::none();
}
