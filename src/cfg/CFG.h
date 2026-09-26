// Basic block construction and control flow graph.
#pragma once

#include "disasm/Disasm.h"

#include <string>
#include <vector>

namespace luaudec
{

struct Edge
{
    enum Kind
    {
        Fallthrough,
        Branch, // taken jump, including loop back edges and loop exits
    };

    int to = -1;
    Kind kind = Fallthrough;
};

struct Block
{
    int id = 0;
    // range of instruction indices (into the decoded Insn vector), end is exclusive
    int begin = 0;
    int end = 0;
    std::vector<Edge> succs;
    std::vector<int> preds;
};

struct CFG
{
    std::vector<Block> blocks;
    // block id for every instruction index
    std::vector<int> blockOfInsn;

    const Block& blockOf(int insnIndex) const
    {
        return blocks[blockOfInsn[insnIndex]];
    }
};

// Does this instruction end a basic block? (jumps, returns, loop instructions and skips)
bool isBlockTerminator(const Insn& insn, bool fastcallEdges);

// Build the CFG for a decoded proto. When fastcallEdges is false the FASTCALL skip
// edges are ignored, which is what the lifter wants since it decompiles the
// fallback CALL instead of the builtin.
CFG buildCFG(const std::vector<Insn>& insns, bool fastcallEdges);

std::string dumpCFG(const Module& m, const Proto& p, const std::vector<Insn>& insns, const CFG& cfg);

} // namespace luaudec
