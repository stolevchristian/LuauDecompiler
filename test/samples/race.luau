-- Race script sample (decompile and recompile checks only; it needs the host's globals to run).
local race_info = {}

local waypoint_uids = {587, 583, 584, 585, 587}
local reset_ob_uid = 588
local scoreboard_text_ob_uid = 589

local MAX_NUM_BEST_TIMES = 5
local best_times = nil

best_times = objectstorage.getItem("best_times")
if best_times ~= nil then
    print("loaded " .. tostring(#best_times) .. " best times from object storage!")
else
    print("No best times found in object storage, starting with empty best times.")
    best_times = {}
end

print("best_times: ")
print(best_times)
print(tostring(best_times))

print("race_info: ")
print(race_info)
print(tostring(race_info))

function updateScoreboardText()
    local s = "---Best times---\n"
    for i = 1, #best_times do
        s = s .. tostring(i) .. ":  " .. best_times[i].av_name .. ":  " .. string.format("%.3f", best_times[i].time) .. " s\n"
    end
    getObjectForUID(scoreboard_text_ob_uid).content = s
end

function checkAddTimeToBestTimes(av, time_s)
    for i = 1, #best_times do
        if time_s < best_times[i].time then
            for z = math.min(#best_times + 1, MAX_NUM_BEST_TIMES), i + 1, -1 do
                best_times[z] = best_times[z - 1]
            end
            best_times[i] = {av_uid = av.uid, av_name = av.name, time = time_s}
            objectstorage.setItem("best_times", best_times)
            updateScoreboardText()
            return
        end
    end
    if #best_times < MAX_NUM_BEST_TIMES then
        best_times[#best_times + 1] = {av_uid = av.uid, av_name = av.name, time = time_s}
        objectstorage.setItem("best_times", best_times)
        updateScoreboardText()
    end
end

function onUserTouchedObject(av, ob)
    print("User touched object " .. tostring(ob.uid))

    local user_race_info = race_info[av.uid]
    if not user_race_info then
        print("Making new user_race_info...")
        user_race_info = {next_waypoint_index = 1, valid = true}
        race_info[av.uid] = user_race_info
    end

    print("user_race_info.next_waypoint_index: " .. tostring(user_race_info.next_waypoint_index) .. ", valid: " .. tostring(user_race_info.valid))

    if ob.uid == reset_ob_uid then
        if user_race_info.next_waypoint_index == 1 or not user_race_info.valid then
            showMessageToUser("prepare to start race, move through start gate to start!", av)
            user_race_info.next_waypoint_index = 1
            user_race_info.valid = true
        end
        return
    end
    if not user_race_info.valid then
        showMessageToUser("Touch the restart object to restart the race.", av)
        return
    end

    if user_race_info.next_waypoint_index >= 2 and ob.uid == waypoint_uids[user_race_info.next_waypoint_index - 1] then
        showMessageToUser("You need to go through the next waypoint!", av)
    elseif ob.uid == waypoint_uids[user_race_info.next_waypoint_index] then
        if user_race_info.next_waypoint_index == 1 then
            local vehicle = av.vehicle_inside
            if not vehicle then
                showMessageToUser("You must be in a vehicle!", av)
                user_race_info.valid = false
                return
            end
            if vehicle.mass < 199.99 or vehicle.mass > 200.01 then
                showMessageToUser("You must be in a standard bike!", av)
                user_race_info.valid = false
                return
            end
            showMessageToUser("Race Started!", av)
            user_race_info.vehicle = vehicle
            addEventListener("onUserExitedVehicle", vehicle.uid, onUserExitedVehicle)
            user_race_info.start_time = getCurrentTime()
            user_race_info.next_waypoint_index = user_race_info.next_waypoint_index + 1
        elseif user_race_info.next_waypoint_index == #waypoint_uids then
            local user_time = getCurrentTime() - user_race_info.start_time
            showMessageToUser("Finish!  your time: " .. string.format("%.3f", user_time) .. " s", av)
            checkAddTimeToBestTimes(av, user_time)
            user_race_info.valid = false
        else
            showMessageToUser("waypoint " .. tostring(user_race_info.next_waypoint_index - 1) .. "/" .. tostring(#waypoint_uids - 2) .. " done", av)
            user_race_info.next_waypoint_index = user_race_info.next_waypoint_index + 1
        end
    else
        showMessageToUser("You went through the wrong waypoint!", av)
        user_race_info.valid = false
    end
end

function onUserExitedVehicle(av, vehicle)
    print("onUserExitedVehicle")
    local user_race_info = race_info[av.uid]
    if user_race_info and user_race_info.valid then
        showMessageToUser("You exited your vehicle during the race and were disqualified!", av)
        user_race_info.valid = false
    end
end

for key, uid in waypoint_uids do
    addEventListener("onUserTouchedObject", uid, onUserTouchedObject)
end
addEventListener("onUserTouchedObject", reset_ob_uid, onUserTouchedObject)
