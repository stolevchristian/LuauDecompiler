-- Roblox-style script: exercises value-derived variable names. It cannot run under plain `luau`
-- (no `game`), so it is only used for decompile and recompile checks.
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")

local SOUND_EVENT_FOLDER_NAME = "DefaultSoundEvents"
local DEFAULT_SERVER_SOUND_EVENT_NAME = "DefaultServerSoundEvent"

local folder = ReplicatedStorage:FindFirstChild(SOUND_EVENT_FOLDER_NAME)
local useDispatcher = UserSettings():IsUserFeatureEnabled("UserUseSoundDispatcher")
local sound = Instance.new("Sound")
local remote = Instance.new("RemoteEvent", sound)
local config = require(script.Parent.Config)
local camera = workspace.CurrentCamera
local offset = Vector3.new(0, 5, 0)
local color = Color3.fromRGB(255, 0, 0)

-- reassigned later, so it must keep a neutral name
local current = ReplicatedStorage:WaitForChild("First")
if useDispatcher then
    current = ReplicatedStorage:WaitForChild("Second")
end

-- assigned from inside a closure, also neutral
local last = Instance.new("Part")
local function replace(part)
    last = part
end

sound.Name = DEFAULT_SERVER_SOUND_EVENT_NAME
sound.Parent = folder
remote.Name = "Fire"
replace(sound)
print(Players.LocalPlayer, RunService:IsServer(), config.volume, camera.CFrame, offset + Vector3.new(1, 1, 1), color, current, last)
