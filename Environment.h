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

class heap {
public:
    /// 所有的指针、整数、字符都看成是8字节大小
    int64_t *ptr;
    /// pointer size
    int size;

    heap(int64_t *p, int s) {
        ptr = p;
        size = s;
    }
};

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
    /// 定义一个堆区供数组和动态分配内存的变量使用
    std::vector<heap> gHeap;

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
    Environment()
            : mStack(), mFuncs(), mFree(NULL), mMalloc(NULL), mInput(NULL), mOutput(NULL), mEntry(NULL), gVars(),
              gHeap() {
    }

    int getStmtVal(Stmt *stmt) { return mStack.back().getStmtVal(stmt); }

    void popStackFrame() { mStack.pop_back(); }


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
                QualType type = vdecl->getType();
                if (type->isIntegerType()) {
                    IntegerLiteral *integer;
                    if (vdecl->hasInit() && (integer = dyn_cast<IntegerLiteral>(vdecl->getInit())))
                        bindGDecl(vdecl, integer->getValue().getSExtValue());
                    else
                        bindGDecl(vdecl, 0);
                } else if (type->isArrayType()) {
                    const ConstantArrayType *array = dyn_cast<ConstantArrayType>(type.getTypePtr());
                    int64_t size = array->getSize().getSExtValue();
                    int64_t *array_storage = new int64_t[size];
                    for (int i = 0; i < size; ++i) {
                        array_storage[i] = 0;
                    }
                    bindGDecl(vdecl, gHeap.size());
                    gHeap.push_back(heap(array_storage, 8));
                } else {
                    throw std::exception();
                }
            }
        }
        mStack.push_back(StackFrame());
    }

    FunctionDecl *getEntry() {
        return mEntry;
    }

    void binOp(BinaryOperator *bop) {
        Expr *left = bop->getLHS();
        Expr *right = bop->getRHS();

        if (bop->isAssignmentOp()) {
            int val = mStack.back().getStmtVal(right);
            /// 目前为止只有数组和指针能做左值
            if (llvm::isa<ArraySubscriptExpr>(left)) {
                ArraySubscriptExpr *array = llvm::dyn_cast<ArraySubscriptExpr>(left);
                int array_base = mStack.back().getStmtVal(array->getBase());
                int array_index = mStack.back().getStmtVal(array->getIdx());
                if (array->getType()->isIntegerType() || array->getType()->isPointerType()) {
                    int64_t *ptr = gHeap[array_base].ptr;
                    *(ptr + array_index) = val;
                } else if (array->getType()->isCharType()) {
                    throw std::exception();
                } else {
                    throw std::exception();
                }
                return;
            }
            if (llvm::isa<UnaryOperator>(left)) {
                int ptr_val = mStack.back().getStmtVal(left);
                int offset = ptr_val / 10000;
                int base = ptr_val % 10000;
                *((int64_t *) gHeap[base].ptr + offset) = val;
                return;
            }
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
                    /// val1 is base, val2 is offset
                    if (left->getType()->isPointerType()) {
                        int base = val1 % 10000;
                        int offset = val1 / 10000 + val2;
                        result = base + offset * 10000;
                    }
                        /// val2 is base, val1 is offset
                    else if (right->getType()->isPointerType()) {
                        int base = val2 % 10000;
                        int offset = val2 / 10000 + val1;
                        result = base + offset * 10000;
                    } else {
                        result = val1 + val2;
                    }
                    break;
                case BO_Sub:
                    /// val1 is base, val2 is offset
                    if (left->getType()->isPointerType()) {
                        int base = val1 % 10000;
                        int offset = val1 / 10000 - val2;
                        result = base + offset * 10000;
                    }
                        /// val2 is base, val1 is offset
                    else if (right->getType()->isPointerType()) {
                        int base = val2 % 10000;
                        int offset = val2 / 10000 - val1;
                        result = base + offset * 10000;
                    } else {
                        result = val1 - val2;
                    }
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
                    throw std::exception();
                    break;
            }
            mStack.back().bindStmt(bop, result);
        } else {
            throw std::exception();
        }
    }

    void unaryOp(UnaryOperator *oper) {
        oper->getOpcode();
        int val = getStmtVal(oper->getSubExpr());
        switch (oper->getOpcode()) {
            case UO_Minus:
                val = -val;
                break;

                /// 由于字典的值是 32 位 int，无法表示指针
                /// 因此这里只能把子语句的 int 值拆成两部分
                /// 10000 以上的值是偏移，10000 以下的值是基地值也就是指针数组的下标
            case UO_Deref:
                break;
            default:
                throw std::exception();
                break;
        }
        mStack.back().bindStmt(oper, val);
    }

    /// 这个表达式就存放数组的值，数组作为左值使用的情况就由BinaryOperator单独特殊处理
    void arraySubscript(ArraySubscriptExpr *array) {
        int array_base = mStack.back().getStmtVal(array->getBase());
        int array_index = mStack.back().getStmtVal(array->getIdx());
        int val;
        if (array->getType()->isIntegerType() || array->getType()->isPointerType()) {
            int64_t *ptr = gHeap[array_base].ptr;
            val = int(*(ptr + array_index));
        } else if (array->getType()->isCharType()) {
            throw std::exception();
        } else {
            throw std::exception();
        }
        mStack.back().bindStmt(array, val);
    }

    // 将字面量整型保存到 statement 栈中供赋值语句使用
    void integer(IntegerLiteral *integer) {
        mStack.back().bindStmt(integer, integer->getValue().getSExtValue());
    }

    void ueot(UnaryExprOrTypeTraitExpr *ueotexpr) {
        UnaryExprOrTypeTrait kind = ueotexpr->getKind();
        int result = 0;
        switch (kind) {
            default:
                throw std::exception();
                break;
            case UETT_SizeOf:
                result = 8;
                break;
        }
        mStack.back().bindStmt(ueotexpr, result);
    }

    void decl(DeclStmt *declstmt) {
        for (DeclStmt::decl_iterator it = declstmt->decl_begin(), ie = declstmt->decl_end();
             it != ie; ++it) {
            Decl *decl = *it;
            if (VarDecl *vardecl = dyn_cast<VarDecl>(decl)) {
                QualType type = vardecl->getType();
                if (type->isIntegerType()) {
                    IntegerLiteral *integer;
                    if (vardecl->hasInit() && (integer = dyn_cast<IntegerLiteral>(vardecl->getInit())))
                        mStack.back().bindDecl(vardecl, integer->getValue().getSExtValue());
                    else
                        mStack.back().bindDecl(vardecl, 0);
                }
                    /// 所有的指针、整数、字符都看成是8字节大小
                else if (type->isArrayType()) {
                    const ConstantArrayType *array;
                    assert(array = dyn_cast<ConstantArrayType>(type.getTypePtr()));
                    int64_t size = array->getSize().getSExtValue();
                    int64_t *array_storage = new int64_t[size];
                    for (int i = 0; i < size; ++i) {
                        array_storage[i] = 0;
                    }
                    mStack.back().bindDecl(vardecl, gHeap.size());
                    gHeap.push_back(heap(array_storage, 8));
                } else if (type->isPointerType()) {
                    mStack.back().bindDecl(vardecl, 0);
                } else {
                    llvm::errs() << type.getTypePtr();
                    throw std::exception();
                }
            }
        }
    }

    void returnStmt(ReturnStmt *returnstmt) {
        Expr *body = returnstmt->getRetValue();
        if (body) {
            // 添加对main函数的判断
            if (mStack.size() > 1) {
                int val = mStack.back().getStmtVal(body);
                CallExpr *origcall = mFuncs.back();
                mFuncs.pop_back();
                mStack.pop_back();
                mStack.back().bindStmt(origcall, val);
            }
        }
    }

    void paren(ParenExpr *parenexpr) {
        int result = mStack.back().getStmtVal(parenexpr->getSubExpr());
        mStack.back().bindStmt(parenexpr, result);
    }

    void declRef(DeclRefExpr *declref) {
        mStack.back().setPC(declref);
        if (declref->getType()->isIntegerType() || declref->getType()->isArrayType() ||
            declref->getType()->isPointerType()) {
            Decl *decl = declref->getFoundDecl();

            int val = getDeclVal(decl);
            mStack.back().bindStmt(declref, val);
        } else if (declref->getType()->isFunctionType()) {

        } else {
            auto t = declref->getType();
            throw std::exception();
        }
    }

    void cast(CastExpr *castexpr) {
        mStack.back().setPC(castexpr);
        if (llvm::isa<UnaryOperator>(castexpr->getSubExpr()) &&
            !strcmp(castexpr->getCastKindName(), "LValueToRValue")) {
            Expr *expr = castexpr->getSubExpr();
            int val = mStack.back().getStmtVal(expr);
            int offset = val / 10000;
            int base = val % 10000;
            mStack.back().bindStmt(castexpr, int(*((int64_t *) gHeap[base].ptr + offset)));
            return;
        }
        if (castexpr->getType()->isIntegerType() ||
            castexpr->getType()->isPointerType()) {
            Expr *expr = castexpr->getSubExpr();
            int val = mStack.back().getStmtVal(expr);
            mStack.back().bindStmt(castexpr, val);
        } else {
            throw std::exception();
        }
    }

    /// 返回值代表是否为内部函数，返回true代表是内部函数
    bool call(CallExpr *callexpr) {
        mStack.back().setPC(callexpr);
        int val = 0;
        FunctionDecl *callee = callexpr->getDirectCallee();
        if (callee->isDefined())
            callee = callee->getDefinition();
        if (callee == mInput) {
            llvm::errs() << "Please Input an Integer Value : ";
            scanf("%d", &val);

            mStack.back().bindStmt(callexpr, val);
        } else if (callee == mOutput) {
            Expr *decl = callexpr->getArg(0);
            val = mStack.back().getStmtVal(decl);
            llvm::errs() << val;
        } else if (callee == mMalloc) {
            Expr *decl = callexpr->getArg(0);
            int subval = mStack.back().getStmtVal(decl);
            if (llvm::isa<IntegerLiteral>(decl)) {
                subval *= 8;
            }
            mStack.back().bindStmt(callexpr, gHeap.size());
            int64_t *h = static_cast<int64_t *>(malloc(subval));
            gHeap.push_back(heap(h, 8));
        } else if (callee == mFree) {

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


