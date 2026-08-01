#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

/*GlobalInitializerFold：主要在Local2Global后使用,
当Local2Global将大尺寸对象的alloca提升为global之后,
GlobalInitializerFold将memcpy消除，从而将初始化时间从运行期改到静态装载期*/
class GlobalInitializerFoldPass
    : public PassDescriptor<GlobalInitializerFoldPass> {
public:
    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);
private:
    //剥离bitcast和全零GEP，得到完整全局对象指针
    static Variable *getWholeObjectGlobal(Value *Pointer);
    //识别初始化memcpy（长度覆盖整个对象）
    static bool isFullGlobalCopy(CallInst *CI, Variable *&Destination,Variable *&Source);
    //把源全局的显式或零初始化内容复制到目标全局初始化器
    static void copyInitializer(Variable *Destination, Variable *Source);
};
