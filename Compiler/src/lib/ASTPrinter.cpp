#include "../../include/lib/ASTPrinter.hpp"
#include <iostream>

namespace {

void printEscapedString(const std::string& value) {
    std::cout << '"';
    for (char character : value) {
        switch (character) {
        case '\a': std::cout << "\\a"; break;
        case '\b': std::cout << "\\b"; break;
        case '\f': std::cout << "\\f"; break;
        case '\n': std::cout << "\\n"; break;
        case '\r': std::cout << "\\r"; break;
        case '\t': std::cout << "\\t"; break;
        case '\v': std::cout << "\\v"; break;
        case '\\': std::cout << "\\\\"; break;
        case '"': std::cout << "\\\""; break;
        default: std::cout << character; break;
        }
    }
    std::cout << '"';
}

} // namespace

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
    for(auto &n : node.getItems()){
        n->accept(*this);
    }
}

void ASTPrinter::visit(ConstDecl &node) {
    std::cout << "const "<< typeToString(node.getType()) << " ";
    if (node.getConstDefList()) {
        node.getConstDefList()->accept(*this);
    }
    std::cout << ";" << std::endl;
}

void ASTPrinter::visit(VarDecl &node) {
    std::cout << typeToString(node.getType()) << " ";
    if (node.getVarDefList()) {
        node.getVarDefList()->accept(*this);
    }
    std::cout << ";" << std::endl;
}

void ASTPrinter::visit(ConstDefList &node) {
    bool first = true;
    for (const auto& def : node.getDefs()) {
        if (!first) {
            std::cout << ", ";
        }
        first = false;
        def->accept(*this);
    }
}

void ASTPrinter::visit(VarDefList &node) {
    bool first = true;
    for (const auto& def : node.getDefs()) {
        if (!first) {
            std::cout << ", ";
        }
        first = false;
        def->accept(*this);
    }
}

void ASTPrinter::visit(ConstDef &node) {
    std::cout << node.getIdent();

    if (node.hasArray() && node.getArray()) {
        node.getArray()->accept(*this);
    }

    if (node.hasInitVal() && node.getInitVal()) {
        std::cout << " = ";
        node.getInitVal()->accept(*this);
    }
}

void ASTPrinter::visit(VarDef &node) {
    std::cout << node.getIdent();

    if (node.hasArray() && node.getArray()) {
        node.getArray()->accept(*this);
    }

    if (node.hasInitVal() && node.getInitVal()) {
        std::cout << " = ";
        node.getInitVal()->accept(*this);
    }
}

// 输出函数签名、形参和函数体。
void ASTPrinter::visit(FuncDef &node) {
    std::cout << typeToString(node.getType()) << " " << node.getIdent();
    std::cout << "(";
    if (node.getParams()) {
        node.getParams()->accept(*this);
    }
    std::cout << ")" << std::endl;

    if (node.getFuncBody()) {
        node.getFuncBody()->accept(*this);
    }
    std::cout << std::endl;
}
void ASTPrinter::visit(FuncParam &node) {
    std::cout << typeToString(node.getType()) << " " << node.getIdent();

    if (node.hasEmptyBrackets() || node.hasDimension()) {
        if (node.hasDimension()) {
            // 依次输出数组形参的显式维度。
            std::cout << "[";
            node.getExpList()->accept(*this);
            std::cout << "]";
        } else {
            // 仅保留数组形参省略的首维。
            std::cout << "[]";
        }
    }
}

void ASTPrinter::visit(FuncParamList &node) {
    bool first = true;
    for (const auto& param : node.getParams()) {
        if (!first) {
            std::cout << ", ";
        }
        first = false;
        param->accept(*this);
    }
}

// 使用花括号递归输出聚合初始化值。
void ASTPrinter::visit(InitValList &node) {
    std::cout << "{";
    bool first = true;
    for (const auto& initVal : node.getInitVals()) {
        if (!first) {
            std::cout << ", ";
        }
        first = false;
        initVal->accept(*this);
    }
    std::cout << "}";
}

void ASTPrinter::visit(InitVal &node) {
    if (node.getValue()) {
        node.getValue()->accept(*this);
    } else {
        std::cout << "{}";
    }
}

// 按语句结构恢复类源码文本。
void ASTPrinter::visit(Block &node) {
    std::cout << "{" << std::endl;
    if (node.getItems()) {
        node.getItems()->accept(*this);
    }
    std::cout << "}" << std::endl;
}

void ASTPrinter::visit(BlockItemList &node) {
    for (const auto& stmt : node.getStmts()) {
        stmt->accept(*this);
    }
}

void ASTPrinter::visit(AssignStmt &node) {
    if (node.getLVal()) {
        node.getLVal()->accept(*this);
    }
    std::cout << " = ";
    if (node.getExp()) {
        node.getExp()->accept(*this);
    }
    std::cout << ";" << std::endl;
}

void ASTPrinter::visit(ExpStmt &node) {
    if (node.getExp()) {
        node.getExp()->accept(*this);
    }
    std::cout << ";" << std::endl;
}

void ASTPrinter::visit(IfStmt &node) {
    std::cout << "if (";
    if (node.getCondition()) {
        node.getCondition()->accept(*this);
    }
    std::cout << ") ";

    // 非块形式的真分支补充花括号，避免输出结构产生歧义。
    if (node.getIfStmt()) {
        bool thenIsBlock = dynamic_cast<Block*>(node.getIfStmt().get());
        if (!thenIsBlock) {
            std::cout << "{" << std::endl << "  ";
            node.getIfStmt()->accept(*this);
            std::cout << std::endl << "}";
        } else {
            node.getIfStmt()->accept(*this);
        }
    }

    // 否分支中的嵌套条件直接组成 else-if 链。
    if (node.hasElse()) {
        std::cout << " else ";

        auto elseIf = dynamic_cast<IfStmt*>(node.getElseStmt().get());
        if (elseIf) {
            elseIf->accept(*this);
        } else {
            bool elseIsBlock = dynamic_cast<Block*>(node.getElseStmt().get());
            if (!elseIsBlock) {
                std::cout << "{" << std::endl << "  ";
                node.getElseStmt()->accept(*this);
                std::cout << std::endl << "}";
            } else {
                node.getElseStmt()->accept(*this);
            }
        }
    }
}
void ASTPrinter::visit(WhileStmt &node) {
    std::cout << "while (";
    if (node.getCondition()) {
        node.getCondition()->accept(*this);
    }
    std::cout << ") ";

    if (node.getWhileStmt()) {
        bool whileIsBlock = dynamic_cast<Block*>(node.getWhileStmt().get());
        if (!whileIsBlock) {
            std::cout << "{" << std::endl << "  ";
            node.getWhileStmt()->accept(*this);
            std::cout << std::endl << "}";
        } else {
            node.getWhileStmt()->accept(*this);
        }
    }
}

void ASTPrinter::visit(BreakStmt &) {
    std::cout << "break;" << std::endl;
}

void ASTPrinter::visit(ContinueStmt &) {
    std::cout << "continue;" << std::endl;
}

void ASTPrinter::visit(ReturnStmt &node) {
    std::cout << "return";
    if (node.hasReturnValue() && node.getReturnValue()) {
        std::cout << " ";
        node.getReturnValue()->accept(*this);
    }
    std::cout << ";" << std::endl;
}

// 输出左值、调用、数组索引和实参列表。
void ASTPrinter::visit(LVal &node) {
    std::cout << node.getIdent();
    if (node.hasArray() && node.getArray()) {
        node.getArray()->accept(*this);
    }
}

void ASTPrinter::visit(FuncCall &node) {
    std::cout << node.getIdent() << "(";
    bool hasPreviousArgument = false;
    if (node.hasFormatString()) {
        printEscapedString(*node.getFormatString());
        hasPreviousArgument = true;
    }
    if (node.hasParams() && node.getParams()) {
        if (hasPreviousArgument) {
            std::cout << ", ";
        }
        node.getParams()->accept(*this);
    }
    std::cout << ")";
}

void ASTPrinter::visit(ArrayList &node) {
    std::cout << "[";
    bool first = true;
    for (const auto& exp : node.getExpressions()) {
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
    for (const auto& param : node.getParams()) {
        if (!first) {
            std::cout << ", ";
        }
        first = false;
        param->accept(*this);
    }
}

// 一元表达式按保存顺序输出操作符；嵌套一元操作需要括号保护结合关系。
void ASTPrinter::visit(UnaryExp &node) {
    const auto& ops = node.getOps();
    const auto& operands = node.getOperands();

    // 无操作符时直接向下输出唯一操作数。
    if (ops.empty()) {
        if (!operands.empty()) {
            operands.front()->accept(*this);
        }
        return;
    }

    // 一元操作符已经按从外到内的顺序保存。
    for (const auto& op : ops) {
        std::cout << typeToString(op);
    }

    if (!operands.empty()) {
        if (auto nestedUnary = dynamic_cast<UnaryExp*>((*operands.begin()).get())) {
            // 嵌套的一元操作链用括号明确结合关系。
            if (!nestedUnary->getOps().empty()) {
                std::cout << "(";
                (*operands.begin())->accept(*this);
                std::cout << ")";
            } else {
                (*operands.begin())->accept(*this);
            }
        } else {
            (*operands.begin())->accept(*this);
        }
    }
}

void ASTPrinter::visit(MulExp &node) {
    const auto& operands = node.getOperands();
    const auto& ops = node.getOps();

    if (ops.empty()) {
        // 无同级操作符时不额外添加括号。
        if (!operands.empty()) {
            operands.front()->accept(*this);
        }
    } else {
        // 同级操作链整体加括号，保持原语法树优先级。
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

void ASTPrinter::visit(AddExp &node) {
    const auto& operands = node.getOperands();
    const auto& ops = node.getOps();

    if (ops.empty()) {
        // 无同级操作符时不额外添加括号。
        if (!operands.empty()) {
            operands.front()->accept(*this);
        }
    } else {
        // 同级操作链整体加括号，保持原语法树优先级。
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

// 关系、相等和逻辑表达式沿用相同的操作链输出规则。
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

// 字面量按解析后的数值直接输出。
void ASTPrinter::visit(ConValue<int> &node) {
    std::cout << node.getValue();
}

void ASTPrinter::visit(ConValue<float> &node) {
    std::cout << node.getValue();
}
