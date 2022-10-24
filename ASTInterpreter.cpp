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
#ifndef NDEBUG
        bop->dump();
#endif
        VisitStmt(bop);
        mEnv->binop(bop);
    }

    // 字面量整型没有子语句，所以不用 VisitStmt()
    virtual void VisitIntegerLiteral(IntegerLiteral *integer) {
#ifndef NDEBUG
        integer->dump();
#endif
        mEnv->integer(integer);
    }

    virtual void VisitDeclRefExpr(DeclRefExpr *expr) {
#ifndef NDEBUG
        expr->dump();
#endif
        VisitStmt(expr);
        mEnv->declRef(expr);
    }

    virtual void VisitIfStmt(IfStmt *stmt) {
#ifndef NDEBUG
        stmt->dump();
#endif
        Expr *cond = stmt->getCond();
        Visit(cond);
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
#ifndef NDEBUG
        stmt->dump();
#endif
        Expr *cond = stmt->getCond();
        Visit(cond);
        while (mEnv->getStmtVal(cond)) {
            Visit(stmt->getBody());
            Visit(cond);
        }
    }

    virtual void VisitForStmt(ForStmt *stmt) {
#ifndef NDEBUG
        stmt->dump();
#endif
        if (stmt->getInit())
            Visit(stmt->getInit());
        Expr *cond = stmt->getCond();
        Stmt *body = stmt->getBody();
        Expr *inc = stmt->getInc();
        if (cond) {
            Visit(cond);
            while (mEnv->getStmtVal(cond)) {
                if (body)
                    Visit(body);
                if (inc)
                    Visit(inc);
                Visit(cond);
            }
        } else {
            exit(1);
        }
    }

    virtual void VisitUnaryOperator(UnaryOperator *oper) {
#ifndef NDEBUG
        oper->dump();
#endif
        VisitStmt(oper);
        mEnv->unaryop(oper);
    }

    virtual void VisitReturnStmt(ReturnStmt *stmt) {
#ifndef NDEBUG
        stmt->dump();
#endif
        VisitStmt(stmt);
        mEnv->returnStmt(stmt);
    }

    virtual void VisitCastExpr(CastExpr *expr) {
#ifndef NDEBUG
        expr->dump();
#endif
        VisitStmt(expr);
        mEnv->cast(expr);
    }

    virtual void VisitCallExpr(CallExpr *call) {
#ifndef NDEBUG
        call->dump();
#endif
        VisitStmt(call);
        if (mEnv->call(call)) {
            FunctionDecl *entry = mEnv->getEntry();
            VisitStmt(entry->getBody());
            if (call->getDirectCallee()->getReturnType()->isVoidType())
                mEnv->popStackFrame();
        }
    }

    virtual void VisitDeclStmt(DeclStmt *declstmt) {
#ifndef NDEBUG
        declstmt->dump();
#endif
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
#ifndef NDEBUG
        decl->dump();
#endif
        mEnv.init(decl);

        FunctionDecl *entry = mEnv.getEntry();
        mVisitor.VisitStmt(entry->getBody());
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
        std::cout << "请输入测试文件编号：" << std::endl;
        std::cin >> index;
        filename.append(index).append(".c");
        std::ifstream t(filename);
        std::string buffer((std::istreambuf_iterator<char>(t)),
                           std::istreambuf_iterator<char>());
#ifndef NDEBUG
        std::cout << buffer << std::endl;
#endif
        clang::tooling::runToolOnCode(std::unique_ptr<clang::FrontendAction>(new InterpreterClassAction), buffer);
    }
}
