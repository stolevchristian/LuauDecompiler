// Luau source printer for the lifted AST.
#pragma once

#include "lift/AST.h"

#include <string>

namespace luaudec
{

bool isIdentifier(const std::string& s);
std::string quoteString(const std::string& s);
std::string formatNumber(double v);

std::string emitExpr(const ExprP& e);
std::string emitBlock(const std::vector<StmtP>& stmts, int indent);

// Print a chunk: the body of the main function without the surrounding function header.
std::string emitChunk(const FunctionBody& main);

} // namespace luaudec
