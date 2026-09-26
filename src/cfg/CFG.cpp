#include "cfg/CFG.h"

#include "Luau/BytecodeUtils.h"

#include <algorithm>
#include <cstdio>

namespace luaudec
{

namespace
{

enum class Flow
{
    Next,            // falls through only
    Target,          // unconditional jump
    NextAndTarget,   // conditional: falls through or jumps
    None,            // return
};

Flow flowOf(const Insn& insn, bool fastcallEdges)
{
    switch (insn.op)
    {
    case LOP_RETURN:
        return Flow::None;

    case LOP_JUMP:
    case LOP_JUMPBACK:
    case LOP_JUMPX:
    case LOP_FORGPREP:
    case LOP_FORGPREP_NEXT:
    case LOP_FORGPREP_INEXT:
        return Flow::Target;

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
    case LOP_FORNPREP:
    case LOP_FORNLOOP:
    case LOP_FORGLOOP:
    case LOP_CMPPROTO:
        return Flow::NextAndTarget;

    case LOP_LOADB:
        return insn.c != 0 ? Flow::Target : Flow::Next;

    case LOP_FASTCALL:
    case LOP_FASTCALL1:
    case LOP_FASTCALL2:
    case LOP_FASTCALL2K:
    case LOP_FASTCALL3:
    case LOP_FASTPCALL:
        return fastcallEdges ? Flow::NextAndTarget : Flow::Next;

    default:
        return Flow::Next;
    }
}

} // namespace

bool isBlockTerminator(const Insn& insn, bool fastcallEdges)
{
    return flowOf(insn, fastcallEdges) != Flow::Next;
}

CFG buildCFG(const std::vector<Insn>& insns, bool fastcallEdges)
{
    CFG cfg;
    const int n = int(insns.size());
    if (n == 0)
        return cfg;

    std::vector<char> leader(n + 1, 0);
    leader[0] = 1;

    for (int i = 0; i < n; ++i)
    {
        Flow flow = flowOf(insns[i], fastcallEdges);
        if (flow == Flow::Next)
            continue;

        if (i + 1 <= n)
            leader[i + 1] = 1;

        if (flow != Flow::None)
        {
            int idx = insnIndexAtPc(insns, uint32_t(insns[i].jumpTarget));
            if (idx < 0)
                throw BytecodeError("jump into the middle of an instruction at pc " + std::to_string(insns[i].pc));
            leader[idx] = 1;
        }
    }

    cfg.blockOfInsn.assign(n, -1);

    for (int i = 0; i < n;)
    {
        Block b;
        b.id = int(cfg.blocks.size());
        b.begin = i;
        int j = i;
        while (true)
        {
            cfg.blockOfInsn[j] = b.id;
            ++j;
            if (j >= n || leader[j])
                break;
        }
        b.end = j;
        cfg.blocks.push_back(b);
        i = j;
    }

    for (Block& b : cfg.blocks)
    {
        const Insn& last = insns[b.end - 1];
        Flow flow = flowOf(last, fastcallEdges);

        auto addEdge = [&](int to, Edge::Kind kind) {
            if (to < 0 || to >= int(cfg.blocks.size()))
                return;
            b.succs.push_back(Edge{to, kind});
        };

        if (flow == Flow::Next || flow == Flow::NextAndTarget)
        {
            if (b.end < n)
                addEdge(cfg.blockOfInsn[b.end], Edge::Fallthrough);
        }

        if (flow == Flow::Target || flow == Flow::NextAndTarget)
        {
            int idx = insnIndexAtPc(insns, uint32_t(last.jumpTarget));
            if (idx >= 0 && idx < n)
                addEdge(cfg.blockOfInsn[idx], Edge::Branch);
        }
    }

    for (const Block& b : cfg.blocks)
        for (const Edge& e : b.succs)
            cfg.blocks[e.to].preds.push_back(b.id);

    return cfg;
}

std::string dumpCFG(const Module& m, const Proto& p, const std::vector<Insn>& insns, const CFG& cfg)
{
    std::string out;
    std::vector<int> labels = computeLabels(insns);
    char buf[256];

    for (const Block& b : cfg.blocks)
    {
        snprintf(buf, sizeof(buf), "block %d: pc %u..%u", b.id, insns[b.begin].pc, insns[b.end - 1].pc);
        out += buf;

        out += "  preds:";
        for (int pred : b.preds)
        {
            snprintf(buf, sizeof(buf), " %d", pred);
            out += buf;
        }

        out += "  succs:";
        for (const Edge& e : b.succs)
        {
            snprintf(buf, sizeof(buf), " %d(%s)", e.to, e.kind == Edge::Fallthrough ? "fall" : "jump");
            out += buf;
        }
        out += "\n";

        for (int i = b.begin; i < b.end; ++i)
        {
            const Insn& insn = insns[i];
            int targetLabel = -1;
            if (insn.jumpTarget >= 0)
            {
                int idx = insnIndexAtPc(insns, uint32_t(insn.jumpTarget));
                if (idx >= 0)
                    targetLabel = labels[idx];
            }
            snprintf(buf, sizeof(buf), "    %4u  ", insn.pc);
            out += buf;
            out += formatInsn(m, p, insn, targetLabel);
            out += "\n";
        }
    }

    return out;
}

} // namespace luaudec
