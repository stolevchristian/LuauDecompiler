// Luau bytecode container parser.
// Mirrors the serialization format written by Luau's BytecodeBuilder::finalize
// and read by luau_load in VM/src/lvmload.cpp.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace luaudec
{

struct Constant
{
    enum Kind
    {
        Nil,
        Boolean,
        Number,
        String,
        Import,
        Table,
        Closure,
        Vector,
        Integer,
        ClassShape,
    };

    Kind kind = Nil;
    bool boolean = false;
    double number = 0.0;
    int64_t integer = 0;
    double vector[4] = {0.0, 0.0, 0.0, 0.0};
    bool vectorIsDouble = false;
    // 1-based index into Module::strings, 0 means no string
    uint32_t stringIndex = 0;
    // packed import id: top 2 bits = path length, then three 10-bit constant indices
    uint32_t importId = 0;
    // table template: constant indices of the keys, optional constant indices of the values (-1 = none)
    std::vector<uint32_t> tableKeys;
    std::vector<int32_t> tableValues;
    bool tableHasValues = false;
    // closure: proto index
    uint32_t closureProto = 0;
    // class shape (experimental bytecode)
    uint32_t className = 0;
    std::vector<uint32_t> classProperties;
    std::vector<uint32_t> classMethods;
};

struct DebugLocal
{
    std::string name;
    uint32_t startpc = 0;
    uint32_t endpc = 0;
    uint8_t reg = 0;
};

struct TypedLocal
{
    uint8_t type = 0;
    uint8_t reg = 0;
    uint32_t startpc = 0;
    uint32_t endpc = 0;
};

struct Proto
{
    uint32_t index = 0;
    uint8_t maxstacksize = 0;
    uint8_t numparams = 0;
    uint8_t numupvalues = 0;
    bool isvararg = false;
    uint8_t flags = 0;

    // raw type info blob and its decoded form (type encoding versions 2 and 3)
    std::vector<uint8_t> typeinfo;
    std::vector<uint8_t> functionType; // LBC_TYPE_FUNCTION, numparams, param types...
    std::vector<uint8_t> upvalueTypes;
    std::vector<TypedLocal> typedLocals;

    std::vector<uint32_t> code;
    std::vector<Constant> constants;
    std::vector<uint32_t> children; // proto indices

    uint32_t linedefined = 0;
    std::string debugname; // empty when absent

    bool hasLineInfo = false;
    std::vector<int> lines; // absolute line per instruction word

    bool hasDebugInfo = false;
    std::vector<DebugLocal> locals;
    std::vector<std::string> upvalueNames;

    std::vector<uint32_t> feedbackSlots;
    uint64_t cost = 0;
};

struct Module
{
    uint8_t version = 0;
    uint8_t typesVersion = 0;
    std::vector<std::string> strings;
    std::vector<std::pair<uint8_t, std::string>> userdataTypes;
    std::vector<Proto> protos;
    uint32_t mainProto = 0;

    // 1-based accessor matching the on-disk encoding
    const std::string& str(uint32_t index1) const;
};

struct BytecodeError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

Module parseBytecode(const uint8_t* data, size_t size);
Module loadBytecodeFile(const std::string& path);

} // namespace luaudec
