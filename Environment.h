//==--- tools/clang-check/ClangInterpreter.cpp - Clang Interpreter tool --------------===//
//===----------------------------------------------------------------------===//
#include <stdio.h>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#ifdef NDEBUG
#undef assert
#define assert(expr) expr
#endif

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
    // 函数调用链，让 Return 语句能够给之前的函数赋值
    std::vector<CallExpr *> mFuncs;

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

    int getStmtVal(Stmt *stmt) { return mStack.back().getStmtVal(stmt); }
    void popStackFrame(){mStack.pop_back();}


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
        } else if (bop->isAdditiveOp() || bop->isMultiplicativeOp() || bop->isComparisonOp()) {
            int val1 = mStack.back().getStmtVal(left);
            int val2 = mStack.back().getStmtVal(right);
            int result;
            switch (bop->getOpcode()) {
                case BO_Add:
                    result = val1 + val2;
                    break;
                case BO_Sub:
                    result = val1 - val2;
                    break;
                case BO_Mul:
                    result = val1 * val2;
                    break;
                case BO_Div:
                    result = val1 / val2;
                    break;
                case BO_Rem:
                    result = val1 % val2;
                    break;
                case BO_GE:
                    result = val1 >= val2;
                    break;
                case BO_GT:
                    result = val1 > val2;
                    break;
                case BO_LE:
                    result = val1 <= val2;
                    break;
                case BO_LT:
                    result = val1 < val2;
                    break;
                case BO_EQ:
                    result = val1 == val2;
                    break;
                case BO_NE:
                    result = val1 != val2;
                    break;
                default:
                    exit(1);
                    break;
            }
            mStack.back().bindStmt(bop, result);
        } else {
            exit(1);
        }
    }

    void unaryop(UnaryOperator *oper) {
        oper->getOpcode();
        int val = getStmtVal(oper->getSubExpr());
        switch (oper->getOpcode()) {
            case UO_Minus:
                val = -val;
                break;
            default:
                exit(1);
                break;
        }
        mStack.back().bindStmt(oper, val);
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
                auto t = vardecl->getType().getTypePtr();
                if (vardecl->hasInit() && (integer = dyn_cast<IntegerLiteral>(vardecl->getInit())))
                    mStack.back().bindDecl(vardecl, integer->getValue().getSExtValue());
                else
                    mStack.back().bindDecl(vardecl, 0);
            }
        }
    }

    void returnStmt(ReturnStmt *returnstmt) {
        Expr *body = returnstmt->getRetValue();
        if (body) {
            int val = mStack.back().getStmtVal(body);
            CallExpr *origcall = mFuncs.back();
            mFuncs.pop_back();
            mStack.pop_back();
            mStack.back().bindStmt(origcall, val);
        }
    }

    void declRef(DeclRefExpr *declref) {
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

    /// 返回值代表是否为内部函数，返回true代表是内部函数
    bool call(CallExpr *callexpr) {
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
            assert(callexpr->getNumArgs() == getGDeclVal(callee));
            StackFrame newFrame = StackFrame();
            for (int i = 0; i < callexpr->getNumArgs(); i++) {
                Expr *expr = callexpr->getArg(i);
                int subval = mStack.back().getStmtVal(expr);
                Decl *parm;
                assert(parm = llvm::dyn_cast<ParmVarDecl>(callee->getParamDecl(i)));
                newFrame.bindDecl(parm, subval);
            }
            if (!callee->getReturnType()->isVoidType())
                mFuncs.push_back(callexpr);
            mStack.push_back(newFrame);
            mEntry = callee;
            return true;
        }
        return false;
    }

};


