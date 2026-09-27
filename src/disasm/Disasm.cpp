#include "disasm/Disasm.h"

#include "Luau/BytecodeUtils.h"

#include <cstdarg>
#include <cstdio>

namespace luaudec
{

namespace
{

void appendf(std::string& out, const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    out += buf;
}

bool printableString(const std::string& s)
{
    for (unsigned char ch : s)
        if (ch < ' ')
            return false;
    return true;
}

const char* baseTypeName(uint8_t type)
{
    switch (type & ~LBC_TYPE_OPTIONAL_BIT)
    {
    case LBC_TYPE_NIL:
        return "nil";
    case LBC_TYPE_BOOLEAN:
        return "boolean";
    case LBC_TYPE_NUMBER:
        return "number";
    case LBC_TYPE_INTEGER:
        return "integer";
    case LBC_TYPE_STRING:
        return "string";
    case LBC_TYPE_TABLE:
        return "table";
    case LBC_TYPE_FUNCTION:
        return "function";
    case LBC_TYPE_THREAD:
        return "thread";
    case LBC_TYPE_USERDATA:
        return "userdata";
    case LBC_TYPE_VECTOR:
        return "vector";
    case LBC_TYPE_BUFFER:
        return "buffer";
    case LBC_TYPE_ANY:
        return "any";
    default:
        return "unknown";
    }
}

std::string typeName(const Module& m, uint8_t type)
{
    uint8_t tag = type & ~LBC_TYPE_OPTIONAL_BIT;
    std::string name;

    if (tag >= LBC_TYPE_TAGGED_USERDATA_BASE && tag < LBC_TYPE_TAGGED_USERDATA_END)
    {
        uint8_t index = uint8_t(tag - LBC_TYPE_TAGGED_USERDATA_BASE + 1);
        for (const auto& ud : m.userdataTypes)
            if (ud.first == index)
                name = ud.second;
        if (name.empty())
            name = "userdata";
    }
    else
    {
        name = baseTypeName(tag);
    }

    if (type & LBC_TYPE_OPTIONAL_BIT)
        name += "?";
    return name;
}

} // namespace

const char* opcodeName(LuauOpcode op)
{
    switch (op)
    {
    case LOP_NOP:
        return "NOP";
    case LOP_BREAK:
        return "BREAK";
    case LOP_LOADNIL:
        return "LOADNIL";
    case LOP_LOADB:
        return "LOADB";
    case LOP_LOADN:
        return "LOADN";
    case LOP_LOADK:
        return "LOADK";
    case LOP_MOVE:
        return "MOVE";
    case LOP_GETGLOBAL:
        return "GETGLOBAL";
    case LOP_SETGLOBAL:
        return "SETGLOBAL";
    case LOP_GETUPVAL:
        return "GETUPVAL";
    case LOP_SETUPVAL:
        return "SETUPVAL";
    case LOP_CLOSEUPVALS:
        return "CLOSEUPVALS";
    case LOP_GETIMPORT:
        return "GETIMPORT";
    case LOP_GETTABLE:
        return "GETTABLE";
    case LOP_SETTABLE:
        return "SETTABLE";
    case LOP_GETTABLEKS:
        return "GETTABLEKS";
    case LOP_SETTABLEKS:
        return "SETTABLEKS";
    case LOP_GETTABLEN:
        return "GETTABLEN";
    case LOP_SETTABLEN:
        return "SETTABLEN";
    case LOP_NEWCLOSURE:
        return "NEWCLOSURE";
    case LOP_NAMECALL:
        return "NAMECALL";
    case LOP_CALL:
        return "CALL";
    case LOP_RETURN:
        return "RETURN";
    case LOP_JUMP:
        return "JUMP";
    case LOP_JUMPBACK:
        return "JUMPBACK";
    case LOP_JUMPIF:
        return "JUMPIF";
    case LOP_JUMPIFNOT:
        return "JUMPIFNOT";
    case LOP_JUMPIFEQ:
        return "JUMPIFEQ";
    case LOP_JUMPIFLE:
        return "JUMPIFLE";
    case LOP_JUMPIFLT:
        return "JUMPIFLT";
    case LOP_JUMPIFNOTEQ:
        return "JUMPIFNOTEQ";
    case LOP_JUMPIFNOTLE:
        return "JUMPIFNOTLE";
    case LOP_JUMPIFNOTLT:
        return "JUMPIFNOTLT";
    case LOP_ADD:
        return "ADD";
    case LOP_SUB:
        return "SUB";
    case LOP_MUL:
        return "MUL";
    case LOP_DIV:
        return "DIV";
    case LOP_MOD:
        return "MOD";
    case LOP_POW:
        return "POW";
    case LOP_ADDK:
        return "ADDK";
    case LOP_SUBK:
        return "SUBK";
    case LOP_MULK:
        return "MULK";
    case LOP_DIVK:
        return "DIVK";
    case LOP_MODK:
        return "MODK";
    case LOP_POWK:
        return "POWK";
    case LOP_AND:
        return "AND";
    case LOP_OR:
        return "OR";
    case LOP_ANDK:
        return "ANDK";
    case LOP_ORK:
        return "ORK";
    case LOP_CONCAT:
        return "CONCAT";
    case LOP_NOT:
        return "NOT";
    case LOP_MINUS:
        return "MINUS";
    case LOP_LENGTH:
        return "LENGTH";
    case LOP_NEWTABLE:
        return "NEWTABLE";
    case LOP_DUPTABLE:
        return "DUPTABLE";
    case LOP_SETLIST:
        return "SETLIST";
    case LOP_FORNPREP:
        return "FORNPREP";
    case LOP_FORNLOOP:
        return "FORNLOOP";
    case LOP_FORGLOOP:
        return "FORGLOOP";
    case LOP_FORGPREP_INEXT:
        return "FORGPREP_INEXT";
    case LOP_FASTCALL3:
        return "FASTCALL3";
    case LOP_FORGPREP_NEXT:
        return "FORGPREP_NEXT";
    case LOP_NATIVECALL:
        return "NATIVECALL";
    case LOP_GETVARARGS:
        return "GETVARARGS";
    case LOP_DUPCLOSURE:
        return "DUPCLOSURE";
    case LOP_PREPVARARGS:
        return "PREPVARARGS";
    case LOP_LOADKX:
        return "LOADKX";
    case LOP_JUMPX:
        return "JUMPX";
    case LOP_FASTCALL:
        return "FASTCALL";
    case LOP_COVERAGE:
        return "COVERAGE";
    case LOP_CAPTURE:
        return "CAPTURE";
    case LOP_SUBRK:
        return "SUBRK";
    case LOP_DIVRK:
        return "DIVRK";
    case LOP_FASTCALL1:
        return "FASTCALL1";
    case LOP_FASTCALL2:
        return "FASTCALL2";
    case LOP_FASTCALL2K:
        return "FASTCALL2K";
    case LOP_FORGPREP:
        return "FORGPREP";
    case LOP_JUMPXEQKNIL:
        return "JUMPXEQKNIL";
    case LOP_JUMPXEQKB:
        return "JUMPXEQKB";
    case LOP_JUMPXEQKN:
        return "JUMPXEQKN";
    case LOP_JUMPXEQKS:
        return "JUMPXEQKS";
    case LOP_IDIV:
        return "IDIV";
    case LOP_IDIVK:
        return "IDIVK";
    case LOP_GETUDATAKS:
        return "GETUDATAKS";
    case LOP_SETUDATAKS:
        return "SETUDATAKS";
    case LOP_NAMECALLUDATA:
        return "NAMECALLUDATA";
    case LOP_NEWCLASSMEMBER:
        return "NEWCLASSMEMBER";
    case LOP_CALLFB:
        return "CALLFB";
    case LOP_CMPPROTO:
        return "CMPPROTO";
    case LOP_FASTPCALL:
        return "FASTPCALL";
    case LOP_NEWCLASS:
        return "NEWCLASS";
    default:
        return "unknown";
    }
}

static int gOpcodeMultiplier = 1;

void setOpcodeMultiplier(int multiplier)
{
    gOpcodeMultiplier = multiplier;
}

int getOpcodeMultiplier()
{
    return gOpcodeMultiplier;
}

std::vector<Insn> decodeProto(const Proto& proto)
{
    std::vector<Insn> insns;
    const std::vector<uint32_t>& code = proto.code;

    for (size_t i = 0; i < code.size();)
    {
        Insn insn;
        insn.pc = uint32_t(i);
        // normalize the opcode byte so every later consumer sees standard encoding
        uint32_t op = (code[i] & 0xff) * uint32_t(gOpcodeMultiplier) & 0xff;
        insn.word = (code[i] & 0xffffff00u) | op;
        if (op >= LOP__COUNT)
            throw BytecodeError("unknown opcode " + std::to_string(op) + " at pc " + std::to_string(i));
        insn.op = LuauOpcode(op);
        insn.length = Luau::getOpLength(insn.op);
        insn.a = LUAU_INSN_A(insn.word);
        insn.b = LUAU_INSN_B(insn.word);
        insn.c = LUAU_INSN_C(insn.word);
        insn.d = LUAU_INSN_D(insn.word);
        insn.e = LUAU_INSN_E(insn.word);

        if (insn.length == 2)
        {
            if (i + 1 >= code.size())
                throw BytecodeError("instruction at pc " + std::to_string(i) + " is missing its AUX word");
            insn.aux = code[i + 1];
            insn.hasAux = true;
        }

        insn.jumpTarget = Luau::getJumpTarget(insn.word, uint32_t(i));
        if (insn.jumpTarget >= 0 && size_t(insn.jumpTarget) > code.size())
            throw BytecodeError("jump target out of range at pc " + std::to_string(i));

        insns.push_back(insn);
        i += insn.length;
    }

    // pair CAPTURE instructions with the closure that precedes them
    for (size_t i = 0; i < insns.size(); ++i)
    {
        if (insns[i].op != LOP_NEWCLOSURE && insns[i].op != LOP_DUPCLOSURE)
            continue;

        for (size_t j = i + 1; j < insns.size() && insns[j].op == LOP_CAPTURE; ++j)
        {
            insns[j].attachedCapture = true;
            insns[i].captures.push_back(Capture{uint8_t(insns[j].a), uint8_t(insns[j].b)});
        }
    }

    return insns;
}

int insnIndexAtPc(const std::vector<Insn>& insns, uint32_t pc)
{
    // instructions are sorted by pc, binary search
    size_t lo = 0, hi = insns.size();
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (insns[mid].pc < pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < insns.size() && insns[lo].pc == pc)
        return int(lo);
    return -1;
}

std::vector<std::string> decodeImportPath(const Module& m, const Proto& p, uint32_t importId)
{
    std::vector<std::string> path;
    int count = int(importId >> 30);
    int ids[3] = {int(importId >> 20) & 1023, int(importId >> 10) & 1023, int(importId) & 1023};

    for (int i = 0; i < count && i < 3; ++i)
    {
        if (size_t(ids[i]) >= p.constants.size() || p.constants[ids[i]].kind != Constant::String)
            throw BytecodeError("import path refers to a non-string constant");
        path.push_back(m.str(p.constants[ids[i]].stringIndex));
    }
    return path;
}

std::string formatConstant(const Module& m, const Proto& p, int k)
{
    if (k < 0 || size_t(k) >= p.constants.size())
        return "<bad constant " + std::to_string(k) + ">";

    const Constant& c = p.constants[k];
    std::string out;

    switch (c.kind)
    {
    case Constant::Nil:
        out = "nil";
        break;
    case Constant::Boolean:
        out = c.boolean ? "true" : "false";
        break;
    case Constant::Number:
        appendf(out, "%.17g", c.number);
        break;
    case Constant::Integer:
        appendf(out, "%lld", (long long)c.integer);
        break;
    case Constant::Vector:
        if (c.vectorIsDouble)
        {
            if (c.vector[3] == 0.0)
                appendf(out, "%.17g, %.17g, %.17g", c.vector[0], c.vector[1], c.vector[2]);
            else
                appendf(out, "%.17g, %.17g, %.17g, %.17g", c.vector[0], c.vector[1], c.vector[2], c.vector[3]);
        }
        else
        {
            if (c.vector[3] == 0.0)
                appendf(out, "%.9g, %.9g, %.9g", float(c.vector[0]), float(c.vector[1]), float(c.vector[2]));
            else
                appendf(out, "%.9g, %.9g, %.9g, %.9g", float(c.vector[0]), float(c.vector[1]), float(c.vector[2]), float(c.vector[3]));
        }
        break;
    case Constant::String:
    {
        const std::string& s = m.str(c.stringIndex);
        if (printableString(s))
        {
            if (s.size() < 32)
                appendf(out, "'%s'", s.c_str());
            else
                appendf(out, "'%.*s'...", 32, s.c_str());
        }
        else
        {
            out += "'";
            for (size_t i = 0; i < s.size() && i < 32; ++i)
            {
                if ((unsigned char)s[i] < ' ')
                    appendf(out, "\\x%02X", (unsigned char)s[i]);
                else
                    out += s[i];
            }
            out += s.size() >= 32 ? "'..." : "'";
        }
        break;
    }
    case Constant::Import:
    {
        std::vector<std::string> path = decodeImportPath(m, p, c.importId);
        for (size_t i = 0; i < path.size(); ++i)
        {
            if (i > 0)
                out += ".";
            out += path[i];
        }
        break;
    }
    case Constant::Table:
        out = "{...}";
        break;
    case Constant::Closure:
    {
        if (c.closureProto < m.protos.size() && !m.protos[c.closureProto].debugname.empty())
            appendf(out, "'%s'", m.protos[c.closureProto].debugname.c_str());
        break;
    }
    case Constant::ClassShape:
    {
        std::string name = c.className < p.constants.size() ? m.str(p.constants[c.className].stringIndex) : "?";
        appendf(out, "class %s (props: %zu, methods: %zu)", name.c_str(), c.classProperties.size(), c.classMethods.size());
        break;
    }
    }

    return out;
}

std::string formatInsn(const Module& m, const Proto& p, const Insn& insn, int label)
{
    std::string out;
    const int a = insn.a, b = insn.b, c = insn.c, d = insn.d;
    const uint32_t aux = insn.aux;

    auto withK = [&](const char* prefix, int k) {
        out += prefix;
        appendf(out, "K%d [", k);
        out += formatConstant(m, p, k);
        out += "]";
    };

    switch (insn.op)
    {
    case LOP_NOP:
        out = "NOP";
        break;
    case LOP_BREAK:
        out = "BREAK";
        break;
    case LOP_LOADNIL:
        appendf(out, "LOADNIL R%d", a);
        break;
    case LOP_LOADB:
        if (c)
            appendf(out, "LOADB R%d %d +%d", a, b, c);
        else
            appendf(out, "LOADB R%d %d", a, b);
        break;
    case LOP_LOADN:
        appendf(out, "LOADN R%d %d", a, d);
        break;
    case LOP_LOADK:
        appendf(out, "LOADK R%d ", a);
        withK("", d);
        break;
    case LOP_MOVE:
        appendf(out, "MOVE R%d R%d", a, b);
        break;
    case LOP_GETGLOBAL:
        appendf(out, "GETGLOBAL R%d ", a);
        withK("", int(aux));
        break;
    case LOP_SETGLOBAL:
        appendf(out, "SETGLOBAL R%d ", a);
        withK("", int(aux));
        break;
    case LOP_GETUPVAL:
        appendf(out, "GETUPVAL R%d %d", a, b);
        break;
    case LOP_SETUPVAL:
        appendf(out, "SETUPVAL R%d %d", a, b);
        break;
    case LOP_CLOSEUPVALS:
        appendf(out, "CLOSEUPVALS R%d", a);
        break;
    case LOP_GETIMPORT:
        appendf(out, "GETIMPORT R%d %d [", a, d);
        out += formatConstant(m, p, d);
        out += "]";
        break;
    case LOP_GETTABLE:
        appendf(out, "GETTABLE R%d R%d R%d", a, b, c);
        break;
    case LOP_SETTABLE:
        appendf(out, "SETTABLE R%d R%d R%d", a, b, c);
        break;
    case LOP_GETTABLEKS:
        appendf(out, "GETTABLEKS R%d R%d ", a, b);
        withK("", int(aux));
        break;
    case LOP_SETTABLEKS:
        appendf(out, "SETTABLEKS R%d R%d ", a, b);
        withK("", int(aux));
        break;
    case LOP_GETTABLEN:
        appendf(out, "GETTABLEN R%d R%d %d", a, b, c + 1);
        break;
    case LOP_SETTABLEN:
        appendf(out, "SETTABLEN R%d R%d %d", a, b, c + 1);
        break;
    case LOP_NEWCLOSURE:
        appendf(out, "NEWCLOSURE R%d P%d", a, d);
        break;
    case LOP_NAMECALL:
        appendf(out, "NAMECALL R%d R%d ", a, b);
        withK("", int(aux));
        break;
    case LOP_CALL:
        appendf(out, "CALL R%d %d %d", a, b - 1, c - 1);
        break;
    case LOP_CALLFB:
        appendf(out, "CALLFB R%d %d %d [%d]", a, b - 1, c - 1, int(aux));
        break;
    case LOP_RETURN:
        appendf(out, "RETURN R%d %d", a, b - 1);
        break;
    case LOP_JUMP:
        appendf(out, "JUMP L%d", label);
        break;
    case LOP_JUMPBACK:
        appendf(out, "JUMPBACK L%d", label);
        break;
    case LOP_JUMPIF:
        appendf(out, "JUMPIF R%d L%d", a, label);
        break;
    case LOP_JUMPIFNOT:
        appendf(out, "JUMPIFNOT R%d L%d", a, label);
        break;
    case LOP_JUMPIFEQ:
    case LOP_JUMPIFLE:
    case LOP_JUMPIFLT:
    case LOP_JUMPIFNOTEQ:
    case LOP_JUMPIFNOTLE:
    case LOP_JUMPIFNOTLT:
        appendf(out, "%s R%d R%d L%d", opcodeName(insn.op), a, int(aux), label);
        break;
    case LOP_ADD:
    case LOP_SUB:
    case LOP_MUL:
    case LOP_DIV:
    case LOP_IDIV:
    case LOP_MOD:
    case LOP_POW:
    case LOP_AND:
    case LOP_OR:
    case LOP_CONCAT:
        appendf(out, "%s R%d R%d R%d", opcodeName(insn.op), a, b, c);
        break;
    case LOP_ADDK:
    case LOP_SUBK:
    case LOP_MULK:
    case LOP_DIVK:
    case LOP_IDIVK:
    case LOP_MODK:
    case LOP_POWK:
    case LOP_ANDK:
    case LOP_ORK:
        appendf(out, "%s R%d R%d ", opcodeName(insn.op), a, b);
        withK("", c);
        break;
    case LOP_SUBRK:
    case LOP_DIVRK:
        appendf(out, "%s R%d ", opcodeName(insn.op), a);
        withK("", b);
        appendf(out, " R%d", c);
        break;
    case LOP_NOT:
    case LOP_MINUS:
    case LOP_LENGTH:
        appendf(out, "%s R%d R%d", opcodeName(insn.op), a, b);
        break;
    case LOP_NEWTABLE:
        appendf(out, "NEWTABLE R%d %d %d", a, b == 0 ? 0 : 1 << (b - 1), int(aux));
        break;
    case LOP_DUPTABLE:
        appendf(out, "DUPTABLE R%d %d", a, d);
        break;
    case LOP_SETLIST:
        appendf(out, "SETLIST R%d R%d %d [%d]", a, b, c - 1, int(aux));
        break;
    case LOP_FORNPREP:
    case LOP_FORNLOOP:
    case LOP_FORGPREP:
    case LOP_FORGPREP_INEXT:
    case LOP_FORGPREP_NEXT:
        appendf(out, "%s R%d L%d", opcodeName(insn.op), a, label);
        break;
    case LOP_FORGLOOP:
        appendf(out, "FORGLOOP R%d L%d %d%s", a, label, uint8_t(aux), int(aux) < 0 ? " [inext]" : "");
        break;
    case LOP_GETVARARGS:
        appendf(out, "GETVARARGS R%d %d", a, b - 1);
        break;
    case LOP_DUPCLOSURE:
        appendf(out, "DUPCLOSURE R%d ", a);
        withK("", d);
        break;
    case LOP_PREPVARARGS:
        appendf(out, "PREPVARARGS %d", a);
        break;
    case LOP_LOADKX:
        appendf(out, "LOADKX R%d ", a);
        withK("", int(aux));
        break;
    case LOP_JUMPX:
        appendf(out, "JUMPX L%d", label);
        break;
    case LOP_FASTCALL:
        appendf(out, "FASTCALL %d L%d", a, label);
        break;
    case LOP_FASTCALL1:
        appendf(out, "FASTCALL1 %d R%d L%d", a, b, label);
        break;
    case LOP_FASTCALL2:
        appendf(out, "FASTCALL2 %d R%d R%d L%d", a, b, int(aux), label);
        break;
    case LOP_FASTCALL2K:
        appendf(out, "FASTCALL2K %d R%d K%d L%d [", a, b, int(aux), label);
        out += formatConstant(m, p, int(aux));
        out += "]";
        break;
    case LOP_FASTCALL3:
        appendf(out, "FASTCALL3 %d R%d R%d R%d L%d", a, b, int(aux & 0xff), int((aux >> 8) & 0xff), label);
        break;
    case LOP_COVERAGE:
        out = "COVERAGE";
        break;
    case LOP_CAPTURE:
        appendf(
            out,
            "CAPTURE %s %c%d",
            a == LCT_UPVAL ? "UPVAL"
            : a == LCT_REF ? "REF"
            : a == LCT_VAL ? "VAL"
                           : "",
            a == LCT_UPVAL ? 'U' : 'R',
            b
        );
        break;
    case LOP_JUMPXEQKNIL:
        appendf(out, "JUMPXEQKNIL R%d L%d%s", a, label, (aux >> 31) ? " NOT" : "");
        break;
    case LOP_JUMPXEQKB:
        appendf(out, "JUMPXEQKB R%d %d L%d%s", a, int(aux & 1), label, (aux >> 31) ? " NOT" : "");
        break;
    case LOP_JUMPXEQKN:
    case LOP_JUMPXEQKS:
        appendf(out, "%s R%d K%d L%d%s [", opcodeName(insn.op), a, int(aux & 0xffffff), label, (aux >> 31) ? " NOT" : "");
        out += formatConstant(m, p, int(aux & 0xffffff));
        out += "]";
        break;
    case LOP_GETUDATAKS:
    case LOP_SETUDATAKS:
    case LOP_NAMECALLUDATA:
        appendf(out, "%s R%d R%d ", opcodeName(insn.op), a, b);
        withK("", int(LUAU_INSN_AUX_KV16(aux)));
        break;
    case LOP_NEWCLASSMEMBER:
        appendf(out, "NEWCLASSMEMBER R%d R%d [", a, c);
        out += formatConstant(m, p, int(aux));
        out += "]";
        break;
    case LOP_CMPPROTO:
        appendf(out, "CMPPROTO R%d #%d L%d", a, int(aux), label);
        break;
    case LOP_FASTPCALL:
        appendf(out, "FASTPCALL %s L%d", a == 0 ? "pcall" : "xpcall", label);
        break;
    case LOP_NEWCLASS:
        if (b == 0xff)
            appendf(out, "NEWCLASS R%d no_base K%d %d [", a, int(aux), c);
        else
            appendf(out, "NEWCLASS R%d R%d K%d %d [", a, b, int(aux), c);
        out += formatConstant(m, p, int(aux));
        out += "]";
        break;
    case LOP_NATIVECALL:
        out = "NATIVECALL";
        break;
    default:
        appendf(out, "UNKNOWN_%d", int(insn.op));
        break;
    }

    return out;
}

std::vector<int> computeLabels(const std::vector<Insn>& insns)
{
    std::vector<int> labels(insns.size(), -1);

    for (const Insn& insn : insns)
    {
        if (insn.jumpTarget < 0)
            continue;
        int idx = insnIndexAtPc(insns, uint32_t(insn.jumpTarget));
        if (idx >= 0)
            labels[idx] = 0;
    }

    int next = 0;
    for (int& l : labels)
        if (l == 0)
            l = next++;

    return labels;
}

std::string disassembleProto(const Module& m, const Proto& p)
{
    std::string out;
    std::vector<Insn> insns = decodeProto(p);
    std::vector<int> labels = computeLabels(insns);

    if (p.hasDebugInfo)
    {
        for (size_t i = 0; i < p.locals.size(); ++i)
        {
            const DebugLocal& l = p.locals[i];
            int startLine = l.startpc < p.lines.size() ? p.lines[l.startpc] : 0;
            appendf(out, "local %d (%s): reg %d, start pc %d line %d, ", int(i), l.name.c_str(), l.reg, l.startpc, startLine);
            if (l.startpc == l.endpc)
            {
                out += "no live range\n";
            }
            else
            {
                int endLine = (l.endpc >= 1 && l.endpc - 1 < p.lines.size()) ? p.lines[l.endpc - 1] : 0;
                appendf(out, "end pc %d line %d\n", l.endpc - 1, endLine);
            }
        }
    }

    for (size_t i = 2; i < p.functionType.size(); ++i)
        appendf(out, "R%d: %s [argument]\n", int(i - 2), typeName(m, p.functionType[i]).c_str());

    for (size_t i = 0; i < p.upvalueTypes.size(); ++i)
        appendf(out, "U%d: %s\n", int(i), typeName(m, p.upvalueTypes[i]).c_str());

    for (const TypedLocal& l : p.typedLocals)
        appendf(out, "R%d: %s from %d to %d\n", l.reg, typeName(m, l.type).c_str(), l.startpc, l.endpc);

    for (size_t i = 0; i < insns.size(); ++i)
    {
        const Insn& insn = insns[i];

        // the reference dump omits the vararg header
        if (insn.op == LOP_PREPVARARGS)
            continue;

        if (labels[i] != -1)
            appendf(out, "L%d: ", labels[i]);

        int targetLabel = -1;
        if (insn.jumpTarget >= 0)
        {
            int idx = insnIndexAtPc(insns, uint32_t(insn.jumpTarget));
            if (idx >= 0)
                targetLabel = labels[idx];
        }

        out += formatInsn(m, p, insn, targetLabel);
        out += "\n";
    }

    return out;
}

std::string disassemble(const Module& m)
{
    std::string out;
    for (const Proto& p : m.protos)
    {
        appendf(out, "Function %d (%s):\n", int(p.index), p.debugname.empty() ? "??" : p.debugname.c_str());
        out += disassembleProto(m, p);
        out += "\n";
    }
    return out;
}

} // namespace luaudec
