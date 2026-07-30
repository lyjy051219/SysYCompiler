#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

//流程：1. 排除递归 main；2. 在入口安全前缀识别完整全局对象 memcpy；
//3. 把源静态初值复制到目标初始化器；4. 统一删除已静态化的运行期调用。
class GlobalInitializerFoldPass
    : public PassDescriptor<GlobalInitializerFoldPass> {
public:
    //把main入口安全前缀中的完整全局拷贝折叠为静态初始化器。
    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
    //判断main是否通过模块直接调用图递归回到自身。
    static bool isRecursiveMain(Module &M, Function *Main);
    //剥离同形bitcast和全零GEP并返回完整全局对象根。
    static Variable *getWholeObjectGlobal(Value *Pointer);
    //识别尺寸和类型均覆盖完整对象的全局到全局memcpy。
    static bool isFullGlobalCopy(CallInst *CI, Variable *&Destination,
                                 Variable *&Source);
    //把源全局的显式或零初始化内容复制到目标全局初始化器。
    static void copyInitializer(Variable *Destination, Variable *Source);
};
