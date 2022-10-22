//==--- tools/clang-check/ClangInterpreter.cpp - Clang Interpreter tool --------------===//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/EvaluatedExprVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

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
    virtual void VisitIntegerLiteral(IntegerLiteral *integer){
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
        mEnv->declref(expr);
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
        mEnv->call(call);
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

#include <iostream>
#include <fstream>

int main(int argc, char **argv) {
    if (argc > 1) {
        std::ifstream t(argv[1]);
        std::string buffer((std::istreambuf_iterator<char>(t)),
                           std::istreambuf_iterator<char>());
#ifndef NDEBUG
        std::cout << buffer << std::endl;
#endif
        clang::tooling::runToolOnCode(std::unique_ptr<clang::FrontendAction>(new InterpreterClassAction), buffer);
    }
}
