// Minimal Luau source AST produced by the lifter and printed by the emitter.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace luaudec
{

struct Expr;
struct Stmt;
struct FunctionBody;

using ExprP = std::shared_ptr<Expr>;
using StmtP = std::shared_ptr<Stmt>;
using FunctionP = std::shared_ptr<FunctionBody>;

enum class ExprKind
{
    Nil,
    True,
    False,
    Number,
    Integer,
    String,
    Vector,
    Vararg,
    Local,    // named variable (str = name, reg = register it lives in)
    Global,   // str = name
    Upvalue,  // str = name
    Index,    // a[b]
    Call,     // a(args)
    MethodCall, // a:str(args)
    Function, // func
    BinOp,    // a str b
    UnOp,     // str a
    Table,    // { items }
    IfElse,   // if a then b else args[0]
    Raw,      // verbatim text (used for unsupported constructs)
};

struct TableItem
{
    ExprP key;   // null for positional items
    ExprP value; // null for a template slot that has not been filled yet (dropped when printed)
};

struct Expr
{
    ExprKind kind = ExprKind::Nil;
    double number = 0.0;
    int64_t integer = 0;
    std::string str;
    double vec[4] = {0.0, 0.0, 0.0, 0.0};
    ExprP a;
    ExprP b;
    std::vector<ExprP> args;
    bool multret = false;      // Call/MethodCall/Vararg that expands to all its results
    bool singleResult = false; // Call to a builtin that always yields exactly one value
    bool hex = false;          // Number: print as a hexadecimal literal
    FunctionP func;
    std::vector<TableItem> items;
    int reg = -1;
};

enum class StmtKind
{
    Local,         // local names = values
    Assign,        // targets = values
    Call,          // expr
    Return,        // return values
    FunctionDecl,  // function expr(...) / function expr:name(...)
    LocalFunction, // local function name(...)
    If,
    While,
    Repeat,
    NumFor,
    GenFor,
    Break,
    Continue,
    Do,
    Comment,
};

struct IfClause
{
    ExprP cond;
    std::vector<StmtP> body;
};

struct Stmt
{
    StmtKind kind = StmtKind::Comment;
    std::vector<std::string> names;  // Local, LocalFunction, NumFor (1), GenFor
    std::vector<ExprP> targets;      // Assign
    std::vector<ExprP> values;       // Local, Assign, Return, NumFor (from, to[, step]), GenFor (iterators)
    ExprP expr;                      // Call, While/Repeat condition, FunctionDecl name path
    std::string methodName;          // FunctionDecl: non-empty for `function obj:name`
    FunctionP func;                  // FunctionDecl, LocalFunction
    std::vector<IfClause> clauses;   // If (first clause is the `if`, the rest are `elseif`)
    bool hasElse = false;
    std::vector<StmtP> elseBody;     // If
    std::vector<StmtP> body;         // While, Repeat, NumFor, GenFor, Do
    std::string text;                // Comment
};

struct FunctionBody
{
    std::vector<std::string> params;
    bool vararg = false;
    std::vector<StmtP> body;
};

inline ExprP mkExpr(ExprKind kind)
{
    auto e = std::make_shared<Expr>();
    e->kind = kind;
    return e;
}

inline ExprP mkNumber(double v)
{
    ExprP e = mkExpr(ExprKind::Number);
    e->number = v;
    return e;
}

inline ExprP mkString(const std::string& s)
{
    ExprP e = mkExpr(ExprKind::String);
    e->str = s;
    return e;
}

inline ExprP mkName(ExprKind kind, const std::string& name, int reg = -1)
{
    ExprP e = mkExpr(kind);
    e->str = name;
    e->reg = reg;
    return e;
}

inline ExprP mkIndex(ExprP obj, ExprP key)
{
    ExprP e = mkExpr(ExprKind::Index);
    e->a = std::move(obj);
    e->b = std::move(key);
    return e;
}

inline ExprP mkBinOp(const std::string& op, ExprP lhs, ExprP rhs)
{
    ExprP e = mkExpr(ExprKind::BinOp);
    e->str = op;
    e->a = std::move(lhs);
    e->b = std::move(rhs);
    return e;
}

inline ExprP mkUnOp(const std::string& op, ExprP operand)
{
    ExprP e = mkExpr(ExprKind::UnOp);
    e->str = op;
    e->a = std::move(operand);
    return e;
}

inline StmtP mkStmt(StmtKind kind)
{
    auto s = std::make_shared<Stmt>();
    s->kind = kind;
    return s;
}

inline StmtP mkComment(const std::string& text)
{
    StmtP s = mkStmt(StmtKind::Comment);
    s->text = text;
    return s;
}

} // namespace luaudec
