// Expression and statement reconstruction from bytecode.
#pragma once

#include "bytecode/Bytecode.h"
#include "lift/AST.h"

#include <string>

namespace luaudec
{

struct LiftOptions
{
    // annotate the output with pc comments for instructions that could not be lifted
    bool verbose = false;
};

// Lift the whole module; returns the body of the main proto with nested closures inlined.
FunctionP liftModule(const Module& m, const LiftOptions& options);

// Convenience: lift and print Luau source.
std::string decompile(const Module& m, const LiftOptions& options);

} // namespace luaudec
