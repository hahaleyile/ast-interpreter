//==--- tools/clang-check/ClangInterpreter.cpp - Clang Interpreter tool --------------===//
//===----------------------------------------------------------------------===//
#include <stdio.h>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

using namespace clang;

class StackFrame {
    /// StackFrame maps Variable Declaration to Value
    /// Which are either integer or addresses (also represented using an Integer value)
    std::map<Decl *, int> mVars;
    std::map<Stmt *, int> mExprs;
    /// The current stmt
    Stmt *mPC;
public:
    StackFrame() : mVars(), mExprs(), mPC() {
    }

    void bindDecl(Decl *decl, int val) {
        mVars[decl] = val;
    }

    int getDeclVal(Decl *decl) {
        assert (mVars.find(decl) != mVars.end());
        return mVars[decl];
    }

    bool hasDeclVal(Decl *decl) {
        return mVars.find(decl) != mVars.end();
    }

    void bindStmt(Stmt *stmt, int val) {
        mExprs[stmt] = val;
    }

    int getStmtVal(Stmt *stmt) {
        assert (mExprs.find(stmt) != mExprs.end());
        return mExprs[stmt];
    }

    void setPC(Stmt *stmt) {
        mPC = stmt;
    }

    Stmt *getPC() {
        return mPC;
    }
};

/// Heap maps address to a value
/*
class Heap {
public:
   int Malloc(int size) ;
   void Free (int addr) ;
   void Update(int addr, int val) ;
   int get(int addr);
};
*/

class Environment {
    std::vector<StackFrame> mStack;

    /// Declartions to the built-in functions
    FunctionDecl *mFree;
    FunctionDecl *mMalloc;
    FunctionDecl *mInput;
    FunctionDecl *mOutput;

    FunctionDecl *mEntry;

    /// 定义一个全局变量字典，包括函数声明，如果是函数则值为函数的参数个数
    std::map<Decl *, int> gVars;

    void bindGDecl(Decl *decl, int parmNum) {
        gVars[decl] = parmNum;
    }

    int getGDeclVal(Decl *decl) {
        assert(gVars.find(decl) != gVars.end());
        return gVars[decl];
    }

    int getDeclVal(Decl *decl) {
        if (mStack.back().hasDeclVal(decl)) {
            return mStack.back().getDeclVal(decl);
        }
        return getGDeclVal(decl);
    }

public:
    /// Get the declartions to the built-in functions
    Environment() : mStack(), mFree(NULL), mMalloc(NULL), mInput(NULL), mOutput(NULL), mEntry(NULL) {
    }


    /// Initialize the Environment
    void init(TranslationUnitDecl *unit) {
        for (TranslationUnitDecl::decl_iterator i = unit->decls_begin(), e = unit->decls_end(); i != e; ++i) {
            if (FunctionDecl *fdecl = dyn_cast<FunctionDecl>(*i)) {
                if (fdecl->getName().equals("FREE")) mFree = fdecl;
                else if (fdecl->getName().equals("MALLOC")) mMalloc = fdecl;
                else if (fdecl->getName().equals("GET")) mInput = fdecl;
                else if (fdecl->getName().equals("PRINT")) mOutput = fdecl;
                else if (fdecl->getName().equals("main")) mEntry = fdecl;
                else bindGDecl(fdecl, fdecl->getNumParams());
            } else if (VarDecl *vdecl = dyn_cast<VarDecl>(*i)) {
                IntegerLiteral *integer;
                if (vdecl->hasInit() && (integer = dyn_cast<IntegerLiteral>(vdecl->getInit())))
                    bindGDecl(vdecl, integer->getValue().getSExtValue());
                else
                    bindGDecl(vdecl, 0);
            }
        }
        mStack.push_back(StackFrame());
    }

    FunctionDecl *getEntry() {
        return mEntry;
    }

    void binop(BinaryOperator *bop) {
        Expr *left = bop->getLHS();
        Expr *right = bop->getRHS();

        if (bop->isAssignmentOp()) {
            int val = mStack.back().getStmtVal(right);
            mStack.back().bindStmt(left, val);
            if (DeclRefExpr *declexpr = dyn_cast<DeclRefExpr>(left)) {
                Decl *decl = declexpr->getFoundDecl();
                mStack.back().bindDecl(decl, val);
            }
        } else if (bop->isAdditiveOp()) {
            int val1 = mStack.back().getStmtVal(left);
            int val2 = mStack.back().getStmtVal(right);
            int result;
            switch (bop->getOpcode()) {
                case clang::BO_Add:
                    result = val1 + val2;
                    break;
                case BO_Sub:
                    result = val1 - val2;
                    break;
                default:
                    assert(false);
                    break;
            }

            mStack.back().bindStmt(bop, result);
        }
    }

    // 将字面量整型保存到 statement 栈中供赋值语句使用
    void integer(IntegerLiteral *integer) {
        mStack.back().bindStmt(integer, integer->getValue().getSExtValue());
    }

    void decl(DeclStmt *declstmt) {
        for (DeclStmt::decl_iterator it = declstmt->decl_begin(), ie = declstmt->decl_end();
             it != ie; ++it) {
            Decl *decl = *it;
            if (VarDecl *vardecl = dyn_cast<VarDecl>(decl)) {
                IntegerLiteral *integer;
                if (vardecl->hasInit() && (integer = dyn_cast<IntegerLiteral>(vardecl->getInit())))
                    mStack.back().bindDecl(vardecl, integer->getValue().getSExtValue());
                else
                    mStack.back().bindDecl(vardecl, 0);
            }
        }
    }

    void declref(DeclRefExpr *declref) {
        mStack.back().setPC(declref);
        if (declref->getType()->isIntegerType()) {
            Decl *decl = declref->getFoundDecl();

            int val = getDeclVal(decl);
            mStack.back().bindStmt(declref, val);
        }
    }

    void cast(CastExpr *castexpr) {
        mStack.back().setPC(castexpr);
        if (castexpr->getType()->isIntegerType()) {
            Expr *expr = castexpr->getSubExpr();
            int val = mStack.back().getStmtVal(expr);
            mStack.back().bindStmt(castexpr, val);
        }
    }

    /// !TODO Support Function Call
    void call(CallExpr *callexpr) {
        mStack.back().setPC(callexpr);
        int val = 0;
        FunctionDecl *callee = callexpr->getDirectCallee();
        if (callee == mInput) {
            llvm::errs() << "Please Input an Integer Value : ";
            scanf("%d", &val);

            mStack.back().bindStmt(callexpr, val);
        } else if (callee == mOutput) {
            Expr *decl = callexpr->getArg(0);
            val = mStack.back().getStmtVal(decl);
            llvm::errs() << val;
        } else {
            StackFrame newFrame = StackFrame();
            for (auto it = callexpr->arg_begin(), ie = callexpr->arg_end(); it != ie; it++) {
                auto expr = *it;
            }
        }
    }
};


