// luaudec: Luau bytecode disassembler and decompiler.
#include "bytecode/Bytecode.h"
#include "cfg/CFG.h"
#include "disasm/Disasm.h"
#include "lift/Lift.h"

#include "Luau/BytecodeUtils.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace luaudec;

static void usage(const char* argv0)
{
    fprintf(stderr, "Usage: %s [mode] [options] file.luauc\n", argv0);
    fprintf(stderr, "\n");
    fprintf(stderr, "Modes:\n");
    fprintf(stderr, "  --decompile   print reconstructed Luau source (default)\n");
    fprintf(stderr, "  --disasm      print a text disassembly in luau-compile --text format\n");
    fprintf(stderr, "  --cfg         print basic blocks and control flow edges per function\n");
    fprintf(stderr, "  --json        print instructions, blocks, edges and constants per function as JSON\n");
    fprintf(stderr, "  --info        print container and proto summary\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -v            annotate decompiled output with diagnostics\n");
}

// ----- JSON export (consumed by the web UI) -----

static std::string jsonString(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char ch : s)
    {
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
        default:
            if (ch < 0x20 || ch == 0x7f)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            }
            else
            {
                out += char(ch);
            }
        }
    }
    return out + "\"";
}

static const char* constantKindName(Constant::Kind kind)
{
    switch (kind)
    {
    case Constant::Nil:
        return "nil";
    case Constant::Boolean:
        return "boolean";
    case Constant::Number:
        return "number";
    case Constant::String:
        return "string";
    case Constant::Import:
        return "import";
    case Constant::Table:
        return "table";
    case Constant::Closure:
        return "closure";
    case Constant::Vector:
        return "vector";
    case Constant::Integer:
        return "integer";
    case Constant::ClassShape:
        return "class";
    }
    return "?";
}

static std::string jsonModule(const Module& m)
{
    std::string out;
    char buf[256];

    snprintf(buf, sizeof(buf), "{\"version\":%d,\"typesVersion\":%d,\"mainProto\":%u,\"stringCount\":%zu,\"protos\":[", m.version, m.typesVersion,
        m.mainProto, m.strings.size());
    out += buf;

    for (size_t pi = 0; pi < m.protos.size(); ++pi)
    {
        const Proto& p = m.protos[pi];
        std::vector<Insn> insns = decodeProto(p);
        std::vector<int> labels = computeLabels(insns);
        CFG cfg = buildCFG(insns, true);

        if (pi)
            out += ",";
        snprintf(buf, sizeof(buf), "{\"index\":%u,\"name\":%s,\"line\":%u,\"params\":%d,\"upvalues\":%d,\"vararg\":%s,\"maxstack\":%d,\"codeSize\":%zu,",
            p.index, "%s", p.linedefined, p.numparams, p.numupvalues, p.isvararg ? "true" : "false", p.maxstacksize, p.code.size());
        {
            std::string head = buf;
            size_t pos = head.find("%s");
            head.replace(pos, 2, jsonString(p.debugname));
            out += head;
        }

        out += "\"children\":[";
        for (size_t i = 0; i < p.children.size(); ++i)
        {
            snprintf(buf, sizeof(buf), "%s%u", i ? "," : "", p.children[i]);
            out += buf;
        }
        out += "],";

        out += "\"constants\":[";
        for (size_t i = 0; i < p.constants.size(); ++i)
        {
            snprintf(buf, sizeof(buf), "%s{\"index\":%zu,\"kind\":\"%s\",\"text\":", i ? "," : "", i, constantKindName(p.constants[i].kind));
            out += buf;
            std::string text;
            try
            {
                text = formatConstant(m, p, int(i));
            }
            catch (const std::exception& e)
            {
                text = std::string("<") + e.what() + ">";
            }
            if (p.constants[i].kind == Constant::Closure)
            {
                snprintf(buf, sizeof(buf), "\"function %u", p.constants[i].closureProto);
                text = std::string(buf + 1) + (text.empty() ? "" : " " + text);
            }
            out += jsonString(text) + "}";
        }
        out += "],";

        out += "\"locals\":[";
        for (size_t i = 0; i < p.locals.size(); ++i)
        {
            const DebugLocal& l = p.locals[i];
            snprintf(buf, sizeof(buf), "%s{\"name\":%%s,\"reg\":%d,\"startpc\":%u,\"endpc\":%u}", i ? "," : "", l.reg, l.startpc, l.endpc);
            std::string item = buf;
            item.replace(item.find("%s"), 2, jsonString(l.name));
            out += item;
        }
        out += "],";

        out += "\"upvalueNames\":[";
        for (size_t i = 0; i < p.upvalueNames.size(); ++i)
            out += (i ? "," : "") + jsonString(p.upvalueNames[i]);
        out += "],";

        out += "\"instructions\":[";
        for (size_t i = 0; i < insns.size(); ++i)
        {
            const Insn& in = insns[i];
            int targetLabel = -1;
            if (in.jumpTarget >= 0)
            {
                int idx = insnIndexAtPc(insns, uint32_t(in.jumpTarget));
                if (idx >= 0)
                    targetLabel = labels[idx];
            }
            std::string text = formatInsn(m, p, in, targetLabel);
            int line = in.pc < p.lines.size() ? p.lines[in.pc] : 0;
            snprintf(buf, sizeof(buf), "%s{\"pc\":%u,\"op\":\"%s\",\"len\":%d,\"label\":%d,\"target\":%d,\"line\":%d,\"text\":", i ? "," : "", in.pc,
                opcodeName(in.op), in.length, labels[i], in.jumpTarget, line);
            out += buf;
            out += jsonString(text) + "}";
        }
        out += "],";

        out += "\"blocks\":[";
        for (size_t b = 0; b < cfg.blocks.size(); ++b)
        {
            const Block& blk = cfg.blocks[b];
            snprintf(buf, sizeof(buf), "%s{\"id\":%d,\"startPc\":%u,\"endPc\":%u,\"succs\":[", b ? "," : "", blk.id, insns[blk.begin].pc, insns[blk.end - 1].pc);
            out += buf;
            for (size_t e = 0; e < blk.succs.size(); ++e)
            {
                const Edge& edge = blk.succs[e];
                const Insn& last = insns[blk.end - 1];
                const char* kind = edge.kind == Edge::Fallthrough ? "fall" : (Luau::isFastCall(last.op) ? "fastcall" : "jump");
                bool back = cfg.blocks[edge.to].begin <= blk.begin;
                snprintf(buf, sizeof(buf), "%s{\"to\":%d,\"kind\":\"%s\",\"back\":%s}", e ? "," : "", edge.to, kind, back ? "true" : "false");
                out += buf;
            }
            out += "]}";
        }
        out += "]}";
    }

    out += "]}\n";
    return out;
}

int main(int argc, char** argv)
{
    enum Mode
    {
        Decompile,
        Disasm,
        Cfg,
        Json,
        Info,
    } mode = Decompile;

    LiftOptions options;
    std::string path;

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];
        if (strcmp(arg, "--disasm") == 0)
            mode = Disasm;
        else if (strcmp(arg, "--cfg") == 0)
            mode = Cfg;
        else if (strcmp(arg, "--json") == 0)
            mode = Json;
        else if (strcmp(arg, "--info") == 0)
            mode = Info;
        else if (strcmp(arg, "--decompile") == 0)
            mode = Decompile;
        else if (strcmp(arg, "-v") == 0)
            options.verbose = true;
        else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
        {
            usage(argv[0]);
            return 0;
        }
        else if (arg[0] == '-' && arg[1] != 0)
        {
            fprintf(stderr, "unknown option %s\n", arg);
            usage(argv[0]);
            return 1;
        }
        else if (path.empty())
            path = arg;
        else
        {
            fprintf(stderr, "only one input file is supported\n");
            return 1;
        }
    }

    if (path.empty())
    {
        usage(argv[0]);
        return 1;
    }

#ifdef _WIN32
    // keep line endings as plain LF so the output can be diffed against luau-compile
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    try
    {
        Module m = loadBytecodeFile(path);

        switch (mode)
        {
        case Disasm:
            fputs(disassemble(m).c_str(), stdout);
            break;

        case Cfg:
            for (const Proto& p : m.protos)
            {
                printf("Function %d (%s):\n", int(p.index), p.debugname.empty() ? "??" : p.debugname.c_str());
                std::vector<Insn> insns = decodeProto(p);
                CFG cfg = buildCFG(insns, true);
                fputs(dumpCFG(m, p, insns, cfg).c_str(), stdout);
                printf("\n");
            }
            break;

        case Json:
            fputs(jsonModule(m).c_str(), stdout);
            break;

        case Info:
            printf("bytecode version %d, types version %d, %zu strings, %zu protos, main proto %u\n", m.version, m.typesVersion, m.strings.size(),
                m.protos.size(), m.mainProto);
            for (const Proto& p : m.protos)
                printf("  proto %u (%s): line %u, %zu insns, %zu constants, %zu children, %d params, %d upvalues%s%s%s\n", p.index,
                    p.debugname.empty() ? "??" : p.debugname.c_str(), p.linedefined, p.code.size(), p.constants.size(), p.children.size(),
                    p.numparams, p.numupvalues, p.isvararg ? ", vararg" : "", p.hasLineInfo ? ", lines" : "", p.hasDebugInfo ? ", debug locals" : "");
            break;

        case Decompile:
            fputs(decompile(m, options).c_str(), stdout);
            break;
        }
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    return 0;
}
