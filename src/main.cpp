// luaudec: Luau bytecode disassembler and decompiler.
#include "bytecode/Bytecode.h"
#include "cfg/CFG.h"
#include "disasm/Disasm.h"
#include "lift/Lift.h"

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
    fprintf(stderr, "  --info        print container and proto summary\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -v            annotate decompiled output with diagnostics\n");
}

int main(int argc, char** argv)
{
    enum Mode
    {
        Decompile,
        Disasm,
        Cfg,
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
