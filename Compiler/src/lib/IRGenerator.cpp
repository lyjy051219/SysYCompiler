#include "../../include/lib/IRGenerator.hpp"
#include "../../include/lib/CFG.hpp"
extern std::unique_ptr<Module> ModuleIR;
Value* IRGenerator::popValue() {
    if (valueStack.empty()) {
        std::cerr << "值栈为空\n";
        assert(0);
    }
    Value* val = valueStack.top();
    valueStack.pop();
    return val;
}

void IRGenerator::pushValue(Value* val) {
    valueStack.push(val);
}

TypeSystem IRGenerator::ASTCastIRType(Type t) {
    TypeSystem irtype;
    switch (t)
    {
    case SY_INT:
        irtype = IR_INT;
        break;
    case SY_FLOAT:
        irtype = IR_FLOAT;
        break;
    default:
        std::cerr << "变量类型不合法\n";
        assert(0);
    }
    return irtype;
}

//类型转换函数
Value* Inter(Value* op, BasicBlock* bb) {
    if (op->getIRType() == INTType::NewInt(4))
        return op;
    if (op->isConst()) {
        if (auto op_int = dynamic_cast<ConstBool*>(op))
            return ConstInt::newConstInt((int)op_int->getVal());
        else if (auto op_float = dynamic_cast<ConstFloat*>(op))
            return ConstInt::newConstInt((int)op_float->getVal());
        else
            assert(false);
    }
    else {
        assert(bb != nullptr);
        if (op->getIRType() == FLOATType::NewFloat())
            return bb->genFPTOSIInst(op);
        else
            assert(false);
    }
    return nullptr;
}

Value* Floater(Value* op, BasicBlock* bb) {
    if (op->getIRType() == FLOATType::NewFloat())
        return op;
    if (op->isConst()) {
        if (auto op_int = dynamic_cast<ConstInt*>(op))
            return ConstFloat::newConstFloat((float)op_int->getVal());
        else if (auto op_float = dynamic_cast<ConstBool*>(op))
            return ConstFloat::newConstFloat((float)op_float->getVal());
        else
            assert(false);
    }
    else {
        assert(bb != nullptr);
        if (op->getIRType() == INTType::NewInt(4))
            return bb->genSITOFPInst(op);
        else
            assert(false);
    }
    return nullptr;
}

// 修复后的 LOrExp 处理函数
void IRGenerator::getOperand(LOrExp &node, BasicBlock* block, BasicBlock* is_true,
                             BasicBlock* is_false) {
    auto& operands = node.getOperands();

    for (auto it = operands.begin(); it != operands.end(); ++it) {
        // 关键：每一轮循环开始，必须将 currentBasicBlock 设置为当前传递下来的 block
        currentBasicBlock = block;

        if (std::next(it) != operands.end()) {
            // 如果不是最后一个，失败时跳到下一个检查块 (nextBB)
            auto nextBB = block->genBlock();
            // 递归调用 LAndExp 的处理逻辑
            getOperand(*(*it), block, is_true, nextBB);
            // 更新 block 为下一轮的起点
            block = nextBB;
        } else {
            // 最后一个操作数：失败跳 is_false
            getOperand(*(*it), block, is_true, is_false);
        }
    }
}
// 修复后的 LAndExp 处理函数
void IRGenerator::getOperand(LAndExp &node, BasicBlock* block, BasicBlock* is_true,
                             BasicBlock* is_false) {
    auto& operands = node.getOperands();

    for (auto it = operands.begin(); it != operands.end(); ++it) {
        // 关键：确保当前指令生成在正确的 block 中
        currentBasicBlock = block;

        // 生成比较表达式的值 (EqExp)
        auto v = getOperand(*(*it), block);

        if (std::next(it) != operands.end()) {
            // 如果不是最后一个，成功时跳到下一个检查块 (nextBB)
            auto nextBB = block->genBlock();
            // 如果 v 为真 -> nextBB; 如果 v 为假 -> is_false (短路)
            block->genCondInst(v, nextBB, is_false);
            // 更新 block 为下一轮的起点
            block = nextBB;
        } else {
            // 最后一个操作数：成功跳 is_true，失败跳 is_false
            block->genCondInst(v, is_true, is_false);
        }
    }
}

Value* IRGenerator::getOperand(EqExp &node, BasicBlock* block) {
    auto i = node.getOperands().begin();
    (*i)->accept(*this);
    auto oper = popValue();
    if (node.get_op_count() == 0 && oper->getIRType() != INTType::NewBool()) {
        switch (oper->getIRType()->getEnum())
        {
        case IR_POINTER:
            oper = block->genBinaryInst(block, oper, BinaryInst::NE,
                                        ConstPtr::newConstPtr(oper->getIRType()));
            break;
        case IR_INT:
            oper = block->genBinaryInst(block, oper, BinaryInst::NE, ConstInt::newConstInt());
            break;
        case IR_FLOAT:
            oper = block->genBinaryInst(block, oper, BinaryInst::NE, ConstFloat::newConstFloat());
            break;
        default:
            assert(0);
        }
        return oper;
    }
    for (auto &j : node.getOps()) {
        i++;
        (*i)->accept(*this);
        auto another = popValue();
        BinaryInst::Operation opcode;
        switch (j)
        {
        case SY_EQ:
            opcode = BinaryInst::E;
            break;
        case SY_NOTEQ:
            opcode = BinaryInst::NE;
            break;
        default:
            std::cerr << "Wrong Opcode for EqExp\n";
            assert(0);
        }
        oper = BasicBlock::genBinaryInst(block, oper, opcode, another);
    }
    return oper;
}

void IRGenerator::visit(CompUnit &node) {
    for (auto &n : node.get_items()) {
        n->accept(*this);
    }
}

void IRGenerator::visit(ConstDecl &node) {
    contextType.push_type(ASTCastIRType(node.get_type()));
    auto &i = node.get_ConstDefList();
    if (i)
        i->accept(*this);
    contextType.pop_type();
}

void IRGenerator::visit(VarDecl &node) {
    contextType.push_type(ASTCastIRType(node.get_type()));
    auto &i = node.get_VarDefList();
    if (i)
        i->accept(*this);
    contextType.pop_type();
}
void IRGenerator::visit(ConstDefList &node) {
    for (auto &i : node.get_defs()) {
        if (i)
            i->accept(*this);
    }
}
void IRGenerator::visit(VarDefList &node) {
    for (auto &i : node.get_defs()) {
        if (i)
            i->accept(*this);
    }
}

void IRGenerator::visit(ConstDef &node) {
    TypeSystem currentType = contextType.getCurrentType();
    std::string ident = node.get_ident();
    if (!functionStack.empty()) {
        if (node.has_array()) {
            auto baseType = IRType::NewTypeByTypeSystem(currentType);
            arrayTypeStack.push(baseType);
            node.get_array()->accept(*this);
            baseType = arrayTypeStack.top();
            arrayTypeStack.pop();
            // 生成 alloca 指令
            auto alloca = currentBasicBlock->genAllocaInst(baseType, ident);
            if (node.has_initval()) {
                currentIRType = baseType;
                node.get_initval()->accept(*this);
                Value* v = popValue();
                std::vector<Value*> args;
                args.push_back(alloca);
                auto src = new Variable(Variable::Constant, baseType, "");
                src->addUse(v);
                args.push_back(src);
                args.push_back(ConstInt::newConstInt(baseType->getSize()));
                args.push_back(ConstBool::newConstBool(false));
                currentBasicBlock->genCallInst("llvm.memcpy.p0.p0.i32", args);
                std::vector<int> temp;
                dynamic_cast<Initializer*>(v)->Var2Store(currentBasicBlock, ident, temp);
            }
        } else {
            auto baseType = IRType::NewTypeByTypeSystem(currentType);
            Value* v;
            if (!node.has_initval()) {
                //如果没有初始化的值就直接创建初始化为0的ConstInt
                if (currentType == IR_INT)
                    v = ConstInt::newConstInt();
                else if (currentType == IR_FLOAT)
                    v = ConstFloat::newConstFloat();
                else
                    assert(0);
            } else {
                //这里依旧是不需要用到currentIRType，因为是只有AddExp类型，只有数组类型才会调用ArrayList，用到currentIRType
                node.get_initval()->accept(*this);
                v = popValue();
            }
            //int x = 3.14 -> int x = 3;针对初始化值和类型不符的处理
            if (currentType == IR_INT) {
                v = Inter(v, nullptr);
            } else if (currentType == IR_FLOAT) {
                v = Floater(v, nullptr);
            }
            ModuleIR->getSymbolTable().register_symbolTable(ident, v);
        }
    } else {
        //全局常量处理
        if (node.has_array()) {
            //注意这里：将所有数组都看作变量数组
            auto baseType = IRType::NewTypeByTypeSystem(currentType);
            arrayTypeStack.push(baseType);
            node.get_array()->accept(*this);
            baseType = arrayTypeStack.top();
            arrayTypeStack.pop();
            //在创建Variable--GlobalVariable的时候就已经完成了注册到符号表以及加入vector之中
            auto var = new Variable(Variable::GlobalVariable, baseType, ident);
            if (node.has_initval()) {
                //将Initval映射成IR的类型，然后将创建好的Value通过stack进行压入
                currentIRType = baseType;
                node.get_initval()->accept(*this);
                var->addUse(popValue());
            }
        } else {
            auto baseType = IRType::NewTypeByTypeSystem(currentType);
            auto tmp = new Variable(Variable::GlobalVariable, baseType, ident);
            Value* v;
            if (!node.has_initval()) {
                //如果没有初始化的值就直接创建初始化为0的ConstInt
                if (currentType == IR_INT)
                    v = ConstInt::newConstInt();
                else if (currentType == IR_FLOAT)
                    v = ConstFloat::newConstFloat();
                else
                    assert(0);
            } else {
                node.get_initval()->accept(*this);
                v = popValue();
            }
            //int x = 3.14 -> int x = 3;针对初始化值和类型不符的处理
            if (currentType == IR_INT) {
                v = Inter(v, nullptr);
            } else if (currentType == IR_FLOAT) {
                v = Floater(v, nullptr);
            } else
                assert(0);
            ModuleIR->getSymbolTable().register_symbolTable(ident, v);
        }
    }
}

void IRGenerator::visit(VarDef &node) {
    TypeSystem currentType = contextType.getCurrentType();
    std::string ident = node.get_ident();
    // 判断是否在函数内部（局部作用域）
    if (!functionStack.empty()) {
        // =======局部变量处理========
        if (node.has_array()) {
            auto baseType = IRType::NewTypeByTypeSystem(currentType);
            arrayTypeStack.push(baseType);
            node.get_array()->accept(*this);
            baseType = arrayTypeStack.top();
            arrayTypeStack.pop();

            // 生成 alloca 指令
            auto alloca = currentBasicBlock->genAllocaInst(baseType, ident);

            if (node.has_initval()) {
                currentIRType = baseType;
                node.get_initval()->accept(*this);
                Value* v = popValue();

                // --- 修复后的 memcpy 调用 ---
                std::vector<Value*> args;
                args.push_back(alloca); // 目的地址

                auto src = new Variable(Variable::Constant, baseType, "");
                src->addUse(v);
                args.push_back(src);    // 源地址

                args.push_back(ConstInt::newConstInt(baseType->getSize())); // 大小
                args.push_back(ConstBool::newConstBool(false)); // volatile

                currentBasicBlock->genCallInst("llvm.memcpy.p0.p0.i32", args);

                std::vector<int> temp;
                dynamic_cast<Initializer*>(v)->Var2Store(currentBasicBlock, ident, temp);
            }
        } else {
            // 非数组局部变量
            auto baseType = IRType::NewTypeByTypeSystem(currentType);
            auto alloca = currentBasicBlock->genAllocaInst(baseType, ident);
            if (node.has_initval()) {
                currentIRType = baseType;
                node.get_initval()->accept(*this);
                Value* v = popValue();
                currentBasicBlock->genStoreInst(v, alloca);
            }
        }
    } else {
        //==========全局变量处理========
        if (node.has_array()) {
            auto baseType = IRType::NewTypeByTypeSystem(currentType);
            arrayTypeStack.push(baseType);
            node.get_array()->accept(*this);
            baseType = arrayTypeStack.top();
            arrayTypeStack.pop();
            auto var = new Variable(Variable::GlobalVariable, baseType, ident);
            if (node.has_initval()) {
                currentIRType = baseType;
                node.get_initval()->accept(*this);
                var->addUse(popValue());
            }
        } else {
            auto irtype = IRType::NewTypeByTypeSystem(currentType);
            auto var = new Variable(Variable::GlobalVariable, irtype, ident);
            if (node.has_initval()) {
                currentIRType = irtype;
                node.get_initval()->accept(*this);
                var->addUse(popValue());
            }
        }
    }
}

void IRGenerator::visit(FuncDef &node) {
    auto SYtoIRType = [](Type t) {
        switch (t) {
        case SY_INT:
            return TypeSystem::IR_INT;
        case SY_FLOAT:
            return TypeSystem::IR_FLOAT;
        case SY_VOID:
            return TypeSystem::IR_VOID;
        default:
            std::cerr << "Type Illegal\n";
            assert(0);
            return TypeSystem::IR_UNDEF;
        }
    };

    //第一步：将SY的类型映射到IR的类型上去
    //第二步：根据IRType和ident在ModuleIR中创建Function
    //包括：创建一个BasicBlock在Function里面，加入符号表，加入vector里面
    auto &f = ModuleIR->newFunction(SYtoIRType(node.get_type()), node.get_ident());
    //第三步：将当前正在处理的Function压入栈中，等到后续FuncParam的时候进行调用
    functionStack.push(&f);
    //第四步：通过ModuleIR下的symbolTable进入新的作用域
    ModuleIR->getSymbolTable().enter_scope();
    //第五步：如果Function中存在形参，就进入FuncParamList中处理形参相关的东西
    if (node.get_params() != nullptr)
        node.get_params()->accept(*this);
    assert(node.get_funcBody() != nullptr);
    //第六步：得到入口基本块
    auto entryBB = f.front();
    currentBasicBlock = entryBB;
    //第七步：生成函数体代码
    node.get_funcBody()->accept(*this);
    //这里在生成BasicBlock的时候需要不断更新currentBasicBlock直到currentBasicBlock停到最后一块上面
    //第八步：创建返回指令--如果最后一个BasicBlock不是以终止指令结束的话
    if (!currentBasicBlock->hasTerminator()) {
        if (f.getIRType()->getEnum() == IR_VOID)
            currentBasicBlock->genRetInst();
        else {
            if (f.getIdent() == "main")
                currentBasicBlock->genRetInst(ConstInt::newConstInt(0));
            else
                currentBasicBlock->genRetInst(UndefValue::NewUndefValue(f.getIRType()));
        }
    }
    //第九步：退出当前作用域
    ModuleIR->getSymbolTable().exit_scope();
    //第十步：弹出当前处理的Function
    functionStack.pop();
}

void IRGenerator::visit(FuncParam &node) {
    TypeSystem currentType = ASTCastIRType(node.get_type());
    std::string ident = node.get_ident();
    auto baseType = IRType::NewTypeByTypeSystem(currentType);
    if (node.has_dimension()) {
        arrayTypeStack.push(baseType);
        node.get_exp_list()->accept(*this);
        baseType = arrayTypeStack.top();
        arrayTypeStack.pop();
        //只要是数组类型就转换成指针类型
        if (node.has_empty_brackets()) {
            baseType = POINTERType::NewPointer(baseType);
        } else {
            //有维度的直接将其指向的类型作为指针
            auto arrayType = dynamic_cast<ARRAYType*>(baseType.get());
            baseType = POINTERType::NewPointer(arrayType->getElementType());
        }
        functionStack.top()->pushParam(ident, new Variable(Variable::Param, baseType, ident));
    } else {
        if (!node.has_empty_brackets())
            functionStack.top()->pushParam(ident, new Variable(Variable::Param, baseType, ident));
        else
            functionStack.top()->pushParam(
                ident, new Variable(Variable::Param, POINTERType::NewPointer(baseType), ident));
    }
}

void IRGenerator::visit(FuncParamList &node) {
    //继续深入到FuncParam层面进行处理
    for (auto &i : node.get_params()) {
        i->accept(*this);
    }
}

void IRGenerator::visit(InitValList &node) {
    //接收两个参数currentIRType和currentBasicBlock
    assert(currentIRType->getEnum() == IR_ARRAY);
    // 创建Initializer对象
    auto ret = new Initializer(currentIRType);
    size_t offs = 0;
    // 构建类型映射表
    std::map<std::shared_ptr<IRType>, std::shared_ptr<IRType>> typeMap;
    auto buildTypeMap = [&typeMap](std::shared_ptr<ARRAYType> tp) {
        while (tp != nullptr) {
            typeMap[tp->getElementType()] = tp;
            // 递归处理子类型
            auto subType = tp->getElementType();
            tp = std::dynamic_pointer_cast<ARRAYType>(subType);
        }
    };
    // 调用构建类型映射
    auto arrayType = std::dynamic_pointer_cast<ARRAYType>(currentIRType);
    assert(arrayType && "InitValList requires an Array Type!"); // 安全检查
    buildTypeMap(arrayType);
    // 定义max_type函数，根据偏移量计算最大对齐类型
    auto max_type = [&](std::shared_ptr<IRType> tp) {
        auto currentType = tp;
        while (offs % currentType->getSize() != 0) {
            currentType = std::dynamic_pointer_cast<ARRAYType>(currentType)->getElementType();
        }
        return currentType;
    };
    auto sub = arrayType->getElementType();
    // 遍历初始化列表
    for (auto &i : node.get_initVals()) {
        auto type_expect = max_type(sub);
        // 保存当前IR类型，设置期望类型，然后处理初始化值
        auto savedIRType = currentIRType;
        currentIRType = type_expect;
        i->accept(*this);
        currentIRType = savedIRType;
        Value* tmp = popValue();
        // 类型转换处理
        if (tmp->getIRType() != type_expect) {
            if (contextType.getCurrentType() == IR_INT) {
                tmp = Inter(tmp, currentBasicBlock);
            } else if (contextType.getCurrentType() == IR_FLOAT) {
                tmp = Floater(tmp, currentBasicBlock);
            } else {
                assert(0 && "Unsupported type conversion");
            }
        }
        offs += tmp->getIRType()->getSize();
        ret->push_back(tmp);
        // 打包操作：将底层元素打包成更高维度的数组
        while (ret->back()->getIRType() != max_type(sub)) {
            auto upper = std::dynamic_pointer_cast<ARRAYType>(typeMap[ret->back()->getIRType()]);
            int ele = upper->getElementCount();
            auto omit = new Initializer(upper);
            for (int i = 0; i < ele; i++) {
                omit->push_back(ret->back());
                ret->pop_back();
            }
            std::reverse(omit->begin(), omit->end());
            ret->push_back(omit);
        }
    }
    // 处理未初始化完的情况
    while (!ret->empty() && ret->back()->getIRType() != sub) {
        auto upper = std::dynamic_pointer_cast<ARRAYType>(typeMap[ret->back()->getIRType()]);
        int ele = upper->getElementCount();
        auto omit = new Initializer(upper);
        for (int i = 0; i < ele && !ret->empty() &&ret->back()->getIRType() == upper->getElementType();i++) {
            omit->push_back(ret->back());
            ret->pop_back();
        }
        std::reverse(omit->begin(), omit->end());
        ret->push_back(omit);
    }
    // 将结果压入值栈
    pushValue(ret);
}
void IRGenerator::visit(InitVal &node) {
    //接收两个参数，一个是currentIRType(在InitValList这里去使用)，一个是currentBasicBlock
    if (auto fuc = dynamic_cast<AddExp*>(node.get_value().get())) {
        //只需要currentBasicBlock一个参数
        //处理AddExp
        fuc->accept(*this);
        //在AddExp层级就进入Value栈里面，后续取出就可以了
        return;
    } else if (auto fuc = dynamic_cast<InitValList*>(node.get_value().get())) {
        //两个参数都需要
        //处理InitValList初始化列表--同样压栈，在该层面就不用返回了
        fuc->accept(*this);
        return;
    } else {
        pushValue(new Initializer(currentIRType));
        return;
    }
}

void IRGenerator::visit(Block &node) {
    if (node.get_items() == nullptr)
        return;
    ModuleIR->getSymbolTable().enter_scope();
    node.get_items()->accept(*this);
    ModuleIR->getSymbolTable().exit_scope();
}

void IRGenerator::visit(BlockItemList &node) {
    for (auto &i : node.get_stmts()) {
        //让每条语句去生成其对应的IR语句
        i->accept(*this);
        if (currentBasicBlock->hasTerminator())
            return;
    }
}

//进入语句生成的部分

void IRGenerator::visit(AssignStmt &node) {
    //Store store i32 20, i32* %y_ptr
    //第一步：计算右值表达式AddExp
    //注意AddExp会将生成的Value加入Value Stack
    node.get_exp()->accept(*this);
    Value* rightValue = popValue();
    //第二步：获取左值表达式LVal的地址
    //注意也需要将地址作为Value压入Value Stack
    getLValPointer(*node.get_lval());
    Value* leftAddress = popValue();
    //第三步：生成store指令
    currentBasicBlock->genStoreInst(rightValue, leftAddress);
}

void IRGenerator::visit(ExpStmt &node) {
    /*SEMICOLON { $$ = new ExpStmt(nullptr); }
AddExp SEMICOLON { $$ = new ExpStmt($1); }
*/
    if (node.get_exp() != nullptr) {
        //AddExp
        node.get_exp()->accept(*this);
        popValue();
    }
}

void IRGenerator::visit(IfStmt &node) {
    // 1. 创建 True/False 块
    auto isTrue = currentBasicBlock->genBlock();
    BasicBlock* isFalse = nullptr;
    BasicBlock* nextBB = nullptr;

    if (node.has_else()) {
        isFalse = currentBasicBlock->genBlock();
        // 处理条件：传入 isTrue 和 isFalse
        getOperand(*(node.get_condition().get()), currentBasicBlock, isTrue, isFalse);
    } else {
        // 没有 else 时，False 直接跳到汇聚块
        // 注意：这里必须先创建 nextBB，因为 False 的目标就是 nextBB
        nextBB = currentBasicBlock->genBlock();
        getOperand(*(node.get_condition().get()), currentBasicBlock, isTrue, nextBB);
    }

    // --- 处理 True 分支 ---
    currentBasicBlock = isTrue;
    node.get_if_stmt()->accept(*this);

    // 如果 True 分支没有终止指令（没有return/break等），跳到汇聚块
    if (!currentBasicBlock->hasTerminator()) {
        if (nextBB == nullptr) nextBB = currentBasicBlock->genBlock();
        currentBasicBlock->genUnCondInst(nextBB);
    }

    // --- 处理 False 分支 ---
    if (isFalse) {
        currentBasicBlock = isFalse;
        node.get_else_stmt()->accept(*this);

        // 如果 False 分支没有终止指令，跳到汇聚块
        if (!currentBasicBlock->hasTerminator()) {
            if (nextBB == nullptr) nextBB = currentBasicBlock->genBlock();
            currentBasicBlock->genUnCondInst(nextBB);
        }
    }

    // --- 汇聚块 ---
    // 只有当 nextBB 被创建时（意味着至少有一条路径汇聚或者没有else），才更新 currentBasicBlock
    if (nextBB) {
        currentBasicBlock = nextBB;
    }
}

void IRGenerator::visit(WhileStmt &node) {
    auto conditionPart = currentBasicBlock->genBlock("wc");
    auto loopPart = currentBasicBlock->genBlock("wloop");
    auto nextBB = currentBasicBlock->genBlock("wn");
    // 跳入条件判断块
    currentBasicBlock->genUnCondInst(conditionPart);
    // 生成条件判断流
    // 注意：这里 nextBB 被传入作为条件为假时的跳转目标
    getOperand(*(node.get_condition().get()), conditionPart, loopPart, nextBB);
    // 保存旧的 break/continue 目标
    auto oldBreakBB = breakBB;
    auto oldContinueBB = continueBB;
    breakBB = nextBB;
    continueBB = conditionPart;
    // 处理循环体
    currentBasicBlock = loopPart;
    node.get_while_stmt()->accept(*this);
    // 如果循环体没有结束指令（比如 return/break），则跳回条件块
    if (!currentBasicBlock->hasTerminator()) {
        currentBasicBlock->genUnCondInst(conditionPart);
    }
    // 恢复 break/continue
    breakBB = oldBreakBB;
    continueBB = oldContinueBB;
    currentBasicBlock = nextBB;
}

void IRGenerator::visit(BreakStmt &node) {
    currentBasicBlock->genUnCondInst(breakBB);
}

void IRGenerator::visit(ContinueStmt &node) {
    currentBasicBlock->genUnCondInst(continueBB);
}

void IRGenerator::visit(ReturnStmt &node) {
    if (node.has_return_value()) {
        // 有返回值
        //进入计算AddExp表达式
        node.get_return_value()->accept(*this);
        Value* retValue = popValue();
        currentBasicBlock->genRetInst(retValue);
    } else {
        // 无返回值
        currentBasicBlock->genRetInst();
    }
}

//进入表达式IR生成的部分
/*关于LVal：
在赋值语句中，它作为左值，需要获取它的地址，在其他语句中，可能作为右值，需要获取它的操作数
所以如果是从AssignStmt中来的，我们需要单独分析并获取它的地址
所以在LVal的visit中，我们还是照常写获取它的操作数，在其他地方，我们再写一个获取它指针的操作
获取指针的操作集成在IRGenerator里面*/

void IRGenerator::visit(LVal &node) {
    // 1. 获取地址
    getLValPointer(node);
    Value* ptr = popValue();

    // 2. 如果是常量，直接返回
    if (ptr->isConst()) {
        pushValue(ptr);
        return;
    }

    // 3. 处理 GEP 指令的常量索引 0 优化 (针对 [10 x i32]* 这种情况)
    if (auto gep = dynamic_cast<GetElementPtrInst*>(ptr)) {
        auto irType = gep->getIRType();
        auto ptrType = std::dynamic_pointer_cast<POINTERType>(irType);
        if (ptrType && ptrType->getPointerType()->getEnum() == IR_ARRAY) {
            gep->addUse(ConstInt::newConstInt(0));
            gep->updateType(); // 记得更新类型
            pushValue(gep);
            return;
        }
    }

    // --- 【修改核心】 ---
    // 4. 判断是否需要 Load
    // 我们需要查看符号表中该变量的原始类型
    auto var = ModuleIR->getSymbolTable().getValueByName(node.get_ident());
    bool isArrayOrPointer = false;

    // 检查原始变量是否是指针或数组类型 (注意 var 可能是 Pointer(Pointer(...)) 因为它是 alloca 出来的)
    if (var) {
        auto varType = var->getIRType();
        if (auto pType = std::dynamic_pointer_cast<POINTERType>(varType)) {
            auto subType = pType->getPointerType();
            if (subType->getEnum() == IR_ARRAY || subType->getEnum() == IR_POINTER) {
                isArrayOrPointer = true;
            }
            // 特殊情况：如果是函数参数中的数组 int arr[]，它在符号表中是 i32** (Pointer->Pointer->Int)
            if (auto subPType = std::dynamic_pointer_cast<POINTERType>(subType)) {
                 if (subPType->getPointerType()->getEnum() == IR_INT ||
                     subPType->getPointerType()->getEnum() == IR_FLOAT) {
                     // 这是一个指针变量（如参数 int* a）
                     isArrayOrPointer = true;
                 }
            }
        }
    }

    // 只有当：
    // 1. 它不是数组/指针变量
    // OR
    // 2. 它是数组/指针变量，但是被完全索引了 (node.get_array() 不为空且有表达式)
    // 才进行 Load。

    bool hasIndices = (node.get_array() != nullptr) && (!node.get_array()->get_expressions().empty());

    if (!isArrayOrPointer || hasIndices) {
        // 普通变量，或者数组元素访问 -> 需要 Load 取值
        Value* loadedValue = currentBasicBlock->genLoadInst(ptr);
        pushValue(loadedValue);
    } else {
        // 数组名或指针名作为参数传递 -> 不需要 Load，直接传地址
        pushValue(ptr);
    }
    // --- 【修改结束】 ---
}
void IRGenerator::getLValPointer(LVal &node) {
    // 获取LVal的指针（地址）然后加入Value Stack中返回
    std::string ident = node.get_ident();
    Value* ptr = ModuleIR->getSymbolTable().getValueByName(ident);
    if (ptr->isConst()) {
        pushValue(ptr);
       return;
    }
    std::vector<Value*> indices;
    if (node.get_array()) {
        auto &exprs = node.get_array()->get_expressions();
        for (auto &expr : exprs) {
            expr->accept(*this);
            Value* index = popValue();
            indices.push_back(index);
        }
    }
    Value* handle;
    auto ptrType = std::dynamic_pointer_cast<POINTERType>(ptr->getIRType());
    assert(ptrType && "LVal symbol in SymbolTable is not a pointer!");
    TypeSystem subTypeEnum = ptrType->getPointerType()->getEnum();

    if (subTypeEnum == IR_ARRAY) {
        // 数组类型：生成GEP指令获取基地址
        handle = currentBasicBlock->genGepInst(ptr);
        dynamic_cast<GetElementPtrInst*>(handle)->addUse(ConstInt::newConstInt(0));
        if (!node.get_array() || node.get_array()->get_expressions().empty()) {
            handle = currentBasicBlock->genGepInst(handle);
            dynamic_cast<GetElementPtrInst*>(handle)->addUse(ConstInt::newConstInt(0));
        }
    }
    else if (subTypeEnum == IR_POINTER) {
        // --- 【修改开始】 ---
        // 加载指针值 (从 i32** 得到 i32*)
        handle = currentBasicBlock->genLoadInst(ptr);
        // --- 【修改结束】 ---
    }
    else {
        // 普通变量
        if (!indices.empty()) {
            std::cerr << "Non-array variable cannot have indices" << std::endl;
            assert(0);
        }
        pushValue(ptr);
        return;
    }

    // 处理数组索引
    for (auto &index : indices) {
        // 注意：if条件中定义的变量 gep 在 else 中也是可见的（虽然是null）
        // 所以 else 中不能再定义同名的 auto gep
        if (auto gep = dynamic_cast<GetElementPtrInst*>(handle)) {
            gep->addUse(index);
            gep->updateType();
        }
        else {
            handle = currentBasicBlock->genGepInst(handle);
            // 【修改点】：将变量名从 gep 改为 new_gep，避免重定义错误
            auto new_gep = dynamic_cast<GetElementPtrInst*>(handle);
            new_gep->addUse(index);
            new_gep->updateType();
        }

        if (index != indices.back())
        {
            auto currentType = handle->getIRType();
            auto t = currentType->getEnum();
            // 处理多维数组或指针数组的解引用
            if (t == IR_POINTER) {
                auto pType = std::dynamic_pointer_cast<POINTERType>(currentType);
                if (pType && pType->getPointerType()->getEnum() == IR_POINTER)
                    handle = currentBasicBlock->genLoadInst(handle);
            }
            else if (t == IR_ARRAY) {
                 // ... 保持原有逻辑
            }
        }
    }
    pushValue(handle);
}

void IRGenerator::visit(UnaryExp &node) {
    //1.获得操作数链表
    auto &operands = node.getOperands();
    auto &ops = node.getOps();
    enterUnaryExp = true;
    pushValue(getOperand(operands, ops));
    enterUnaryExp = false;
}

void IRGenerator::visit(MulExp &node) {
    auto &operands = node.getOperands();
    auto &ops = node.getOps();
    pushValue(getOperand(operands, ops));
}

void IRGenerator::visit(AddExp &node) {
    auto &operands = node.getOperands();
    auto &ops = node.getOps();
    pushValue(getOperand(operands, ops));
}

void IRGenerator::visit(RelExp &node) {
    auto &operands = node.getOperands();
    auto &ops = node.getOps();
    pushValue(getOperand(operands, ops));
}

void IRGenerator::visit(EqExp &node) {
    auto &operands = node.getOperands();
    auto &ops = node.getOps();
    pushValue(getOperand(operands, ops));
}

void IRGenerator::visit(LAndExp &node) {
    auto &operands = node.getOperands();
    auto &ops = node.getOps();
    pushValue(getOperand(operands, ops));
}

void IRGenerator::visit(LOrExp &node) {
    auto &operands = node.getOperands();
    auto &ops = node.getOps();
    pushValue(getOperand(operands, ops));
}

//函数调用表达式
void IRGenerator::visit(FuncCall &node) {
    // 为当前函数调用创建新的参数列表
    std::vector<Value*> currentArgs;
    argsStack.push(currentArgs);

    if (node.has_params())
        node.get_params()->accept(*this);

    // 获取当前函数的参数列表
    std::vector<Value*> args = argsStack.top();
    argsStack.pop();
    if (node.get_ident() == "starttime" || node.get_ident() == "stoptime") {
        Value* linenoVal = ConstInt::newConstInt(node.get_lineno());
        args.push_back(linenoVal);
    }
    auto inst = currentBasicBlock->genCallInst(node.get_ident(), args);
    pushValue(inst);
}


void IRGenerator::visit(FuncRParamList &node) {
    for (auto &i : node.get_params()) {
        i->accept(*this);
        // 将参数加入当前栈顶的参数列表
        argsStack.top().push_back(popValue());
    }
}

void IRGenerator::visit(ArrayList &node) { //数组声明修饰符
    /*调用该函数的时候，需要在外面传入arrayTypeStack*/
    auto baseType = arrayTypeStack.top();
    arrayTypeStack.pop();
    auto &arr = node.get_expressions();
    for (auto i = arr.rbegin(); i != arr.rend(); i++) {
        //获取维度表达式(AddExp)int a [2][3](a后面的就是维度表达式)，记得压入Value stack //TODO
        (*i)->accept(*this);
        Value* dimValue = popValue();
        if (auto constInt = dynamic_cast<ConstInt*>(dimValue)) {
            baseType = ARRAYType::NewArray(baseType, constInt->getVal());
        } else if (auto constFloat = dynamic_cast<ConstFloat*>(dimValue)) {
            //这里看看需不需要static_cast的类型转换
            baseType = ARRAYType::NewArray(baseType, static_cast<int>(constFloat->getVal()));
        } else
            assert(0);
    }
    arrayTypeStack.push(baseType);
}

void IRGenerator::visit(ConValue<int> &node) {
    pushValue(ConstInt::newConstInt(node.get_value()));
}

void IRGenerator::visit(ConValue<float> &node) {
    pushValue(ConstFloat::newConstFloat(node.get_value()));
}

/*

表达式调用层级：
LOrExp → LAndExp → EqExp → RelExp → AddExp → MulExp → UnaryExp → PrimaryExp
│
├── LVal
├── FuncCall
└── ConstValue

语句包含：
Stmt
->AssignStmt
->ExpStmt
->IfStmt
->WhileStmt
->BreakStmt
->ContinueStmt
->ReturnStmt
//一定要注意下面这两种情况的处理
->还有VarDef/ConstDef --局部变量和常量部分
->Block嵌套

CompUnit
->ConstDecl/VarDecl
->ConstDefList/VarDefList
->ConstDef/VarDef --全局变量/常量
->FuncDef
->FuncParamList
->FuncParam --函数形参
->Block --语句块
->BlockItemList
->Stmt语句层级
ArrayList:数组维度
InitVal/InitValList：只有初始化的时候才会被调用
*/