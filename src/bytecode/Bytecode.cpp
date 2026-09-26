#include "bytecode/Bytecode.h"

#include "Luau/Bytecode.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace luaudec
{

namespace
{

struct Reader
{
    const uint8_t* data;
    size_t size;
    size_t offset = 0;

    void need(size_t n) const
    {
        if (offset + n > size)
            throw BytecodeError("unexpected end of bytecode at offset " + std::to_string(offset));
    }

    uint8_t byte()
    {
        need(1);
        return data[offset++];
    }

    template<typename T>
    T raw()
    {
        need(sizeof(T));
        T result;
        memcpy(&result, data + offset, sizeof(T));
        offset += sizeof(T);
        return result;
    }

    uint32_t varint()
    {
        uint32_t result = 0;
        unsigned shift = 0;
        uint8_t b;
        do
        {
            b = byte();
            result |= uint32_t(b & 127) << shift;
            shift += 7;
        } while (b & 128);
        return result;
    }

    uint64_t varint64()
    {
        uint64_t result = 0;
        unsigned shift = 0;
        uint8_t b;
        do
        {
            b = byte();
            result |= uint64_t(b & 127) << shift;
            shift += 7;
        } while (b & 128);
        return result;
    }

    std::string bytes(size_t n)
    {
        need(n);
        std::string s(reinterpret_cast<const char*>(data + offset), n);
        offset += n;
        return s;
    }
};

void parseTypeInfo(Proto& p, uint8_t typesVersion)
{
    if (p.typeinfo.empty())
        return;

    Reader r{p.typeinfo.data(), p.typeinfo.size()};

    if (typesVersion == 1)
    {
        // version 1 only stores the function signature
        p.functionType.assign(p.typeinfo.begin(), p.typeinfo.end());
        return;
    }

    uint32_t typeSize = r.varint();
    uint32_t upvalCount = r.varint();
    uint32_t localCount = r.varint();

    r.need(typeSize);
    p.functionType.assign(p.typeinfo.begin() + r.offset, p.typeinfo.begin() + r.offset + typeSize);
    r.offset += typeSize;

    for (uint32_t i = 0; i < upvalCount; ++i)
        p.upvalueTypes.push_back(r.byte());

    for (uint32_t i = 0; i < localCount; ++i)
    {
        TypedLocal l;
        l.type = r.byte();
        l.reg = r.byte();
        l.startpc = r.varint();
        l.endpc = l.startpc + r.varint();
        p.typedLocals.push_back(l);
    }
}

Proto parseProto(Reader& r, const Module& m, uint32_t index)
{
    Proto p;
    p.index = index;

    p.maxstacksize = r.byte();
    p.numparams = r.byte();
    p.numupvalues = r.byte();
    p.isvararg = r.byte() != 0;

    if (m.version >= 4)
    {
        p.flags = r.byte();

        uint32_t typesize = r.varint();
        if (typesize)
        {
            r.need(typesize);
            p.typeinfo.assign(r.data + r.offset, r.data + r.offset + typesize);
            r.offset += typesize;
            parseTypeInfo(p, m.typesVersion);
        }
    }

    uint32_t sizecode = r.varint();
    p.code.reserve(sizecode);
    for (uint32_t i = 0; i < sizecode; ++i)
        p.code.push_back(r.raw<uint32_t>());

    uint32_t sizek = r.varint();
    p.constants.reserve(sizek);
    for (uint32_t i = 0; i < sizek; ++i)
    {
        Constant c;
        uint8_t tag = r.byte();
        switch (tag)
        {
        case LBC_CONSTANT_NIL:
            c.kind = Constant::Nil;
            break;
        case LBC_CONSTANT_BOOLEAN:
            c.kind = Constant::Boolean;
            c.boolean = r.byte() != 0;
            break;
        case LBC_CONSTANT_NUMBER:
            c.kind = Constant::Number;
            c.number = r.raw<double>();
            break;
        case LBC_CONSTANT_VECTOR:
            c.kind = Constant::Vector;
            for (int j = 0; j < 4; ++j)
                c.vector[j] = r.raw<float>();
            break;
        case LBC_CONSTANT_VECTORD:
            c.kind = Constant::Vector;
            c.vectorIsDouble = true;
            for (int j = 0; j < 4; ++j)
                c.vector[j] = r.raw<double>();
            break;
        case LBC_CONSTANT_STRING:
            c.kind = Constant::String;
            c.stringIndex = r.varint();
            if (c.stringIndex > m.strings.size())
                throw BytecodeError("string constant index out of range");
            break;
        case LBC_CONSTANT_IMPORT:
            c.kind = Constant::Import;
            c.importId = r.raw<uint32_t>();
            break;
        case LBC_CONSTANT_TABLE:
        {
            c.kind = Constant::Table;
            uint32_t keys = r.varint();
            for (uint32_t j = 0; j < keys; ++j)
                c.tableKeys.push_back(r.varint());
            break;
        }
        case LBC_CONSTANT_TABLE_WITH_CONSTANTS:
        {
            c.kind = Constant::Table;
            c.tableHasValues = true;
            uint32_t keys = r.varint();
            for (uint32_t j = 0; j < keys; ++j)
            {
                c.tableKeys.push_back(r.varint());
                c.tableValues.push_back(r.raw<int32_t>());
            }
            break;
        }
        case LBC_CONSTANT_CLOSURE:
            c.kind = Constant::Closure;
            c.closureProto = r.varint();
            break;
        case LBC_CONSTANT_CLASS_SHAPE:
        {
            c.kind = Constant::ClassShape;
            c.className = r.varint();
            uint32_t numProperties = r.varint();
            uint32_t numMethods = r.varint();
            for (uint32_t j = 0; j < numProperties; ++j)
                c.classProperties.push_back(r.varint());
            for (uint32_t j = 0; j < numMethods; ++j)
                c.classMethods.push_back(r.varint());
            break;
        }
        case LBC_CONSTANT_INTEGER:
        {
            c.kind = Constant::Integer;
            bool negative = r.byte() != 0;
            uint64_t magnitude = r.varint64();
            c.integer = negative ? int64_t(~magnitude + 1) : int64_t(magnitude);
            break;
        }
        default:
            throw BytecodeError("unknown constant type " + std::to_string(tag));
        }
        p.constants.push_back(std::move(c));
    }

    uint32_t sizep = r.varint();
    for (uint32_t i = 0; i < sizep; ++i)
        p.children.push_back(r.varint());

    p.linedefined = r.varint();
    uint32_t nameIndex = r.varint();
    if (nameIndex)
        p.debugname = m.str(nameIndex);

    if (r.byte())
    {
        p.hasLineInfo = true;
        uint8_t linegaplog2 = r.byte();

        uint32_t intervals = sizecode == 0 ? 1 : ((sizecode - 1) >> linegaplog2) + 1;

        std::vector<uint8_t> lineinfo(sizecode);
        uint8_t lastoffset = 0;
        for (uint32_t i = 0; i < sizecode; ++i)
        {
            lastoffset += r.byte();
            lineinfo[i] = lastoffset;
        }

        std::vector<int> abslineinfo(intervals);
        int lastline = 0;
        for (uint32_t i = 0; i < intervals; ++i)
        {
            lastline += r.raw<int32_t>();
            abslineinfo[i] = lastline;
        }

        p.lines.resize(sizecode);
        for (uint32_t i = 0; i < sizecode; ++i)
            p.lines[i] = abslineinfo[i >> linegaplog2] + lineinfo[i];
    }

    if (r.byte())
    {
        p.hasDebugInfo = true;

        uint32_t sizelocvars = r.varint();
        for (uint32_t i = 0; i < sizelocvars; ++i)
        {
            DebugLocal l;
            l.name = m.str(r.varint());
            l.startpc = r.varint();
            l.endpc = r.varint();
            l.reg = r.byte();
            p.locals.push_back(std::move(l));
        }

        uint32_t sizeupvalues = r.varint();
        for (uint32_t i = 0; i < sizeupvalues; ++i)
            p.upvalueNames.push_back(m.str(r.varint()));
    }

    if (m.version >= 11)
    {
        uint32_t feedbackvecsize = r.varint();
        for (uint32_t i = 0; i < feedbackvecsize; ++i)
        {
            uint8_t slottype = r.byte();
            if (slottype != LFT_CALLTARGET)
                throw BytecodeError("unknown feedback slot type " + std::to_string(slottype));
            p.feedbackSlots.push_back(r.varint());
        }
    }

    if (m.version >= 12 && (p.flags & LPF_INLINABLE) != 0)
        p.cost = r.varint64();

    return p;
}

} // namespace

const std::string& Module::str(uint32_t index1) const
{
    static const std::string empty;
    if (index1 == 0)
        return empty;
    if (index1 > strings.size())
        throw BytecodeError("string table index out of range");
    return strings[index1 - 1];
}

Module parseBytecode(const uint8_t* data, size_t size)
{
    Reader r{data, size};
    Module m;

    m.version = r.byte();

    if (m.version == 0)
        throw BytecodeError("bytecode contains a compile error: " + std::string(reinterpret_cast<const char*>(data + 1), size - 1));

    if ((m.version < LBC_VERSION_MIN || m.version > LBC_VERSION_MAX) && m.version != LBC_VERSION_CLASSES)
        throw BytecodeError(
            "unsupported bytecode version " + std::to_string(m.version) + " (expected " + std::to_string(LBC_VERSION_MIN) + ".." +
            std::to_string(LBC_VERSION_MAX) + ")"
        );

    if (m.version >= 4)
    {
        m.typesVersion = r.byte();
        if (m.typesVersion < LBC_TYPE_VERSION_MIN || m.typesVersion > LBC_TYPE_VERSION_MAX)
            throw BytecodeError("unsupported type encoding version " + std::to_string(m.typesVersion));
    }

    uint32_t stringCount = r.varint();
    m.strings.reserve(stringCount);
    for (uint32_t i = 0; i < stringCount; ++i)
    {
        uint32_t length = r.varint();
        m.strings.push_back(r.bytes(length));
    }

    if (m.typesVersion == 3)
    {
        uint8_t index = r.byte();
        while (index != 0)
        {
            uint32_t nameRef = r.varint();
            m.userdataTypes.emplace_back(index, m.str(nameRef));
            index = r.byte();
        }
    }

    uint32_t protoCount = r.varint();
    m.protos.reserve(protoCount);
    for (uint32_t i = 0; i < protoCount; ++i)
    {
        size_t protoSize = 0;
        if (m.version >= 12)
            protoSize = r.varint();
        size_t protoStart = r.offset;

        m.protos.push_back(parseProto(r, m, i));

        if (m.version >= 12)
        {
            if (r.offset > protoStart + protoSize)
                throw BytecodeError("proto " + std::to_string(i) + " overruns its declared size");
            // skip unknown trailing data, matching the loader
            r.offset = protoStart + protoSize;
        }
    }

    m.mainProto = r.varint();
    if (m.mainProto >= m.protos.size())
        throw BytecodeError("main proto index out of range");

    for (const Proto& p : m.protos)
        for (uint32_t child : p.children)
            if (child >= m.protos.size())
                throw BytecodeError("child proto index out of range");

    return m;
}

Module loadBytecodeFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw BytecodeError("cannot open " + path);

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty())
        throw BytecodeError(path + " is empty");

    return parseBytecode(data.data(), data.size());
}

} // namespace luaudec
