--!nonstrict
-- Roblox CoreScript-style sample (decompile and recompile checks only; needs `game` to run).
local Players = game:GetService("Players")
local CoreGuiService = game:GetService("CoreGui")
local RobloxReplicatedStorage = game:GetService("RobloxReplicatedStorage")
local StarterPlayer = game:GetService("StarterPlayer")

local RobloxGui = CoreGuiService:FindFirstChild("RobloxGui")
local CoreGuiModules = RobloxGui:FindFirstChild("Modules")
local CommonModules = CoreGuiModules:FindFirstChild("Common")

local Rigging = require(CommonModules:FindFirstChild("RagdollRigging"))
local HumanoidReadyUtil = require(CommonModules:FindFirstChild("HumanoidReadyUtil"))

local avatarJointUpgrade = game:GetEngineFeature("AvatarJointUpgradeFeature")

local localPlayer = Players.LocalPlayer
if not localPlayer then
	Players:GetPropertyChangedSignal("LocalPlayer"):Wait()
	localPlayer = Players.LocalPlayer
end

local DeathTypeValue = RobloxReplicatedStorage:WaitForChild("DeathType", math.huge)

if not DeathTypeValue or (not avatarJointUpgrade and (DeathTypeValue.Value :: any) ~= "Ragdoll") then
	return
end

local remote
if not avatarJointUpgrade then
	remote = RobloxReplicatedStorage:WaitForChild("OnRagdoll", math.huge)
	if not (remote and remote:IsA("RemoteEvent")) then
		return
	end
end

local function onOwnedHumanoidDeath(character, humanoid)
	if avatarJointUpgrade then
		if DeathTypeValue.Value == "Ragdoll" then
			Rigging.disableMotors(character, humanoid.RigType)
		end
	else
		local motors = Rigging.disableMotors(character, humanoid.RigType)
		local animator = humanoid:FindFirstChildWhichIsA("Animator")
		if animator then
			animator:ApplyJointVelocities(motors)
		end
		remote:FireServer(humanoid)
		wait(0.1)
		Rigging.easeJointFriction_OLD(character, 0.85)
	end
end

HumanoidReadyUtil.registerHumanoidReady(function(player, character, humanoid)
	local characterRemovingConn
	local diedConn
	local function disconnect()
		characterRemovingConn:Disconnect()
		diedConn:Disconnect()
	end

	diedConn = humanoid.Died:Connect(function()
		disconnect()
		if DeathTypeValue.Value == "Ragdoll" then
			delay(2.0, function()
				Rigging.disableParticleEmittersAndFadeOut(character, 0.4)
			end)
		end
		if player == localPlayer then
			onOwnedHumanoidDeath(character, humanoid)
		end
	end)

	characterRemovingConn = player.CharacterRemoving:Connect(function(removedCharacter)
		if removedCharacter == character then
			disconnect()
		end
	end)
end)
