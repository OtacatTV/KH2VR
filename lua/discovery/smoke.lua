-- Discovery smoke test: proves Panacea loaded LuaBackend and LuaBackend runs KH2 scripts.
-- Writes a heartbeat file (path from the KH2VR_RUN_DIR file below) every ~60 frames.
-- Copy into <mod_path>\kh2\scripts\ for a run; remove afterwards.

local out_path = nil
local frames = 0
local every = 60 -- live setting: heartbeat interval in frames

local function write(msg)
    if not out_path then return end
    local f = io.open(out_path, "w")
    if f then f:write(msg); f:close() end
end

function _OnInit()
    -- The run folder is read from a sidecar file next to this script, so no path is hard-coded.
    -- LuaBackend opens no `debug` library; it sets SCRIPT_PATH to the scripts folder instead.
    local cfg = io.open((SCRIPT_PATH or ".") .. "\\kh2vr_run_dir.txt", "r")
    if cfg then
        local dir = cfg:read("l"); cfg:close()
        if dir and #dir > 0 then out_path = dir .. "\\lua_heartbeat.txt" end
    end
    write(string.format("init os.clock=%.3f\n", os.clock()))
    if ConsolePrint then ConsolePrint("kh2vr smoke: init") end
end

function _OnFrame()
    frames = frames + 1
    if frames % every == 0 then
        write(string.format("frames=%d os.clock=%.3f\n", frames, os.clock()))
    end
end
