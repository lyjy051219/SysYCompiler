#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SideEffect.hpp"
#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>
//MemoryAccess一些基本接口
MemoryAccess::MemoryAccess(Kind K, unsigned AccessID, BasicBlock *BB,
                           Instruction *I)
    : AccessKind(K), ID(AccessID), Block(BB), Inst(I) {}
MemoryAccess::Kind MemoryAccess::getKind() const {return AccessKind;}
bool MemoryAccess::isLiveOnEntry() const {return AccessKind == Kind::LiveOnEntry;}
bool MemoryAccess::isPhi() const {return AccessKind == Kind::Phi;}
bool MemoryAccess::isUse() const {return AccessKind == Kind::Use;}
bool MemoryAccess::isDef() const {return AccessKind == Kind::Def;}
unsigned MemoryAccess::getID() const {return ID;}
BasicBlock *MemoryAccess::getBlock() const {return Block;}
Instruction *MemoryAccess::getInstruction() const {return Inst;}
MemoryAccess *MemoryAccess::getDefiningAccess() const {return DefiningAccess;}
const std::vector<std::pair<BasicBlock *, MemoryAccess *>> &
MemoryAccess::getIncomingAccesses() const {return IncomingAccesses;}
const std::vector<MemoryAccess *> &MemoryAccess::getUsers() const {
    return Users;
}
//MemorySSAResult一些基本接口
MemorySSAResult::MemorySSAResult() = default;
MemorySSAResult::MemorySSAResult(MemorySSAResult &&) noexcept = default;
MemorySSAResult &MemorySSAResult::operator=(MemorySSAResult &&) noexcept = default;
MemoryAccess *MemorySSAResult::getLiveOnEntry() const {return LiveOnEntry;}
MemoryAccess *MemorySSAResult::getMemoryAccess(Instruction *I) const {
    auto It = InstructionAccesses.find(I);
    return It == InstructionAccesses.end() ? nullptr : It->second;
}
MemoryAccess *MemorySSAResult::getBlockEntry(BasicBlock *BB) const {
    auto It = BlockEntries.find(BB);
    return It == BlockEntries.end() ? nullptr : It->second;
}
MemoryAccess *MemorySSAResult::getBlockExit(BasicBlock *BB) const {
    auto It = BlockExits.find(BB);
    return It == BlockExits.end() ? nullptr : It->second;
}
const std::vector<MemoryAccess *> &
MemorySSAResult::getBlockAccesses(BasicBlock *BB) const {
    static const std::vector<MemoryAccess *> Empty;
    auto It = BlockAccesses.find(BB);
    return It == BlockAccesses.end() ? Empty : It->second;
}
//返回该函数整张MemorySSA图的节点数
std::size_t MemorySSAResult::size() const {return OwnedAccesses.size();}
//创建一个新的MemoryAccess节点，并加入整张MemorySSA图
MemoryAccess *MemorySSAResult::createAccess(MemoryAccess::Kind K,
                                            BasicBlock *BB,
                                            Instruction *I) {
    const unsigned ID = static_cast<unsigned>(OwnedAccesses.size());
    OwnedAccesses.emplace_back(new MemoryAccess(K, ID, BB, I));
    return OwnedAccesses.back().get();
}

//建立正向使用边：某个内存状态 → 后续使用该状态的节点
void MemorySSAResult::addUser(MemoryAccess *Definition,
                              MemoryAccess *User) {
    if (!Definition || !User) return;
    if (!User->isPhi() ||
        std::find(Definition->Users.begin(), Definition->Users.end(), User) ==
            Definition->Users.end())
        Definition->Users.push_back(User);
}

//检查MemoryDef是否可能修改内存访问位置(MemoryLocation)
bool MemorySSAResult::mayClobber(
    MemoryAccess *Access, const MemoryLocation &Location,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) const {
    /*
    struct MemoryLocation {
    Value *Pointer = nullptr;
    std::optional<std::size_t> Size;
    };
    */
    //1.获得内存访问位置的起始地址指针
    Value *Ptr = Location.Pointer;
    if (!Access || !Access->isDef() || !Ptr) return true;
    //2.获得MemoryDef对应的IR指令（Store/Call）
    Instruction *I = Access->getInstruction();
    if (!I) return true;
    //3.Store 使用带访问宽度的别名查询，只有明确 NoAlias 才可跳过。
    if (auto *SI = I->as<StoreInst>()) {
        Value *StorePtr = SI->getPointerOperand();
        //MemoryDef->MemoryLocation
        //两个MemoryLocation进行比较 -- 最终落入到BasicAA分析
        return !StorePtr ||
               AA.aliasWithIndexRange(Location, MemoryLocation::get(SI), LI) !=
                   AliasInfo::NoAlias;
    }
    //3.Call 使用跨函数固定点 ModRef 摘要；未知指针来源已在摘要中保守表示。
    if (I->as<CallInst>()) {
        //依旧是MemoryDef->MemoryLocation
        //两个MemoryLocation进行比较 -- 最终落入到BasicAA分析
        ModRefInfo MRI = getInstModRefInfo(Location, I, AA, LI, FAM);
        return isModSet(MRI);
    }
    //4.构建器尚未识别的新MemoryDef种类一律视作潜在写入
    return true;
}

MemoryAccess *MemorySSAResult::getClobberingAccess(
    Instruction *At, Value *Ptr, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    MemoryAccess *MA = getMemoryAccess(At);
    if (!MA) return nullptr;
    return getClobberingAccess(
        MA->getDefiningAccess(), MemoryLocation(Ptr, std::nullopt), AA, LI,
        FAM, MaxVisitedAccesses);
}
MemoryAccess *MemorySSAResult::getClobberingAccess(
    Instruction *At, const MemoryLocation &Location, BasicAAResult &AA,
    const LoopInfo &LI, FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    MemoryAccess *MA = getMemoryAccess(At);
    if (!MA) return nullptr;
    return getClobberingAccess(MA->getDefiningAccess(), Location, AA, LI,
                               FAM, MaxVisitedAccesses);
}
MemoryAccess *MemorySSAResult::getClobberingAccess(
    MemoryAccess *Start, Value *Ptr, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    return getClobberingAccess(
        Start, MemoryLocation(Ptr, std::nullopt), AA, LI, FAM,
        MaxVisitedAccesses);
}

MemoryClobberResult MemorySSAResult::getClobberingAccesses(
    Instruction *At, const MemoryLocation &Location, BasicAAResult &AA,
    const LoopInfo &LI, FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    MemoryAccess *MA = getMemoryAccess(At);
    if (!MA) return {};
    return getClobberingAccesses(MA->getDefiningAccess(), Location, AA, LI,
                                 FAM, MaxVisitedAccesses);
}

//核心查询1：从后向前收集每条路径上最后一条可能改变查询位置的内存访问节点
MemoryClobberResult MemorySSAResult::getClobberingAccesses(
    MemoryAccess *Start, const MemoryLocation &Location, BasicAAResult &AA,
    const LoopInfo &LI, FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    MemoryClobberResult Result;
    if (!Start || !Location.Pointer || MaxVisitedAccesses == 0)
        return Result;

    std::vector<MemoryAccess *> Worklist{Start};
    std::unordered_set<MemoryAccess *> Visited;
    std::unordered_set<MemoryAccess *> SeenClobbers;

    while (!Worklist.empty()) {
        MemoryAccess *Current = Worklist.back();
        Worklist.pop_back();
        if (!Current) return {};
        if (!Visited.insert(Current).second) continue;
        if (Visited.size() > MaxVisitedAccesses) {
            return {};
        }
        //遇到LiveOnEntry：说明反向路径已经到达函数入口
        if (Current->isLiveOnEntry()) {
            if (SeenClobbers.insert(Current).second)
                Result.Clobbers.push_back(Current);
            continue;
        }
        //遇到Def（Store/Call）：通过BasicAA分析查询位置和MemoryDef地址是否重叠
        //如果重叠，说明这个MemoryDef可能改变这个位置，加入FirstClobbers
        if (Current->isDef()) {
            if (mayClobber(Current, Location, AA, LI, FAM)) {
                if (SeenClobbers.insert(Current).second)
                    Result.Clobbers.push_back(Current);
                continue;
            }
            Worklist.push_back(Current->getDefiningAccess());
            continue;
        }
        //遇到Use：Use不改变内存直接越过
        if (Current->isUse()) {
            Worklist.push_back(Current->getDefiningAccess());
            continue;
        }
        //遇到Phi：将所有前驱内存状态加入工作表
        if (!Current->isPhi() || Current->getIncomingAccesses().empty())
            return {};
        if (!Result.NearestPhi) Result.NearestPhi = Current;
        for (const auto &Incoming : Current->getIncomingAccesses()) {
            if (!Incoming.second) return {};
            Worklist.push_back(Incoming.second);
        }
    }

    if (Result.Clobbers.empty()) return {};
    std::sort(Result.Clobbers.begin(), Result.Clobbers.end(),
              [](MemoryAccess *LHS, MemoryAccess *RHS) {
                  return LHS->getID() < RHS->getID();
              });
    Result.Complete = true;
    return Result;
}

MemoryAccess *MemorySSAResult::getClobberingAccess(
    MemoryAccess *Start, const MemoryLocation &Location, BasicAAResult &AA,
    const LoopInfo &LI, FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    MemoryClobberResult Result = getClobberingAccesses(
        Start, Location, AA, LI, FAM, MaxVisitedAccesses);
    if (!Result.Complete) return nullptr;
    //如果所有路径最终得到同一个first clobber，直接返回
    if (Result.Clobbers.size() == 1)
        return Result.Clobbers.front();
    //如果first clobber多个，不能归约成单个写；返回反向遍历实际遇到的最近Phi
    //稀疏构图下Phi可能位于多个透明Def之后，不能只检查一层前驱
    return Result.NearestPhi;
}
//查询接口3：从ToState反向遍历到From执行后的内存边界，查询是否有可能修改的位置
/*
从 ToState 沿 DefiningAccess / MemoryPhi::IncomingAccesses 反向遍历，检查每一条内存状态路径能否无冲突地到达 From 执行后的状态
不同节点的处理方式是：
MemoryUse：不修改内存，直接向前遍历。
MemoryDef：先用 AA/ModRef 判断是否可能修改 Location；不冲突才能继续。
MemoryPhi：分叉遍历所有 incoming 路径。
Boundary：成功到达 From 执行后的内存状态。
LiveOnEntry：未到 Boundary 就到入口，说明存在绕过 From 的路径，失败。
*/
bool MemorySSAResult::hasClobberBetween(
    Instruction *From, MemoryAccess *ToState,
    const MemoryLocation &Location, BasicAAResult &AA,
    const LoopInfo &LI, FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    if (!From || !ToState || !Location.Pointer ||
        MaxVisitedAccesses == 0)
        return true;
    MemoryAccess *FromAccess = getMemoryAccess(From);
    if (!FromAccess) return true;

    //Use不产生新状态，因此边界是它消费的状态；Def本身就是执行后的新状态。
    MemoryAccess *Boundary = FromAccess->isDef()
                                 ? FromAccess
                                 : FromAccess->getDefiningAccess();
    MemoryAccess *Start = ToState->isUse()
                              ? ToState->getDefiningAccess()
                              : ToState;
    if (!Boundary || !Start) return true;

    enum class VisitState { Visiting, Safe, Unsafe };
    std::unordered_map<MemoryAccess *, VisitState> States;
    std::size_t VisitedAccesses = 0;

    //只有每条反向路径都能到达Boundary且中途没有相关Def，才能证明区间透明。
    std::function<bool(MemoryAccess *)> IsTransparent =
        [&](MemoryAccess *Current) -> bool {
        if (Current == Boundary) return true;
        if (!Current || Current->isLiveOnEntry()) return false;

        auto It = States.find(Current);
        if (It != States.end()) {
            if (It->second == VisitState::Safe) return true;
            //正在访问说明遇到MemoryPhi回环；它可能绕过From，保守视作有写。
            return false;
        }
        if (++VisitedAccesses > MaxVisitedAccesses) return false;
        States[Current] = VisitState::Visiting;

        bool Safe = false;
        if (Current->isDef()) {
            Safe = !mayClobber(Current, Location, AA, LI, FAM) &&
                   IsTransparent(Current->getDefiningAccess());
        } else if (Current->isUse()) {
            Safe = IsTransparent(Current->getDefiningAccess());
        } else if (Current->isPhi() &&
                   !Current->getIncomingAccesses().empty()) {
            Safe = true;
            for (const auto &Incoming : Current->getIncomingAccesses()) {
                if (!Incoming.second || !IsTransparent(Incoming.second)) {
                    Safe = false;
                    break;
                }
            }
        }

        States[Current] = Safe ? VisitState::Safe : VisitState::Unsafe;
        return Safe;
    };

    return !IsTransparent(Start);
}

bool MemorySSAResult::hasInterferingAccessInLoop(
    const MemoryLocation &Location, const Loop &L,
    Instruction *Ignore, bool CheckReads, BasicAAResult &AA,
    const LoopInfo &LI, FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    if (!Location.Pointer || MaxVisitedAccesses == 0) return true;
    std::size_t VisitedAccesses = 0;
    for (BasicBlock *BB : L.getBlocksIncludingSubLoops()) {
        if (!BB) continue;
        for (MemoryAccess *Access : getBlockAccesses(BB)) {
            Instruction *I = Access ? Access->getInstruction() : nullptr;
            if (!I || I == Ignore) continue;
            if (++VisitedAccesses > MaxVisitedAccesses) return true;
            ModRefInfo MRI = getInstModRefInfo(
                Location, I, AA, LI, FAM);
            if (isModSet(MRI) || (CheckReads && isRefSet(MRI)))
                return true;
        }
    }
    return false;
}

//通过MemorySSA图分析StoreInst是否冗余写入，如果是，则可以通过DSE删除
/*
拓宽DSE能力：
store %v, %p
; 跨越多个块、NoAlias写和只读调用，并且可以处理循环
store %v, %p（写入同一个Value，并且store均完全覆盖value的大小）
DSE依赖MemorySSA可以删除第二个Store
*/
bool MemorySSAResult::isStoreValueRedundant(
    StoreInst *Store, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    //1.检查store是否完整覆盖其指针指向的对象
    //store i32 %v, i32* %p，对象4字节，store必须也写入4字节
    if (!Store || MaxVisitedAccesses == 0 || !Store->fullyCoversItsPointer())
        return false;

    //2.获得MemorySSA图中store对应的MemoryDef节点
    MemoryAccess *Candidate = getMemoryAccess(Store);
    if (!Candidate || !Candidate->isDef()) return false;
    //要求地址和写入值都存在
    Value *Ptr = Store->getPointerOperand();
    Value *StoredValue = Store->getValueOperand();
    if (!Ptr || !StoredValue || Ptr->isUndef() || StoredValue->isUndef())
        return false;
    //拒绝无法得到根指针的store
    Value *PointerRoot = Value::stripPointerCastsAndGEP(Ptr);
    if (!PointerRoot || PointerRoot->isUndef()) return false;

    //3.若store位于循环之中，要求地址和写入值在循环中不变
    Loop *InnermostLoop = LI.getLoopFor(Store->getParent());
    for (Loop *L = InnermostLoop; L;L = L->getParentLoop()) {
        if (!LoopInfo::isLoopInvariant(Ptr, L) ||
            !LoopInfo::isLoopInvariant(StoredValue, L))
            return false;
    }

    //3.沿MemorySSA图反向遍历
    std::vector<MemoryAccess *> Worklist{Candidate->getDefiningAccess()};
    std::unordered_set<MemoryAccess *> Visited;
    bool SawConcreteSameValueStore = false;
    while (!Worklist.empty()) {
        MemoryAccess *Current = Worklist.back();
        Worklist.pop_back();
        if (!Current || !Visited.insert(Current).second) continue;
        if (Visited.size() > MaxVisitedAccesses) return false;
        if (Current->isLiveOnEntry()) return false;
        //如果遇到自己，说明沿着自支撑回边又回到自己了；如果没有在循环中，说明有路径绕过了这个store
        //无论如何都不能把自己返回，必须沿着前驱继续遍历
        if (Current == Candidate) {
            if (!InnermostLoop) return false;
            continue;
        }
        //遇到MemoryPhi和MemoryUse，沿着所有前驱继续反向遍历
        if (Current->isPhi()) {
            if (Current->getIncomingAccesses().empty()) return false;
            for (const auto &Incoming : Current->getIncomingAccesses()) {
                if (!Incoming.second) return false;
                Worklist.push_back(Incoming.second);
            }
            continue;
        }
        if (Current->isUse()) {
            Worklist.push_back(Current->getDefiningAccess());
            continue;
        }
        //遇到MemoryDef
        //如果遇到前置Store
        Instruction *I = Current->getInstruction();
        if (auto *PreviousStore =
                I ? I->as<StoreInst>() : nullptr) {
            Value *PreviousPtr = PreviousStore->getPointerOperand();
            //检查该Store和前置Sore是不是MustAlias，并且前置Store也完整覆盖，并且两个Store必须写入同一个Value
            AliasInfo AI = PreviousPtr
                               ? AA.aliasWithIndexRange(Ptr, PreviousPtr, LI)
                               : AliasInfo::MayAlias;
            if (AI == AliasInfo::NoAlias) {
                Worklist.push_back(Current->getDefiningAccess());
                continue;
            }

            if (AI != AliasInfo::MustAlias ||
                !PreviousStore->fullyCoversItsPointer() ||
                PreviousStore->getValueOperand() != StoredValue)
                return false;
            //如果找到这么一个Store，标记为True
            SawConcreteSameValueStore = true;
            continue;
        }
        //如果是Call
        if (I && I->as<CallInst>()) {
            //通过BasicAA分析Call是否可能修改目标位置
            ModRefInfo MRI = getInstModRefInfo(
                MemoryLocation::get(Store), I, AA, LI, FAM);
            //如果可能直接返回false
            if (isModSet(MRI)) return false;
            //如果不可能，沿着前驱继续遍历
            Worklist.push_back(Current->getDefiningAccess());
            continue;
        }
        return false;
    }
    return SawConcreteSameValueStore;
}

bool MemorySSAResult::isStoreOverwrittenOnAllPaths(
    StoreInst *Store, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    if (!Store || MaxVisitedAccesses == 0) return false;
    MemoryAccess *Candidate = getMemoryAccess(Store);
    MemoryLocation CandidateLocation = MemoryLocation::get(Store);
    if (!Candidate || !Candidate->isDef() || !CandidateLocation.Pointer ||
        !CandidateLocation.Size) {
        return false;
    }

    enum class ScanResult { Kill, Transparent, Blocked };
    std::size_t VisitedAccesses = 0;

    //扫描一段顺序路径：完整覆盖写结束该路径；读、部分写和未知调用阻断证明。
    auto ScanFrom = [&](Instruction *First) {
        for (Instruction *I = First; I; I = I->getNext()) {
            if (++VisitedAccesses > MaxVisitedAccesses)
                return ScanResult::Blocked;

            if (auto *Killer = I->as<StoreInst>()) {
                //回到同一静态Store属于下一次循环执行，不能用它证明删除自身安全。
                if (Killer == Store) return ScanResult::Blocked;
                MemoryLocation KillerLocation = MemoryLocation::get(Killer);
                AliasInfo AI = AA.aliasWithIndexRange(
                    CandidateLocation, KillerLocation, LI);
                if (AI == AliasInfo::NoAlias) continue;
                bool FullyCovered =
                    AI == AliasInfo::MustAlias && KillerLocation.Size &&
                    *KillerLocation.Size >= *CandidateLocation.Size;
                return FullyCovered ? ScanResult::Kill
                                    : ScanResult::Blocked;
            }

            ModRefInfo MRI = getInstModRefInfo(
                CandidateLocation, I, AA, LI, FAM);
            if (isRefSet(MRI) || isModSet(MRI))
                return ScanResult::Blocked;
        }
        return ScanResult::Transparent;
    };

    ScanResult Tail = ScanFrom(Store->getNext());
    if (Tail == ScanResult::Kill) return true;
    if (Tail == ScanResult::Blocked) return false;

    BasicBlock *StoreBlock = Store->getParent();
    if (!StoreBlock) return false;

    enum class BlockState { Visiting, Proven, Failed };
    std::unordered_map<BasicBlock *, BlockState> States;

    //稀疏Users不会表示没有内存访问的CFG边，因此严格的all-path证明沿CFG递归；
    //共享汇合块可以复用结果，透明回环遇到Visiting后保守失败。
    std::function<bool(BasicBlock *)> ProveBlock =
        [&](BasicBlock *BB) -> bool {
        if (!BB) return false;
        auto It = States.find(BB);
        if (It != States.end()) {
            if (It->second == BlockState::Proven) return true;
            return false;
        }
        States[BB] = BlockState::Visiting;

        ScanResult BlockResult = ScanFrom(BB->front());
        if (BlockResult == ScanResult::Kill) {
            States[BB] = BlockState::Proven;
            return true;
        }
        if (BlockResult == ScanResult::Blocked) {
            States[BB] = BlockState::Failed;
            return false;
        }

        std::vector<BasicBlock *> Successors = BB->getSuccessors();
        if (Successors.empty()) {
            States[BB] = BlockState::Failed;
            return false;
        }
        for (BasicBlock *Successor : Successors) {
            if (!ProveBlock(Successor)) {
                States[BB] = BlockState::Failed;
                return false;
            }
        }
        States[BB] = BlockState::Proven;
        return true;
    };

    std::vector<BasicBlock *> Successors = StoreBlock->getSuccessors();
    if (Successors.empty()) return false;
    for (BasicBlock *Successor : Successors)
        if (!ProveBlock(Successor)) return false;
    return true;
}

bool MemorySSAResult::invalidate(Function &F,const PreservationStatus &PA) {
    (void)F;
    return !PA.isKept(MemorySSAAnalysis::ID());
}
PassID MemorySSAAnalysis::Key;

// MemorySSA主流程先用迭代支配边界放置稀疏Phi，再沿支配树前序构建统一内存状态图
/*
核心思想：
只在MemoryDef定义块的迭代支配边界创建MemoryPhi
每个 store/可能写内存的call 产生一个新内存状态
每个 load/只读call 使用当前内存状态，但不产生新状态
无Phi基本块从直接支配者出口继承入口状态
*/
MemorySSAResult MemorySSAAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const {
    MemorySSAResult Result;
    //1.创建LiveOnEntry，位于函数入口
    Result.LiveOnEntry = Result.createAccess(MemoryAccess::Kind::LiveOnEntry);
    if (F.size() == 0) return Result;

    //2.复用FunctionAnalysisPassManager取得支配树
    //为了调用IDF接口，判断基本块是否可达，辅助rename流程实现
    DominatorTree &DT = FAM.getResult<DominantAnalysis>(F);

    //3.创建MemoryDef/MemoryUse
    //收集所有MemoryDef所在的基本块，供后续计算迭代支配边界
    std::set<BasicBlock *> DefBlocks;
    for (BasicBlock *BB : F) {
        if (!BB) continue;
        std::vector<MemoryAccess *> &Accesses = Result.BlockAccesses[BB];
        for (Instruction *I : *BB) {
            if (!I) continue;
            //Load：MemoryUse
            if (I->as<LoadInst>()) {
                MemoryAccess *Use =
                    Result.createAccess(MemoryAccess::Kind::Use, BB, I);
                Result.InstructionAccesses[I] = Use;
                Accesses.push_back(Use);
                continue;
            }
            //Call按程序内存ModRef分类：只读调用是MemoryUse，纯调用不进入内存图；
            //计时和I/O可观察性由独立顺序屏障维护，不伪装成程序内存写入
            if (auto *Call = I->as<CallInst>()) {
                ModRefInfo MRI = getCallProgramModRefInfo(Call, FAM);
                if (!isModSet(MRI)) {
                    if (isRefSet(MRI)) {
                        MemoryAccess *Use =
                            Result.createAccess(
                                MemoryAccess::Kind::Use, BB, I);
                        Result.InstructionAccesses[I] = Use;
                        Accesses.push_back(Use);
                    }
                    continue;
                }
            }
            //Store/可能写内存的Call：MemoryDef
            if (I->as<StoreInst>() || I->as<CallInst>()) {
                MemoryAccess *Def =
                    Result.createAccess(MemoryAccess::Kind::Def, BB, I);
                Result.InstructionAccesses[I] = Def;
                Accesses.push_back(Def);
                //收集MemoryDef所在的基本块（只有在支配树上可达才加入DefBlocks）
                if (DT.getNode(BB)) DefBlocks.insert(BB);
            }
        }
    }

    //4.根据迭代支配边界计算Phi块，创建MemoryPhi节点
    std::set<BasicBlock *> PhiBlocks =  DT.getIteratedDominanceFrontier(DefBlocks);
    std::unordered_map<BasicBlock *, MemoryAccess *> BlockPhis;
    for (BasicBlock *BB : F) {
        if (!BB || !PhiBlocks.count(BB)) continue;
        BlockPhis[BB] =
            Result.createAccess(MemoryAccess::Kind::Phi, BB);
    }

    //5.沿着支配树进行rename
    //连接节点之间的DefiningAccess，Users边，以及每个块的入口和出口内存状态
    for (BasicBlock *BB : DT.getBlocksInPreOrder()) {
        if (!BB) continue;
        MemoryAccess *Inherited = Result.LiveOnEntry;
        if (BasicBlock *IDom = DT.getIDom(BB)) {
            MemoryAccess *IDomExit = Result.getBlockExit(IDom);
            if (IDomExit) Inherited = IDomExit;
        }

        auto PhiIt = BlockPhis.find(BB);
        MemoryAccess *Current =
            PhiIt == BlockPhis.end() ? Inherited : PhiIt->second;
        Result.BlockEntries[BB] = Current;

        for (MemoryAccess *Access : Result.BlockAccesses[BB]) {
            Access->DefiningAccess = Current;
            Result.addUser(Current, Access);
            if (Access->isDef()) Current = Access;
        }
        Result.BlockExits[BB] = Current;
    }

    //6.处理不可达块：所有不可达块挂到LiveOnEntry上
    for (BasicBlock *BB : F) {
        if (!BB || DT.getNode(BB)) continue;
        MemoryAccess *Current = Result.LiveOnEntry;
        Result.BlockEntries[BB] = Current;
        for (MemoryAccess *Access : Result.BlockAccesses[BB]) {
            Access->DefiningAccess = Result.LiveOnEntry;
            Result.addUser(Result.LiveOnEntry, Access);
            if (Access->isDef()) Current = Access;
        }
        Result.BlockExits[BB] = Current;
    }

    //7.填写MemoryPhi的Incoming
    BasicBlock *RootBlock = DT.Root ? DT.Root->Block : nullptr;
    auto RootPhiIt = BlockPhis.find(RootBlock);
    if (RootPhiIt != BlockPhis.end())
        RootPhiIt->second->IncomingAccesses.push_back(
            {nullptr, Result.LiveOnEntry});
    if (RootPhiIt != BlockPhis.end())
        Result.addUser(Result.LiveOnEntry, RootPhiIt->second);

    for (BasicBlock *Pred : F) {
        if (!Pred || !DT.getNode(Pred)) continue;
        MemoryAccess *PredExit = Result.getBlockExit(Pred);
        if (!PredExit) continue;

        std::unordered_set<BasicBlock *> SeenSuccessors;
        for (BasicBlock *Succ : Pred->getSuccessors()) {
            if (!Succ || !SeenSuccessors.insert(Succ).second)
                continue;
            auto PhiIt = BlockPhis.find(Succ);
            if (PhiIt == BlockPhis.end()) continue;
            PhiIt->second->IncomingAccesses.push_back(
                {Pred, PredExit});
            Result.addUser(PredExit, PhiIt->second);
        }
    }

    return Result;
}
