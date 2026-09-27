// Instruction decoding and text disassembly.
// The text output matches BytecodeBuilder::dumpCurrentFunction so it can be
// diffed against `luau-compile --text` (minus source and REMARK lines).
#pragma once

#include "bytecode/Bytecode.h"

#include "Luau/Bytecode.h"

#include <string>
#include <vector>

namespace luaudec
{

struct Capture
{
    uint8_t type = 0;  // LuauCaptureType
    uint8_t index = 0; // register (VAL/REF) or upvalue index (UPVAL)
};

struct Insn
{
    uint32_t pc = 0;
    LuauOpcode op = LOP_NOP;
    uint32_t word = 0;
    uint32_t aux = 0;
    bool hasAux = false;
    int length = 1;

    // decoded fields for every encoding; only the ones the opcode uses are meaningful
    int a = 0;
    int b = 0;
    int c = 0;
    int d = 0;
    int e = 0;

    // absolute pc of the jump target, or -1 (includes FASTCALL skip targets and LOADB skips)
    int jumpTarget = -1;

    // NEWCLOSURE/DUPCLOSURE: the CAPTURE instructions that follow, in upvalue order
    std::vector<Capture> captures;
    // CAPTURE: true when it has been attached to a preceding closure instruction
    bool attachedCapture = false;
};

// Roblox ships bytecode whose opcode byte is multiplied by 227; decoding multiplies it back by
// the modular inverse (203). 1 leaves standard bytecode untouched.
void setOpcodeMultiplier(int multiplier);
int getOpcodeMultiplier();

// Decode all instructions of a proto. Multi-word instructions produce one Insn.
std::vector<Insn> decodeProto(const Proto& proto);

// Index of the Insn that starts at the given pc, or -1 if the pc is inside an instruction.
int insnIndexAtPc(const std::vector<Insn>& insns, uint32_t pc);

const char* opcodeName(LuauOpcode op);

// Import path pieces for an import id (1 to 3 names).
std::vector<std::string> decodeImportPath(const Module& m, const Proto& p, uint32_t importId);

// Constant formatted like BytecodeBuilder::dumpConstant with detailed=false.
std::string formatConstant(const Module& m, const Proto& p, int k);

// One instruction formatted like BytecodeBuilder::dumpInstruction (without trailing newline).
std::string formatInsn(const Module& m, const Proto& p, const Insn& insn, int targetLabel);

// Per-instruction label ids (sequential for jump targets, -1 otherwise), like the dump.
std::vector<int> computeLabels(const std::vector<Insn>& insns);

std::string disassembleProto(const Module& m, const Proto& p);
std::string disassemble(const Module& m);

} // namespace luaudec
