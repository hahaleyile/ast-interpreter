//==--- tools/clang-check/ClangInterpreter.cpp - Clang Interpreter tool --------------===//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/EvaluatedExprVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"
#include <iostream>
#include <fstream>

using namespace clang;

#include "Environment.h"

class InterpreterVisitor :
        public EvaluatedExprVisitor<InterpreterVisitor> {
public:
    explicit InterpreterVisitor(const ASTContext &context, Environment *env)
            : EvaluatedExprVisitor(context), mEnv(env) {}

    virtual ~InterpreterVisitor() {}

    virtual void VisitBinaryOperator(BinaryOperator *bop) {
        int depth = mEnv->getCurrentDepth();
        for (auto *SubStmt: bop->children()) {
            if (SubStmt) {
                Visit(SubStmt);
                if (depth != mEnv->getCurrentDepth())
                    return;
            }
        }
        mEnv->binOp(bop);
    }

    // 字面量整型没有子语句，所以不用 VisitStmt()
    virtual void VisitIntegerLiteral(IntegerLiteral *integer) {
        mEnv->integer(integer);
    }

    virtual void VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *expr) {
        mEnv->ueot(expr);
    }

    virtual void VisitParenExpr(ParenExpr *expr) {
        int depth = mEnv->getCurrentDepth();
        for (auto *SubStmt: expr->children()) {
            if (SubStmt) {
                Visit(SubStmt);
                if (depth != mEnv->getCurrentDepth())
                    return;
            }
        }
        mEnv->paren(expr);
    }

    virtual void VisitDeclRefExpr(DeclRefExpr *expr) {
        int depth = mEnv->getCurrentDepth();
        for (auto *SubStmt: expr->children()) {
            if (SubStmt) {
                Visit(SubStmt);
                if (depth != mEnv->getCurrentDepth())
                    return;
            }
        }
        mEnv->declRef(expr);
    }

    virtual void VisitIfStmt(IfStmt *stmt) {
        Expr *cond = stmt->getCond();
        int depth = mEnv->getCurrentDepth();
        Visit(cond);
        if (depth != mEnv->getCurrentDepth())
            return;
        if (mEnv->getStmtVal(cond)) {
            Visit(stmt->getThen());
        } else {
            // 需要手动处理没有 Else 分支的情况
            if (Stmt *elseStmt = stmt->getElse()) {
                Visit(elseStmt);
            }
        }
    }

    virtual void VisitWhileStmt(WhileStmt *stmt) {
        Expr *cond = stmt->getCond();
        int depth = mEnv->getCurrentDepth();
        Visit(cond);
        if (depth != mEnv->getCurrentDepth())
            return;
        while (mEnv->getStmtVal(cond)) {
            Visit(stmt->getBody());
            if (depth != mEnv->getCurrentDepth())
                return;
            Visit(cond);
            if (depth != mEnv->getCurrentDepth())
                return;
        }
    }

    virtual void VisitForStmt(ForStmt *stmt) {
        int depth = mEnv->getCurrentDepth();
        if (stmt->getInit()) {
            Visit(stmt->getInit());
            if (depth != mEnv->getCurrentDepth())
                return;
        }
        Expr *cond = stmt->getCond();
        Stmt *body = stmt->getBody();
        Expr *inc = stmt->getInc();
        if (cond) {
            Visit(cond);
            if (depth != mEnv->getCurrentDepth())
                return;
            while (mEnv->getStmtVal(cond)) {
                if (body) {
                    Visit(body);
                    if (depth != mEnv->getCurrentDepth())
                        return;
                }
                if (inc) {
                    Visit(inc);
                    if (depth != mEnv->getCurrentDepth())
                        return;
                }
                Visit(cond);
                if (depth != mEnv->getCurrentDepth())
                    return;
            }
        } else {
            throw std::exception();
        }
    }

    virtual void VisitArraySubscriptExpr(ArraySubscriptExpr *expr) {
        int depth = mEnv->getCurrentDepth();
        for (auto *SubStmt: expr->children()) {
            if (SubStmt) {
                Visit(SubStmt);
                if (depth != mEnv->getCurrentDepth())
                    return;
            }
        }
        mEnv->arraySubscript(expr);
    }

    virtual void VisitUnaryOperator(UnaryOperator *oper) {
        int depth = mEnv->getCurrentDepth();
        for (auto *SubStmt: oper->children()) {
            if (SubStmt) {
                Visit(SubStmt);
                if (depth != mEnv->getCurrentDepth())
                    return;
            }
        }
        mEnv->unaryOp(oper);
    }

    /// 常见错误情况是死循环
    /// 光赋值返回值还不够，应该让这一层函数的所有语句立即返回
    /// 错误情况是例如return语句是if的分支，则if语句里的VisitStmt都会执行完即使应该返回了
    /// 这会导致本该返回的函数继续执行下面的语句，其中就有了call语句
    /// 修复方法是在访问子语句前获取一次调用深读，访问之后再获取一次并比对是否一样
    virtual void VisitReturnStmt(ReturnStmt *stmt) {
        int depth = mEnv->getCurrentDepth();
        for (auto *SubStmt: stmt->children()) {
            if (SubStmt) {
                Visit(SubStmt);
                if (depth != mEnv->getCurrentDepth())
                    return;
            }
        }
        mEnv->returnStmt(stmt);
    }

    virtual void VisitCastExpr(CastExpr *expr) {
        int depth = mEnv->getCurrentDepth();
        for (auto *SubStmt: expr->children()) {
            if (SubStmt) {
                Visit(SubStmt);
                if (depth != mEnv->getCurrentDepth())
                    return;
            }
        }
        mEnv->cast(expr);
    }

    virtual void VisitCallExpr(CallExpr *call) {
        int depth = mEnv->getCurrentDepth();
        Expr **args = call->getArgs();
        for (int i = 0; i < call->getNumArgs(); i++) {
            Visit(args[i]);
            if (depth != mEnv->getCurrentDepth())
                return;
        }
        if (mEnv->call(call)) {
            depth = mEnv->getCurrentDepth();
            FunctionDecl *entry = mEnv->getEntry();
            for (auto *SubStmt: entry->getBody()->children()) {
                if (SubStmt) {
                    Visit(SubStmt);
                    if (depth != mEnv->getCurrentDepth())
                        return;
                }
            }
            if (call->getDirectCallee()->getReturnType()->isVoidType())
                mEnv->popStackFrame();
        }
    }

    virtual void VisitDeclStmt(DeclStmt *declstmt) {
        mEnv->decl(declstmt);
    }

private:
    Environment *mEnv;
};

class InterpreterConsumer : public ASTConsumer {
public:
    explicit InterpreterConsumer(const ASTContext &context) : mEnv(),
                                                              mVisitor(context, &mEnv) {
    }

    virtual ~InterpreterConsumer() {}

    virtual void HandleTranslationUnit(clang::ASTContext &Context) {
        TranslationUnitDecl *decl = Context.getTranslationUnitDecl();
        mEnv.init(decl);

        FunctionDecl *entry = mEnv.getEntry();
        int depth = mEnv.getCurrentDepth();
        for (auto *SubStmt: entry->getBody()->children()) {
            if (SubStmt) {
                mVisitor.Visit(SubStmt);
                if (depth != mEnv.getCurrentDepth())
                    return;
            }
        }
    }

private:
    Environment mEnv;
    InterpreterVisitor mVisitor;
};

class InterpreterClassAction : public ASTFrontendAction {
public:
    virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
            clang::CompilerInstance &Compiler, llvm::StringRef InFile) {
        return std::make_unique<InterpreterConsumer>(Compiler.getASTContext());
    }
};


int main(int argc, char **argv) {
    if (argc > 1) {
        std::string filename = std::string(argv[1]);
        std::string index;
        if (argc > 2) {
            index = std::string(argv[2]);
        } else {
            std::cout << "请输入测试文件编号：" << std::endl;
            std::cin >> index;
        }
        filename.append(index).append(".c");
        std::ifstream t(filename);
        std::string buffer((std::istreambuf_iterator<char>(t)),
                           std::istreambuf_iterator<char>());
        clang::tooling::runToolOnCode(std::unique_ptr<clang::FrontendAction>(new InterpreterClassAction), buffer);
    }
}
