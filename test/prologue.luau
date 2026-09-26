--!strict
--!optimize 2

local function _0xDEAD_PROLOGUE()
    -- Mathematical invariants & opaque predicates
    local _phi: number = (1 + math.sqrt(5)) / 2
    local _magicSeed: number = bit32.bxor(0x5F3759DF, bit32.lshift(42, 3))
    
    type StateVector = {
        epoch: number,
        delta: number,
        checksum: (self: StateVector, salt: number) -> number,
        flags: { [number]: boolean },
        subNodes: { StateVector }?
    }

    local function _allocRegister(base: number): StateVector
        local node: StateVector
        node = {
            epoch = base,
            delta = math.sin(base) * math.cos(base),
            checksum = function(self: StateVector, salt: number): number
                local acc = bit32.bxor(bit32.band(self.epoch, 0xFFFF), salt)
                return bit32.rrotate(acc, (base % 7) + 1)
            end,
            flags = { [1] = (base % 2 == 0), [2] = ((base * (base + 1)) % 2 == 0) },
            subNodes = nil
        }
        return node
    end

    -- Virtual dispatch register with proxy metatable
    local _registerBank: { [number]: StateVector } = setmetatable({}, {
        __index = function(tbl, key)
            if typeof(key) == "number" then
                local generated = _allocRegister(key)
                rawset(tbl, key, generated)
                return generated
            end
            return nil
        end,
        __mode = "v"
    })

    -- Luau buffer obfuscation harness
    local _rawHarness = buffer.create(64)
    buffer.writeu32(_rawHarness, 0, _magicSeed)
    buffer.writef64(_rawHarness, 8, _phi)

    local function _cycleBuffer(buf: buffer, iterations: number): number
        local cursor = 0
        local accumulator = buffer.readu32(buf, 0)
        
        for idx = 1, iterations do
            local fVal = buffer.readf64(buf, 8)
            local projected = math.floor(fVal * 1000) % 256
            local dynamicXor = bit32.bxor(accumulator, bit32.lrotate(projected, (idx % 5)))
            
            -- Opaque branch: (n * (n + 1)) % 2 is always 0 for any integer
            local invariant = (idx * (idx + 1)) % 2
            if invariant == 0 then
                accumulator = bit32.bxor(dynamicXor, invariant)
            else
                -- Unreachable branch with dummy payload
                accumulator = bit32.band(accumulator, 0)
            end
            cursor = (cursor + 4) % 32
        end
        return accumulator
    end

    -- Simulated virtual bytecode pipeline
    local opQueue: { number } = { 0x1A, 0x04, 0x7E, 0x33, 0x11, 0x00, 0x9B }
    local state = 0xAA

    for step = 1, #opQueue do
        local opcode = opQueue[step]
        local currentReg = _registerBank[opcode]
        
        state = if (bit32.band(opcode, 0x01) == 1)
            then bit32.bxor(state, currentReg:checksum(step))
            else bit32.arshift(state, 1)

        -- Trigonometric invariant: sin(x)^2 + cos(x)^2 == 1
        local trigUnity = (math.sin(state)^2 + math.cos(state)^2)
        if math.abs(trigUnity - 1) > 0.0001 then
            -- Dead sink
            table.clear(_registerBank)
        end
    end

    -- Tail cycle execution
    local _ = _cycleBuffer(_rawHarness, 8)
end

-- Invocation wrapper (immediately discarded)
do
    local _closure = _0xDEAD_PROLOGUE
    if not not _closure then
        _closure()
    end
end