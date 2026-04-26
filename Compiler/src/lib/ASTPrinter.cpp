#include "../../include/lib/ASTPrinter.hpp"
#include <iostream>

std::string typeToString(Type type) {
    switch(type) {
        case SY_INT: return "int";
        case SY_FLOAT: return "float";
        case SY_VOID: return "void";
        case SY_ADD: return "+";
        case SY_SUB: return "-";
        case SY_MUL: return "*";
        case SY_MOD: return "%";
        case SY_DIV: return "/";
        case SY_GREAT: return ">";
        case SY_GREATEQ: return ">=";
        case SY_LESS: return "<";
        case SY_LESSEQ: return "<=";
        case SY_EQ: return "==";
        case SY_NOTEQ: return "!=";
        case SY_OR: return "||";
        case SY_AND: return "&&";
        case SY_NOT: return "!";
        case SY_ASSIGN: return "=";
        default: return "unknown";
    }
}

void ASTPrinter::visit(CompUnit &node){
    std::cout << "======================================" << std::endl;
    std::cout << "                AST调试                " << std::endl;
    std::cout << "======================================" << std::endl;
    for(auto &n : node.get_items()){
        n->accept(*this);
    }
}

void ASTPrinter::visit(ConstDecl &node) {
    std::cout << "const "<< typeToString(node.get_type()) << " ";
    if (node.get_ConstDefList()) {
        node.get_ConstDefList()->accept(*this);
    }
    std::cout << ";" << std::endl;
}

void ASTPrinter::visit(VarDecl &node) {
    std::cout << typeToString(node.get_type()) << " ";
    if (node.get_VarDefList()) {
        node.get_VarDefList()->accept(*this);
    }
    std::cout << ";" << std::endl;
}

void ASTPrinter::visit(ConstDefList &node) {
    bool first = true;
    for (const auto& def : node.get_defs()) {
        if (!first) {
            std::cout << ", ";
        }
        first = false;
        def->accept(*this);
    }
}

void ASTPrinter::visit(VarDefList &node) {
    bool first = true;
    for (const auto& def : node.get_defs()) {
        if (!first) {
            std::cout << ", ";
        }
        first = false;
        def->accept(*this);
    }
}

void ASTPrinter::visit(ConstDef &node) {
    std::cout << node.get_ident();

    if (node.has_array() && node.get_array()) {
        node.get_array()->accept(*this);
    }

    if (node.has_initval() && node.get_initval()) {
        std::cout << " = ";
        node.get_initval()->accept(*this);
    }
}

void ASTPrinter::visit(VarDef &node) {
    std::cout << node.get_ident();

    if (node.has_array() && node.get_array()) {
        node.get_array()->accept(*this);
    }

    if (node.has_initval() && node.get_initval()) {
        std::cout << " = ";
        node.get_initval()->accept(*this);
    }
}

// 函数相关visit实现
void ASTPrinter::visit(FuncDef &node) {
    std::cout << typeToString(node.get_type()) << " " << node.get_ident();
    std::cout << "(";
    if (node.get_params()) {
        node.get_params()->accept(*this);
    }
    std::cout << ")" << std::endl;

    if (node.get_funcBody()) {
        node.get_funcBody()->accept(*this);
    }
    std::cout << std::endl;
}
void ASTPrinter::visit(FuncParam &node) {
    std::cout << typeToString(node.get_type()) << " " << node.get_ident();

    if (node.has_empty_brackets() || node.has_dimension()) {
        if (node.has_dimension()) {
            // 有具体维度
            std::cout << "[";
            node.get_exp_list()->accept(*this);
            std::cout << "]";
        } else {
            // 无维度（函数参数常见情况）
            std::cout << "[]";
        }
    }
}

void ASTPrinter::visit(FuncParamList &node) {
    bool first = true;
    for (const auto& param : node.get_params()) {
        if (!first) {
            std::cout << ", ";
        }
        first = false;
        param->accept(*this);
    }
}

// 初始化相关visit实现
void ASTPrinter::visit(InitValList &node) {
    std::cout << "{";
    bool first = true;
    for (const auto& initVal : node.get_initVals()) {
        if (!first) {
            std::cout << ", ";
        }
        first = false;
        initVal->accept(*this);
    }
    std::cout << "}";
}

void ASTPrinter::visit(InitVal &node) {
    if (node.get_value()) {
        node.get_value()->accept(*this);
    } else {
        std::cout << "{}";
    }
}

// 语句相关visit实现
void ASTPrinter::visit(Block &node) {
    std::cout << "{" << std::endl;
    if (node.get_items()) {
        node.get_items()->accept(*this);
    }
    std::cout << "}" << std::endl;
}

void ASTPrinter::visit(BlockItemList &node) {
    for (const auto& stmt : node.get_stmts()) {
        stmt->accept(*this);
    }
}

void ASTPrinter::visit(AssignStmt &node) {
    if (node.get_lval()) {
        node.get_lval()->accept(*this);
    }
    std::cout << " = ";
    if (node.get_exp()) {
        node.get_exp()->accept(*this);
    }
    std::cout << ";" << std::endl;
}

void ASTPrinter::visit(ExpStmt &node) {
    if (node.get_exp()) {
        node.get_exp()->accept(*this);
    }
    std::cout << ";" << std::endl;
}

void ASTPrinter::visit(IfStmt &node) {
    std::cout << "if (";
    if (node.get_condition()) {
        node.get_condition()->accept(*this);
    }
    std::cout << ") ";

    // 处理then分支
    if (node.get_if_stmt()) {
        bool thenIsBlock = dynamic_cast<Block*>(node.get_if_stmt().get());
        if (!thenIsBlock) {
            std::cout << "{" << std::endl << "  ";
            node.get_if_stmt()->accept(*this);
            std::cout << std::endl << "}";
        } else {
            node.get_if_stmt()->accept(*this);
        }
    }

    // 处理else分支
    if (node.has_else()) {
        std::cout << " else ";

        // 检查else分支是否是另一个IfStmt（即else if）
        auto elseIf = dynamic_cast<IfStmt*>(node.get_else_stmt().get());
        if (elseIf) {
            // 直接打印else if，不添加大括号
            elseIf->accept(*this);
        } else {
            bool elseIsBlock = dynamic_cast<Block*>(node.get_else_stmt().get());
            if (!elseIsBlock) {
                std::cout << "{" << std::endl << "  ";
                node.get_else_stmt()->accept(*this);
                std::cout << std::endl << "}";
            } else {
                node.get_else_stmt()->accept(*this);
            }
        }
    }
}
void ASTPrinter::visit(WhileStmt &node) {
    std::cout << "while (";
    if (node.get_condition()) {
        node.get_condition()->accept(*this);
    }
    std::cout << ") ";

    if (node.get_while_stmt()) {
        bool whileIsBlock = dynamic_cast<Block*>(node.get_while_stmt().get());
        if (!whileIsBlock) {
            std::cout << "{" << std::endl << "  ";
            node.get_while_stmt()->accept(*this);
            std::cout << std::endl << "}";
        } else {
            node.get_while_stmt()->accept(*this);
        }
    }
}

void ASTPrinter::visit(BreakStmt &/*node*/) {
    std::cout << "break;" << std::endl;
}

void ASTPrinter::visit(ContinueStmt &/*node*/) {
    std::cout << "continue;" << std::endl;
}

void ASTPrinter::visit(ReturnStmt &node) {
    std::cout << "return";
    if (node.has_return_value() && node.get_return_value()) {
        std::cout << " ";
        node.get_return_value()->accept(*this);
    }
    std::cout << ";" << std::endl;
}

// 表达式相关visit实现
void ASTPrinter::visit(LVal &node) {
    std::cout << node.get_ident();
    if (node.has_array() && node.get_array()) {
        node.get_array()->accept(*this);
    }
}

void ASTPrinter::visit(FuncCall &node) {
    std::cout << node.get_ident() << "(";
    if (node.has_params() && node.get_params()) {
        node.get_params()->accept(*this);
    }
    std::cout << ")";
}

void ASTPrinter::visit(ArrayList &node) {
    std::cout << "[";
    bool first = true;
    for (const auto& exp : node.get_expressions()) {
        if (!first) {
            std::cout << "][";
        }
        first = false;
        exp->accept(*this);
    }
    std::cout << "]";
}

void ASTPrinter::visit(FuncRParamList &node) {
    bool first = true;
    for (const auto& param : node.get_params()) {
        if (!first) {
            std::cout << ", ";
        }
        first = false;
        param->accept(*this);
    }
}

// 表达式visit实现 - 不使用模板函数
void ASTPrinter::visit(UnaryExp &node) {
    const auto& ops = node.getOps();
    const auto& operands = node.getOperands();

    // 如果没有操作符，直接打印操作数
    if (ops.empty()) {
        if (!operands.empty()) {
            operands.front()->accept(*this);
        }
        return;
    }

    // 打印所有操作符
    for (const auto& op : ops) {
        std::cout << typeToString(op);
    }

    // 打印操作数
    if (!operands.empty()) {
        // 检查操作数是否也是一元表达式
        if (auto nestedUnary = dynamic_cast<UnaryExp*>((*operands.begin()).get())) {
            // 如果嵌套的一元表达式也有操作符，需要括号
            if (!nestedUnary->getOps().empty()) {
                std::cout << "(";
                (*operands.begin())->accept(*this);
                std::cout << ")";
            } else {
                (*operands.begin())->accept(*this);
            }
        } else {
            // 其他类型的操作数直接打印
            (*operands.begin())->accept(*this);
        }
    }
}

void ASTPrinter::visit(MulExp &node) {
    const auto& operands = node.getOperands();
    const auto& ops = node.getOps();

    if (ops.empty()) {
        // 没有操作符，直接打印操作数
        if (!operands.empty()) {
            operands.front()->accept(*this);
        }
    } else {
        // 有操作符，添加括号确保优先级
        std::cout << "(";
        auto opIt = ops.begin();
        auto operandIt = operands.begin();

        // 打印第一个操作数
        if (operandIt != operands.end()) {
            (*operandIt)->accept(*this);
            ++operandIt;
        }

        // 依次打印操作符和操作数
        while (opIt != ops.end() && operandIt != operands.end()) {
            std::cout << " " << typeToString(*opIt) << " ";
            (*operandIt)->accept(*this);
            ++opIt;
            ++operandIt;
        }
        std::cout << ")";
    }
}

void ASTPrinter::visit(AddExp &node) {
    const auto& operands = node.getOperands();
    const auto& ops = node.getOps();

    if (ops.empty()) {
        // 没有操作符，直接打印操作数
        if (!operands.empty()) {
            operands.front()->accept(*this);
        }
    } else {
        // 有操作符，添加括号确保优先级
        std::cout << "(";
        auto opIt = ops.begin();
        auto operandIt = operands.begin();

        // 打印第一个操作数
        if (operandIt != operands.end()) {
            (*operandIt)->accept(*this);
            ++operandIt;
        }

        // 依次打印操作符和操作数
        while (opIt != ops.end() && operandIt != operands.end()) {
            std::cout << " " << typeToString(*opIt) << " ";
            (*operandIt)->accept(*this);
            ++opIt;
            ++operandIt;
        }
        std::cout << ")";
    }
}

// 对其他表达式类型也做同样的修改：RelExp, EqExp, LAndExp, LOrExp
void ASTPrinter::visit(RelExp &node) {
    const auto& operands = node.getOperands();
    const auto& ops = node.getOps();

    if (ops.empty()) {
        if (!operands.empty()) {
            operands.front()->accept(*this);
        }
    } else {
        std::cout << "(";
        auto opIt = ops.begin();
        auto operandIt = operands.begin();

        if (operandIt != operands.end()) {
            (*operandIt)->accept(*this);
            ++operandIt;
        }

        while (opIt != ops.end() && operandIt != operands.end()) {
            std::cout << " " << typeToString(*opIt) << " ";
            (*operandIt)->accept(*this);
            ++opIt;
            ++operandIt;
        }
        std::cout << ")";
    }
}

void ASTPrinter::visit(EqExp &node) {
    const auto& operands = node.getOperands();
    const auto& ops = node.getOps();

    if (ops.empty()) {
        if (!operands.empty()) {
            operands.front()->accept(*this);
        }
    } else {
        std::cout << "(";
        auto opIt = ops.begin();
        auto operandIt = operands.begin();

        if (operandIt != operands.end()) {
            (*operandIt)->accept(*this);
            ++operandIt;
        }

        while (opIt != ops.end() && operandIt != operands.end()) {
            std::cout << " " << typeToString(*opIt) << " ";
            (*operandIt)->accept(*this);
            ++opIt;
            ++operandIt;
        }
        std::cout << ")";
    }
}

void ASTPrinter::visit(LAndExp &node) {
    const auto& operands = node.getOperands();
    const auto& ops = node.getOps();

    if (ops.empty()) {
        if (!operands.empty()) {
            operands.front()->accept(*this);
        }
    } else {
        std::cout << "(";
        auto opIt = ops.begin();
        auto operandIt = operands.begin();

        if (operandIt != operands.end()) {
            (*operandIt)->accept(*this);
            ++operandIt;
        }

        while (opIt != ops.end() && operandIt != operands.end()) {
            std::cout << " " << typeToString(*opIt) << " ";
            (*operandIt)->accept(*this);
            ++opIt;
            ++operandIt;
        }
        std::cout << ")";
    }
}

void ASTPrinter::visit(LOrExp &node) {
    const auto& operands = node.getOperands();
    const auto& ops = node.getOps();

    if (ops.empty()) {
        if (!operands.empty()) {
            operands.front()->accept(*this);
        }
    } else {
        std::cout << "(";
        auto opIt = ops.begin();
        auto operandIt = operands.begin();

        if (operandIt != operands.end()) {
            (*operandIt)->accept(*this);
            ++operandIt;
        }

        while (opIt != ops.end() && operandIt != operands.end()) {
            std::cout << " " << typeToString(*opIt) << " ";
            (*operandIt)->accept(*this);
            ++opIt;
            ++operandIt;
        }
        std::cout << ")";
    }
}

// 常量visit实现
void ASTPrinter::visit(ConValue<int> &node) {
    std::cout << node.get_value();
}

void ASTPrinter::visit(ConValue<float> &node) {
    std::cout << node.get_value();
}