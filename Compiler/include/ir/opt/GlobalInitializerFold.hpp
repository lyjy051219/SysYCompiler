#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// 流程：1. 排除递归 main；2. 在入口安全前缀识别完整全局对象 memcpy；
// 3. 把源静态初值复制到目标初始化器；4. 统一删除已静态化的运行期调用。
class GlobalInitializerFoldPass
    : public PassDescriptor<GlobalInitializerFoldPass> {
public:
    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
    static bool isRecursiveMain(Module &M, Function *Main);
    static Variable *getWholeObjectGlobal(Value *Pointer);
    static bool isFullGlobalCopy(CallInst *CI, Variable *&Destination,
                                 Variable *&Source);
    static void copyInitializer(Variable *Destination, Variable *Source);
};
