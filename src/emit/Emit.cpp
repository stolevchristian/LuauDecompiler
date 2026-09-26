#include "emit/Emit.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace luaudec
{

namespace
{

const char* kKeywords[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while",
};

// Operator precedence, higher binds tighter. Matches the Luau parser.
enum Prec
{
    PrecOr = 1,
    PrecAnd = 2,
    PrecCompare = 3,
    PrecConcat = 4,
    PrecAdd = 5,
    PrecMul = 6,
    PrecUnary = 7,
    PrecPow = 8,
    PrecPrimary = 9,
};

int binPrec(const std::string& op)
{
    if (op == "or")
        return PrecOr;
    if (op == "and")
        return PrecAnd;
    if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "~=" || op == "==")
        return PrecCompare;
    if (op == "..")
        return PrecConcat;
    if (op == "+" || op == "-")
        return PrecAdd;
    if (op == "*" || op == "/" || op == "//" || op == "%")
        return PrecMul;
    if (op == "^")
        return PrecPow;
    return PrecCompare;
}

bool rightAssoc(const std::string& op)
{
    return op == ".." || op == "^";
}

int exprPrec(const Expr& e)
{
    switch (e.kind)
    {
    case ExprKind::BinOp:
        return binPrec(e.str);
    case ExprKind::UnOp:
        return PrecUnary;
    case ExprKind::Number:
        return (e.number < 0 || (e.number == 0 && std::signbit(e.number)) || std::isnan(e.number) || std::isinf(e.number)) ? PrecUnary : PrecPrimary;
    case ExprKind::Integer:
        return e.integer < 0 ? PrecUnary : PrecPrimary;
    case ExprKind::IfElse:
        return 0; // binds looser than everything, always parenthesized as an operand
    default:
        return PrecPrimary;
    }
}

// Can this expression be used as a prefix expression (callee, indexed object) without parentheses?
bool isPrefixExpr(const Expr& e)
{
    switch (e.kind)
    {
    case ExprKind::Local:
    case ExprKind::Global:
    case ExprKind::Upvalue:
    case ExprKind::Index:
    case ExprKind::Call:
    case ExprKind::MethodCall:
        return true;
    default:
        return false;
    }
}

std::string indentStr(int indent)
{
    return std::string(size_t(indent) * 4, ' ');
}

// indentation of the statement currently being printed, so function expressions nest correctly
int gCurrentIndent = 0;

std::string emitExprPrec(const ExprP& e, int minPrec);

std::string emitPrefix(const ExprP& e)
{
    std::string s = emitExpr(e);
    if (!isPrefixExpr(*e))
        return "(" + s + ")";
    return s;
}

// A call or `...` in the last position of an expression list expands to all of its values;
// wrap it in parentheses when the bytecode only kept one value.
std::string emitListItem(const ExprP& e, bool last)
{
    std::string s = emitExpr(e);
    bool expands = e->kind == ExprKind::Call || e->kind == ExprKind::MethodCall || e->kind == ExprKind::Vararg;
    if (last && expands && !e->multret && !e->singleResult)
        return "(" + s + ")";
    return s;
}

std::string emitArgs(const std::vector<ExprP>& args)
{
    std::string s = "(";
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i > 0)
            s += ", ";
        s += emitListItem(args[i], i + 1 == args.size());
    }
    s += ")";
    return s;
}

std::string emitFunction(const FunctionBody& f, int indent, bool skipFirstParam)
{
    std::string s = "(";
    bool first = true;
    for (size_t i = skipFirstParam ? 1 : 0; i < f.params.size(); ++i)
    {
        if (!first)
            s += ", ";
        first = false;
        s += f.params[i];
    }
    if (f.vararg)
    {
        if (!first)
            s += ", ";
        s += "...";
    }
    s += ")\n";
    int saved = gCurrentIndent;
    s += emitBlock(f.body, indent + 1);
    gCurrentIndent = saved;
    s += indentStr(indent) + "end";
    return s;
}

std::string emitKeyedItem(const ExprP& key)
{
    if (key->kind == ExprKind::String && isIdentifier(key->str))
        return key->str;
    return "[" + emitExpr(key) + "]";
}

std::string emitExprPrec(const ExprP& e, int minPrec)
{
    std::string s = emitExpr(e);
    if (exprPrec(*e) < minPrec)
        return "(" + s + ")";
    return s;
}

} // namespace

bool isIdentifier(const std::string& s)
{
    if (s.empty())
        return false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        char ch = s[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || (i > 0 && ch >= '0' && ch <= '9');
        if (!ok)
            return false;
    }
    for (const char* kw : kKeywords)
        if (s == kw)
            return false;
    return true;
}

// Length of the valid UTF-8 sequence starting at i, or 0 if the bytes are not valid UTF-8.
static size_t utf8SequenceLength(const std::string& s, size_t i)
{
    unsigned char lead = (unsigned char)s[i];
    size_t len = 0;
    if (lead >= 0xC2 && lead <= 0xDF)
        len = 2;
    else if (lead >= 0xE0 && lead <= 0xEF)
        len = 3;
    else if (lead >= 0xF0 && lead <= 0xF4)
        len = 4;
    else
        return 0;
    if (i + len > s.size())
        return 0;
    for (size_t k = 1; k < len; ++k)
        if (((unsigned char)s[i + k] & 0xC0) != 0x80)
            return 0;
    return len;
}

std::string quoteString(const std::string& s)
{
    std::string out = "\"";
    for (size_t i = 0; i < s.size(); ++i)
    {
        unsigned char ch = (unsigned char)s[i];

        if (ch >= 0x80)
        {
            // keep well-formed UTF-8 readable, escape stray high bytes
            size_t len = utf8SequenceLength(s, i);
            if (len)
            {
                out.append(s, i, len);
                i += len - 1;
            }
            else
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\%03d", int(ch));
                out += buf;
            }
            continue;
        }

        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\a':
            out += "\\a";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\v':
            out += "\\v";
            break;
        case 0:
            out += "\\0";
            // a following digit would extend the escape
            if (i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9')
                out += "\\" + std::to_string(int(ch));
            break;
        default:
            if (ch < 32 || ch == 127)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\%03d", int(ch));
                out += buf;
            }
            else
            {
                out += char(ch);
            }
            break;
        }
    }
    out += "\"";
    return out;
}

std::string formatNumber(double v)
{
    if (std::isnan(v))
        return "0/0";
    if (std::isinf(v))
        return v < 0 ? "-math.huge" : "math.huge";

    if (v == std::floor(v) && std::fabs(v) < 1e15)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.0f", v);
        if (v == 0 && std::signbit(v))
            return "-0";
        return buf;
    }

    for (int precision = 15; precision <= 17; ++precision)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (strtod(buf, nullptr) == v)
            return buf;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

std::string emitExpr(const ExprP& e)
{
    switch (e->kind)
    {
    case ExprKind::Nil:
        return "nil";
    case ExprKind::True:
        return "true";
    case ExprKind::False:
        return "false";
    case ExprKind::Number:
        if (e->hex && e->number >= 0 && e->number == std::floor(e->number) && e->number < 18446744073709551616.0)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)e->number);
            return buf;
        }
        return formatNumber(e->number);
    case ExprKind::Integer:
        return std::to_string((long long)e->integer);
    case ExprKind::String:
        return quoteString(e->str);
    case ExprKind::Vector:
    {
        std::string s = "vector.create(" + formatNumber(e->vec[0]) + ", " + formatNumber(e->vec[1]) + ", " + formatNumber(e->vec[2]);
        if (e->vec[3] != 0.0)
            s += ", " + formatNumber(e->vec[3]);
        return s + ")";
    }
    case ExprKind::Vararg:
        return "...";
    case ExprKind::Local:
    case ExprKind::Global:
    case ExprKind::Upvalue:
        return e->str;
    case ExprKind::Index:
        if (e->b->kind == ExprKind::String && isIdentifier(e->b->str))
            return emitPrefix(e->a) + "." + e->b->str;
        return emitPrefix(e->a) + "[" + emitExpr(e->b) + "]";
    case ExprKind::Call:
        return emitPrefix(e->a) + emitArgs(e->args);
    case ExprKind::MethodCall:
        return emitPrefix(e->a) + ":" + e->str + emitArgs(e->args);
    case ExprKind::Function:
        return "function" + emitFunction(*e->func, gCurrentIndent, false);
    case ExprKind::BinOp:
    {
        int prec = binPrec(e->str);
        bool right = rightAssoc(e->str);
        std::string lhs = emitExprPrec(e->a, right ? prec + 1 : prec);
        // `and`/`or` pick the same value whichever way they nest, so a same-operator right operand needs no parentheses
        bool sameLogical = (e->str == "and" || e->str == "or") && e->b->kind == ExprKind::BinOp && e->b->str == e->str;
        std::string rhs = emitExprPrec(e->b, (right || sameLogical) ? prec : prec + 1);
        return lhs + " " + e->str + " " + rhs;
    }
    case ExprKind::UnOp:
    {
        std::string operand = emitExprPrec(e->a, PrecUnary);
        if (e->str == "not")
            return "not " + operand;
        // avoid "- -x" turning into a comment
        if (e->str == "-" && !operand.empty() && operand[0] == '-')
            return "-(" + operand + ")";
        return e->str + operand;
    }
    case ExprKind::Table:
    {
        // template slots that were never filled carry no value
        std::vector<const TableItem*> items;
        for (const TableItem& item : e->items)
            if (item.value)
                items.push_back(&item);
        if (items.empty())
            return "{}";
        std::string s = "{";
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i > 0)
                s += ", ";
            const TableItem& item = *items[i];
            if (item.key)
                s += emitKeyedItem(item.key) + " = " + emitExpr(item.value);
            else
                s += emitListItem(item.value, i + 1 == items.size());
        }
        return s + "}";
    }
    case ExprKind::IfElse:
        return "if " + emitExpr(e->a) + " then " + emitExpr(e->b) + " else " + emitExpr(e->args[0]);
    case ExprKind::Raw:
        return e->str;
    }
    return "nil";
}

namespace
{

std::string emitStmt(const StmtP& s, int indent)
{
    std::string ind = indentStr(indent);
    std::string out;
    gCurrentIndent = indent;

    switch (s->kind)
    {
    case StmtKind::Local:
    {
        out = ind + "local ";
        for (size_t i = 0; i < s->names.size(); ++i)
            out += (i ? ", " : "") + s->names[i];
        if (!s->values.empty())
        {
            out += " = ";
            for (size_t i = 0; i < s->values.size(); ++i)
                out += (i ? ", " : "") + emitExpr(s->values[i]);
        }
        return out + "\n";
    }
    case StmtKind::Assign:
    {
        out = ind;
        for (size_t i = 0; i < s->targets.size(); ++i)
            out += (i ? ", " : "") + emitExpr(s->targets[i]);
        out += " = ";
        for (size_t i = 0; i < s->values.size(); ++i)
            out += (i ? ", " : "") + emitExpr(s->values[i]);
        return out + "\n";
    }
    case StmtKind::Call:
    {
        std::string call = emitExpr(s->expr);
        // a statement starting with '(' is ambiguous with the previous line
        if (!call.empty() && call[0] == '(')
            call = ";" + call;
        return ind + call + "\n";
    }
    case StmtKind::Return:
    {
        out = ind + "return";
        for (size_t i = 0; i < s->values.size(); ++i)
            out += (i ? ", " : " ") + emitListItem(s->values[i], i + 1 == s->values.size());
        return out + "\n";
    }
    case StmtKind::FunctionDecl:
    {
        out = ind + "function " + emitExpr(s->expr);
        if (!s->methodName.empty())
            out += ":" + s->methodName;
        out += emitFunction(*s->func, indent, !s->methodName.empty());
        return out + "\n";
    }
    case StmtKind::LocalFunction:
        return ind + "local function " + s->names[0] + emitFunction(*s->func, indent, false) + "\n";
    case StmtKind::If:
    {
        for (size_t i = 0; i < s->clauses.size(); ++i)
        {
            out += ind + (i == 0 ? "if " : "elseif ") + emitExpr(s->clauses[i].cond) + " then\n";
            out += emitBlock(s->clauses[i].body, indent + 1);
        }
        if (s->hasElse)
        {
            out += ind + "else\n";
            out += emitBlock(s->elseBody, indent + 1);
        }
        return out + ind + "end\n";
    }
    case StmtKind::While:
        out = ind + "while " + emitExpr(s->expr) + " do\n" + emitBlock(s->body, indent + 1) + ind + "end\n";
        return out;
    case StmtKind::Repeat:
        out = ind + "repeat\n" + emitBlock(s->body, indent + 1) + ind + "until " + emitExpr(s->expr) + "\n";
        return out;
    case StmtKind::NumFor:
    {
        out = ind + "for " + s->names[0] + " = " + emitExpr(s->values[0]) + ", " + emitExpr(s->values[1]);
        if (s->values.size() > 2)
            out += ", " + emitExpr(s->values[2]);
        out += " do\n" + emitBlock(s->body, indent + 1) + ind + "end\n";
        return out;
    }
    case StmtKind::GenFor:
    {
        out = ind + "for ";
        for (size_t i = 0; i < s->names.size(); ++i)
            out += (i ? ", " : "") + s->names[i];
        out += " in ";
        for (size_t i = 0; i < s->values.size(); ++i)
            out += (i ? ", " : "") + emitExpr(s->values[i]);
        out += " do\n" + emitBlock(s->body, indent + 1) + ind + "end\n";
        return out;
    }
    case StmtKind::Break:
        return ind + "break\n";
    case StmtKind::Continue:
        return ind + "continue\n";
    case StmtKind::Do:
        return ind + "do\n" + emitBlock(s->body, indent + 1) + ind + "end\n";
    case StmtKind::Comment:
        return ind + "-- " + s->text + "\n";
    }
    return "";
}

} // namespace

std::string emitBlock(const std::vector<StmtP>& stmts, int indent)
{
    std::string out;
    for (const StmtP& s : stmts)
        out += emitStmt(s, indent);
    return out;
}

std::string emitChunk(const FunctionBody& main)
{
    return emitBlock(main.body, 0);
}

} // namespace luaudec
