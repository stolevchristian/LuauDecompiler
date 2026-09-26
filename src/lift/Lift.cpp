#include "lift/Lift.h"

#include "cfg/CFG.h"
#include "disasm/Disasm.h"
#include "emit/Emit.h"

#include "Luau/Bytecode.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>

namespace luaudec
{

namespace
{

struct LiftError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

bool isCondJump(LuauOpcode op)
{
    switch (op)
    {
    case LOP_JUMPIF:
    case LOP_JUMPIFNOT:
    case LOP_JUMPIFEQ:
    case LOP_JUMPIFLE:
    case LOP_JUMPIFLT:
    case LOP_JUMPIFNOTEQ:
    case LOP_JUMPIFNOTLE:
    case LOP_JUMPIFNOTLT:
    case LOP_JUMPXEQKNIL:
    case LOP_JUMPXEQKB:
    case LOP_JUMPXEQKN:
    case LOP_JUMPXEQKS:
        return true;
    default:
        return false;
    }
}

bool isUncondJump(LuauOpcode op)
{
    return op == LOP_JUMP || op == LOP_JUMPBACK || op == LOP_JUMPX;
}

bool isFastCallOp(LuauOpcode op)
{
    switch (op)
    {
    case LOP_FASTCALL:
    case LOP_FASTCALL1:
    case LOP_FASTCALL2:
    case LOP_FASTCALL2K:
    case LOP_FASTCALL3:
    case LOP_FASTPCALL:
        return true;
    default:
        return false;
    }
}

// Builtins reachable through FASTCALL that always produce exactly one value (so a call to them in
// the last position of an expression list needs no truncating parentheses).
bool builtinReturnsOneValue(int id)
{
    switch (id)
    {
    case LBF_NONE:
    case LBF_ASSERT:
    case LBF_MATH_FREXP:
    case LBF_MATH_MODF:
    case LBF_STRING_BYTE:
    case LBF_TABLE_INSERT:
    case LBF_TABLE_UNPACK:
    case LBF_SELECT_VARARG:
    case LBF_BUFFER_WRITEU8:
    case LBF_BUFFER_WRITEU16:
    case LBF_BUFFER_WRITEU32:
    case LBF_BUFFER_WRITEF32:
    case LBF_BUFFER_WRITEF64:
    case LBF_BUFFER_WRITEINTEGER:
        return false;
    default:
        return true;
    }
}

// Instructions that never transfer control and can appear inside a condition chain block.
bool isSimpleInsn(const Insn& insn)
{
    switch (insn.op)
    {
    case LOP_RETURN:
    case LOP_JUMP:
    case LOP_JUMPBACK:
    case LOP_JUMPX:
    case LOP_FORNPREP:
    case LOP_FORNLOOP:
    case LOP_FORGPREP:
    case LOP_FORGPREP_NEXT:
    case LOP_FORGPREP_INEXT:
    case LOP_FORGLOOP:
    case LOP_CMPPROTO:
        return false;
    case LOP_LOADB:
        return insn.c == 0;
    default:
        return !isCondJump(insn.op);
    }
}

bool isPure(const ExprP& e)
{
    switch (e->kind)
    {
    case ExprKind::Nil:
    case ExprKind::True:
    case ExprKind::False:
    case ExprKind::Number:
    case ExprKind::Integer:
    case ExprKind::String:
    case ExprKind::Vector:
    case ExprKind::Vararg:
    case ExprKind::Local:
    case ExprKind::Function:
        return true;
    default:
        return false;
    }
}

bool isLiteral(const ExprP& e)
{
    switch (e->kind)
    {
    case ExprKind::Nil:
    case ExprKind::True:
    case ExprKind::False:
    case ExprKind::Number:
    case ExprKind::Integer:
    case ExprKind::String:
    case ExprKind::Vector:
        return true;
    default:
        return false;
    }
}

bool exprReadsReg(const ExprP& e, int reg)
{
    if (!e)
        return false;
    if (e->kind == ExprKind::Local && e->reg == reg)
        return true;
    if (exprReadsReg(e->a, reg) || exprReadsReg(e->b, reg))
        return true;
    for (const ExprP& arg : e->args)
        if (exprReadsReg(arg, reg))
            return true;
    for (const TableItem& item : e->items)
        if (exprReadsReg(item.key, reg) || exprReadsReg(item.value, reg))
            return true;
    return false;
}

ExprP negate(const ExprP& e)
{
    if (e->kind == ExprKind::UnOp && e->str == "not")
        return e->a;
    if (e->kind == ExprKind::True)
        return mkExpr(ExprKind::False);
    if (e->kind == ExprKind::False || e->kind == ExprKind::Nil)
        return mkExpr(ExprKind::True);
    if (e->kind == ExprKind::BinOp)
    {
        if (e->str == "==")
            return mkBinOp("~=", e->a, e->b);
        if (e->str == "~=")
            return mkBinOp("==", e->a, e->b);
        // De Morgan: only truthiness matters for the result of `not`
        if (e->str == "and")
            return mkBinOp("or", negate(e->a), negate(e->b));
        if (e->str == "or")
            return mkBinOp("and", negate(e->a), negate(e->b));
    }
    return mkUnOp("not", e);
}

ExprP mkAnd(const ExprP& a, const ExprP& b)
{
    if (a->kind == ExprKind::True)
        return b;
    if (b->kind == ExprKind::True)
        return a;
    if (a->kind == ExprKind::False)
        return a;
    return mkBinOp("and", a, b);
}

ExprP mkOr(const ExprP& a, const ExprP& b)
{
    if (a->kind == ExprKind::False)
        return b;
    if (b->kind == ExprKind::False)
        return a;
    if (a->kind == ExprKind::True)
        return a;
    return mkBinOp("or", a, b);
}

// ---------------------------------------------------------------------------
// Per-proto register analysis: uses/defs, multret pairing, liveness
// ---------------------------------------------------------------------------

struct Analysis
{
    const Module& m;
    const Proto& p;
    std::vector<Insn> insns;
    CFG cfg;
    std::vector<std::vector<int>> uses;
    std::vector<std::vector<int>> defs;
    std::vector<int> multretBase; // for CALL B=0 / RETURN B=0 / SETLIST C=0: first register of the multret producer
    std::vector<int> builtinOfCall; // per CALL: the builtin id of the FASTCALL guarding it, or -1
    std::vector<std::vector<char>> liveIn;
    std::vector<std::vector<char>> liveOut;
    int numRegs = 0;

    Analysis(const Module& module, const Proto& proto)
        : m(module)
        , p(proto)
    {
        insns = decodeProto(p);
        cfg = buildCFG(insns, false);
        numRegs = std::max<int>(p.maxstacksize, 1) + 8;
        computeAccess();
        computeLiveness();
        computeBuiltins();
    }

    // A FASTCALL skips over the fallback sequence and the CALL itself, so the CALL it belongs to is the
    // instruction right before the skip target.
    void computeBuiltins()
    {
        builtinOfCall.assign(insns.size(), -1);
        for (const Insn& in : insns)
        {
            if (!isFastCallOp(in.op) || in.op == LOP_FASTPCALL || in.jumpTarget < 0)
                continue;
            int idx = insnIndexAtPc(insns, uint32_t(in.jumpTarget));
            if (idx > 0 && (insns[idx - 1].op == LOP_CALL || insns[idx - 1].op == LOP_CALLFB))
                builtinOfCall[idx - 1] = in.a;
        }
    }

    int idxOfPc(int pc) const
    {
        int idx = insnIndexAtPc(insns, uint32_t(pc));
        if (idx < 0)
        {
            if (size_t(pc) == p.code.size())
                return int(insns.size());
            throw LiftError("jump into the middle of an instruction at pc " + std::to_string(pc));
        }
        return idx;
    }

    int targetIdx(int insnIdx) const
    {
        return idxOfPc(insns[insnIdx].jumpTarget);
    }

    void computeAccess()
    {
        uses.assign(insns.size(), {});
        defs.assign(insns.size(), {});
        multretBase.assign(insns.size(), -1);

        int producer = -1;

        for (size_t i = 0; i < insns.size(); ++i)
        {
            const Insn& in = insns[i];
            std::vector<int>& u = uses[i];
            std::vector<int>& d = defs[i];
            const int a = in.a, b = in.b, c = in.c;

            auto useRange = [&](int from, int to) {
                for (int r = from; r <= to; ++r)
                    u.push_back(r);
            };

            switch (in.op)
            {
            case LOP_LOADNIL:
            case LOP_LOADB:
            case LOP_LOADN:
            case LOP_LOADK:
            case LOP_LOADKX:
            case LOP_GETGLOBAL:
            case LOP_GETUPVAL:
            case LOP_GETIMPORT:
            case LOP_NEWTABLE:
            case LOP_DUPTABLE:
                d.push_back(a);
                break;
            case LOP_MOVE:
            case LOP_GETTABLEKS:
            case LOP_GETTABLEN:
            case LOP_NOT:
            case LOP_MINUS:
            case LOP_LENGTH:
            case LOP_ADDK:
            case LOP_SUBK:
            case LOP_MULK:
            case LOP_DIVK:
            case LOP_IDIVK:
            case LOP_MODK:
            case LOP_POWK:
            case LOP_ANDK:
            case LOP_ORK:
            case LOP_GETUDATAKS:
                u.push_back(b);
                d.push_back(a);
                break;
            case LOP_SUBRK:
            case LOP_DIVRK:
                u.push_back(c);
                d.push_back(a);
                break;
            case LOP_SETGLOBAL:
            case LOP_SETUPVAL:
                u.push_back(a);
                break;
            case LOP_GETTABLE:
            case LOP_ADD:
            case LOP_SUB:
            case LOP_MUL:
            case LOP_DIV:
            case LOP_IDIV:
            case LOP_MOD:
            case LOP_POW:
            case LOP_AND:
            case LOP_OR:
                u.push_back(b);
                u.push_back(c);
                d.push_back(a);
                break;
            case LOP_SETTABLE:
                u.push_back(a);
                u.push_back(b);
                u.push_back(c);
                break;
            case LOP_SETTABLEKS:
            case LOP_SETTABLEN:
            case LOP_SETUDATAKS:
                u.push_back(a);
                u.push_back(b);
                break;
            case LOP_CONCAT:
                useRange(b, c);
                d.push_back(a);
                break;
            case LOP_NEWCLOSURE:
            case LOP_DUPCLOSURE:
                for (const Capture& cap : in.captures)
                    if (cap.type == LCT_VAL || cap.type == LCT_REF)
                        u.push_back(cap.index);
                d.push_back(a);
                break;
            case LOP_NAMECALL:
            case LOP_NAMECALLUDATA:
                u.push_back(b);
                d.push_back(a);
                d.push_back(a + 1);
                break;
            case LOP_CALL:
            case LOP_CALLFB:
                if (b == 0)
                {
                    if (producer < 0)
                        throw LiftError("CALL with MULTRET arguments has no producer at pc " + std::to_string(in.pc));
                    multretBase[i] = producer;
                    useRange(a, producer);
                }
                else
                {
                    useRange(a, a + b - 1);
                }
                producer = -1;
                if (c == 0)
                {
                    d.push_back(a);
                    producer = a;
                }
                else
                {
                    for (int r = 0; r < c - 1; ++r)
                        d.push_back(a + r);
                }
                break;
            case LOP_RETURN:
                if (b == 0)
                {
                    if (producer < 0)
                        throw LiftError("RETURN with MULTRET has no producer at pc " + std::to_string(in.pc));
                    multretBase[i] = producer;
                    useRange(a, producer);
                }
                else
                {
                    useRange(a, a + b - 2);
                }
                producer = -1;
                break;
            case LOP_SETLIST:
                u.push_back(a);
                if (c == 0)
                {
                    if (producer < 0)
                        throw LiftError("SETLIST with MULTRET has no producer at pc " + std::to_string(in.pc));
                    multretBase[i] = producer;
                    useRange(b, producer);
                }
                else
                {
                    useRange(b, b + c - 2);
                }
                producer = -1;
                break;
            case LOP_GETVARARGS:
                if (b == 0)
                {
                    d.push_back(a);
                    producer = a;
                }
                else
                {
                    for (int r = 0; r < b - 1; ++r)
                        d.push_back(a + r);
                }
                break;
            case LOP_JUMPIF:
            case LOP_JUMPIFNOT:
            case LOP_JUMPXEQKNIL:
            case LOP_JUMPXEQKB:
            case LOP_JUMPXEQKN:
            case LOP_JUMPXEQKS:
            case LOP_CMPPROTO:
                u.push_back(a);
                break;
            case LOP_JUMPIFEQ:
            case LOP_JUMPIFLE:
            case LOP_JUMPIFLT:
            case LOP_JUMPIFNOTEQ:
            case LOP_JUMPIFNOTLE:
            case LOP_JUMPIFNOTLT:
                u.push_back(a);
                u.push_back(int(in.aux));
                break;
            case LOP_FORNPREP:
            case LOP_FORNLOOP:
                useRange(a, a + 2);
                d.push_back(a + 2);
                break;
            case LOP_FORGPREP:
            case LOP_FORGPREP_NEXT:
            case LOP_FORGPREP_INEXT:
                useRange(a, a + 2);
                break;
            case LOP_FORGLOOP:
            {
                useRange(a, a + 2);
                int n = int(in.aux & 0xff);
                d.push_back(a + 2);
                for (int r = 0; r < n; ++r)
                    d.push_back(a + 3 + r);
                break;
            }
            default:
                // no register traffic: NOP, BREAK, CLOSEUPVALS, JUMP*, PREPVARARGS, COVERAGE, CAPTURE, FASTCALL*
                break;
            }
        }
    }

    void computeLiveness()
    {
        const size_t nb = cfg.blocks.size();
        liveIn.assign(nb, std::vector<char>(numRegs, 0));
        liveOut.assign(nb, std::vector<char>(numRegs, 0));

        std::vector<std::vector<char>> gen(nb, std::vector<char>(numRegs, 0));
        std::vector<std::vector<char>> kill(nb, std::vector<char>(numRegs, 0));

        for (const Block& b : cfg.blocks)
        {
            for (int i = b.begin; i < b.end; ++i)
            {
                for (int r : uses[i])
                    if (r < numRegs && !kill[b.id][r])
                        gen[b.id][r] = 1;
                for (int r : defs[i])
                    if (r < numRegs)
                        kill[b.id][r] = 1;
            }
        }

        bool changed = true;
        while (changed)
        {
            changed = false;
            for (size_t bi = nb; bi-- > 0;)
            {
                const Block& b = cfg.blocks[bi];
                std::vector<char> out(numRegs, 0);
                for (const Edge& e : b.succs)
                    for (int r = 0; r < numRegs; ++r)
                        out[r] |= liveIn[e.to][r];

                std::vector<char> in(numRegs, 0);
                for (int r = 0; r < numRegs; ++r)
                    in[r] = gen[bi][r] | (out[r] & !kill[bi][r]);

                if (out != liveOut[bi] || in != liveIn[bi])
                {
                    liveOut[bi] = out;
                    liveIn[bi] = in;
                    changed = true;
                }
            }
        }
    }

    bool usesReg(int idx, int r) const
    {
        return std::find(uses[idx].begin(), uses[idx].end(), r) != uses[idx].end();
    }

    bool defsReg(int idx, int r) const
    {
        return std::find(defs[idx].begin(), defs[idx].end(), r) != defs[idx].end();
    }

    // Is register r live right before instruction idx? idx == insns.size() means the end of the function.
    bool liveAt(int idx, int r) const
    {
        if (idx >= int(insns.size()) || r >= numRegs)
            return false;
        const Block& b = cfg.blockOf(idx);
        for (int i = idx; i < b.end; ++i)
        {
            if (usesReg(i, r))
                return true;
            if (defsReg(i, r))
                return false;
        }
        return liveOut[b.id][r] != 0;
    }

    bool isTableFill(int idx, int tableReg) const
    {
        const Insn& in = insns[idx];
        switch (in.op)
        {
        case LOP_SETTABLEKS:
        case LOP_SETTABLE:
        case LOP_SETTABLEN:
            return in.b == tableReg && in.a != tableReg && (in.op != LOP_SETTABLE || in.c != tableReg);
        case LOP_SETLIST:
            return in.a == tableReg;
        default:
            return false;
        }
    }

    struct UseInfo
    {
        int count = 0;
        bool escapes = false;
    };

    // Count the reads of the value defined for register r by instruction defIdx.
    // Table fills (SET* into a freshly created table) are not counted so constructors can be folded.
    UseInfo countUses(int defIdx, int r) const
    {
        return countUsesFrom(defIdx + 1, r, insns[defIdx].op == LOP_NEWTABLE || insns[defIdx].op == LOP_DUPTABLE);
    }

    // Count the read sites of register r reachable from instruction startIdx before it is written
    // again, following control flow. Loop instructions re-reading their setup registers do not count.
    UseInfo countUsesFrom(int startIdx, int r, bool table) const
    {
        UseInfo info;
        if (startIdx >= int(insns.size()) || r >= numRegs)
            return info;

        std::vector<char> visited(cfg.blocks.size(), 0);
        std::vector<int> work;

        auto scan = [&](int from, int blockId) {
            const Block& b = cfg.blocks[blockId];
            for (int i = from; i < b.end; ++i)
            {
                const Insn& in = insns[i];
                bool loopReread = (in.op == LOP_FORNLOOP || in.op == LOP_FORGLOOP) && r >= in.a && r <= in.a + 2;
                if (usesReg(i, r) && !loopReread && !(table && isTableFill(i, r)))
                    info.count++;
                if (defsReg(i, r))
                    return;
            }
            if (!liveOut[blockId][r])
                return;
            for (const Edge& e : b.succs)
                if (!visited[e.to])
                {
                    visited[e.to] = 1;
                    work.push_back(e.to);
                }
        };

        int startBlock = cfg.blockOfInsn[startIdx];
        scan(startIdx, startBlock);
        while (!work.empty())
        {
            int blockId = work.back();
            work.pop_back();
            if (!liveIn[blockId][r])
                continue;
            scan(cfg.blocks[blockId].begin, blockId);
        }
        return info;
    }

    std::set<int> writtenIn(int beginIdx, int endIdx) const
    {
        std::set<int> regs;
        for (int i = std::max(beginIdx, 0); i < endIdx && i < int(insns.size()); ++i)
            for (int r : defs[i])
                regs.insert(r);
        return regs;
    }
};

// ---------------------------------------------------------------------------
// Lifter
// ---------------------------------------------------------------------------

struct Pending
{
    ExprP expr;
    int defIdx = -1;
    std::string declName; // non-empty: materializes as a declaration or assignment of this variable
    bool isDecl = false;
    std::string method; // NAMECALL: expr is the object, method is the name
    int nresults = 1;   // multi-result call kept pending for a generic for
};

struct VarInfo
{
    std::string name;
    int scope = -1;
    // sticky variables are real locals (debug names, parameters, captured or loop-carried values):
    // every later write to the register is an assignment. Non-sticky ones are temporaries that were
    // promoted because they are read more than once; the register may be reused for other temporaries.
    bool sticky = false;
    // captured by value: the compiler proved the local is never assigned again, so a later write to the
    // register starts a different variable
    bool immutable = false;
    // end of the debug local range the name came from (inlined functions leave named locals whose
    // register is reused afterwards)
    uint32_t debugEndPc = 0xffffffffu;
};

// A jump out of a loop to a point beyond its exit (typically an inlined `return`). It becomes
// `flag = true; break` inside the loop and `if not flag then ... end` around the skipped code.
struct Escape
{
    int target = -1;
    std::string flag;
};

struct LoopCtx
{
    int breakTarget = -1;    // insn index reached by `break`
    int continueTarget = -1; // insn index reached by `continue`
    int headerIdx = -1;      // first instruction of the loop
    bool untilAllowed = false;
    bool sawUntil = false;
    ExprP untilCond;
    std::optional<Escape> escape;
};

struct CondChain
{
    ExprP cond;   // condition under which control reaches thenStart
    int thenStart = -1;
    int falseTarget = -1;
    int count = 0; // number of jumps in the chain
};

// Shared across every function of a module so generated names never collide.
struct NameContext
{
    int counter = 0;
    std::set<std::string> reserved; // global names read or written anywhere in the module
    std::set<std::string> methods;  // names invoked with `obj:name(...)` anywhere in the module
};

struct Lifter
{
    const Module& m;
    const Proto& p;
    const LiftOptions& options;
    Analysis an;
    std::vector<std::string> upvalNames;
    NameContext* names;
    std::set<std::string> used; // names visible at this point (reserved globals, enclosing and own locals)
    int forDepth = 0;
    bool selfParam = false; // this function is stored as a method: its first parameter is `self`
    std::vector<int> joinStack;          // join points of the if statements being lifted (jumps to them end the branch)
    std::optional<Escape> pendingEscape; // a loop body jumped past its exit; the enclosing ranges must skip to the target
    std::vector<std::string> escapeFlags; // flags declared at the top of the function for such jumps

    std::vector<std::optional<Pending>> pending;
    std::vector<VarInfo> vars;
    std::vector<int> scopeStack;
    int scopeCounter = 0;
    std::vector<StmtP>* out = nullptr;
    std::map<int, int> loopEnds; // header idx -> back jump idx
    std::set<int> loopBackJumps;
    int exprRegionReg = -1;
    std::vector<int> consuming; // registers consumed by the statement being built

    struct Snapshot
    {
        std::vector<std::optional<Pending>> pending;
        std::vector<VarInfo> vars;
        std::vector<int> scopeStack;
        int scopeCounter;
        int nameCounter;
        std::set<std::string> used;
        std::optional<Escape> pendingEscape;
    };

    Lifter(const Module& module, const Proto& proto, const LiftOptions& opts, std::vector<std::string> upvals, NameContext* nameContext,
        const std::set<std::string>& inherited)
        : m(module)
        , p(proto)
        , options(opts)
        , an(module, proto)
        , upvalNames(std::move(upvals))
        , names(nameContext)
        , used(inherited)
    {
        pending.resize(an.numRegs);
        vars.resize(an.numRegs);

        used.insert(names->reserved.begin(), names->reserved.end());
        used.insert(upvalNames.begin(), upvalNames.end());
        for (const DebugLocal& l : p.locals)
            used.insert(l.name);

        for (size_t i = 0; i < an.insns.size(); ++i)
        {
            const Insn& in = an.insns[i];
            if ((in.op == LOP_JUMPBACK || in.op == LOP_JUMPX || in.op == LOP_JUMP) && in.jumpTarget >= 0 && uint32_t(in.jumpTarget) <= in.pc)
            {
                int header = an.idxOfPc(in.jumpTarget);
                auto it = loopEnds.find(header);
                if (it == loopEnds.end() || it->second < int(i))
                    loopEnds[header] = int(i);
            }
        }
        for (auto& kv : loopEnds)
            loopBackJumps.insert(kv.second);
    }

    Snapshot snapshot() const
    {
        return Snapshot{pending, vars, scopeStack, scopeCounter, names->counter, used, pendingEscape};
    }

    void restore(const Snapshot& s)
    {
        pending = s.pending;
        vars = s.vars;
        scopeStack = s.scopeStack;
        scopeCounter = s.scopeCounter;
        names->counter = s.nameCounter;
        used = s.used;
        pendingEscape = s.pendingEscape;
    }

    // ----- names and scopes -----

    // A fresh variable name: the hint when it is a free identifier, otherwise the next vN.
    std::string newName(const std::string& hint = std::string())
    {
        if (!hint.empty() && isIdentifier(hint))
        {
            if (!used.count(hint))
                return hint;
            for (int n = 2; n < 10; ++n)
                if (!used.count(hint + std::to_string(n)))
                    return hint + std::to_string(n);
        }
        std::string name;
        do
            name = "v" + std::to_string(++names->counter);
        while (used.count(name));
        return name;
    }

    // Name for a register whose pending value is being turned into a variable.
    std::string pickName(int reg, const Pending& P)
    {
        if (const DebugLocal* dl = debugLocalForDef(reg, P.defIdx))
            return dl->name;
        std::string hint;
        if (P.expr->kind == ExprKind::Function && size_t(P.expr->integer) < m.protos.size())
            hint = m.protos[P.expr->integer].debugname;
        else if (P.expr->kind == ExprKind::Nil)
            hint = closureNameAssignedLater(reg, P.defIdx);
        return newName(hint);
    }

    // `local f; ... function f() end`: a nil placeholder that later receives a named closure.
    std::string closureNameAssignedLater(int reg, int defIdx) const
    {
        for (int i = defIdx + 1; i < int(an.insns.size()); ++i)
        {
            const Insn& in = an.insns[i];
            if (!an.defsReg(i, reg))
                continue;
            uint32_t protoIndex = 0;
            if (in.op == LOP_NEWCLOSURE && size_t(in.d) < p.children.size())
                protoIndex = p.children[in.d];
            else if (in.op == LOP_DUPCLOSURE && size_t(in.d) < p.constants.size() && p.constants[in.d].kind == Constant::Closure)
                protoIndex = p.constants[in.d].closureProto;
            else
                return "";
            return protoIndex < m.protos.size() ? m.protos[protoIndex].debugname : "";
        }
        return "";
    }

    int currentScope() const
    {
        return scopeStack.empty() ? 0 : scopeStack.back();
    }

    void pushScope()
    {
        scopeStack.push_back(++scopeCounter);
    }

    void popScope()
    {
        int id = scopeStack.back();
        scopeStack.pop_back();
        for (int r = 0; r < int(vars.size()); ++r)
            if (vars[r].scope == id)
                clearVar(r);
    }

    // Forget the variable in a register and release its name unless another register still uses it.
    void clearVar(int reg)
    {
        std::string name = vars[reg].name;
        vars[reg] = VarInfo{};
        if (name.empty() || names->reserved.count(name))
            return;
        for (const VarInfo& v : vars)
            if (v.name == name)
                return;
        for (const std::string& up : upvalNames)
            if (up == name)
                return;
        used.erase(name);
    }

    bool hasVar(int reg) const
    {
        return reg < int(vars.size()) && !vars[reg].name.empty();
    }

    void setVar(int reg, const std::string& name, bool sticky = false)
    {
        vars[reg] = VarInfo{name, currentScope(), sticky};
        used.insert(name);
    }

    bool hasStickyVar(int reg) const
    {
        return hasVar(reg) && vars[reg].sticky && !vars[reg].immutable;
    }

    // A variable a new write to the register may be an assignment to.
    bool hasReusableVar(int reg) const
    {
        return hasVar(reg) && !vars[reg].immutable;
    }

    const DebugLocal* debugLocalAt(int reg, int pc) const
    {
        for (const DebugLocal& l : p.locals)
            if (l.reg == reg && l.startpc <= uint32_t(pc) && uint32_t(pc) < l.endpc)
                return &l;
        return nullptr;
    }

    // The debug local (if any) whose value is produced by the definition of reg at instruction defIdx.
    const DebugLocal* debugLocalForDef(int reg, int defIdx) const
    {
        if (!p.hasDebugInfo)
            return nullptr;

        uint32_t defPc = an.insns[defIdx].pc;
        const DebugLocal* best = nullptr;

        for (const DebugLocal& l : p.locals)
        {
            if (l.reg != reg || l.startpc == l.endpc)
                continue;
            if (l.startpc <= defPc && defPc < l.endpc)
                return &l; // assignment to a live local
            if (l.startpc < defPc)
                continue;
            if (best && best->startpc <= l.startpc)
                continue;

            // the local starts later: make sure nothing else writes the register in between
            bool ok = true;
            for (int i = defIdx + 1; i < int(an.insns.size()) && an.insns[i].pc < l.startpc; ++i)
            {
                if (an.defsReg(i, reg) || !isSimpleInsn(an.insns[i]) || isCondJump(an.insns[i].op))
                {
                    ok = false;
                    break;
                }
            }
            if (ok)
                best = &l;
        }
        return best;
    }

    // ----- statements -----

    void pushStmt(const StmtP& s)
    {
        // `local x` immediately followed by `x = value` is one declaration
        if (s->kind == StmtKind::Assign && s->targets.size() == 1 && s->values.size() == 1 && s->targets[0]->kind == ExprKind::Local && !out->empty())
        {
            const StmtP& prev = out->back();
            if (prev->kind == StmtKind::Local && prev->values.empty() && prev->names.size() == 1 && prev->names[0] == s->targets[0]->str)
            {
                prev->values.push_back(s->values[0]);
                return;
            }
        }
        out->push_back(s);
    }

    void comment(const std::string& text)
    {
        pushStmt(mkComment(text));
    }

    void unsupported(int idx, const char* why)
    {
        comment(std::string("luaudec: ") + why + ": " + formatInsn(m, p, an.insns[idx], -1) + " (pc " + std::to_string(an.insns[idx].pc) + ")");
    }

    std::vector<int> pendingOrder() const
    {
        std::vector<int> regs;
        for (int r = 0; r < int(pending.size()); ++r)
            if (pending[r])
                regs.push_back(r);
        std::sort(regs.begin(), regs.end(), [&](int x, int y) {
            return pending[x]->defIdx < pending[y]->defIdx;
        });
        return regs;
    }

    bool needsFlush(const Pending& P) const
    {
        return !P.declName.empty() || !isPure(P.expr);
    }

    // Emit pending values that were defined before defIdx and have to stay ordered.
    void flushBefore(int defIdx)
    {
        for (int r : pendingOrder())
        {
            if (!pending[r] || pending[r]->defIdx >= defIdx)
                continue;
            if (needsFlush(*pending[r]))
                materialize(r);
        }
    }

    // Turn a pending value into a statement. Returns the expression to use for later reads.
    ExprP materialize(int reg)
    {
        Pending P = *pending[reg];
        pending[reg].reset();
        flushBefore(P.defIdx);

        if (!P.method.empty())
        {
            // a NAMECALL without its CALL: keep the method lookup visible
            P.expr = mkIndex(P.expr, mkString(P.method));
        }

        if (P.nresults > 1)
        {
            // multi-result call left over from a generic for that did not materialize
            P.expr->multret = false;
        }

        std::string name = P.declName;
        bool isDecl = P.isDecl;
        if (name.empty())
        {
            if (P.expr->kind == ExprKind::Call || P.expr->kind == ExprKind::MethodCall)
            {
                if (!an.liveAt(P.defIdx + 1, reg) && !exprIsReferencedLater(reg, P.defIdx))
                {
                    StmtP s = mkStmt(StmtKind::Call);
                    P.expr->multret = false;
                    s->expr = P.expr;
                    pushStmt(s);
                    return mkExpr(ExprKind::Nil);
                }
            }
            name = pickName(reg, P);
            setVar(reg, name);
            if (const DebugLocal* dl = debugLocalForDef(reg, P.defIdx))
                if (dl->name == name)
                    vars[reg].debugEndPc = dl->endpc;
            isDecl = true;
        }

        if (P.expr->kind == ExprKind::Function)
        {
            // `local function f` for a declaration, `function f` for an assignment to an existing local
            StmtP s = mkStmt(isDecl ? StmtKind::LocalFunction : StmtKind::FunctionDecl);
            if (isDecl)
                s->names.push_back(name);
            else
                s->expr = mkName(ExprKind::Local, name, reg);
            s->func = P.expr->func;
            pushStmt(s);
            return mkName(ExprKind::Local, name, reg);
        }

        StmtP s = mkStmt(isDecl ? StmtKind::Local : StmtKind::Assign);
        if (isDecl)
            s->names.push_back(name);
        else
            s->targets.push_back(mkName(ExprKind::Local, name, reg));
        P.expr->multret = false;
        s->values.push_back(P.expr);
        pushStmt(s);

        return mkName(ExprKind::Local, name, reg);
    }

    bool exprIsReferencedLater(int reg, int defIdx) const
    {
        return an.countUses(defIdx, reg).count > 0;
    }

    void flushAll()
    {
        flushFrom(0);
    }

    // End of a block: values defined inside it are emitted or dropped; values that were pending before
    // the block started stay pending for the code after it.
    void flushFrom(int beginIdx)
    {
        for (int r : pendingOrder())
        {
            if (!pending[r] || pending[r]->defIdx < beginIdx)
                continue;
            if (needsFlush(*pending[r]))
                materialize(r);
            else
                pending[r].reset();
        }
    }

    void beforeStatement()
    {
        for (int r : pendingOrder())
            if (pending[r] && needsFlush(*pending[r]))
                materialize(r);
    }

    void emit(const StmtP& s)
    {
        beforeStatement();
        pushStmt(s);
    }

    // Flush state before a nested construct that spans instructions [beginIdx, endIdx).
    void beforeConstruct(int beginIdx, int endIdx)
    {
        std::set<int> written = an.writtenIn(beginIdx, endIdx);
        for (int r : pendingOrder())
        {
            if (!pending[r])
                continue;
            const Pending& P = *pending[r];

            // literals and references to variables the construct does not touch can stay folded,
            // unless the construct overwrites the register itself (a loop-carried value)
            bool keep = P.declName.empty() && P.method.empty() && !written.count(r) &&
                        (isLiteral(P.expr) || (P.expr->kind == ExprKind::Local && !written.count(P.expr->reg)));
            if (keep)
                continue;

            if (needsFlush(P) || an.liveAt(beginIdx, r))
                materialize(r);
            else
                pending[r].reset();
        }
    }

    // Make sure register reg holds a named variable declared in the current scope chain, with its
    // current value stored. The variable becomes sticky: later writes are assignments.
    std::string declareBefore(int reg, bool sticky = true, int hintIdx = -1)
    {
        if (pending[reg])
        {
            Pending& P = *pending[reg];
            if (P.declName.empty())
            {
                if (hasReusableVar(reg) && (vars[reg].sticky || !sticky))
                {
                    P.declName = vars[reg].name;
                    P.isDecl = false;
                }
                else
                {
                    P.declName = pickName(reg, P);
                    P.isDecl = true;
                    setVar(reg, P.declName);
                    if (const DebugLocal* dl = debugLocalForDef(reg, P.defIdx))
                        if (dl->name == P.declName)
                            vars[reg].debugEndPc = dl->endpc;
                }
            }
            materialize(reg);
            if (sticky)
                vars[reg].sticky = true;
            return vars[reg].name;
        }

        // no new value: the register still holds the variable it had, immutable or not
        if (hasVar(reg))
        {
            if (sticky)
                vars[reg].sticky = true;
            return vars[reg].name;
        }

        // no value yet: a debug local that becomes live at the join names the variable
        const DebugLocal* dl = hintIdx >= 0 && hintIdx < int(an.insns.size()) ? debugLocalAt(reg, int(an.insns[hintIdx].pc)) : nullptr;
        std::string name = dl ? dl->name : newName();
        setVar(reg, name, sticky);
        if (dl && name == dl->name)
            vars[reg].debugEndPc = dl->endpc;
        StmtP s = mkStmt(StmtKind::Local);
        s->names.push_back(name);
        pushStmt(s);
        return name;
    }

    // ----- register reads and writes -----

    bool isConsuming(int reg) const
    {
        return std::find(consuming.begin(), consuming.end(), reg) != consuming.end();
    }

    ExprP use(int reg)
    {
        if (reg >= int(pending.size()))
            return mkName(ExprKind::Raw, "R" + std::to_string(reg));

        if (pending[reg])
        {
            Pending& P = *pending[reg];

            if (!P.declName.empty())
                return materialize(reg);

            if (!isPure(P.expr))
            {
                // keep evaluation order: a later impure value that is not part of this statement
                // would otherwise be emitted before this one
                for (int r : pendingOrder())
                {
                    if (r == reg || !pending[r] || isConsuming(r))
                        continue;
                    if (pending[r]->defIdx > P.defIdx && needsFlush(*pending[r]))
                        return materialize(reg);
                }
            }

            ExprP e = P.expr;
            if (!P.method.empty())
                e = mkIndex(e, mkString(P.method));
            pending[reg].reset();
            return e;
        }

        if (hasVar(reg))
            return mkName(ExprKind::Local, vars[reg].name, reg);

        // reading a register nothing wrote: give it a name so the output still parses
        std::string name = newName();
        setVar(reg, name);
        if (options.verbose)
            comment("luaudec: read of undefined register R" + std::to_string(reg));
        return mkName(ExprKind::Local, name, reg);
    }

    void dropPending(int reg)
    {
        if (!pending[reg])
            return;
        if (needsFlush(*pending[reg]))
            materialize(reg);
        else
            pending[reg].reset();
    }

    bool isNamedClosure(const Pending& P) const
    {
        return P.expr->kind == ExprKind::Function && size_t(P.expr->integer) < m.protos.size() &&
               isIdentifier(m.protos[P.expr->integer].debugname);
    }

    // Does the instruction right after the closure creation (past its CAPTUREs) store, pass or call it?
    // Those forms (`function t.f()`, `f(function ...)`, `return function ...`) keep the closure inline.
    bool closureConsumedNext(int reg, int defIdx) const
    {
        int j = defIdx + 1;
        while (j < int(an.insns.size()) && an.insns[j].op == LOP_CAPTURE && an.insns[j].attachedCapture)
            j++;
        if (j >= int(an.insns.size()) || !an.usesReg(j, reg))
            return false;
        switch (an.insns[j].op)
        {
        case LOP_SETGLOBAL:
        case LOP_SETUPVAL:
        case LOP_SETTABLEKS:
        case LOP_SETTABLE:
        case LOP_SETTABLEN:
        case LOP_SETLIST:
        case LOP_CALL:
        case LOP_CALLFB:
        case LOP_RETURN:
        case LOP_MOVE:
            return true;
        default:
            return false;
        }
    }

    // Is the instruction after defIdx a JUMPIF/JUMPIFNOT on reg whose skipped region only sets reg?
    // That is the `e1 and e2` / `e1 or e2` value form, which keeps e1 pending.
    bool startsAndOrValue(int reg, int defIdx)
    {
        int j = defIdx + 1;
        if (j >= int(an.insns.size()))
            return false;
        const Insn& jump = an.insns[j];
        if ((jump.op != LOP_JUMPIF && jump.op != LOP_JUMPIFNOT) || jump.a != reg)
            return false;
        int target = an.targetIdx(j);
        if (target <= j + 1)
            return false;
        const Insn& last = an.insns[target - 1];
        if (isUncondJump(last.op) || isCondJump(last.op) || !an.defsReg(target - 1, reg))
            return false;
        for (int k = j + 1; k < target; ++k)
            if (!isSimpleInsn(an.insns[k]) && !isCondJump(an.insns[k].op))
                return false;
        return true;
    }

    // Record the value of a register write. useFrom overrides where the reads of this value are
    // counted from (patterns spanning several blocks define their result at the join point).
    void def(int reg, ExprP expr, int defIdx, const std::string& method = std::string(), int nresults = 1, int useFrom = -1)
    {
        // pending values that read this register would observe the new value
        for (int r : pendingOrder())
        {
            if (r == reg || !pending[r] || !exprReadsReg(pending[r]->expr, reg))
                continue;
            const Pending& Q = *pending[r];
            if (Q.declName.empty() && isPure(Q.expr) && an.countUses(Q.defIdx, r).count == 0)
                pending[r].reset(); // dead temp
            else
                materialize(r);
        }

        dropPending(reg);

        // a debug-named local whose range has ended no longer owns the register
        if (hasVar(reg) && vars[reg].debugEndPc <= an.insns[defIdx].pc)
            clearVar(reg);

        Pending P;
        P.expr = std::move(expr);
        P.defIdx = defIdx;
        P.method = method;
        P.nresults = nresults;

        // a MULTRET producer is consumed by the very next instruction and can never be a variable
        if (reg == exprRegionReg || !method.empty() || nresults > 1 || P.expr->multret)
        {
            pending[reg] = P;
            return;
        }

        const DebugLocal* dl = debugLocalForDef(reg, defIdx);
        Analysis::UseInfo info = useFrom >= 0 ? an.countUsesFrom(useFrom, reg, false) : an.countUses(defIdx, reg);

        bool boolLiteral = P.expr->kind == ExprKind::True || P.expr->kind == ExprKind::False;
        bool nextIsCondJump = defIdx + 1 < int(an.insns.size()) && isCondJump(an.insns[defIdx + 1].op);

        if (dl)
        {
            // assign when the register already holds this local, or a variable declared by an enclosing
            // block for this join (its name may differ when the debug range only starts at the join)
            if (hasReusableVar(reg) && (vars[reg].name == dl->name || vars[reg].scope != currentScope()))
            {
                P.declName = vars[reg].name;
                P.isDecl = false;
            }
            else
            {
                P.declName = dl->name;
                P.isDecl = true;
                setVar(reg, dl->name, true);
            }
            vars[reg].debugEndPc = dl->endpc;
        }
        else if (useFrom < 0 && (startsAndOrValue(reg, defIdx) || (boolLiteral && nextIsCondJump)))
        {
            // stays pending; liftIf folds it into an and/or or boolean expression if the pattern matches
        }
        else if (hasStickyVar(reg) || (hasReusableVar(reg) && vars[reg].scope != currentScope()))
        {
            // real locals, and any variable declared in an enclosing block, are assigned rather than redeclared
            P.declName = vars[reg].name;
            P.isDecl = false;
        }
        else if (P.expr->kind == ExprKind::Function && isNamedClosure(P) && !closureConsumedNext(reg, defIdx))
        {
            // a closure the compiler named is a `local function` in the source: declare it where it is defined
            P.declName = pickName(reg, P);
            P.isDecl = true;
            setVar(reg, P.declName);
        }
        else if (info.count >= 2)
        {
            // a promoted temporary: a fresh local, even if the register held another temporary before
            P.declName = pickName(reg, P);
            P.isDecl = true;
            setVar(reg, P.declName);
        }

        pending[reg] = P;

        // keep declarations in program order unless a table constructor may still be filled
        if (!P.declName.empty() && P.expr->kind != ExprKind::Table)
            materialize(reg);
    }

    // Declare several registers at once (multi-result calls and varargs).
    void defMulti(int firstReg, int count, ExprP value, int defIdx)
    {
        std::vector<std::string> names;
        bool allSame = true;
        for (int i = 0; i < count; ++i)
        {
            int reg = firstReg + i;
            dropPending(reg);
            const DebugLocal* dl = debugLocalForDef(reg, defIdx);
            std::string name = dl ? dl->name : (hasVar(reg) && !dl ? vars[reg].name : newName());
            if (!(hasVar(reg) && vars[reg].name == name))
                allSame = false;
            names.push_back(name);
        }

        beforeStatement();

        StmtP s = mkStmt(allSame ? StmtKind::Assign : StmtKind::Local);
        for (int i = 0; i < count; ++i)
        {
            int reg = firstReg + i;
            if (allSame)
                s->targets.push_back(mkName(ExprKind::Local, names[i], reg));
            else
            {
                s->names.push_back(names[i]);
                setVar(reg, names[i], true);
            }
        }
        value->multret = true; // the call or vararg expands into all the declared names
        s->values.push_back(value);
        pushStmt(s);
    }

    // ----- constants -----

    ExprP constant(int k) const
    {
        if (k < 0 || size_t(k) >= p.constants.size())
            throw LiftError("constant index out of range: " + std::to_string(k));

        const Constant& c = p.constants[k];
        switch (c.kind)
        {
        case Constant::Nil:
            return mkExpr(ExprKind::Nil);
        case Constant::Boolean:
            return mkExpr(c.boolean ? ExprKind::True : ExprKind::False);
        case Constant::Number:
            return mkNumber(c.number);
        case Constant::Integer:
        {
            ExprP e = mkExpr(ExprKind::Integer);
            e->integer = c.integer;
            return e;
        }
        case Constant::String:
            return mkString(m.str(c.stringIndex));
        case Constant::Vector:
        {
            ExprP e = mkExpr(ExprKind::Vector);
            for (int i = 0; i < 4; ++i)
                e->vec[i] = c.vector[i];
            return e;
        }
        case Constant::Import:
            return importExpr(c.importId);
        case Constant::Table:
            return tableTemplate(c);
        case Constant::Closure:
            throw LiftError("closure constant used as a value");
        case Constant::ClassShape:
            return mkName(ExprKind::Raw, "--[[class shape]] nil");
        }
        return mkExpr(ExprKind::Nil);
    }

    std::string constantString(int k) const
    {
        if (k < 0 || size_t(k) >= p.constants.size() || p.constants[k].kind != Constant::String)
            throw LiftError("expected a string constant at K" + std::to_string(k));
        return m.str(p.constants[k].stringIndex);
    }

    ExprP importExpr(uint32_t importId) const
    {
        std::vector<std::string> path = decodeImportPath(m, p, importId);
        if (path.empty())
            throw LiftError("empty import path");
        ExprP e = mkName(ExprKind::Global, path[0]);
        for (size_t i = 1; i < path.size(); ++i)
            e = mkIndex(e, mkString(path[i]));
        return e;
    }

    ExprP tableTemplate(const Constant& c) const
    {
        // keys appear in source order; entries without a constant value are slots that the
        // SETTABLEKS instructions following DUPTABLE fill in
        ExprP t = mkExpr(ExprKind::Table);
        for (size_t i = 0; i < c.tableKeys.size(); ++i)
        {
            TableItem item;
            item.key = constant(int(c.tableKeys[i]));
            if (c.tableHasValues && c.tableValues[i] >= 0)
                item.value = constant(c.tableValues[i]);
            t->items.push_back(item);
        }
        return t;
    }

    // ----- closures -----

    std::string ensureVar(int reg)
    {
        return declareBefore(reg);
    }

    FunctionP liftChild(uint32_t protoIndex, const Insn& in, bool asMethod = false)
    {
        if (protoIndex >= m.protos.size())
            throw LiftError("child proto index out of range");

        const Proto& child = m.protos[protoIndex];
        std::vector<std::string> upnames;

        for (size_t i = 0; i < in.captures.size(); ++i)
        {
            const Capture& cap = in.captures[i];
            std::string name;

            if (cap.type == LCT_UPVAL)
            {
                name = cap.index < upvalNames.size() ? upvalNames[cap.index] : "u" + std::to_string(cap.index);
            }
            else
            {
                // the closure refers to the variable by the name it has in this function
                name = ensureVar(cap.index);
                if (cap.type == LCT_VAL)
                    vars[cap.index].immutable = true;
            }
            upnames.push_back(name);
        }

        for (size_t i = upnames.size(); i < child.numupvalues; ++i)
            upnames.push_back(i < child.upvalueNames.size() ? child.upvalueNames[i] : "u" + std::to_string(i));

        return liftProto(m, child, options, upnames, names, used, asMethod);
    }

    static FunctionP liftProto(const Module& m, const Proto& p, const LiftOptions& options, std::vector<std::string> upvals, NameContext* names,
        const std::set<std::string>& inherited, bool selfParam = false);

    // Is this closure stored right away as a field whose name is invoked as a method somewhere?
    bool storedAsMethod(int defIdx, int reg, uint32_t protoIndex) const
    {
        if (protoIndex >= m.protos.size() || m.protos[protoIndex].numparams == 0)
            return false;
        int j = defIdx + 1;
        while (j < int(an.insns.size()) && an.insns[j].op == LOP_CAPTURE && an.insns[j].attachedCapture)
            j++;
        if (j >= int(an.insns.size()) || an.insns[j].op != LOP_SETTABLEKS || an.insns[j].a != reg)
            return false;
        int k = int(an.insns[j].aux);
        if (size_t(k) >= p.constants.size() || p.constants[k].kind != Constant::String)
            return false;
        return names->methods.count(m.str(p.constants[k].stringIndex)) != 0;
    }

    bool childIsMethod(uint32_t protoIndex) const
    {
        if (protoIndex >= m.protos.size())
            return false;
        const Proto& child = m.protos[protoIndex];
        if (child.numparams == 0)
            return false;
        for (const DebugLocal& l : child.locals)
            if (l.reg == 0 && l.startpc == 0 && l.name == "self")
                return true;
        return false;
    }

    // ----- expression helpers -----

    ExprP compare(const char* op, ExprP lhs, ExprP rhs)
    {
        // `a > b` is compiled as `b < a`; undo the swap when the left side is a folded literal
        if ((std::string(op) == "<" || std::string(op) == "<=") && isLiteral(lhs) && !isLiteral(rhs))
        {
            std::string flipped = std::string(op) == "<" ? ">" : ">=";
            return mkBinOp(flipped, rhs, lhs);
        }
        return mkBinOp(op, lhs, rhs);
    }

    // The condition under which the conditional jump at idx is taken.
    ExprP jumpCondition(int idx)
    {
        const Insn& in = an.insns[idx];
        consuming.clear();
        for (int r : an.uses[idx])
            consuming.push_back(r);

        ExprP result;
        switch (in.op)
        {
        case LOP_JUMPIF:
            result = use(in.a);
            break;
        case LOP_JUMPIFNOT:
            result = negate(use(in.a));
            break;
        case LOP_JUMPIFEQ:
        {
            ExprP a = use(in.a);
            result = mkBinOp("==", a, use(int(in.aux)));
            break;
        }
        case LOP_JUMPIFNOTEQ:
        {
            ExprP a = use(in.a);
            result = mkBinOp("~=", a, use(int(in.aux)));
            break;
        }
        case LOP_JUMPIFLE:
        {
            ExprP a = use(in.a);
            result = compare("<=", a, use(int(in.aux)));
            break;
        }
        case LOP_JUMPIFLT:
        {
            ExprP a = use(in.a);
            result = compare("<", a, use(int(in.aux)));
            break;
        }
        case LOP_JUMPIFNOTLE:
        {
            ExprP a = use(in.a);
            result = negate(compare("<=", a, use(int(in.aux))));
            break;
        }
        case LOP_JUMPIFNOTLT:
        {
            ExprP a = use(in.a);
            result = negate(compare("<", a, use(int(in.aux))));
            break;
        }
        case LOP_JUMPXEQKNIL:
            result = mkBinOp("==", use(in.a), mkExpr(ExprKind::Nil));
            if (in.aux >> 31)
                result = negate(result);
            break;
        case LOP_JUMPXEQKB:
            result = mkBinOp("==", use(in.a), mkExpr((in.aux & 1) ? ExprKind::True : ExprKind::False));
            if (in.aux >> 31)
                result = negate(result);
            break;
        case LOP_JUMPXEQKN:
        case LOP_JUMPXEQKS:
            result = mkBinOp("==", use(in.a), constant(int(in.aux & 0xffffff)));
            if (in.aux >> 31)
                result = negate(result);
            break;
        default:
            throw LiftError("not a conditional jump");
        }
        consuming.clear();
        return result;
    }

    // ----- straight-line instructions -----

    void liftSimple(int idx)
    {
        const Insn& in = an.insns[idx];
        const int a = in.a, b = in.b, c = in.c;

        auto binop = [&](const char* op) {
            consuming = {b, c};
            ExprP lhs = use(b);
            ExprP rhs = use(c);
            consuming.clear();
            def(a, mkBinOp(op, lhs, rhs), idx);
        };
        auto binopK = [&](const char* op) {
            consuming = {b};
            ExprP lhs = use(b);
            consuming.clear();
            def(a, mkBinOp(op, lhs, constant(c)), idx);
        };
        auto unop = [&](const char* op) {
            consuming = {b};
            ExprP operand = use(b);
            consuming.clear();
            def(a, mkUnOp(op, operand), idx);
        };

        switch (in.op)
        {
        case LOP_NOP:
        case LOP_BREAK:
        case LOP_COVERAGE:
        case LOP_PREPVARARGS:
        case LOP_NATIVECALL:
            break;

        case LOP_CLOSEUPVALS:
        {
            // captured locals in registers >= A go out of scope here; when the closing is not part of
            // a break/continue/return path the compiler may reuse those registers for new temporaries
            bool exitPath = idx + 1 < int(an.insns.size()) && (isUncondJump(an.insns[idx + 1].op) || an.insns[idx + 1].op == LOP_RETURN);
            if (!exitPath)
            {
                for (int r = a; r < int(vars.size()); ++r)
                {
                    if (pending[r] && !pending[r]->declName.empty())
                        materialize(r);
                    if (!p.hasDebugInfo || !debugLocalAt(r, int(an.insns[idx].pc) + 1))
                        clearVar(r);
                }
            }
            break;
        }

        case LOP_CAPTURE:
            if (!in.attachedCapture)
                unsupported(idx, "stray CAPTURE");
            break;

        case LOP_FASTCALL:
        case LOP_FASTCALL1:
        case LOP_FASTCALL2:
        case LOP_FASTCALL2K:
        case LOP_FASTCALL3:
        case LOP_FASTPCALL:
            // the fallback CALL that follows carries the full semantics
            break;

        case LOP_LOADNIL:
            def(a, mkExpr(ExprKind::Nil), idx);
            break;
        case LOP_LOADB:
            def(a, mkExpr(b ? ExprKind::True : ExprKind::False), idx);
            if (c != 0)
                unsupported(idx, "LOADB skip outside a boolean pattern");
            break;
        case LOP_LOADN:
            def(a, mkNumber(in.d), idx);
            break;
        case LOP_LOADK:
            def(a, constant(in.d), idx);
            break;
        case LOP_LOADKX:
            def(a, constant(int(in.aux)), idx);
            break;
        case LOP_MOVE:
        {
            // copying a closure means it is a named local being read
            if (pending[b] && pending[b]->method.empty() && pending[b]->expr->kind == ExprKind::Function)
                declareBefore(b, false);
            consuming = {b};
            ExprP e = use(b);
            consuming.clear();
            def(a, e, idx);
            break;
        }
        case LOP_GETGLOBAL:
            def(a, mkName(ExprKind::Global, constantString(int(in.aux))), idx);
            break;
        case LOP_SETGLOBAL:
        {
            consuming = {a};
            ExprP value = use(a);
            consuming.clear();
            std::string name = constantString(int(in.aux));
            if (value->kind == ExprKind::Function && isIdentifier(name))
            {
                StmtP s = mkStmt(StmtKind::FunctionDecl);
                s->expr = mkName(ExprKind::Global, name);
                s->func = value->func;
                emit(s);
            }
            else
            {
                StmtP s = mkStmt(StmtKind::Assign);
                s->targets.push_back(mkName(ExprKind::Global, name));
                s->values.push_back(value);
                emit(s);
            }
            break;
        }
        case LOP_GETUPVAL:
            def(a, mkName(ExprKind::Upvalue, upvalName(b)), idx);
            break;
        case LOP_SETUPVAL:
        {
            consuming = {a};
            ExprP value = use(a);
            consuming.clear();
            StmtP s = mkStmt(StmtKind::Assign);
            s->targets.push_back(mkName(ExprKind::Upvalue, upvalName(b)));
            s->values.push_back(value);
            emit(s);
            break;
        }
        case LOP_GETIMPORT:
            def(a, constant(in.d), idx);
            break;
        case LOP_GETTABLE:
        {
            consuming = {b, c};
            ExprP obj = use(b);
            ExprP key = use(c);
            consuming.clear();
            def(a, mkIndex(obj, key), idx);
            break;
        }
        case LOP_GETTABLEKS:
        case LOP_GETUDATAKS:
        {
            consuming = {b};
            ExprP obj = use(b);
            consuming.clear();
            int k = in.op == LOP_GETTABLEKS ? int(in.aux) : int(LUAU_INSN_AUX_KV16(in.aux));
            def(a, mkIndex(obj, mkString(constantString(k))), idx);
            break;
        }
        case LOP_GETTABLEN:
        {
            consuming = {b};
            ExprP obj = use(b);
            consuming.clear();
            def(a, mkIndex(obj, mkNumber(c + 1)), idx);
            break;
        }
        case LOP_SETTABLE:
        {
            consuming = {a, c};
            ExprP key = use(c);
            ExprP value = use(a);
            consuming.clear();
            storeField(b, key, value, idx);
            break;
        }
        case LOP_SETTABLEKS:
        case LOP_SETUDATAKS:
        {
            int k = in.op == LOP_SETTABLEKS ? int(in.aux) : int(LUAU_INSN_AUX_KV16(in.aux));
            consuming = {a};
            ExprP value = use(a);
            consuming.clear();
            storeField(b, mkString(constantString(k)), value, idx);
            break;
        }
        case LOP_SETTABLEN:
        {
            consuming = {a};
            ExprP value = use(a);
            consuming.clear();
            storeField(b, mkNumber(c + 1), value, idx);
            break;
        }
        case LOP_NEWCLOSURE:
        case LOP_DUPCLOSURE:
        {
            uint32_t protoIndex;
            if (in.op == LOP_NEWCLOSURE)
            {
                if (size_t(in.d) >= p.children.size())
                    throw LiftError("child proto index out of range");
                protoIndex = p.children[in.d];
            }
            else
            {
                if (size_t(in.d) >= p.constants.size() || p.constants[in.d].kind != Constant::Closure)
                    throw LiftError("DUPCLOSURE constant is not a closure");
                protoIndex = p.constants[in.d].closureProto;
            }

            bool selfCapture = false;
            for (const Capture& cap : in.captures)
                if ((cap.type == LCT_VAL || cap.type == LCT_REF) && cap.index == a)
                    selfCapture = true;

            if (selfCapture)
            {
                // local function f() ... f() ... end
                dropPending(a);
                const DebugLocal* dl = debugLocalForDef(a, idx);
                std::string name = dl ? dl->name : newName();
                setVar(a, name, true);
                FunctionP body = liftChild(protoIndex, in);
                StmtP s = mkStmt(StmtKind::LocalFunction);
                s->names.push_back(name);
                s->func = body;
                emit(s);
            }
            else
            {
                ExprP fn = mkExpr(ExprKind::Function);
                fn->func = liftChild(protoIndex, in, storedAsMethod(idx, a, protoIndex));
                fn->integer = protoIndex;
                def(a, fn, idx);
            }
            break;
        }
        case LOP_NAMECALL:
        case LOP_NAMECALLUDATA:
        {
            consuming = {b};
            ExprP obj = use(b);
            consuming.clear();
            int k = in.op == LOP_NAMECALL ? int(in.aux) : int(LUAU_INSN_AUX_KV16(in.aux));
            dropPending(a + 1);
            def(a, obj, idx, constantString(k));
            break;
        }
        case LOP_CALL:
        case LOP_CALLFB:
            liftCall(idx);
            break;
        case LOP_RETURN:
        {
            std::vector<ExprP> values = readValues(a, b, idx);
            bool last = idx + 1 == int(an.insns.size());
            if (last && values.empty())
                break;
            StmtP s = mkStmt(StmtKind::Return);
            s->values = values;
            emit(s);
            break;
        }
        case LOP_ADD:
            binop("+");
            break;
        case LOP_SUB:
            binop("-");
            break;
        case LOP_MUL:
            binop("*");
            break;
        case LOP_DIV:
            binop("/");
            break;
        case LOP_IDIV:
            binop("//");
            break;
        case LOP_MOD:
            binop("%");
            break;
        case LOP_POW:
            binop("^");
            break;
        case LOP_AND:
            binop("and");
            break;
        case LOP_OR:
            binop("or");
            break;
        case LOP_ADDK:
            binopK("+");
            break;
        case LOP_SUBK:
            binopK("-");
            break;
        case LOP_MULK:
            binopK("*");
            break;
        case LOP_DIVK:
            binopK("/");
            break;
        case LOP_IDIVK:
            binopK("//");
            break;
        case LOP_MODK:
            binopK("%");
            break;
        case LOP_POWK:
            binopK("^");
            break;
        case LOP_ANDK:
            binopK("and");
            break;
        case LOP_ORK:
            binopK("or");
            break;
        case LOP_SUBRK:
        case LOP_DIVRK:
        {
            consuming = {c};
            ExprP rhs = use(c);
            consuming.clear();
            def(a, mkBinOp(in.op == LOP_SUBRK ? "-" : "/", constant(b), rhs), idx);
            break;
        }
        case LOP_CONCAT:
        {
            consuming.clear();
            for (int r = b; r <= c; ++r)
                consuming.push_back(r);
            std::vector<ExprP> parts;
            for (int r = b; r <= c; ++r)
                parts.push_back(use(r));
            consuming.clear();
            ExprP e = parts.back();
            for (size_t i = parts.size() - 1; i-- > 0;)
                e = mkBinOp("..", parts[i], e);
            def(a, e, idx);
            break;
        }
        case LOP_NOT:
            unop("not");
            break;
        case LOP_MINUS:
            unop("-");
            break;
        case LOP_LENGTH:
            unop("#");
            break;
        case LOP_NEWTABLE:
            def(a, mkExpr(ExprKind::Table), idx);
            break;
        case LOP_DUPTABLE:
            def(a, constant(in.d), idx);
            break;
        case LOP_SETLIST:
            liftSetList(idx);
            break;
        case LOP_GETVARARGS:
        {
            ExprP va = mkExpr(ExprKind::Vararg);
            if (b == 0)
            {
                va->multret = true;
                def(a, va, idx);
            }
            else if (b == 2)
                def(a, va, idx);
            else if (b > 2)
                defMulti(a, b - 1, va, idx);
            break;
        }
        default:
            unsupported(idx, "unsupported instruction");
            break;
        }
    }

    std::string upvalName(int index) const
    {
        if (index < int(upvalNames.size()))
            return upvalNames[index];
        return "u" + std::to_string(index);
    }

    // Values for RETURN / call arguments: registers [first, first+count-1], or up to the multret producer.
    std::vector<ExprP> readValues(int first, int bPlusOne, int idx)
    {
        std::vector<ExprP> values;
        int last;
        if (bPlusOne == 0)
            last = an.multretBase[idx];
        else
            last = first + bPlusOne - 2;

        consuming.clear();
        for (int r = first; r <= last; ++r)
            consuming.push_back(r);
        for (int r = first; r <= last; ++r)
            values.push_back(use(r));
        consuming.clear();

        if (bPlusOne == 0 && !values.empty())
            values.back()->multret = true;
        return values;
    }

    static bool sameKey(const ExprP& a, const ExprP& b)
    {
        if (!a || !b || a->kind != b->kind)
            return false;
        if (a->kind == ExprKind::String)
            return a->str == b->str;
        if (a->kind == ExprKind::Number)
            return a->number == b->number;
        return false;
    }

    void storeField(int tableReg, ExprP key, ExprP value, int idx)
    {
        bool functionField = value->kind == ExprKind::Function && key->kind == ExprKind::String && isIdentifier(key->str);

        if (pending[tableReg] && pending[tableReg]->expr->kind == ExprKind::Table && pending[tableReg]->method.empty())
        {
            // a named table keeps `function t.name()` declarations out of its constructor;
            // a temporary constructor absorbs everything, including closures
            bool namedTable = !pending[tableReg]->declName.empty();
            if (!(functionField && namedTable))
            {
                ExprP table = pending[tableReg]->expr;
                for (TableItem& item : table->items)
                {
                    if (!item.value && sameKey(item.key, key))
                    {
                        item.value = value; // fill the slot the DUPTABLE template reserved
                        return;
                    }
                }
                TableItem item;
                item.key = key;
                item.value = value;
                table->items.push_back(item);
                return;
            }
        }

        // the target of a field store must be a prefix expression: give constructors and other
        // values a name first
        if (pending[tableReg] && pending[tableReg]->method.empty())
        {
            ExprKind kind = pending[tableReg]->expr->kind;
            bool prefix = kind == ExprKind::Local || kind == ExprKind::Global || kind == ExprKind::Upvalue || kind == ExprKind::Index ||
                          kind == ExprKind::Call || kind == ExprKind::MethodCall;
            if (!prefix)
                declareBefore(tableReg, false);
        }

        consuming = {tableReg};
        ExprP table = use(tableReg);
        consuming.clear();

        if (functionField && isFunctionPath(table))
        {
            StmtP s = mkStmt(StmtKind::FunctionDecl);
            uint32_t protoIndex = uint32_t(value->integer);
            bool inferredSelf = !value->func->params.empty() && value->func->params[0] == "self";
            if (childIsMethod(protoIndex) || inferredSelf)
            {
                s->expr = table;
                s->methodName = key->str;
            }
            else
            {
                s->expr = mkIndex(table, key);
            }
            s->func = value->func;
            emit(s);
            return;
        }

        StmtP s = mkStmt(StmtKind::Assign);
        s->targets.push_back(mkIndex(table, key));
        s->values.push_back(value);
        emit(s);
    }

    static bool isFunctionPath(const ExprP& e)
    {
        if (e->kind == ExprKind::Local || e->kind == ExprKind::Global || e->kind == ExprKind::Upvalue)
            return isIdentifier(e->str);
        if (e->kind == ExprKind::Index)
            return e->b->kind == ExprKind::String && isIdentifier(e->b->str) && isFunctionPath(e->a);
        return false;
    }

    void liftSetList(int idx)
    {
        const Insn& in = an.insns[idx];
        int last = in.c == 0 ? an.multretBase[idx] : in.b + in.c - 2;

        consuming.clear();
        for (int r = in.b; r <= last; ++r)
            consuming.push_back(r);
        std::vector<ExprP> values;
        for (int r = in.b; r <= last; ++r)
            values.push_back(use(r));
        consuming.clear();

        if (in.c == 0 && !values.empty())
            values.back()->multret = true;

        int startIndex = int(in.aux);

        if (pending[in.a] && pending[in.a]->expr->kind == ExprKind::Table)
        {
            ExprP table = pending[in.a]->expr;
            int positional = 0;
            for (const TableItem& item : table->items)
                if (!item.key)
                    positional++;

            for (size_t i = 0; i < values.size(); ++i)
            {
                TableItem item;
                if (positional + 1 != startIndex + int(i) || (i + 1 < values.size() && values[i]->multret))
                    item.key = mkNumber(startIndex + int(i));
                else
                    positional++;
                item.value = values[i];
                table->items.push_back(item);
            }
            return;
        }

        consuming = {in.a};
        ExprP table = use(in.a);
        consuming.clear();
        for (size_t i = 0; i < values.size(); ++i)
        {
            StmtP s = mkStmt(StmtKind::Assign);
            s->targets.push_back(mkIndex(table, mkNumber(startIndex + int(i))));
            values[i]->multret = false;
            s->values.push_back(values[i]);
            emit(s);
        }
    }

    void liftCall(int idx)
    {
        const Insn& in = an.insns[idx];
        const int a = in.a;
        int lastArg = in.b == 0 ? an.multretBase[idx] : a + in.b - 1;

        // calling a closure directly reads better as a named local function than as an IIFE
        if (pending[a] && pending[a]->method.empty() && pending[a]->expr->kind == ExprKind::Function)
            declareBefore(a, false);

        consuming.clear();
        for (int r = a; r <= lastArg; ++r)
            consuming.push_back(r);

        ExprP call;
        if (pending[a] && !pending[a]->method.empty())
        {
            Pending P = *pending[a];
            pending[a].reset();
            pending[a + 1].reset();
            call = mkExpr(ExprKind::MethodCall);
            call->a = P.expr;
            call->str = P.method;
            for (int r = a + 2; r <= lastArg; ++r)
                call->args.push_back(use(r));
        }
        else
        {
            ExprP fn = use(a);
            call = mkExpr(ExprKind::Call);
            call->a = fn;
            for (int r = a + 1; r <= lastArg; ++r)
                call->args.push_back(use(r));
        }
        consuming.clear();

        if (in.b == 0 && !call->args.empty())
            call->args.back()->multret = true;

        int builtin = an.builtinOfCall[idx];
        if (builtin >= 0 && builtinReturnsOneValue(builtin))
            call->singleResult = true;

        // bit manipulation reads better with hexadecimal masks
        if (call->kind == ExprKind::Call && call->a->kind == ExprKind::Index && call->a->a->kind == ExprKind::Global && call->a->a->str == "bit32")
            for (ExprP& arg : call->args)
                if (arg->kind == ExprKind::Number && arg->number > 9 && arg->number == std::floor(arg->number))
                    arg->hex = true;

        int nresults = in.c - 1;
        if (in.c == 0)
        {
            call->multret = true;
            def(a, call, idx);
        }
        else if (nresults == 0)
        {
            StmtP s = mkStmt(StmtKind::Call);
            s->expr = call;
            emit(s);
        }
        else if (nresults == 1)
        {
            def(a, call, idx);
        }
        else
        {
            bool feedsFor = idx + 1 < int(an.insns.size()) && an.insns[idx + 1].a == a &&
                            (an.insns[idx + 1].op == LOP_FORGPREP || an.insns[idx + 1].op == LOP_FORGPREP_NEXT || an.insns[idx + 1].op == LOP_FORGPREP_INEXT);
            if (feedsFor && nresults == 3)
            {
                for (int r = 1; r < nresults; ++r)
                    dropPending(a + r);
                def(a, call, idx, std::string(), nresults);
            }
            else
            {
                defMulti(a, nresults, call, idx);
            }
        }
    }

    // ----- control flow -----

    bool isLoopHeader(int idx) const
    {
        return loopEnds.count(idx) != 0;
    }

    bool isJoinTarget(int target) const
    {
        return std::find(joinStack.begin(), joinStack.end(), target) != joinStack.end();
    }

    // Lift [i, stop) as `if not flag then ... end`.
    void liftSkippedRegion(int i, int stop, LoopCtx* ctx, const std::string& flag)
    {
        if (i >= stop)
            return;
        beforeConstruct(i, stop);
        declareLiveAfter(i, stop, stop);
        beforeStatement();
        std::vector<StmtP> body;
        liftBlock(i, stop, ctx, body);
        pushStmt(makeIf(mkUnOp("not", mkName(ExprKind::Local, flag)), body));
    }

    // A nested construct ended with a jump past its end: skip the code up to the target.
    // Returns the index to continue from.
    int resolveEscape(int i, int end, LoopCtx* ctx)
    {
        Escape esc = *pendingEscape;

        if (esc.target <= end)
        {
            pendingEscape.reset();
            liftSkippedRegion(i, esc.target, ctx, esc.flag);
            return esc.target;
        }

        if (ctx && esc.target >= ctx->breakTarget)
        {
            // the jump also leaves this loop
            emit(makeIf(mkName(ExprKind::Local, esc.flag), {mkStmt(StmtKind::Break)}));
            if (esc.target != ctx->breakTarget)
                ctx->escape = esc; // the loop that contains this one re-raises it after its own statement
            pendingEscape.reset();
            return i;
        }

        // inside a branch: the rest of the branch is skipped, the enclosing range continues the job
        liftSkippedRegion(i, end, ctx, esc.flag);
        return end;
    }

    void liftRange(int begin, int end, LoopCtx* ctx)
    {
        int i = begin;
        while (i < end)
        {
            if (pendingEscape)
            {
                i = resolveEscape(i, end, ctx);
                if (pendingEscape && i >= end)
                    return;
                continue;
            }

            auto loop = loopEnds.find(i);
            if (loop != loopEnds.end() && loop->second < end)
            {
                liftLoop(i, loop->second, ctx);
                i = loop->second + 1;
                continue;
            }

            const Insn& in = an.insns[i];

            if (in.op == LOP_FORNPREP)
            {
                i = liftNumFor(i, end);
                continue;
            }
            if (in.op == LOP_FORGPREP || in.op == LOP_FORGPREP_NEXT || in.op == LOP_FORGPREP_INEXT)
            {
                i = liftGenFor(i, end);
                continue;
            }
            if (isCondJump(in.op))
            {
                i = liftIf(i, end, ctx);
                if (ctx && ctx->sawUntil)
                    return;
                continue;
            }
            if (isUncondJump(in.op) || (in.op == LOP_LOADB && in.c != 0))
            {
                if (in.op == LOP_LOADB)
                    liftSimple(i);
                int target = an.targetIdx(i);
                if (ctx && target == ctx->breakTarget)
                    emit(mkStmt(StmtKind::Break));
                else if (ctx && (target == ctx->continueTarget || target == ctx->headerIdx))
                    emit(mkStmt(StmtKind::Continue));
                else if ((target == end || isJoinTarget(target)) && i + 1 == end)
                {
                    // jump to the end of this range or to the join of an enclosing if: nothing to do
                }
                else if (ctx && target > ctx->breakTarget && (!ctx->escape || ctx->escape->target == target))
                {
                    // leaving the loop to a point past its exit (an inlined return): flag it and break
                    if (!ctx->escape)
                    {
                        Escape esc;
                        esc.target = target;
                        esc.flag = newName("escaped");
                        used.insert(esc.flag);
                        escapeFlags.push_back(esc.flag);
                        ctx->escape = esc;
                    }
                    StmtP set = mkStmt(StmtKind::Assign);
                    set->targets.push_back(mkName(ExprKind::Local, ctx->escape->flag));
                    set->values.push_back(mkExpr(ExprKind::True));
                    emit(set);
                    emit(mkStmt(StmtKind::Break));
                }
                else if (target > i && target <= end)
                {
                    // forward jump inside the range that the structurer did not claim: lift the skipped
                    // code as a nested block after the jump, which preserves semantics when the target
                    // is only reachable from here
                    comment("luaudec: unstructured forward jump to pc " + std::to_string(in.jumpTarget) + " (pc " + std::to_string(in.pc) + ")");
                }
                else
                    unsupported(i, "unstructured jump");
                i++;
                continue;
            }
            if (in.op == LOP_FORNLOOP || in.op == LOP_FORGLOOP || in.op == LOP_CMPPROTO)
            {
                unsupported(i, "unexpected loop instruction");
                i++;
                continue;
            }

            if (int consumed = trySwap(i))
            {
                i += consumed;
                continue;
            }

            liftSimple(i);
            i++;
        }
    }

    // `a, b = b, a` compiles to MOVE tmp, a; MOVE a, b; MOVE b, tmp.
    int trySwap(int idx)
    {
        if (idx + 2 >= int(an.insns.size()))
            return 0;
        const Insn& m1 = an.insns[idx];
        const Insn& m2 = an.insns[idx + 1];
        const Insn& m3 = an.insns[idx + 2];
        if (m1.op != LOP_MOVE || m2.op != LOP_MOVE || m3.op != LOP_MOVE)
            return 0;
        int tmp = m1.a, ra = m1.b, rb = m2.b;
        if (m2.a != ra || m3.a != rb || m3.b != tmp || ra == rb || tmp == ra || tmp == rb)
            return 0;
        if (!hasStickyVar(ra) || !hasStickyVar(rb) || pending[ra] || pending[rb])
            return 0;
        if (an.countUsesFrom(idx + 3, tmp, false).count > 0)
            return 0;

        dropPending(tmp);
        // ra receives rb's value and rb receives ra's; list the lower register first like the source usually does
        int first = std::min(ra, rb), second = std::max(ra, rb);
        StmtP s = mkStmt(StmtKind::Assign);
        s->targets = {mkName(ExprKind::Local, vars[first].name, first), mkName(ExprKind::Local, vars[second].name, second)};
        s->values = {mkName(ExprKind::Local, vars[second].name, second), mkName(ExprKind::Local, vars[first].name, first)};
        emit(s);
        return 3;
    }

    // Lift [begin, end) as a nested block in a fresh scope.
    void liftBlock(int begin, int end, LoopCtx* ctx, std::vector<StmtP>& stmts)
    {
        std::vector<StmtP>* saved = out;
        out = &stmts;
        pushScope();
        liftRange(begin, end, ctx);
        flushFrom(begin);
        popScope();
        out = saved;
    }

    // Declare variables that are assigned inside [begin, end) and still needed at joinIdx.
    void declareLiveAfter(int begin, int end, int joinIdx, const std::set<int>& exclude = {})
    {
        std::set<int> written = an.writtenIn(begin, end);
        for (int r : written)
        {
            if (exclude.count(r))
                continue;
            if (an.liveAt(joinIdx, r))
                declareBefore(r, false, joinIdx);
        }
    }

    void liftLoop(int headerIdx, int backIdx, LoopCtx* outer)
    {
        int exitIdx = backIdx + 1;

        beforeConstruct(headerIdx, exitIdx);
        std::set<int> written = an.writtenIn(headerIdx, exitIdx);
        for (int r : written)
            if (an.liveIn[an.cfg.blockOf(headerIdx).id][r] || an.liveAt(exitIdx, r))
                declareBefore(r, false);
        beforeStatement();

        LoopCtx ctx;
        ctx.breakTarget = exitIdx;
        ctx.continueTarget = backIdx;
        ctx.headerIdx = headerIdx;

        // while loop: a condition chain at the header that exits the loop
        ChainCands cands = findCondChain(headerIdx, backIdx);
        for (size_t i = cands.prefixes.size(); i-- > 0;)
        {
            const ChainPrefix& prefix = cands.prefixes[i];
            if (!prefix.single() || prefix.falseTarget() != exitIdx || prefix.thenStart > backIdx)
                continue;

            CondChain chain = buildCond(cands, prefix);
            StmtP s = mkStmt(StmtKind::While);
            s->expr = chain.cond;
            liftBlock(chain.thenStart, backIdx, &ctx, s->body);
            finishLoop(ctx, s);
            return;
        }

        ctx.untilAllowed = true;
        std::vector<StmtP> body;
        liftBlock(headerIdx, backIdx, &ctx, body);

        if (ctx.sawUntil)
        {
            StmtP s = mkStmt(StmtKind::Repeat);
            s->body = body;
            s->expr = ctx.untilCond;
            finishLoop(ctx, s);
        }
        else
        {
            StmtP s = mkStmt(StmtKind::While);
            s->expr = mkExpr(ExprKind::True);
            s->body = body;
            finishLoop(ctx, s);
        }
    }

    // Emit a finished loop statement. When its body jumped past the loop exit, reset the escape
    // flag before the loop and hand the escape to the enclosing range.
    void finishLoop(LoopCtx& ctx, const StmtP& loopStmt)
    {
        if (ctx.escape)
        {
            StmtP reset = mkStmt(StmtKind::Assign);
            reset->targets.push_back(mkName(ExprKind::Local, ctx.escape->flag));
            reset->values.push_back(mkExpr(ExprKind::False));
            pushStmt(reset);
        }
        pushStmt(loopStmt);
        if (ctx.escape)
            pendingEscape = ctx.escape;
    }

    int liftNumFor(int idx, int end)
    {
        const Insn& in = an.insns[idx];
        int exitIdx = an.targetIdx(idx);
        int loopIdx = exitIdx - 1;
        int base = in.a;

        if (loopIdx <= idx || an.insns[loopIdx].op != LOP_FORNLOOP || an.insns[loopIdx].a != base || exitIdx > end)
        {
            unsupported(idx, "malformed numeric for");
            return idx + 1;
        }

        consuming = {base, base + 1, base + 2};
        ExprP from = use(base + 2);
        ExprP to = use(base);
        ExprP step = use(base + 1);
        consuming.clear();

        // when the loop variable is captured by a closure the compiler keeps it in the register right
        // after the control registers and copies the index into it at the top of every iteration
        int bodyBegin = idx + 1;
        int varReg = base + 2;
        if (bodyBegin < loopIdx && an.insns[bodyBegin].op == LOP_MOVE && an.insns[bodyBegin].b == base + 2 && an.insns[bodyBegin].a == base + 3)
        {
            varReg = an.insns[bodyBegin].a;
            bodyBegin++;
        }

        std::set<int> exclude = {base, base + 1, base + 2, varReg};
        beforeConstruct(idx, exitIdx);
        declareLiveAfter(bodyBegin, loopIdx, exitIdx, exclude);
        declareLoopCarried(bodyBegin, loopIdx, exclude);
        beforeStatement();

        StmtP s = mkStmt(StmtKind::NumFor);
        s->values = {from, to};
        if (!(step->kind == ExprKind::Number && step->number == 1.0))
            s->values.push_back(step);

        LoopCtx ctx;
        ctx.breakTarget = exitIdx;
        ctx.continueTarget = loopIdx;
        ctx.headerIdx = idx;

        static const char* kIndexNames[] = {"i", "j", "k"};

        std::vector<StmtP>* saved = out;
        out = &s->body;
        pushScope();
        const DebugLocal* dl = debugLocalAt(varReg, int(an.insns[bodyBegin < loopIdx ? bodyBegin : loopIdx].pc));
        std::string varName = dl ? dl->name : newName(forDepth < 3 ? kIndexNames[forDepth] : "");
        setVar(varReg, varName, true);
        s->names.push_back(varName);
        for (int r = base; r < base + 3; ++r)
            if (r != varReg)
                clearVar(r);
        forDepth++;
        liftRange(bodyBegin, loopIdx, &ctx);
        forDepth--;
        flushAll();
        popScope();
        out = saved;

        finishLoop(ctx, s);
        return exitIdx;
    }

    // Values assigned inside the loop body and read at its top on the next iteration must be
    // variables declared before the loop.
    void declareLoopCarried(int bodyBegin, int bodyEnd, const std::set<int>& exclude)
    {
        std::set<int> written = an.writtenIn(bodyBegin, bodyEnd);
        for (int r : written)
            if (!exclude.count(r) && an.liveAt(bodyBegin, r))
                declareBefore(r, false); // writes inside the body are in a nested scope and assign anyway
    }

    int liftGenFor(int idx, int end)
    {
        const Insn& in = an.insns[idx];
        int loopIdx = an.targetIdx(idx);
        int base = in.a;

        if (loopIdx <= idx || an.insns[loopIdx].op != LOP_FORGLOOP || an.insns[loopIdx].a != base || loopIdx >= end)
        {
            unsupported(idx, "malformed generic for");
            return idx + 1;
        }

        int exitIdx = loopIdx + 1;
        int nvars = int(an.insns[loopIdx].aux & 0xff);

        std::vector<ExprP> iterators;
        if (pending[base] && pending[base]->nresults == 3)
        {
            Pending P = *pending[base];
            pending[base].reset();
            P.expr->multret = true;
            iterators.push_back(P.expr);
        }
        else
        {
            consuming = {base, base + 1, base + 2};
            for (int r = base; r < base + 3; ++r)
                iterators.push_back(use(r));
            consuming.clear();
            while (iterators.size() > 1 && iterators.back()->kind == ExprKind::Nil)
                iterators.pop_back();
        }

        std::set<int> exclude;
        for (int r = base; r < base + 3 + nvars; ++r)
            exclude.insert(r);

        beforeConstruct(idx, exitIdx);
        declareLiveAfter(idx + 1, loopIdx, exitIdx, exclude);
        declareLoopCarried(idx + 1, loopIdx, exclude);
        beforeStatement();

        StmtP s = mkStmt(StmtKind::GenFor);
        s->values = iterators;

        LoopCtx ctx;
        ctx.breakTarget = exitIdx;
        ctx.continueTarget = loopIdx;
        ctx.headerIdx = idx;

        // `for i, v in ipairs` / `for k, v in pairs`
        bool arrayLike = !iterators.empty() && iterators[0]->kind == ExprKind::Call && iterators[0]->a->kind == ExprKind::Global &&
                         iterators[0]->a->str == "ipairs";
        const char* hints2[] = {arrayLike ? "i" : "k", "v"};

        std::vector<StmtP>* saved = out;
        out = &s->body;
        pushScope();
        for (int r = base; r < base + 3; ++r)
            clearVar(r);
        int bodyPc = int(an.insns[idx + 1 < loopIdx ? idx + 1 : loopIdx].pc);
        for (int v = 0; v < nvars; ++v)
        {
            int reg = base + 3 + v;
            const DebugLocal* dl = debugLocalAt(reg, bodyPc);
            std::string hint = nvars == 1 ? "v" : (v < 2 ? hints2[v] : "");
            std::string name = dl ? dl->name : newName(hint);
            setVar(reg, name, true);
            s->names.push_back(name);
        }
        liftRange(idx + 1, loopIdx, &ctx);
        flushAll();
        popScope();
        out = saved;

        finishLoop(ctx, s);
        return exitIdx;
    }

    // Try to lift [begin, end) as an expression that only defines register reg.
    // On success returns the expression and leaves the state as it was before.
    std::optional<ExprP> liftValueRegion(int begin, int end, int reg, LoopCtx* ctx)
    {
        if (begin > end)
            return std::nullopt;

        Snapshot snap = snapshot();
        int savedRegion = exprRegionReg;
        exprRegionReg = reg;

        std::vector<StmtP> stmts;
        std::vector<StmtP>* saved = out;
        out = &stmts;
        bool ok = true;
        try
        {
            liftRange(begin, end, ctx);
        }
        catch (const LiftError&)
        {
            ok = false;
        }
        out = saved;
        exprRegionReg = savedRegion;

        std::optional<ExprP> result;
        if (ok && stmts.empty() && pending[reg] && pending[reg]->defIdx >= begin && pending[reg]->method.empty())
        {
            bool others = false;
            for (int r = 0; r < int(pending.size()); ++r)
            {
                if (r == reg || !pending[r])
                    continue;
                bool existed = snap.pending[r] && snap.pending[r]->defIdx == pending[r]->defIdx;
                if (!existed && needsFlush(*pending[r]))
                    others = true;
            }
            if (!others)
                result = pending[reg]->expr;
        }

        restore(snap);
        return result;
    }

    // Lift a region that liftValueRegion already accepted, consuming its inputs from the live state.
    ExprP liftValueRegionForReal(int begin, int end, int reg, LoopCtx* ctx, const ExprP& fallback)
    {
        Snapshot snap = snapshot();
        int savedRegion = exprRegionReg;
        exprRegionReg = reg;

        std::vector<StmtP> scratch;
        std::vector<StmtP>* saved = out;
        out = &scratch;
        bool ok = true;
        try
        {
            liftRange(begin, end, ctx);
        }
        catch (const LiftError&)
        {
            ok = false;
        }
        out = saved;
        exprRegionReg = savedRegion;

        if (ok && scratch.empty() && pending[reg])
        {
            ExprP value = pending[reg]->expr;
            pending[reg].reset();
            return value;
        }

        restore(snap);
        return fallback;
    }

    struct ChainBlock
    {
        int start; // first instruction of the block (setup instructions)
        int jump;  // the conditional jump that ends it
    };

    struct ChainPrefix
    {
        int count;
        int thenStart;
        std::vector<int> falses; // distinct non-chain jump targets, ascending (1 or 2 entries)

        bool single() const
        {
            return falses.size() == 1;
        }
        int falseTarget() const
        {
            return falses.front();
        }
    };

    struct ChainCands
    {
        std::vector<ChainBlock> blocks;
        std::vector<ChainPrefix> prefixes; // valid prefixes in increasing length
    };

    // Find the chain of condition blocks starting at start (which may begin with setup
    // instructions before its first conditional jump). Blocks must not produce statements.
    ChainCands findCondChain(int start, int limit)
    {
        ChainCands result;

        Snapshot snap = snapshot();
        {
            std::vector<StmtP> scratch;
            std::vector<StmtP>* saved = out;
            out = &scratch;
            int cursor = start;
            while (cursor < limit && (cursor == start || !isLoopHeader(cursor)))
            {
                int k = cursor;
                while (k < limit && isSimpleInsn(an.insns[k]))
                    k++;
                if (k >= limit || !isCondJump(an.insns[k].op))
                    break;

                size_t before = scratch.size();
                bool ok = true;
                try
                {
                    for (int j = cursor; j < k; ++j)
                        liftSimple(j);
                    if (scratch.size() == before)
                        jumpCondition(k);
                }
                catch (const LiftError&)
                {
                    ok = false;
                }
                if (!ok || scratch.size() != before)
                    break;

                result.blocks.push_back(ChainBlock{cursor, k});
                cursor = k + 1;
            }
            out = saved;
        }
        restore(snap);

        for (int n = 1; n <= int(result.blocks.size()); ++n)
        {
            int S = result.blocks[n - 1].jump + 1;
            std::set<int> intermediate;
            for (int j = 1; j < n; ++j)
                intermediate.insert(result.blocks[j].start);

            std::set<int> falses;
            bool ok = true;
            for (int j = 0; j < n; ++j)
            {
                int t = an.targetIdx(result.blocks[j].jump);
                if (t == S || intermediate.count(t))
                    continue;
                if (t >= start && t < S)
                {
                    ok = false; // jumps into the middle of a chain block
                    break;
                }
                falses.insert(t);
            }
            if (!ok || falses.empty() || falses.size() > 2)
                continue;
            ChainPrefix prefix;
            prefix.count = n;
            prefix.thenStart = S;
            prefix.falses.assign(falses.begin(), falses.end());
            result.prefixes.push_back(prefix);
        }

        return result;
    }

    // Does every jump of the chain that exits to F leave the operand value in reg? That is how the
    // compiler materializes `a and b` / `a or b` when the falsy (or truthy) operand is the result.
    // orForm is set from the last block: JUMPIF falls into the region when the value is falsy.
    bool chainCarriesValue(const ChainCands& cands, const ChainPrefix& prefix, int reg, int F, bool& orForm) const
    {
        const ChainBlock& last = cands.blocks[prefix.count - 1];
        const Insn& lastJump = an.insns[last.jump];
        if ((lastJump.op != LOP_JUMPIF && lastJump.op != LOP_JUMPIFNOT) || lastJump.a != reg)
            return false;

        for (int j = 0; j < prefix.count; ++j)
        {
            const ChainBlock& blk = cands.blocks[j];
            if (an.targetIdx(blk.jump) != F)
                continue;
            const Insn& jump = an.insns[blk.jump];
            if ((jump.op != LOP_JUMPIF && jump.op != LOP_JUMPIFNOT) || jump.a != reg)
                return false;
            bool defined = false;
            for (int k = blk.start; k < blk.jump; ++k)
                if (an.defsReg(k, reg))
                    defined = true;
            if (!defined)
                return false;
        }

        orForm = lastJump.op == LOP_JUMPIF;
        return true;
    }

    // Lift the chain blocks for real and build the condition under which control reaches one of
    // the targets in trueSet (thenStart counts as reached when the last block falls through).
    CondChain buildCond(const ChainCands& cands, const ChainPrefix& prefix, const std::set<int>& trueSet, bool fallIsTrue)
    {
        std::vector<ExprP> jumpConds;
        std::vector<int> targets;
        std::vector<int> starts;
        for (int j = 0; j < prefix.count; ++j)
        {
            const ChainBlock& blk = cands.blocks[j];
            for (int k = blk.start; k < blk.jump; ++k)
                liftSimple(k);
            jumpConds.push_back(jumpCondition(blk.jump));
            targets.push_back(an.targetIdx(blk.jump));
            starts.push_back(blk.start);
        }

        CondChain chain;
        chain.count = prefix.count;
        chain.thenStart = prefix.thenStart;
        chain.falseTarget = prefix.falseTarget();
        chain.cond = buildChain(jumpConds, targets, starts, 0, prefix.count, trueSet, fallIsTrue);
        return chain;
    }

    // Statement form: the condition for reaching thenStart.
    CondChain buildCond(const ChainCands& cands, const ChainPrefix& prefix)
    {
        return buildCond(cands, prefix, {prefix.thenStart}, true);
    }

    // Condition for reaching a true target from block lo; falling off block hi-1 is true iff fallIsTrue.
    ExprP buildChain(const std::vector<ExprP>& conds, const std::vector<int>& targets, const std::vector<int>& starts, int lo, int hi, const std::set<int>& trueSet, bool fallIsTrue)
    {
        if (lo >= hi)
            return mkExpr(fallIsTrue ? ExprKind::True : ExprKind::False);

        ExprP jc = conds[lo];
        int t = targets[lo];

        for (int j = lo + 1; j < hi; ++j)
        {
            if (starts[j] == t)
            {
                ExprP inner = buildChain(conds, targets, starts, lo + 1, j, trueSet, false);
                ExprP rest = buildChain(conds, targets, starts, j, hi, trueSet, fallIsTrue);
                return mkOr(mkAnd(negate(jc), inner), rest);
            }
        }

        if (trueSet.count(t))
            return mkOr(jc, buildChain(conds, targets, starts, lo + 1, hi, trueSet, fallIsTrue));
        return mkAnd(negate(jc), buildChain(conds, targets, starts, lo + 1, hi, trueSet, fallIsTrue));
    }

    int liftIf(int idx, int end, LoopCtx* ctx)
    {
        const Insn& first = an.insns[idx];

        // value pattern: `Rx = e1; JUMPIF(NOT) Rx L; Rx = e2; L:` is `e1 and/or e2`
        if ((first.op == LOP_JUMPIF || first.op == LOP_JUMPIFNOT) && pending[first.a] && pending[first.a]->declName.empty() &&
            pending[first.a]->method.empty())
        {
            int reg = first.a;
            int target = an.targetIdx(idx);
            if (target > idx + 1 && target <= end)
            {
                bool trailingJump = isUncondJump(an.insns[target - 1].op) || (an.insns[target - 1].op == LOP_JUMPIF && an.insns[target - 1].a == reg);
                if (!trailingJump)
                {
                    // take e1 out first: the region overwrites the register and would otherwise flush it
                    Pending lhsPending = *pending[reg];
                    pending[reg].reset();
                    std::optional<ExprP> rhs = liftValueRegion(idx + 1, target, reg, ctx);
                    if (rhs)
                    {
                        ExprP combined = mkBinOp(first.op == LOP_JUMPIFNOT ? "and" : "or", lhsPending.expr, *rhs);
                        def(reg, combined, target - 1, std::string(), 1, target);
                        return target;
                    }
                    pending[reg] = lhsPending;
                }
            }
        }

        ChainCands cands = findCondChain(idx, end);
        if (cands.prefixes.empty())
        {
            unsupported(idx, "unstructured conditional jump");
            return idx + 1;
        }

        // value patterns are checked on the shortest chains first so a dead `a and b or c`
        // is not swallowed by the condition of the statement that follows it
        for (const ChainPrefix& prefix : cands.prefixes)
        {
            int S = prefix.thenStart;
            int F = prefix.falseTarget();

            // boolean materialization: JUMP cond; LOADB Rx b +1; LOADB Rx !b
            // optionally preceded by `LOADB Rx lit` with jumps to the join keeping lit
            if (F == S + 1 && F < end && an.insns[S].op == LOP_LOADB && an.insns[S].c != 0 && an.targetIdx(S) == F + 1 &&
                an.insns[F].op == LOP_LOADB && an.insns[F].a == an.insns[S].a && an.insns[F].c == 0 && an.insns[F].b != an.insns[S].b)
            {
                int reg = an.insns[S].a;
                int join = F + 1;
                bool preloaded = pending[reg] && pending[reg]->declName.empty() &&
                                 (pending[reg]->expr->kind == ExprKind::True || pending[reg]->expr->kind == ExprKind::False);
                if (prefix.single() || (preloaded && prefix.falses.size() == 2 && prefix.falses[1] == join))
                {
                    std::set<int> trueSet;
                    if (an.insns[F].b)
                        trueSet.insert(F);
                    if (preloaded && pending[reg]->expr->kind == ExprKind::True)
                        trueSet.insert(join);
                    if (preloaded)
                        pending[reg].reset();
                    CondChain chain = buildCond(cands, prefix, trueSet, an.insns[S].b != 0);
                    def(reg, chain.cond, F, std::string(), 1, join);
                    return join;
                }
            }

            if (!prefix.single() || F > end)
                continue;

            // The value regions are checked after the condition consumed its operands, in program
            // order; the snapshot is restored when no pattern matches this prefix.
            Snapshot beforeCond = snapshot();
            CondChain chain = buildCond(cands, prefix);

            // `cond and e2 or e3`: JUMP !cond -> F; Rx = e2; JUMPIF Rx -> T; F: Rx = e3; T:
            if (F - 1 > S && an.insns[F - 1].op == LOP_JUMPIF)
            {
                int reg = an.insns[F - 1].a;
                int T = an.targetIdx(F - 1);
                if (T > F && T <= end)
                {
                    std::optional<ExprP> e2 = liftValueRegion(S, F - 1, reg, ctx);
                    std::optional<ExprP> e3 = e2 ? liftValueRegion(F, T, reg, ctx) : std::nullopt;
                    if (e2 && e3)
                    {
                        ExprP v2 = liftValueRegionForReal(S, F - 1, reg, ctx, *e2);
                        ExprP v3 = liftValueRegionForReal(F, T, reg, ctx, *e3);
                        def(reg, mkOr(mkAnd(chain.cond, v2), v3), T - 1, std::string(), 1, T);
                        return T;
                    }
                }
            }

            // A chain whose false exits carry a value in Rx followed by a region computing Rx:
            //   `Rx = lit; JUMP !cond -> F; Rx = e2; F:`      is `cond and e2` (lit false) / `not cond or e2` (lit true)
            //   `... Rx = a; JUMPIFNOT Rx -> F; Rx = e2; F:` is `cond and e2` where cond materializes a
            //   `... Rx = a; JUMPIF Rx -> F; Rx = e2; F:`    is `not cond or e2`
            if (F - 1 >= S && !isUncondJump(an.insns[F - 1].op) && !isCondJump(an.insns[F - 1].op) && an.defs[F - 1].size() == 1)
            {
                int reg = an.defs[F - 1][0];
                std::optional<Pending> literal;
                if (pending[reg] && pending[reg]->declName.empty() && pending[reg]->method.empty() &&
                    (pending[reg]->expr->kind == ExprKind::True || pending[reg]->expr->kind == ExprKind::False))
                    literal = *pending[reg];

                bool orForm = false;
                bool applies = false;
                if (literal)
                {
                    orForm = literal->expr->kind == ExprKind::True;
                    applies = true;
                }
                else if (chainCarriesValue(cands, prefix, reg, F, orForm))
                {
                    applies = true;
                }

                if (applies)
                {
                    if (literal)
                        pending[reg].reset();
                    std::optional<ExprP> e2 = liftValueRegion(S, F, reg, ctx);
                    if (e2)
                    {
                        ExprP v2 = liftValueRegionForReal(S, F, reg, ctx, *e2);
                        ExprP value = orForm ? mkOr(negate(chain.cond), v2) : mkAnd(chain.cond, v2);
                        def(reg, value, F - 1, std::string(), 1, F);
                        return F;
                    }
                    if (literal)
                        pending[reg] = *literal;
                }
            }

            restore(beforeCond);
        }

        const ChainPrefix* chosen = nullptr;
        for (const ChainPrefix& prefix : cands.prefixes)
            if (prefix.single())
                chosen = &prefix;
        if (!chosen)
        {
            unsupported(idx, "conditional jump with several targets");
            return idx + 1;
        }

        CondChain chain = buildCond(cands, *chosen);
        int S = chain.thenStart;
        int F = chain.falseTarget;

        // repeat ... until cond: the chain falls through into the back jump and exits the loop
        if (ctx && ctx->untilAllowed && S == end && F == ctx->breakTarget)
        {
            ctx->sawUntil = true;
            ctx->untilCond = negate(chain.cond);
            return S;
        }

        // the false edge leaves the loop: if not cond then break end
        if (ctx && F == ctx->breakTarget)
        {
            emit(makeIf(negate(chain.cond), {mkStmt(StmtKind::Break)}));
            return S;
        }

        // the false edge goes back to the loop header: if not cond then continue end
        if (ctx && (F == ctx->headerIdx || (F == ctx->continueTarget && F < S)))
        {
            emit(makeIf(negate(chain.cond), {mkStmt(StmtKind::Continue)}));
            return S;
        }

        if (F < S || F > end)
        {
            unsupported(idx, "conditional jump leaves the current block");
            return S;
        }

        int thenEnd = F;
        int elseEnd = -1;
        StmtP tail;

        if (F - 1 >= S && isUncondJump(an.insns[F - 1].op))
        {
            int T = an.targetIdx(F - 1);
            bool emptyThen = F - 1 == S;
            if (ctx && T == ctx->breakTarget)
            {
                thenEnd = F - 1;
                tail = mkStmt(StmtKind::Break);
            }
            else if (ctx && emptyThen && (T == ctx->continueTarget || T == ctx->headerIdx))
            {
                // `if c then continue end` rather than an if with an empty then and everything in the else
                thenEnd = F - 1;
                tail = mkStmt(StmtKind::Continue);
            }
            else if (T > F && T <= end)
            {
                thenEnd = F - 1;
                elseEnd = T;
            }
            else if (T > end && isJoinTarget(T))
            {
                // nested inside another if's branch: jumping to the outer join ends this branch too
                thenEnd = F - 1;
                elseEnd = end;
            }
            else if (ctx && (T == ctx->continueTarget || T == ctx->headerIdx))
            {
                thenEnd = F - 1;
                tail = mkStmt(StmtKind::Continue);
            }
        }

        int joinIdx = elseEnd >= 0 ? elseEnd : F;

        beforeConstruct(S, joinIdx);
        declareLiveAfter(S, joinIdx, joinIdx);
        beforeStatement();

        StmtP s = mkStmt(StmtKind::If);
        IfClause clause;
        clause.cond = chain.cond;
        joinStack.push_back(joinIdx);
        liftBlock(S, thenEnd, ctx, clause.body);
        joinStack.pop_back();
        if (tail)
            clause.body.push_back(tail);
        s->clauses.push_back(clause);

        if (elseEnd >= 0)
        {
            std::vector<StmtP> elseBody;
            joinStack.push_back(joinIdx);
            liftBlock(F, elseEnd, ctx, elseBody);
            joinStack.pop_back();

            // both branches assign the same variable and nothing else: x = if c then a else b
            if (!tail && clause.body.size() == 1 && elseBody.size() == 1 && isSimpleAssign(clause.body[0]) && isSimpleAssign(elseBody[0]) &&
                clause.body[0]->targets[0]->str == elseBody[0]->targets[0]->str)
            {
                ExprP value = mkExpr(ExprKind::IfElse);
                value->a = chain.cond;
                value->b = clause.body[0]->values[0];
                value->args.push_back(elseBody[0]->values[0]);
                StmtP assign = mkStmt(StmtKind::Assign);
                assign->targets = clause.body[0]->targets;
                assign->values.push_back(value);
                pushStmt(assign);
                return joinIdx;
            }

            if (elseBody.size() == 1 && elseBody[0]->kind == StmtKind::If)
            {
                for (const IfClause& c : elseBody[0]->clauses)
                    s->clauses.push_back(c);
                s->hasElse = elseBody[0]->hasElse;
                s->elseBody = elseBody[0]->elseBody;
            }
            else
            {
                s->hasElse = true;
                s->elseBody = elseBody;
            }
        }

        pushStmt(s);
        return joinIdx;
    }

    static bool isSimpleAssign(const StmtP& s)
    {
        return s->kind == StmtKind::Assign && s->targets.size() == 1 && s->values.size() == 1 && s->targets[0]->kind == ExprKind::Local &&
               !s->values[0]->multret;
    }

    static StmtP makeIf(ExprP cond, std::vector<StmtP> body)
    {
        StmtP s = mkStmt(StmtKind::If);
        IfClause clause;
        clause.cond = std::move(cond);
        clause.body = std::move(body);
        s->clauses.push_back(clause);
        return s;
    }

    // ----- entry point -----

    FunctionP liftFunction()
    {
        FunctionP fn = std::make_shared<FunctionBody>();
        fn->vararg = p.isvararg;

        for (int i = 0; i < p.numparams; ++i)
        {
            const DebugLocal* dl = debugLocalAt(i, 0);
            std::string name = dl ? dl->name : newName(i == 0 && selfParam ? "self" : "");
            setVar(i, name, true);
            fn->params.push_back(name);
        }

        out = &fn->body;
        pushScope();
        liftRange(0, int(an.insns.size()), nullptr);
        flushAll();
        popScope();

        if (!escapeFlags.empty())
        {
            StmtP decl = mkStmt(StmtKind::Local);
            decl->names = escapeFlags;
            fn->body.insert(fn->body.begin(), decl);
        }

        return fn;
    }
};

FunctionP Lifter::liftProto(const Module& m, const Proto& p, const LiftOptions& options, std::vector<std::string> upvals, NameContext* names,
    const std::set<std::string>& inherited, bool selfParam)
{
    try
    {
        Lifter lifter(m, p, options, std::move(upvals), names, inherited);
        lifter.selfParam = selfParam;
        return lifter.liftFunction();
    }
    catch (const std::exception& e)
    {
        FunctionP fn = std::make_shared<FunctionBody>();
        fn->vararg = p.isvararg;
        for (int i = 0; i < p.numparams; ++i)
            fn->params.push_back("p" + std::to_string(i + 1));
        fn->body.push_back(mkComment(std::string("luaudec: failed to lift function ") + std::to_string(p.index) + ": " + e.what()));
        std::string dump = disassembleProto(m, p);
        size_t pos = 0;
        while (pos < dump.size())
        {
            size_t next = dump.find('\n', pos);
            if (next == std::string::npos)
                next = dump.size();
            fn->body.push_back(mkComment(dump.substr(pos, next - pos)));
            pos = next + 1;
        }
        return fn;
    }
}

} // namespace

FunctionP liftModule(const Module& m, const LiftOptions& options)
{
    NameContext names;

    // never give a local the name of a global the module uses; it would shadow the global
    for (const Proto& p : m.protos)
    {
        for (const Insn& in : decodeProto(p))
        {
            if (in.op == LOP_GETGLOBAL || in.op == LOP_SETGLOBAL)
            {
                if (in.aux < p.constants.size() && p.constants[in.aux].kind == Constant::String)
                    names.reserved.insert(m.str(p.constants[in.aux].stringIndex));
            }
            else if (in.op == LOP_GETIMPORT)
            {
                if (size_t(in.d) < p.constants.size() && p.constants[in.d].kind == Constant::Import)
                {
                    std::vector<std::string> path = decodeImportPath(m, p, p.constants[in.d].importId);
                    if (!path.empty())
                        names.reserved.insert(path[0]);
                }
            }
            else if (in.op == LOP_NAMECALL)
            {
                if (in.aux < p.constants.size() && p.constants[in.aux].kind == Constant::String)
                    names.methods.insert(m.str(p.constants[in.aux].stringIndex));
            }
        }
    }

    std::vector<std::string> upvals;
    std::set<std::string> inherited;
    return Lifter::liftProto(m, m.protos[m.mainProto], options, upvals, &names, inherited);
}

std::string decompile(const Module& m, const LiftOptions& options)
{
    FunctionP main = liftModule(m, options);
    return emitChunk(*main);
}

} // namespace luaudec
