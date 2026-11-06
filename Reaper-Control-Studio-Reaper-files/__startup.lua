-- REAPER_ProjectName_WebControl.lua  (v4 · routing · carátulas guardadas en REAPER)
-- Ayudante del control web. Hace de puente entre la página web y REAPER:
--   * Publica el nombre del proyecto activo (cabecera).
--   * Ejecuta órdenes: añadir, renombrar o eliminar pistas, añadir/quitar FX y activar/desactivar FX.
--   * ROUTING: publica los envíos (sends), recepciones y salidas de hardware de la pista que miras en la web
--     y aplica los cambios (volumen, pan, mute, modo pre/post, canales, crear/quitar envíos, salida al master).
--   * Publica la cadena de FX y los parámetros del FX que estás editando en la web,
--     y aplica los cambios que haces allí (EQ con nodos arrastrables, sliders de cualquier FX).
-- Uso: Actions > Show action list > New action > Load ReaScript... > elige este archivo > Run.
-- Si ya tenías una versión corriendo, REAPER preguntará si terminarla: elige "Terminate instances"
-- y vuelve a ejecutarlo.
-- (Opcional) Añádelo a SWS "Startup actions" o a __startup.lua para que arranque con REAPER.

local SEC = "WebControl"
local HELPER_VERSION = "studio-v22"
local MAXP = 128                 -- máximo de parámetros que se publican por FX

local lastUploadRaw = ""
local uploadSession = nil
local lastProject = nil
local lastProjectPath = nil
local lastProjectPathState = nil
local lastProjectDirty = nil
local lastTimeline, lastTimelinePos = "", ""
local lastTimelineProject, lastTimelineState = nil, -1
local lastTimelineScan, lastTimelinePosScan = 0, 0
local recentProjects = {}
local watch = { track = nil, fx = -1 }
local lastWatchRaw, lastSetRaw, lastEqSetRaw = "", "", reaper.GetExtState(SEC, "eqset")
local lastEqResetRaw = reaper.GetExtState(SEC, "eqreset")
local pub = { list = "", params = "", mapKey = "" }
local paramCache = {}
local lastPublish = 0
local lastPadEvents, activePadNotes = {}, {}
for channel = 1, 16 do lastPadEvents[channel] = reaper.GetExtState(SEC, "pad" .. channel) end

local function pollPad()
  for slot = 1, 16 do
    local raw = reaper.GetExtState(SEC, "pad" .. slot)
    if raw ~= "" and raw ~= lastPadEvents[slot] then
      lastPadEvents[slot] = raw
      local action, note, value, channel = raw:match("^(%a+)|(%d+)|(%d+)|(%d+)|%d+$")
      note, value, channel = tonumber(note), tonumber(value), tonumber(channel)
      if action and note and note >= 0 and note <= 127 and value and channel == slot then
        if action == "on" then
          local previous = activePadNotes[channel]
          if previous then reaper.StuffMIDIMessage(0, 0x80 + channel - 1, previous, 0) end
          reaper.StuffMIDIMessage(0, 0x90 + channel - 1, note, math.max(1, math.min(127, value)))
          activePadNotes[channel] = note
        elseif action == "off" then
          if activePadNotes[channel] == note then
            reaper.StuffMIDIMessage(0, 0x80 + channel - 1, note, 0)
            activePadNotes[channel] = nil
          end
        elseif action == "vol" then
          reaper.StuffMIDIMessage(0, 0xB0 + channel - 1, 7, math.max(0, math.min(127, value)))
        end
      end
    end
  end
end

-- routing
local CAT = { s = 0, h = 1, r = -1 }            -- s = envío, h = salida hardware, r = recepción
local RPARM = { v = "D_VOL", p = "D_PAN", m = "B_MUTE", d = "I_SENDMODE", s = "I_SRCCHAN", c = "I_DSTCHAN" }
local rwatch = nil                               -- pista cuyo routing se está viendo en la web
local lastRWatchRaw, lastRSetRaw = "", ""
local rpub = { info = "", hw = "" }

---------------------------------------------------------------- utilidades
local function hexdecode(h)
  return (h:gsub("%x%x", function(c) return string.char(tonumber(c, 16)) end))
end

local function hexencode(s)
  return (tostring(s or ""):gsub(".", function(c) return string.format("%02x", string.byte(c)) end))
end

local function rememberProject(path)
  if not path or path == "" then return end
  for i = #recentProjects, 1, -1 do if recentProjects[i] == path then table.remove(recentProjects, i) end end
  table.insert(recentProjects, 1, path)
  while #recentProjects > 20 do table.remove(recentProjects) end
  local encoded = {}
  for i, p in ipairs(recentProjects) do encoded[i] = hexencode(p) end
  reaper.SetExtState(SEC, "recent", table.concat(encoded, "|"), true)
end

local function clean(s)
  return (tostring(s or ""):gsub("[|~\t\r\n]", " "))
end

local function result(nonce, ok, msg)
  reaper.SetExtState(SEC, "result", nonce .. "|" .. (ok and "1" or "0") .. "|" .. msg, false)
end

-- Recibe archivos elegidos desde un navegador normal. Los datos llegan en bloques hexadecimales.
local function pollUpload()
  local raw = reaper.GetExtState(SEC, "upload")
  if raw == "" or raw == lastUploadRaw then return end
  lastUploadRaw = raw
  reaper.DeleteExtState(SEC, "upload", false)
  local id, kind, ix, total, nameHex, dataHex = raw:match("^([%w%-]+)|([iod])|(%d+)|(%d+)|([%x]*)|([%x]*)$")
  ix, total = tonumber(ix), tonumber(total)
  local function ack(nextIx, status, path)
    reaper.SetExtState(SEC, "uploadack", table.concat({id or "?", tostring(nextIx or 0), status or "error", path or ""}, "|"), false)
  end
  if not id or not ix or not total or total < 1 or ix >= total or #dataHex > 4096 then ack(0, "error", hexencode("Bloque de archivo inválido.")); return end
  if not uploadSession or uploadSession.id ~= id then
    if ix ~= 0 then ack(0, "error", hexencode("La transferencia perdi? su primer bloque.")); return end
    local meta = hexdecode(nameHex)
    local batch, rel = meta:match("^([%w%-]+)|(.+)$")
    if not batch then batch, rel = id, meta end
    if not batch:match("^[%w%-]+$") then ack(0, "error", hexencode("Identificador de transferencia inválido.")); return end
    if rel == "" then rel = kind == "o" and "proyecto.rpp" or "audio.bin" end
    if kind == "o" and not rel:lower():match("%.rpp$") then ack(0, "error", hexencode("Selecciona un proyecto .RPP.")); return end
    local dir = reaper.GetResourcePath() .. "/WebControlUploads/" .. batch
    reaper.RecursiveCreateDirectory(dir, 0)
    local parts = {}
    for part in rel:gmatch("[^/\\]+") do
      part = part:gsub('[<>:"|?*%c]', "_"):gsub("^%.+", "")
      if part ~= "" and part ~= "." and part ~= ".." then parts[#parts + 1] = part end
    end
    if #parts == 0 then ack(0, "error", hexencode("Nombre de archivo no válido.")); return end
    local parent = dir
    for i = 1, #parts - 1 do parent = parent .. "/" .. parts[i]; reaper.RecursiveCreateDirectory(parent, 0) end
    local path = parent .. "/" .. parts[#parts]
    uploadSession = { id = id, kind = kind, total = total, next = 0, path = path }
  end
  local u = uploadSession
  if u.id ~= id or u.kind ~= kind or u.total ~= total or u.next ~= ix then ack(u.next, "error", hexencode("Bloques recibidos fuera de orden.")); return end
  local f, err = io.open(u.path, ix == 0 and "wb" or "ab")
  if not f then ack(ix, "error", hexencode(tostring(err))); uploadSession = nil; return end
  local ok, wrote, writeErr = pcall(function() local yes, err = f:write(hexdecode(dataHex)); f:close(); return yes, err end)
  if not ok or not wrote then pcall(function() f:close() end); ack(ix, "error", hexencode(tostring(writeErr or wrote))); uploadSession = nil; return end
  u.next = ix + 1
  if u.next >= u.total then
    ack(u.next, "done", hexencode(u.path))
    uploadSession = nil
  else
    ack(u.next, "ok", "")
  end
end

local function getTrack(idx)
  if idx == 0 then return reaper.GetMasterTrack(0) end
  return reaper.GetTrack(0, idx - 1)          -- el web numera las pistas desde 1 (0 = master)
end

local function trackName(tr)
  local _, n = reaper.GetSetMediaTrackInfo_String(tr, "P_NAME", "", false)
  return (n and n ~= "") and n or "sin nombre"
end

local function fxName(tr, i)
  local _, n = reaper.TrackFX_GetFXName(tr, i, "")
  return n or ""
end

local function paramName(tr, fx, p)
  local _, n = reaper.TrackFX_GetParamName(tr, fx, p, "")
  return n or ""
end

local function paramText(tr, fx, p)
  local _, s = reaper.TrackFX_GetFormattedParamValue(tr, fx, p, "")
  return s or ""
end

-- "1.50 kHz" -> 1500 ; "-3.0 dB" -> -3 ; "-inf dB" -> -150
local function parseNum(s)
  local l = tostring(s):lower():gsub("?", "-"):gsub(",", ".")
  if l:find("inf") then
    if l:find("%-") then return -150 else return 150 end
  end
  local n = l:match("([%-%+]?%d*%.?%d+)")
  if not n then return nil end
  n = tonumber(n)
  if not n then return nil end
  if l:find("%d%s*k") then n = n * 1000 end
  return n
end

local function isEqParam(name)
  local l = name:lower()
  return l:find("freq") or l:find("gain") or l:find("bw") or l:find("bandwidth")
      or l:find("%f[%a]q%f[%A]")
end

---------------------------------------------------------------- órdenes (cmd)
-- Secciones de la canción = regiones de REAPER.
-- Salto en cola: al tocar una sección en la web, la reproducción va a ella cuando termina la sección actual.
local JUMP_LEAD = 0.06          -- segundos de antelación para compensar la latencia del salto
local pendingJump = nil
local lastPlayPos = 0

-- Enmarca una sección en REAPER como si se hubiera hecho a mano:
-- selección de tiempo + puntos de loop. El botón de loop usa el REPEAT nativo de REAPER.
local function frameRegion(startAt, stopAt)
  if not startAt or not stopAt or stopAt <= startAt then return end
  local ts, te = reaper.GetSet_LoopTimeRange2(0, false, false, 0, 0, false)
  if math.abs((ts or 0) - startAt) > 0.0005 or math.abs((te or 0) - stopAt) > 0.0005 then
    reaper.GetSet_LoopTimeRange2(0, true, false, startAt, stopAt, false)   -- selección de tiempo
  end
  local ls, le = reaper.GetSet_LoopTimeRange2(0, false, true, 0, 0, false)
  if math.abs((ls or 0) - startAt) > 0.0005 or math.abs((le or 0) - stopAt) > 0.0005 then
    reaper.GetSet_LoopTimeRange2(0, true, true, startAt, stopAt, false)    -- puntos de loop
  end
  reaper.UpdateTimeline()
end

local function regionList()
  local list = {}
  local _, nMarkers, nRegions = reaper.CountProjectMarkers(0)
  for m = 0, (nMarkers or 0) + (nRegions or 0) - 1 do
    local _, isrgn, rpos, rend, rname, rindex, rcolor = reaper.EnumProjectMarkers3(0, m)
    if isrgn then list[#list + 1] = { start = rpos, stop = rend, name = rname or "", index = rindex, color = rcolor } end
  end
  table.sort(list, function(a, b) return a.start < b.start end)
  return list
end

local function pollJump()
  local playing = (reaper.GetPlayState() & 1) ~= 0
  local pj = pendingJump
  if not playing then pendingJump = nil lastPlayPos = 0 return end
  local pos = reaper.GetPlayPosition2()
  local prev = lastPlayPos
  lastPlayPos = pos
  if not pj then return end
  -- si el REPEAT de REAPER ya dio la vuelta al final de la sección, se salta igualmente
  local wrapped = pos < prev - 0.5 and prev >= pj.stop - 0.5
  if pos < pj.from - 0.25 and not wrapped then pendingJump = nil return end   -- el usuario movió el cabezal hacia atrás
  if wrapped or pos >= pj.stop - JUMP_LEAD then
    pendingJump = nil
    reaper.SetEditCurPos(pj.target, true, true)   -- mover la vista y buscar durante la reproducción
    frameRegion(pj.target, pj.targetEnd)           -- la sección destino pasa a ser la marcada en REAPER
  end
end

local function handle(cmd, nonce)
  local parts = {}
  for p in cmd:gmatch("[^_]+") do parts[#parts + 1] = p end
  local kind = parts[1]

  if kind == "section" then
    local startAt, stopAt = tonumber(parts[2] or ""), tonumber(parts[3] or "")
    if not startAt or not stopAt or stopAt <= startAt then result(nonce, false, "Sección no válida.") return end
    pendingJump = nil
    frameRegion(startAt, stopAt)
    reaper.SetEditCurPos(startAt, true, (reaper.GetPlayState() & 1) ~= 0)
    result(nonce, true, "Sección marcada en REAPER.")
    return
  end

  if kind == "frame" then   -- marca una sección en REAPER sin mover el cabezal
    local startAt, stopAt = tonumber(parts[2] or ""), tonumber(parts[3] or "")
    if startAt and stopAt and stopAt > startAt then frameRegion(startAt, stopAt) end
    result(nonce, true, "Sección marcada.")
    return
  end

  if kind == "rep" then     -- botón de loop = REPEAT nativo de REAPER
    local on = parts[2] == "1"
    local startAt, stopAt = tonumber(parts[3] or ""), tonumber(parts[4] or "")
    if on and startAt and stopAt and stopAt > startAt then frameRegion(startAt, stopAt) end
    reaper.GetSetRepeatEx(0, on and 1 or 0)
    result(nonce, true, on and "Loop activado." or "Loop desactivado.")
    return
  end

  if kind == "loopoff" then
    reaper.GetSetRepeatEx(0, 0)
    result(nonce, true, "Loop desactivado.")
    return
  end

  if kind == "move" then
    local sourceId, targetId = tonumber(parts[2] or ""), tonumber(parts[3] or "")
    local source, target = sourceId and getTrack(sourceId), targetId and getTrack(targetId)
    if not source or not target or source == target or sourceId == 0 or targetId == 0 then
      result(nonce, false, "Selecciona dos pistas distintas para reordenar.") return
    end
    local function parentMap()
      local parents, stack = {}, {}
      for i = 0, reaper.CountTracks(0) - 1 do
        local tr = reaper.GetTrack(0, i)
        parents[i + 1] = stack[#stack] or 0
        local depth = math.floor(reaper.GetMediaTrackInfo_Value(tr, "I_FOLDERDEPTH"))
        if depth > 0 then for _ = 1, depth do stack[#stack + 1] = i + 1 end
        elseif depth < 0 then for _ = 1, -depth do table.remove(stack) end end
      end
      return parents
    end
    local parents = parentMap()
    if parents[sourceId] ~= parents[targetId] then
      result(nonce, false, "Suelta la pista dentro de su mismo grupo. Para mover un grupo completo, arrastra el nombre de su carpeta.") return
    end
    local sourceDepth = math.floor(reaper.GetMediaTrackInfo_Value(source, "I_FOLDERDEPTH"))
    local firstIndex, lastIndex = sourceId - 1, sourceId - 1
    if sourceDepth > 0 then
      local nesting = sourceDepth
      for i = firstIndex + 1, reaper.CountTracks(0) - 1 do
        nesting = nesting + math.floor(reaper.GetMediaTrackInfo_Value(reaper.GetTrack(0, i), "I_FOLDERDEPTH"))
        lastIndex = i
        if nesting <= 0 then break end
      end
    end
    if targetId - 1 >= firstIndex and targetId - 1 <= lastIndex then
      result(nonce, false, "Suelta la carpeta fuera de sus propias pistas.") return
    end
    local blockSize = lastIndex - firstIndex + 1
    local tail = reaper.GetTrack(0, lastIndex)
    local tailDepth = math.floor(reaper.GetMediaTrackInfo_Value(tail, "I_FOLDERDEPTH"))
    local hasLaterSibling, previousSiblingId = false, nil
    for id, parentId in pairs(parents) do
      if parentId == parents[sourceId] then
        if id > lastIndex then hasLaterSibling = true end
        if id < sourceId and (not previousSiblingId or id > previousSiblingId) then previousSiblingId = id end
      end
    end
    local transferDepth = 0
    if not hasLaterSibling then
      transferDepth = sourceDepth > 0 and math.min(0, tailDepth + 1) or math.min(0, sourceDepth)
    end
    local previousSiblingEnd
    if transferDepth < 0 and previousSiblingId then
      local previousIndex = previousSiblingId - 1
      local previousDepth = math.floor(reaper.GetMediaTrackInfo_Value(reaper.GetTrack(0, previousIndex), "I_FOLDERDEPTH"))
      if previousDepth > 0 then
        local nesting = previousDepth
        for i = previousIndex + 1, firstIndex - 1 do
          nesting = nesting + math.floor(reaper.GetMediaTrackInfo_Value(reaper.GetTrack(0, i), "I_FOLDERDEPTH"))
          previousIndex = i
          if nesting <= 0 then break end
        end
      end
      previousSiblingEnd = reaper.GetTrack(0, previousIndex)
    end
    local priorSelection = {}
    for i = 0, reaper.CountTracks(0) - 1 do
      local tr = reaper.GetTrack(0, i)
      priorSelection[tr] = reaper.GetMediaTrackInfo_Value(tr, "I_SELECTED") > 0
    end
    reaper.Undo_BeginBlock()
    for i = 0, reaper.CountTracks(0) - 1 do reaper.SetMediaTrackInfo_Value(reaper.GetTrack(0, i), "I_SELECTED", 0) end
    for i = firstIndex, lastIndex do reaper.SetMediaTrackInfo_Value(reaper.GetTrack(0, i), "I_SELECTED", 1) end
    -- REAPER removes selected tracks before applying the destination index.
    local insertAt = targetId - 1
    if firstIndex < targetId - 1 then insertAt = insertAt - blockSize end
    reaper.ReorderSelectedTracks(insertAt, 0)
    if transferDepth < 0 and previousSiblingEnd and reaper.ValidatePtr2(0, previousSiblingEnd, "MediaTrack*") then
      if sourceDepth > 0 then reaper.SetMediaTrackInfo_Value(tail, "I_FOLDERDEPTH", tailDepth - transferDepth)
      else reaper.SetMediaTrackInfo_Value(source, "I_FOLDERDEPTH", 0) end
      local d = math.floor(reaper.GetMediaTrackInfo_Value(previousSiblingEnd, "I_FOLDERDEPTH"))
      reaper.SetMediaTrackInfo_Value(previousSiblingEnd, "I_FOLDERDEPTH", d + transferDepth)
    end
    for tr, wasSelected in pairs(priorSelection) do
      if reaper.ValidatePtr2(0, tr, "MediaTrack*") then reaper.SetMediaTrackInfo_Value(tr, "I_SELECTED", wasSelected and 1 or 0) end
    end
    reaper.Undo_EndBlock("Web: reordenar pista", -1)
    reaper.TrackList_AdjustWindows(false); reaper.UpdateArrange()
    result(nonce, true, sourceDepth > 0 and "Carpeta y pistas reordenadas." or "Pista reordenada.")

  elseif kind == "folder" then
    local name = hexdecode(parts[2] or "")
    if name == "" then name = "Carpeta" end
    local n = reaper.CountTracks(0)
    local nesting = 0
    for i = 0, n - 1 do nesting = nesting + math.floor(reaper.GetMediaTrackInfo_Value(reaper.GetTrack(0, i), "I_FOLDERDEPTH")) end
    local lastDepth = n > 0 and math.floor(reaper.GetMediaTrackInfo_Value(reaper.GetTrack(0, n - 1), "I_FOLDERDEPTH")) or 0
    if n > 0 and nesting > 0 and lastDepth > 0 then
      result(nonce, false, "Asigna al menos una pista a la carpeta vacía antes de crear otra.") return
    end
    reaper.Undo_BeginBlock()
    if n > 0 and nesting > 0 and lastDepth <= 0 then
      local last = reaper.GetTrack(0, n - 1)
      local depth = math.floor(reaper.GetMediaTrackInfo_Value(last, "I_FOLDERDEPTH"))
      reaper.SetMediaTrackInfo_Value(last, "I_FOLDERDEPTH", depth - nesting)
    end
    reaper.InsertTrackAtIndex(n, true)
    local folder = reaper.GetTrack(0, n)
    reaper.GetSetMediaTrackInfo_String(folder, "P_NAME", name, true)
    reaper.SetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH", 1)
    reaper.Undo_EndBlock("Web: crear carpeta de pistas", -1)
    reaper.TrackList_AdjustWindows(false)
    reaper.UpdateArrange()
    result(nonce, true, "Carpeta creada. Usa «Asignar pistas» para añadir pistas existentes: " .. name)

  elseif kind == "folderedit" then
    local idx = tonumber(parts[2] or "")
    local folder = idx and getTrack(idx)
    if not folder or reaper.GetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH") <= 0 then
      result(nonce, false, "La carpeta ya no existe.") return
    end
    local function parseIds(encoded)
      local out = {}
      for id in hexdecode(encoded or ""):gmatch("%d+") do out[#out + 1] = tonumber(id) end
      return out
    end
    local addIds, removeIds = parseIds(parts[3]), parseIds(parts[4])
    if #addIds == 0 and #removeIds == 0 then result(nonce, false, "No seleccionaste movimientos.") return end
    local function folderIndex()
      return math.floor(reaper.GetMediaTrackInfo_Value(folder, "IP_TRACKNUMBER")) - 1
    end
    local function directMember(child)
      local fidx = folderIndex()
      local depth = math.floor(reaper.GetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH"))
      local nesting = depth
      for i = fidx + 1, reaper.CountTracks(0) - 1 do
        local tr = reaper.GetTrack(0, i)
        if tr == child then return nesting == depth end
        nesting = nesting + math.floor(reaper.GetMediaTrackInfo_Value(tr, "I_FOLDERDEPTH"))
        if nesting <= 0 then return false end
      end
      return false
    end
    local directCount = 0
    for i = folderIndex() + 1, reaper.CountTracks(0) - 1 do
      local tr = reaper.GetTrack(0, i)
      if directMember(tr) then directCount = directCount + 1 end
    end
    local addTracks, removeTracks = {}, {}
    for _, id in ipairs(addIds) do
      local tr = getTrack(id)
      if tr and tr ~= folder and reaper.GetMediaTrackInfo_Value(tr, "I_FOLDERDEPTH") <= 0 then addTracks[#addTracks + 1] = tr end
    end
    for _, id in ipairs(removeIds) do
      local tr = getTrack(id)
      if tr and reaper.GetMediaTrackInfo_Value(tr, "I_FOLDERDEPTH") <= 0 and directMember(tr) then removeTracks[#removeTracks + 1] = tr end
    end
    if #addTracks == 0 and #removeTracks == 0 then result(nonce, false, "No hay pistas validas para mover.") return end
    local priorSelection = {}
    for i = 0, reaper.CountTracks(0) - 1 do
      local tr = reaper.GetTrack(0, i)
      priorSelection[tr] = reaper.GetMediaTrackInfo_Value(tr, "I_SELECTED") > 0
    end
    reaper.Undo_BeginBlock()
    for _, tr in ipairs(removeTracks) do
      if reaper.ValidatePtr2(0, tr, "MediaTrack*") then
        for i = 0, reaper.CountTracks(0) - 1 do reaper.SetMediaTrackInfo_Value(reaper.GetTrack(0, i), "I_SELECTED", 0) end
        reaper.SetMediaTrackInfo_Value(tr, "I_SELECTED", 1)
        reaper.ReorderSelectedTracks(reaper.CountTracks(0), 0)
      end
    end
    if #addTracks == 0 and #removeTracks >= directCount then
      reaper.SetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH", 0)
    end
    if #addTracks > 0 then
      if reaper.GetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH") <= 0 then reaper.SetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH", 1) end
      local fidx, count = folderIndex(), reaper.CountTracks(0)
      local nesting = math.floor(reaper.GetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH"))
      local insertAt, hasChildren = count, false
      for i = fidx + 1, count - 1 do
        nesting = nesting + math.floor(reaper.GetMediaTrackInfo_Value(reaper.GetTrack(0, i), "I_FOLDERDEPTH"))
        if nesting <= 0 then insertAt, hasChildren = i, true; break end
      end
      for i = 0, count - 1 do reaper.SetMediaTrackInfo_Value(reaper.GetTrack(0, i), "I_SELECTED", 0) end
      local valid = 0
      for _, tr in ipairs(addTracks) do
        if reaper.ValidatePtr2(0, tr, "MediaTrack*") then reaper.SetMediaTrackInfo_Value(tr, "I_SELECTED", 1); valid = valid + 1 end
      end
      if valid > 0 then reaper.ReorderSelectedTracks(insertAt, hasChildren and 2 or 1) end
    end
    for tr, wasSelected in pairs(priorSelection) do
      if reaper.ValidatePtr2(0, tr, "MediaTrack*") then reaper.SetMediaTrackInfo_Value(tr, "I_SELECTED", wasSelected and 1 or 0) end
    end
    reaper.Undo_EndBlock("Web: mover pistas dentro o fuera de carpeta", -1)
    reaper.TrackList_AdjustWindows(false); reaper.UpdateArrange()
    result(nonce, true, "Se actualizaron las pistas de la carpeta.")

  elseif kind == "folderadd" then
    local idx = tonumber(parts[2] or "")
    local childName = hexdecode(parts[3] or "")
    if childName == "" then childName = "Pista nueva" end
    local folder = idx and getTrack(idx)
    if not folder or reaper.GetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH") <= 0 then
      result(nonce, false, "La pista seleccionada ya no es una carpeta.") return
    end
    local count = reaper.CountTracks(0)
    local start = idx - 1
    local nesting = math.floor(reaper.GetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH"))
    local insertAt = count
    for i = start + 1, count - 1 do
      nesting = nesting + math.floor(reaper.GetMediaTrackInfo_Value(reaper.GetTrack(0, i), "I_FOLDERDEPTH"))
      if nesting <= 0 then insertAt = i; break end
    end
    reaper.Undo_BeginBlock()
    reaper.InsertTrackAtIndex(insertAt, true)
    local child = reaper.GetTrack(0, insertAt)
    reaper.GetSetMediaTrackInfo_String(child, "P_NAME", childName, true)
    reaper.SetMediaTrackInfo_Value(child, "I_FOLDERDEPTH", 0)
    reaper.Undo_EndBlock("Web: añadir pista a carpeta", -1)
    reaper.TrackList_AdjustWindows(false); reaper.UpdateArrange()
    result(nonce, true, "Pista añadida dentro de la carpeta.")

  elseif kind == "folderdel" then
    local idx = tonumber(parts[2] or "")
    local folder = idx and getTrack(idx)
    if not folder or reaper.GetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH") <= 0 then
      result(nonce, false, "La pista seleccionada ya no es una carpeta.") return
    end
    local start, count = idx - 1, reaper.CountTracks(0)
    local nesting = math.floor(reaper.GetMediaTrackInfo_Value(folder, "I_FOLDERDEPTH"))
    local finish = start
    for i = start + 1, count - 1 do
      nesting = nesting + math.floor(reaper.GetMediaTrackInfo_Value(reaper.GetTrack(0, i), "I_FOLDERDEPTH"))
      finish = i
      if nesting <= 0 then break end
    end
    reaper.Undo_BeginBlock()
    for i = finish, start, -1 do reaper.DeleteTrack(reaper.GetTrack(0, i)) end
    reaper.Undo_EndBlock("Web: eliminar carpeta y su contenido", -1)
    reaper.TrackList_AdjustWindows(false); reaper.UpdateArrange()
    result(nonce, true, "Carpeta y pistas contenidas eliminadas.")

  elseif kind == "tab" then
    local idx = tonumber(parts[2] or "")
    local proj = idx and reaper.EnumProjects(idx)
    if not proj then result(nonce, false, "Esa canción ya no está abierta.") return end
    reaper.SelectProjectInstance(proj)
    result(nonce, true, "Canción cambiada.")

  elseif kind == "playexclusive" then
    local idx = tonumber(parts[2] or "")
    local target = idx and reaper.EnumProjects(idx)
    if not target then result(nonce, false, "Project not open.") return end
    local i = 0
    while true do
      local proj = reaper.EnumProjects(i)
      if not proj then break end
      if proj ~= target and (reaper.GetPlayStateEx(proj) & 3) ~= 0 then
        reaper.OnStopButtonEx(proj)
      end
      i = i + 1
    end
    reaper.SelectProjectInstance(target)
    if (reaper.GetPlayStateEx(target) & 1) == 0 then reaper.OnPlayButtonEx(target) end
    result(nonce, true, "Other projects stopped; selected project is playing.")
  elseif kind == "close" then
    reaper.Main_OnCommand(40860, 0) -- File: Close current project tab (REAPER muestra su aviso de guardado)
    result(nonce, true, "Se solicitó cerrar el proyecto activo.")

  elseif kind == "new" then
    reaper.Main_OnCommand(40859, 0) -- File: New project tab
    result(nonce, true, "Nueva pestaña de proyecto creada.")

  elseif kind == "open" then
    local path = hexdecode(parts[2] or "")
    if path == "" or not reaper.file_exists(path) then result(nonce, false, "No se encontró el archivo de proyecto.") return end
    reaper.Main_OnCommand(40859, 0)
    reaper.Main_openProject(path)
    result(nonce, true, "Proyecto abierto en una pestaña nueva.")

  elseif kind == "save" then
    local _, currentPath = reaper.EnumProjects(-1)
    if not currentPath or currentPath == "" then result(nonce, false, "Este proyecto aún no tiene ruta. Usa Guardar como…") return end
    local ok, err = pcall(reaper.Main_SaveProject, 0, false)
    if not ok then result(nonce, false, "No se pudo guardar el proyecto: " .. tostring(err))
    else result(nonce, true, "Proyecto guardado.") end

  elseif kind == "discard" then
    local project = reaper.EnumProjects(-1)
    if not project or not reaper.IsProjectDirty(project) then result(nonce, true, "No hay cambios pendientes.") return end
    local undone = 0
    while reaper.IsProjectDirty(project) and undone < 1000 do
      local before = reaper.GetProjectStateChangeCount(project)
      reaper.Undo_DoUndo2(project)
      undone = undone + 1
      if reaper.GetProjectStateChangeCount(project) == before then break end
    end
    if reaper.IsProjectDirty(project) then result(nonce, false, "REAPER no pudo volver al último estado guardado. El proyecto sigue abierto.")
    else reaper.UpdateArrange(); result(nonce, true, "Cambios descartados. Proyecto restaurado al último guardado.") end

  elseif kind == "saveas" then
    local path = hexdecode(parts[2] or "")
    if path == "" then result(nonce, false, "Indica una ruta para guardar el proyecto.") return end
    if not path:lower():match("%.rpp$") then path = path .. ".RPP" end
    local ok, err = pcall(reaper.Main_SaveProjectEx, 0, path, 8)
    if not ok then result(nonce, false, "No se pudo guardar el proyecto: " .. tostring(err)); return end
    if not reaper.file_exists(path) then result(nonce, false, "REAPER no creó el archivo de proyecto en la ubicación elegida."); return end
    result(nonce, true, "Proyecto guardado: " .. (path:match("([^/\\]+)$") or path))

  elseif kind == "projrename" then
    local newName = hexdecode(parts[2] or "")
    newName = newName:gsub('[\\/:*?"<>|]', " "):match("^%s*(.-)%s*$") or ""
    newName = newName:gsub("%.[Rr][Pp][Pp]$", "")
    if newName == "" then result(nonce, false, "El nombre del proyecto no puede quedar vacío.") return end
    local _, currentPath = reaper.EnumProjects(-1)
    if not currentPath or currentPath == "" then result(nonce, false, "Este proyecto aún no tiene ruta. Usa Guardar como…") return end
    local dir = currentPath:match("^(.*)[/\\][^/\\]+$")
    if not dir then result(nonce, false, "No se pudo determinar la carpeta del proyecto.") return end
    local sep = currentPath:find("\\", 1, true) and "\\" or "/"
    local newPath = dir .. sep .. newName .. ".RPP"
    if newPath:lower() == currentPath:lower() then
      if newPath == currentPath then result(nonce, true, "El proyecto ya se llama así.") return end
    elseif reaper.file_exists(newPath) then
      result(nonce, false, "Ya existe un proyecto llamado \"" .. newName .. "\" en esa carpeta.") return
    end
    local ok, err = pcall(reaper.Main_SaveProjectEx, 0, newPath, 8)
    if not ok then result(nonce, false, "No se pudo renombrar el proyecto: " .. tostring(err)) return end
    if not reaper.file_exists(newPath) then result(nonce, false, "REAPER no creó el archivo con el nuevo nombre.") return end
    if newPath:lower() ~= currentPath:lower() then pcall(os.remove, currentPath) end
    result(nonce, true, "Proyecto renombrado: " .. newName)

  elseif kind == "import" then
    local path = hexdecode(parts[2] or "")
    if path == "" or not reaper.file_exists(path) then result(nonce, false, "No se encontró el archivo para importar.") return end
    local beforeTracks = reaper.CountTracks(0)
    local beforeItems = reaper.CountMediaItems(0)
    reaper.Undo_BeginBlock()
    reaper.InsertMedia(path, 1) -- importa el archivo en una pista nueva
    local addedTracks = reaper.CountTracks(0) - beforeTracks
    local addedItems = reaper.CountMediaItems(0) - beforeItems
    reaper.Undo_EndBlock("Web: importar archivo", -1)
    if addedItems > 0 then
      reaper.TrackList_AdjustWindows(false)
      reaper.UpdateArrange()
      result(nonce, true, "Archivo importado en " .. tostring(math.max(1, addedTracks)) .. " pista(s).")
    else
      result(nonce, false, "REAPER no pudo importar ese archivo.")
    end

  elseif kind == "add" then
    local name = hexdecode(parts[2] or "")
    if name == "" then name = "Pista" end
    reaper.Undo_BeginBlock()
    local n = reaper.CountTracks(0)
    reaper.InsertTrackAtIndex(n, true)
    local tr = reaper.GetTrack(0, n)
    reaper.GetSetMediaTrackInfo_String(tr, "P_NAME", name, true)
    reaper.Undo_EndBlock("Web: añadir pista", -1)
    reaper.TrackList_AdjustWindows(false)
    reaper.UpdateArrange()
    result(nonce, true, "Pista añadida: " .. name)

  elseif kind == "del" then
    local idx = tonumber(parts[2] or "")
    local tr = nil
    if idx and idx > 0 then tr = getTrack(idx) end
    if not tr then result(nonce, false, "No se encontró esa pista (o es el master).") return end
    local name = trackName(tr)
    reaper.Undo_BeginBlock()
    reaper.DeleteTrack(tr)
    reaper.Undo_EndBlock("Web: eliminar pista", -1)
    reaper.TrackList_AdjustWindows(false)
    reaper.UpdateArrange()
    result(nonce, true, "Pista eliminada: " .. name)

  elseif kind == "rename" then
    local idx = tonumber(parts[2] or "")
    local name = hexdecode(parts[3] or "")
    local tr = nil
    if idx and idx > 0 then tr = getTrack(idx) end
    if not tr then result(nonce, false, "No se encontró esa pista (o es el master).") return end
    name = name:match("^%s*(.-)%s*$") or ""
    if name == "" then result(nonce, false, "El nombre de la pista no puede quedar vacío.") return end
    local oldName = trackName(tr)
    reaper.Undo_BeginBlock()
    reaper.GetSetMediaTrackInfo_String(tr, "P_NAME", name, true)
    reaper.Undo_EndBlock("Web: renombrar pista", -1)
    reaper.TrackList_AdjustWindows(false)
    reaper.UpdateArrange()
    local _, savedName = reaper.GetSetMediaTrackInfo_String(tr, "P_NAME", "", false)
    if savedName ~= name then
      result(nonce, false, "REAPER no aceptó el nuevo nombre de la pista.")
    else
      result(nonce, true, "Pista renombrada: " .. oldName .. " → " .. savedName)
    end

  elseif kind == "fx" then
    local idx = tonumber(parts[2] or "")
    local fxname = hexdecode(parts[3] or "")
    local tr = nil
    if idx then tr = getTrack(idx) end
    if not tr then result(nonce, false, "No se encontró esa pista.") return end
    if fxname == "" then result(nonce, false, "Falta el nombre del FX.") return end
    reaper.Undo_BeginBlock()
    local fxi = reaper.TrackFX_AddByName(tr, fxname, false, -1)   -- -1 = siempre crea una instancia nueva
    reaper.Undo_EndBlock("Web: añadir FX " .. fxname, -1)
    if fxi >= 0 then
      result(nonce, true, fxname .. " añadido a " .. (idx == 0 and "MASTER" or trackName(tr)))
    else
      result(nonce, false, "REAPER no encontró el FX \"" .. fxname .. "\".")
    end

  elseif kind == "fxen" then          -- fxen_<pista>_<fx>_<0|1>
    local idx, fxi = tonumber(parts[2] or ""), tonumber(parts[3] or "")
    local tr = nil
    if idx then tr = getTrack(idx) end
    if not tr or not fxi or fxi >= reaper.TrackFX_GetCount(tr) then
      result(nonce, false, "No se encontró ese FX.") return
    end
    reaper.TrackFX_SetEnabled(tr, fxi, parts[4] == "1")
    result(nonce, true, "FX " .. (parts[4] == "1" and "activado" or "desactivado"))

  elseif kind == "fxdel" then         -- fxdel_<pista>_<fx>
    local idx, fxi = tonumber(parts[2] or ""), tonumber(parts[3] or "")
    local tr = nil
    if idx then tr = getTrack(idx) end
    if not tr or not fxi or fxi >= reaper.TrackFX_GetCount(tr) then
      result(nonce, false, "No se encontró ese FX.") return
    end
    local name = fxName(tr, fxi)
    reaper.Undo_BeginBlock()
    reaper.TrackFX_Delete(tr, fxi)
    reaper.Undo_EndBlock("Web: quitar FX", -1)
    result(nonce, true, "FX quitado: " .. name)

  elseif kind == "color" then         -- color_<pista>_<RRGGBB | 0 = color por defecto>
    local idx = tonumber(parts[2] or "")
    local tr = (idx and idx > 0) and getTrack(idx) or nil
    if not tr then result(nonce, false, "No se encontró esa pista (o es el master).") return end
    local hex = parts[3] or "0"
    reaper.Undo_BeginBlock()
    if hex == "0" then
      reaper.SetMediaTrackInfo_Value(tr, "I_CUSTOMCOLOR", 0)
    else
      local r, g, b = tonumber(hex:sub(1, 2), 16), tonumber(hex:sub(3, 4), 16), tonumber(hex:sub(5, 6), 16)
      if not (r and g and b) then
        reaper.Undo_EndBlock("Web: color de pista", -1)
        result(nonce, false, "Color no válido.") return
      end
      reaper.SetMediaTrackInfo_Value(tr, "I_CUSTOMCOLOR", reaper.ColorToNative(r, g, b) | 0x1000000)
    end
    reaper.Undo_EndBlock("Web: color de pista", -1)
    reaper.TrackList_AdjustWindows(false)
    reaper.UpdateArrange()
    result(nonce, true, hex == "0" and ("Color por defecto: " .. trackName(tr)) or ("Color cambiado: " .. trackName(tr)))

  elseif kind == "radd" then          -- radd_<pista origen>_<pista destino>
    local a, b = tonumber(parts[2] or ""), tonumber(parts[3] or "")
    local src = (a and a > 0) and getTrack(a) or nil
    local dst = (b and b > 0) and getTrack(b) or nil
    if not src or not dst then result(nonce, false, "No se encontró alguna de las pistas.") return end
    if src == dst then result(nonce, false, "Una pista no puede enviarse a sí misma.") return end
    reaper.Undo_BeginBlock()
    local idx = reaper.CreateTrackSend(src, dst)
    reaper.Undo_EndBlock("Web: crear envío", -1)
    if idx and idx >= 0 then
      result(nonce, true, "Envío creado: " .. trackName(src) .. " → " .. trackName(dst))
    else
      result(nonce, false, "REAPER no pudo crear ese envío (¿bucle de feedback?).")
    end

  elseif kind == "rhw" then           -- rhw_<pista>_<canal de salida>
    local t, ch = tonumber(parts[2] or ""), tonumber(parts[3] or "")
    local tr = t and getTrack(t) or nil
    if not tr or not ch then result(nonce, false, "No se encontró esa pista.") return end
    reaper.Undo_BeginBlock()
    reaper.CreateTrackSend(tr, nil)               -- nil = salida de hardware
    local n = reaper.GetTrackNumSends(tr, 1)
    if n > 0 then reaper.SetTrackSendInfo_Value(tr, 1, n - 1, "I_DSTCHAN", ch) end
    reaper.Undo_EndBlock("Web: salida de hardware", -1)
    result(nonce, true, "Salida de hardware añadida a " .. (t == 0 and "MASTER" or trackName(tr)))

  elseif kind == "rdel" then          -- rdel_<pista>_<s|h|r>_<n>
    local t, c, i = tonumber(parts[2] or ""), CAT[parts[3] or ""], tonumber(parts[4] or "")
    local tr = t and getTrack(t) or nil
    if not tr or c == nil or not i then result(nonce, false, "No se encontró ese ruteo.") return end
    if i >= reaper.GetTrackNumSends(tr, c) then result(nonce, false, "Ese ruteo ya no existe.") return end
    reaper.Undo_BeginBlock()
    reaper.RemoveTrackSend(tr, c, i)
    reaper.Undo_EndBlock("Web: quitar ruteo", -1)
    result(nonce, true, "Ruteo quitado")

  elseif kind == "rmain" then         -- rmain_<pista>_<0|1>  (envío al master / carpeta padre)
    local t = tonumber(parts[2] or "")
    local tr = (t and t > 0) and getTrack(t) or nil
    if not tr then result(nonce, false, "No se encontró esa pista.") return end
    reaper.SetMediaTrackInfo_Value(tr, "B_MAINSEND", parts[3] == "1" and 1 or 0)
    reaper.TrackList_AdjustWindows(false)
    result(nonce, true, "Envío al master " .. (parts[3] == "1" and "activado" or "desactivado"))

  elseif kind == "padtemplate" and parts[2] ~= "load" then
    local target, namedPads, selectedKontakt, firstKontakt
    for i = 0, reaper.CountTracks(0) - 1 do
      local tr = reaper.GetTrack(0, i)
      local isKontakt = false
      local fxCount = reaper.TrackFX_GetCount(tr)
      for fx = 0, fxCount - 1 do
        if fxName(tr, fx):lower():find("kontakt", 1, true) then isKontakt = true; break end
      end
      if trackName(tr):lower():match("^%s*pads?%s*$") then
        if fxCount > 0 then namedPads = tr end
        if isKontakt then target = tr; break end
      end
      if isKontakt then
        firstKontakt = firstKontakt or tr
        if reaper.GetMediaTrackInfo_Value(tr, "I_SELECTED") > 0 then
          selectedKontakt = selectedKontakt or tr
        end
      end
    end
    target = target or namedPads or selectedKontakt or firstKontakt
    if not target then result(nonce, false, "No encontré una pista con Kontakt para crear la plantilla."); return end
    local ok, chunk = reaper.GetTrackStateChunk(target, "", false)
    if not ok or not chunk or not chunk:find("<TRACK", 1, true) then result(nonce, false, "REAPER no pudo leer el estado de la pista Kontakt."); return end
    local folder = reaper.GetResourcePath() .. "/TrackTemplates"
    reaper.RecursiveCreateDirectory(folder, 0)
    local path = folder .. "/WebControl Pads.RTrackTemplate"
    local tempPath = path .. ".tmp"
    local file, err = io.open(tempPath, "wb")
    if not file then result(nonce, false, "No pude guardar la plantilla: " .. tostring(err)); return end
    local wrote, writeErr = file:write(chunk, chunk:sub(-1) ~= "\n" and "\n" or "")
    local flushed, flushErr = file:flush()
    local closed, closeErr = file:close()
    if not wrote or not flushed or not closed then
      os.remove(tempPath)
      result(nonce, false, "No pude completar la plantilla: " .. tostring(writeErr or flushErr or closeErr))
      return
    end
    local oldFile = io.open(path, "rb")
    local hadOld = oldFile ~= nil
    if oldFile then oldFile:close() end
    local backupPath = path .. ".bak"
    os.remove(backupPath)
    if hadOld then
      local backedUp, backupErr = os.rename(path, backupPath)
      if not backedUp then
        os.remove(tempPath)
        result(nonce, false, "No pude conservar la plantilla anterior: " .. tostring(backupErr))
        return
      end
    end
    local moved, moveErr = os.rename(tempPath, path)
    if not moved then
      if hadOld then os.rename(backupPath, path) end
      os.remove(tempPath)
      result(nonce, false, "No pude reemplazar la plantilla guardada: " .. tostring(moveErr))
      return
    end
    if hadOld then os.remove(backupPath) end
    result(nonce, true, "Plantilla de Kontakt guardada: " .. path)

  elseif kind == "padtemplate" and parts[2] == "load" then
    local path = reaper.GetResourcePath() .. "/TrackTemplates/WebControl Pads.RTrackTemplate"
    local file = io.open(path, "rb")
    if not file then result(nonce, false, "No encuentro la plantilla WebControl Pads. Guarda primero una pista Kontakt con GUARDAR PLANTILLA."); return end
    local chunk = file:read("*a")
    file:close()
    if not chunk or chunk == "" or not chunk:find("<TRACK", 1, true) then
      result(nonce, false, "La plantilla de Kontakt está vacía o dañada.") return
    end
    local index = reaper.CountTracks(0)
    reaper.Undo_BeginBlock()
    reaper.InsertTrackAtIndex(index, true)
    local tr = reaper.GetTrack(0, index)
    local ok = tr and reaper.SetTrackStateChunk(tr, chunk, false)
    if not ok then
      if tr then reaper.DeleteTrack(tr) end
      reaper.Undo_EndBlock("Cargar plantilla WebControl Pads (falló)", -1)
      result(nonce, false, "REAPER no pudo cargar la plantilla Kontakt.") return
    end
    reaper.GetSetMediaTrackInfo_String(tr, "P_NAME", "PADS", true)
    for i = 0, reaper.CountTracks(0) - 1 do reaper.SetTrackSelected(reaper.GetTrack(0, i), i == index) end
    reaper.TrackList_AdjustWindows(false)
    reaper.UpdateArrange()
    reaper.Undo_EndBlock("Cargar plantilla WebControl Pads", -1)
    result(nonce, true, "Plantilla de pads cargada. Asigna en Kontakt los canales A1 y A2 a tus instrumentos.")

  elseif kind == "secsave" then
    -- Reemplaza TODAS las regiones del proyecto por las secciones enviadas desde la web (borrador editado).
    local payload = hexdecode(parts[2] or "")
    if payload:sub(1, 1) ~= "S" then result(nonce, false, "Datos de secciones no válidos.") return end
    local rows = {}
    for line in payload:gmatch("[^\n]+") do
      local a, b, hex, name = line:match("^([%d%.]+)|([%d%.]+)|(%x%x%x%x%x%x)|(.*)$")
      a, b = tonumber(a), tonumber(b)
      if a and b and b > a then rows[#rows + 1] = { s = a, e = b, hex = hex, name = name } end
    end
    reaper.Undo_BeginBlock()
    reaper.PreventUIRefresh(1)
    for _, r in ipairs(regionList()) do reaper.DeleteProjectMarker(0, r.index, true) end
    for _, r in ipairs(rows) do
      local rr, gg, bb = tonumber(r.hex:sub(1, 2), 16), tonumber(r.hex:sub(3, 4), 16), tonumber(r.hex:sub(5, 6), 16)
      reaper.AddProjectMarker2(0, true, r.s, r.e, r.name, -1, reaper.ColorToNative(rr, gg, bb) | 0x1000000)
    end
    reaper.PreventUIRefresh(-1)
    reaper.UpdateTimeline()
    reaper.Undo_EndBlock("Editar secciones de la canción", -1)
    lastTimeline = ""   -- obliga a republicar la línea de tiempo con las secciones nuevas
    result(nonce, true, "Secciones guardadas (" .. #rows .. "). Guarda el proyecto para conservarlas en el archivo .RPP.")

  elseif kind == "jump" then
    local target = tonumber(parts[2] or "")
    local targetEnd = tonumber(parts[3] or "") or 0
    if not target or target < 0 then result(nonce, false, "Destino de salto no válido.") return end
    if (reaper.GetPlayState() & 1) == 0 then
      pendingJump = nil
      frameRegion(target, targetEnd)
      reaper.SetEditCurPos(target, true, false)
      result(nonce, true, "Posición cambiada.") return
    end
    local pos = reaper.GetPlayPosition2()
    local current
    for _, r in ipairs(regionList()) do if pos >= r.start and pos < r.stop then current = r end end
    if not current or pos >= current.stop - JUMP_LEAD then
      pendingJump = nil
      reaper.SetEditCurPos(target, true, true)
      frameRegion(target, targetEnd)
      result(nonce, true, "Salto inmediato.") return
    end
    -- siempre se programa (incluso a la sección siguiente): con REPEAT activo REAPER volvería al inicio
    pendingJump = { target = target, targetEnd = targetEnd, from = current.start, stop = current.stop }
    result(nonce, true, "Salto programado al terminar la sección.")

  elseif kind == "jumpcancel" then
    pendingJump = nil
    result(nonce, true, "Salto cancelado.")

  else
    result(nonce, false, "Orden desconocida.")
  end
end

---------------------------------------------------------------- FX vigilado
local function listString(tr)
  local n = reaper.TrackFX_GetCount(tr)
  local t = {}
  for i = 0, n - 1 do
    t[#t + 1] = i .. "~" .. (reaper.TrackFX_GetEnabled(tr, i) and "1" or "0") .. "~" .. clean(fxName(tr, i))
  end
  return watch.track .. "|" .. table.concat(t, "|")
end

local function paramsString(tr, fx)
  local n = reaper.TrackFX_GetNumParams(tr, fx)
  if n > MAXP then n = MAXP end
  local t = {}
  for p = 0, n - 1 do
    local v = reaper.TrackFX_GetParamNormalized(tr, fx, p)
    local c = paramCache[p]
    if not c or c.norm ~= v then
      local formatted = clean(paramText(tr, fx, p))
      c = { norm = v, txt = formatted, name = c and c.name or clean(paramName(tr, fx, p)), real = parseNum(formatted) }
      paramCache[p] = c
    end
    t[#t + 1] = p .. "~" .. string.format("%.5f", v) .. "~" .. c.txt .. "~" .. c.name .. "~" .. (c.real and string.format("%.9g", c.real) or "")
  end
  return watch.track .. "|" .. fx .. "|" .. table.concat(t, "|")
end

-- Tabla de conversión valor normalizado -> valor real (Hz, dB, Q) para los parámetros de EQ.
-- Sirve para que la web dibuje y arrastre los nodos en frecuencia/ganancia reales.
local function mapString(tr, fx)
  local n = reaper.TrackFX_GetNumParams(tr, fx)
  if n > MAXP then n = MAXP end
  local out = {}
  for p = 0, n - 1 do
    if isEqParam(paramName(tr, fx, p)) then
      local vals, ok = {}, true
      for i = 0, 100 do
        local _, s = reaper.TrackFX_FormatParamValueNormalized(tr, fx, p, i / 100, "")
        local v = parseNum(s or "")
        if not v then ok = false break end
        vals[#vals + 1] = string.format("%.5g", v)
      end
      if ok then
        local meta = ""
        if reaper.TrackFX_GetEQParam then
          local apiOk, isEq, bandType, bandIdx, paramType = pcall(reaper.TrackFX_GetEQParam, tr, fx, p)
          if apiOk and isEq then meta = "~" .. tostring(bandType) .. "~" .. tostring(bandIdx) .. "~" .. tostring(paramType) end
        end
        out[#out + 1] = p .. "~" .. table.concat(vals, ",") .. meta
      end
    end
  end
  return watch.track .. "|" .. fx .. "|" .. clean(fxName(tr, fx)) .. "|" .. table.concat(out, "|")
end

local function clearPublished()
  reaper.DeleteExtState(SEC, "fxlist", false)
  reaper.DeleteExtState(SEC, "fxparams", false)
  reaper.DeleteExtState(SEC, "fxmap", false)
end

local function pollWatch()
  local raw = reaper.GetExtState(SEC, "watch")
  if raw == lastWatchRaw then return end
  lastWatchRaw = raw
  local t, f = raw:match("^(%d+)_(%-?%d+)$")
  watch.track = t and tonumber(t) or nil
  watch.fx = f and tonumber(f) or -1
  paramCache = {}
  pub.list, pub.params, pub.mapKey = "", "", ""
  if not watch.track then clearPublished() end
end

local function pollSet()
  local raw = reaper.GetExtState(SEC, "set")
  if raw == "" or raw == lastSetRaw then return end
  lastSetRaw = raw
  local t, f, body = raw:match("^(%d+)_(%d+)_(.-)_[^_]+$")
  if not t then return end
  local tr = getTrack(tonumber(t))
  if not tr then return end
  local fx = tonumber(f)
  if fx >= reaper.TrackFX_GetCount(tr) then return end
  for p, v in body:gmatch("(%d+):([%d%.%-eE]+)") do
    local val = tonumber(v)
    if val then
      if val < 0 then val = 0 elseif val > 1 then val = 1 end
      reaper.TrackFX_SetParamNormalized(tr, fx, tonumber(p), val)
    end
  end
end

local function pollEqSet()
  local raw = reaper.GetExtState(SEC, "eqset")
  if raw == "" or raw == lastEqSetRaw then return end
  lastEqSetRaw = raw
  local t, f, body = raw:match("^(%d+)_(%d+)_(.-)_[^_]+$")
  if not t then return end
  local tr = getTrack(tonumber(t))
  local fx = tonumber(f)
  if not tr or not fx or fx >= reaper.TrackFX_GetCount(tr) or not reaper.TrackFX_SetEQParam then return end
  for bt, bi, pt, value in body:gmatch("(%-?%d+):(%d+):(%d+):([%+%-]?[%d%.eE]+)") do
    pcall(reaper.TrackFX_SetEQParam, tr, fx, tonumber(bt), tonumber(bi), tonumber(pt), tonumber(value), false)
  end
end

local function pollEqReset()
  local raw = reaper.GetExtState(SEC, "eqreset")
  if raw == "" or raw == lastEqResetRaw then return end
  lastEqResetRaw = raw
  local t, f = raw:match("^(%d+)_(%d+)_")
  local tr, fx = t and getTrack(tonumber(t)), tonumber(f)
  if not tr or not fx or fx >= reaper.TrackFX_GetCount(tr) or not reaper.TrackFX_SetPresetByIndex
      or not fxName(tr, fx):lower():find("reaeq", 1, true) then
    reaper.SetExtState(SEC, "eqresetResult", "error", false); return
  end
  reaper.Undo_BeginBlock()
  local ok, reset = pcall(reaper.TrackFX_SetPresetByIndex, tr, fx, -2) -- -2 = preset de fábrica
  reaper.Undo_EndBlock("Web: restablecer valores predeterminados de ReaEQ", -1)
  reaper.SetExtState(SEC, "eqresetResult", (ok and reset) and "ok" or "error", false)
  if ok and reset then
    paramCache = {}
    pub.params, pub.mapKey = "", ""
  end
end

local function publish()
  if not watch.track then return end
  local tr = getTrack(watch.track)
  if not tr then return end
  local ls = listString(tr)
  if ls ~= pub.list then
    pub.list = ls
    reaper.SetExtState(SEC, "fxlist", ls, false)
  end
  if watch.fx >= 0 and watch.fx < reaper.TrackFX_GetCount(tr) then
    local key = watch.track .. "_" .. watch.fx .. "_" .. reaper.TrackFX_GetNumParams(tr, watch.fx) .. "_" .. fxName(tr, watch.fx)
    if key ~= pub.mapKey then
      pub.mapKey = key
      paramCache = {}
      pub.params = ""
      reaper.SetExtState(SEC, "fxmap", mapString(tr, watch.fx), false)
    end
    local ps = paramsString(tr, watch.fx)
    if ps ~= pub.params then
      pub.params = ps
      reaper.SetExtState(SEC, "fxparams", ps, false)
    end
  end
end

---------------------------------------------------------------- routing
local function trackNumber(tr)
  if tr == reaper.GetMasterTrack(0) then return 0 end
  return math.floor(reaper.GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER"))
end

-- pista de destino (envío) o de origen (recepción) de un ruteo
local function otherTrack(tr, cat, i)
  local parm = (cat == -1) and "P_SRCTRACK" or "P_DESTTRACK"
  local function valid(p)
    if not p or type(p) == "number" then return nil end
    local ok, v = pcall(reaper.ValidatePtr2, 0, p, "MediaTrack*")
    if ok and v then return p end
    return nil
  end
  local ok, p = pcall(reaper.GetTrackSendInfo_Value, tr, cat, i, parm)
  if ok and valid(p) then return p end
  ok, p = pcall(reaper.GetSetTrackSendInfo, tr, cat, i, parm, 0)
  if ok and valid(p) then return p end
  if reaper.BR_GetMediaTrackSendInfo_Track then            -- SWS, si está instalado
    ok, p = pcall(reaper.BR_GetMediaTrackSendInfo_Track, tr, cat, i, (cat == -1) and 0 or 1)
    if ok and valid(p) then return p end
  end
  return nil
end

local function hwChanName(dst)
  local ch = math.floor(dst) & 1023
  local mono = (math.floor(dst) & 1024) ~= 0
  local n = reaper.GetOutputChannelName(ch) or ("Out " .. (ch + 1))
  if not mono then
    n = n .. " / " .. (reaper.GetOutputChannelName(ch + 1) or ("Out " .. (ch + 2)))
  end
  return n
end

local function routingInfo(tr)
  local items = {}
  local function collect(cat, letter)
    local n = reaper.GetTrackNumSends(tr, cat)
    for i = 0, n - 1 do
      local function g(k) return reaper.GetTrackSendInfo_Value(tr, cat, i, k) end
      local vol, pan, mute, mode = g("D_VOL"), g("D_PAN"), g("B_MUTE"), g("I_SENDMODE")
      local src, dst = math.floor(g("I_SRCCHAN")), math.floor(g("I_DSTCHAN"))
      local other, name = -1, ""
      if cat == 1 then
        name = hwChanName(dst)
      else
        local o = otherTrack(tr, cat, i)
        if o then other = trackNumber(o); name = trackName(o) end
      end
      items[#items + 1] = table.concat({
        letter, i, other, clean(name),
        string.format("%.5f", vol), string.format("%.4f", pan),
        (mute ~= 0) and 1 or 0, math.floor(mode), src, dst }, "~")
    end
  end
  collect(0, "s"); collect(-1, "r"); collect(1, "h")
  local main = -1
  if rwatch ~= 0 then main = (reaper.GetMediaTrackInfo_Value(tr, "B_MAINSEND") ~= 0) and 1 or 0 end
  return tostring(rwatch) .. "|" .. main .. "|" .. table.concat(items, "|")
end

local function hwString()
  local n = reaper.GetNumAudioOutputs()
  if n > 64 then n = 64 end
  local t = {}
  for i = 0, n - 1 do
    t[#t + 1] = i .. "~" .. clean(reaper.GetOutputChannelName(i) or ("Out " .. (i + 1)))
  end
  return tostring(rwatch) .. "|" .. table.concat(t, "|")
end

local function pollRWatch()
  local raw = reaper.GetExtState(SEC, "rwatch")
  if raw == lastRWatchRaw then return end
  lastRWatchRaw = raw
  rwatch = tonumber(raw:match("^(%d+)$"))          -- nil si la web mandó "off"
  rpub.info, rpub.hw = "", ""
  reaper.DeleteExtState(SEC, "rtinfo", false)
  reaper.DeleteExtState(SEC, "rthw", false)
end

local function pollRSet()
  local raw = reaper.GetExtState(SEC, "rset")
  if raw == "" or raw == lastRSetRaw then return end
  lastRSetRaw = raw
  local t, c, i, body = raw:match("^(%d+)_(%a)_(%d+)_(.-)_[^_]+$")
  if not t or CAT[c] == nil then return end
  local tr = getTrack(tonumber(t))
  if not tr then return end
  local cat, idx = CAT[c], tonumber(i)
  if idx >= reaper.GetTrackNumSends(tr, cat) then return end
  for k, v in body:gmatch("(%a):(%-?[%d%.]+)") do
    local parm, val = RPARM[k], tonumber(v)
    if parm and val then
      if k == "v" then
        if val < 0 then val = 0 elseif val > 4 then val = 4 end
      elseif k == "p" then
        if val < -1 then val = -1 elseif val > 1 then val = 1 end
      elseif k == "m" then
        val = (val ~= 0) and 1 or 0
      else
        val = math.floor(val)
      end
      reaper.SetTrackSendInfo_Value(tr, cat, idx, parm, val)
    end
  end
end

local function publishRouting()
  if not rwatch then return end
  local tr = getTrack(rwatch)
  if not tr then return end
  local s = routingInfo(tr)
  if s ~= rpub.info then
    rpub.info = s
    reaper.SetExtState(SEC, "rtinfo", s, false)
  end
  if rpub.hw == "" then
    rpub.hw = hwString()
    reaper.SetExtState(SEC, "rthw", rpub.hw, false)
  end
end

---------------------------------------------------------------- proyecto
local function updateProject()
  local project, fn = reaper.EnumProjects(-1)          -- proyecto de la pestaña activa
  local name = ""
  local pathState = (fn and fn ~= "") and hexencode(fn) or "-"
  if pathState ~= lastProjectPathState then
    reaper.SetExtState(SEC, "projectpath", pathState, false)
    lastProjectPathState = pathState
  end
  if fn and fn ~= "" then
    if fn ~= lastProjectPath then rememberProject(fn); lastProjectPath = fn end
    name = fn:match("([^/\\]+)%.[Rr][Pp][Pp]$") or fn:match("([^/\\]+)$") or ""
  end
  if name == "" then name = "Sin título" end
  if name ~= lastProject then
    reaper.SetExtState(SEC, "project", name, false)
    lastProject = name
  end
  local dirtyState = (project and reaper.IsProjectDirty(project) == 1) and "1" or "0"
  if dirtyState ~= lastProjectDirty then
    reaper.SetExtState(SEC, "projectdirty", dirtyState, false)
    lastProjectDirty = dirtyState
  end
end

-- Onda de la mezcla (estilo REAPER): picos máx/mín por canal (L/R), sumando todos los clips audibles.
-- Se recalcula SOLO cuando cambia la disposición de los clips (caché por firma).
local waveCache = { sig = "", row = "", seq = 0 }

local function waveRow(dur)
  local list, sigParts = {}, { string.format("%.3f", dur) }
  for i = 0, reaper.CountTracks(0) - 1 do
    local tr = reaper.GetTrack(0, i)
    if reaper.GetMediaTrackInfo_Value(tr, "B_MUTE") == 0 then
      for j = 0, reaper.GetTrackNumMediaItems(tr) - 1 do
        if #list >= 800 then break end
        local it = reaper.GetTrackMediaItem(tr, j)
        if reaper.GetMediaItemInfo_Value(it, "B_MUTE") == 0 then
          local take = reaper.GetActiveTake(it)
          local src = take and reaper.GetMediaItemTake_Source(take)
          if src then
            local e = {
              src = src,
              pos = reaper.GetMediaItemInfo_Value(it, "D_POSITION"),
              len = reaper.GetMediaItemInfo_Value(it, "D_LENGTH"),
              off = reaper.GetMediaItemTakeInfo_Value(take, "D_STARTOFFS"),
              rate = math.max(0.01, reaper.GetMediaItemTakeInfo_Value(take, "D_PLAYRATE")),
              vol = math.abs(reaper.GetMediaItemInfo_Value(it, "D_VOL") * reaper.GetMediaItemTakeInfo_Value(take, "D_VOL")),
            }
            list[#list + 1] = e
            sigParts[#sigParts + 1] = string.format("%.4f,%.4f,%.4f,%.4f,%.3f,%s", e.pos, e.len, e.off, e.rate, e.vol,
              tostring(reaper.GetMediaSourceFileName(src, "") or ""))
          end
        end
      end
    end
  end
  local sig = table.concat(sigParts, ";")
  -- Si REAPER todavía no ha generado los picos PCM, no fijar el resultado vacío:
  -- la siguiente pasada podrá obtener la onda cuando el caché de medios esté listo.
  if sig == waveCache.sig and waveCache.row ~= "" then return waveCache.row end

local bins = math.max(500, math.min(3000, math.floor(dur * 20)))
  local dt = dur / bins
  local mx, mn = { {}, {} }, { {}, {} }
  for c = 1, 2 do for b = 1, bins do mx[c][b] = 0; mn[c][b] = 0 end end
  local stereo, any = false, false

  for _, e in ipairs(list) do
    local n0 = math.max(0, math.ceil(e.pos / dt))
    local n1 = math.min(bins - 1, math.floor((e.pos + e.len) / dt) - 1)
    local cnt = n1 - n0 + 1
    if cnt >= 1 and e.len > 0 then
      local ch = math.max(1, math.min(2, reaper.GetMediaSourceNumChannels(e.src) or 1))
      local buf = reaper.new_array(cnt * ch * 2)
      local start = e.off + (n0 * dt - e.pos) * e.rate
      local ok, rv = pcall(reaper.PCM_Source_GetPeaks, e.src, 1 / (dt * e.rate), start, ch, cnt, 0, buf)
      local got = ok and rv and (rv & 0xFFFFF) or 0
      if got > 0 then
        any = true
        if ch >= 2 then stereo = true end
        for k = 1, math.min(got, cnt) do
          local b = n0 + k
          for c = 1, 2 do
            local cc = math.min(c, ch)
            local posi = (k - 1) * ch + cc
            local hi, lo = buf[posi] or 0, buf[cnt * ch + posi] or 0
            mx[c][b] = mx[c][b] + hi * e.vol
            mn[c][b] = mn[c][b] + lo * e.vol
          end
        end
      end
    end
  end

  local row = ""
  if any then
    -- Onda unica: se combinan L y R (maximo de los dos) para enviar la mitad de datos y dibujar una sola onda.
    local cmx, cmn = {}, {}
    local peak = 0
    for b = 1, bins do
      cmx[b] = math.max(mx[1][b], mx[2][b])
      cmn[b] = math.min(mn[1][b], mn[2][b])
      peak = math.max(peak, math.abs(cmx[b]), math.abs(cmn[b]))
    end
    if peak > 0 then
      local scale = 0.98 / peak
      local function enc(v)
        v = math.floor(v * scale * 255 + 0.5)
        if v < 0 then v = 0 elseif v > 255 then v = 255 end
        return v
      end
      local out = {}
      for b = 1, bins do out[b] = string.format("%02x%02x", enc(cmx[b]), enc(-cmn[b])) end
      row = table.concat({ "W", tostring(bins), "0", table.concat(out), "" }, "~")
    end
  end
  waveCache.sig, waveCache.row = sig, row
  return row
end

local function timelineString()
  local rows, maxEnd, itemCount = {}, reaper.GetProjectLength(0), 0
  local tracks = reaper.CountTracks(0)
  for i = 0, tracks - 1 do
    local tr = reaper.GetTrack(0, i)
    local idx = math.floor(reaper.GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER"))
    local depth = math.floor(reaper.GetMediaTrackInfo_Value(tr, "I_FOLDERDEPTH"))
    rows[#rows + 1] = "T~" .. idx .. "~" .. depth .. "~" .. hexencode(trackName(tr))
    local items = reaper.GetTrackNumMediaItems(tr)
    for j = 0, items - 1 do
      if itemCount >= 500 then break end
      local it = reaper.GetTrackMediaItem(tr, j)
      local pos = reaper.GetMediaItemInfo_Value(it, "D_POSITION")
      local len = reaper.GetMediaItemInfo_Value(it, "D_LENGTH")
      local take = reaper.GetActiveTake(it)
      local itemName = ""
      if take then local _, nm = reaper.GetSetMediaItemTakeInfo_String(take, "P_NAME", "", false); itemName = nm or "" end
      if itemName == "" then
        local src = take and reaper.GetMediaItemTake_Source(take)
        if src then local nm = reaper.GetMediaSourceFileName(src, ""); itemName = (nm or ""):match("([^/\\]+)$") or "" end
      end
      if itemName == "" then itemName = "Clip " .. tostring(j + 1) end
      local peaks = ""
      rows[#rows + 1] = table.concat({ "I", idx, string.format("%.4f", pos), string.format("%.4f", len), hexencode(itemName), peaks }, "~")
      maxEnd = math.max(maxEnd, pos + len)
      itemCount = itemCount + 1
    end
  end
  -- regiones (secciones de la canción) para la línea de tiempo compacta del master
  local _, nMarkers, nRegions = reaper.CountProjectMarkers(0)
  local total = (nMarkers or 0) + (nRegions or 0)
  local regCount = 0
  for m = 0, total - 1 do
    local _, isrgn, rpos, rend, rname, _, rcolor = reaper.EnumProjectMarkers3(0, m)
    if isrgn and regCount < 200 then
      local rgb = "8a94a6"
      if rcolor and rcolor ~= 0 then
        local r, g, b = reaper.ColorFromNative(rcolor & 0xFFFFFF)
        rgb = string.format("%02x%02x%02x", r, g, b)
      end
      rows[#rows + 1] = table.concat({ "R", string.format("%.4f", rpos), string.format("%.4f", rend), hexencode(rname or ""), rgb }, "~")
      maxEnd = math.max(maxEnd, rend)
      regCount = regCount + 1
    end
  end
  local dur = math.max(maxEnd, 1)
  local okw, wrow = pcall(waveRow, dur)
  if okw and wrow and wrow ~= "" then rows[#rows + 1] = wrow end
  return string.format("%.4f", dur) .. "|" .. table.concat(rows, "|")
end

local function folderMapString()
  local rows, stack = {}, {}
  for i = 0, reaper.CountTracks(0) - 1 do
    local tr = reaper.GetTrack(0, i)
    local depth = math.floor(reaper.GetMediaTrackInfo_Value(tr, "I_FOLDERDEPTH"))
    local parentId = stack[#stack] or 0
    local parentName, parentColor = "", 0
    if parentId > 0 then
      local parent = getTrack(parentId)
      parentName = hexencode(trackName(parent))
      parentColor = math.floor(reaper.GetMediaTrackInfo_Value(parent, "I_CUSTOMCOLOR"))
    end
    rows[#rows + 1] = table.concat({ tostring(i + 1), tostring(depth), tostring(parentId), parentName, tostring(parentColor) }, "~")
    if depth > 0 then for _ = 1, depth do stack[#stack + 1] = i + 1 end
    elseif depth < 0 then for _ = 1, -depth do table.remove(stack) end end
  end
  return #rows > 0 and table.concat(rows, "|") or "-"
end

local function timelinePosition()
  local playing = (reaper.GetPlayState() & 1) ~= 0
  local cursor = reaper.GetCursorPosition()
  local pos = playing and reaper.GetPlayPosition2() or cursor
  return string.format("%.3f|%d|%.3f", pos, playing and 1 or 0, cursor)
end

-- Publica el tempo y el compás activos, teniendo en cuenta los marcadores de tempo del proyecto.
local lastMetainfo = ""
local function publishMetainfo()
  local pos = (reaper.GetPlayState() & 1) ~= 0 and reaper.GetPlayPosition2() or reaper.GetCursorPosition()
  local bpm = reaper.Master_GetTempo()
  if reaper.CountTempoTimeSigMarkers and reaper.GetTempoTimeSigMarker then
    for i = 0, reaper.CountTempoTimeSigMarkers(0) - 1 do
      local ok, at, _, _, markerBpm = reaper.GetTempoTimeSigMarker(0, i)
      if ok and at <= pos and markerBpm and markerBpm > 0 then bpm = markerBpm elseif ok and at > pos then break end
    end
  end
  local num, den = reaper.TimeMap_GetTimeSigAtTime(0, pos)
  num, den = math.max(1, math.floor(num or 4)), math.max(1, math.floor(den or 4))
  local value = string.format("%.3f|%d|%d", bpm or 120, num, den)
  if value ~= lastMetainfo then
    lastMetainfo = value
    reaper.SetExtState(SEC, "metainfo", value, false)
  end
end

---------------------------------------------------------------- carátulas (se guardan en REAPER)
-- Cada carátula es un archivo de texto en <REAPER resource path>/WebControlCovers/<hash>.txt
-- (contiene una URL o una imagen en base64; "-" = sin carátula / ya se intentó buscar).
local COVER_DIR = reaper.GetResourcePath() .. "/WebControlCovers"
local coverCache, coverPublished = {}, {}
local pendingCover, lastCoverRaw = nil, ""

local function coverHash(s)
  local h1, h2 = 5381, 52711
  for i = 1, #s do
    local b = s:byte(i)
    h1 = (h1 * 33 + b) % 4294967296
    h2 = (h2 * 131 + b + i) % 4294967296
  end
  return string.format("%08x%08x", h1, h2)
end

local function coverFile(hash) return COVER_DIR .. "/" .. hash .. ".txt" end

local function coverLoad(hash)
  local c = coverCache[hash]
  if c then return c end
  c = { state = "n", value = "", ver = "0" }
  local f = io.open(coverFile(hash), "rb")
  if f then
    local v = f:read("*a") or ""
    f:close()
    v = v:gsub("%s+$", "")
    if v == "-" then c.state = "e"
    elseif v ~= "" then c.state = "s"; c.value = v; c.ver = coverHash(v):sub(1, 10) end
  end
  coverCache[hash] = c
  return c
end

local function coverSave(hash, value)
  reaper.RecursiveCreateDirectory(COVER_DIR, 0)
  local f = io.open(coverFile(hash), "wb")
  if not f then return false end
  f:write(value)
  f:close()
  coverCache[hash] = nil
  return true
end

-- La web manda la carátula en bloques hexadecimales (cvc_<id>_<n>) y luego la orden coverset = <hash>|<id>|<total>
local function pollCoverSet()
  local raw = reaper.GetExtState(SEC, "coverset")
  if raw ~= "" and raw ~= lastCoverRaw then
    lastCoverRaw = raw
    reaper.DeleteExtState(SEC, "coverset", false)
    local hash, id, total = raw:match("^(%x+)|(%w+)|(%d+)$")
    if hash and #hash == 16 and tonumber(total) > 0 and tonumber(total) <= 200 then
      pendingCover = { hash = hash, id = id, total = tonumber(total), t = reaper.time_precise() }
    end
  end
  if not pendingCover then return end
  local p, parts, complete = pendingCover, {}, true
  for ix = 0, p.total - 1 do
    local c = reaper.GetExtState(SEC, "cvc_" .. p.id .. "_" .. ix)
    if c == "" then complete = false; break end
    parts[#parts + 1] = c
  end
  if complete or reaper.time_precise() - p.t > 6 then
    for ix = 0, p.total - 1 do reaper.DeleteExtState(SEC, "cvc_" .. p.id .. "_" .. ix, false) end
    pendingCover = nil
    if complete then
      local value = hexdecode(table.concat(parts))
      if value ~= "" and #value <= 300000 then coverSave(p.hash, value) end
    end
  end
end

---------------------------------------------------------------- canciones (pestañas de proyecto)
local lastSongs = ""
local lastPlaySong = ""
local function publishSongs()
  local cur = reaper.EnumProjects(-1)
  local rows, active, i = {}, 0, 0
  local playIdx, pausedIdx
  while true do
    local proj, fn = reaper.EnumProjects(i)
    if not proj then break end
    if proj == cur then active = i end
    local pst = reaper.GetPlayStateEx(proj) or 0
    if (pst & 1) ~= 0 then playIdx = playIdx or i elseif (pst & 2) ~= 0 and not pausedIdx then pausedIdx = i end
    local has = fn and fn ~= ""
    local name = has and (fn:match("([^/\\]+)%.[Rr][Pp][Pp]$") or fn:match("([^/\\]+)$") or fn) or "Sin título"
    local hash = coverHash(has and hexencode(fn) or ("name:" .. name))
    local cv = coverLoad(hash)
    if cv.state == "s" and not coverPublished[cv.ver] then
      reaper.SetExtState(SEC, "cv_" .. cv.ver, cv.value, false)
      coverPublished[cv.ver] = true
    end
    rows[#rows + 1] = hexencode(name) .. "~" .. (has and hexencode(fn) or "-") .. "~" .. hash .. "~" .. cv.state .. "~" .. cv.ver
    i = i + 1
  end
  local ps = playIdx and (playIdx .. "|1") or (pausedIdx and (pausedIdx .. "|2")) or "-1|0"
  if ps ~= lastPlaySong then lastPlaySong = ps; reaper.SetExtState(SEC, "playsong", ps, false) end
  local s = active .. "|" .. table.concat(rows, "|")
  if s ~= lastSongs then lastSongs = s; reaper.SetExtState(SEC, "songs", s, false) end
end

---------------------------------------------------------------- bucle principal
local lastSongsScan, lastMetainfoScan, tlSeenProject, tlSeenState, tlChangedAt = 0, 0, nil, -1, 0
local waveRetryUntil, lastWaveRetry, waveRetryCount = 0, 0, 0

local function loop()
  updateProject()
  do   -- la lista de canciones no necesita recalcularse en cada ciclo (ahorra CPU del hilo principal de REAPER)
    local tnow = reaper.time_precise()
    if tnow - lastSongsScan > 0.25 then lastSongsScan = tnow; pcall(publishSongs) end
  end

  local cmd = reaper.GetExtState(SEC, "cmd")
  if cmd ~= "" then
    reaper.DeleteExtState(SEC, "cmd", false)
    local nonce = cmd:match("_(%d+)$") or "0"
    local ok, err = pcall(handle, cmd, nonce)
    if not ok then result(nonce, false, "Error: " .. tostring(err)) end
  end

  pcall(pollPad)
  pcall(pollJump)

  pollUpload()
  pcall(pollCoverSet)
  pollWatch()
  pollSet()
  pollEqSet()
  pollEqReset()
  pollRWatch()
  pollRSet()

  local now = reaper.time_precise()
  if now - lastMetainfoScan > 0.25 then
    lastMetainfoScan = now
    pcall(publishMetainfo)
  end
  if now - lastTimelineScan > 0.15 then
    lastTimelineScan = now
    local project = reaper.EnumProjects(-1)
    local state = project and reaper.GetProjectStateChangeCount(project) or -1
    local projectChanged = project ~= lastTimelineProject
    if projectChanged then
      -- Evitar reutilizar la firma de onda de la pestaña anterior.
      waveCache.sig, waveCache.row, lastTimeline = "", "", ""
      waveRetryUntil, lastWaveRetry, waveRetryCount = now + 3, now, 0
    end
    -- mover un fader cambia el estado del proyecto muchas veces por segundo: se espera a que se estabilice
    -- para recalcular la línea de tiempo (así el arrastre de faders no compite con ese trabajo)
    if project ~= tlSeenProject or state ~= tlSeenState then tlSeenProject, tlSeenState, tlChangedAt = project, state, now end
    local retryWave = now < waveRetryUntil and waveRetryCount < 3
      and lastTimeline:find("|I~", 1, true) and not lastTimeline:find("|W~", 1, true)
      and now - lastWaveRetry >= 0.8
    if ((projectChanged or state ~= lastTimelineState)
       and (projectChanged or now - tlChangedAt > 0.35)) or retryWave then
      reaper.SetExtState(SEC, "foldermap", folderMapString(), false)
      local ok3, timeline = pcall(timelineString)
      if ok3 and timeline ~= lastTimeline then
        lastTimeline = timeline
        reaper.SetExtState(SEC, "timeline", timeline, false)
        waveCache.seq = waveCache.seq + 1
        reaper.SetExtState(SEC, "timelinesig", os.time() .. "-" .. waveCache.seq, false)
        if timeline:find("|I~", 1, true) and not timeline:find("|W~", 1, true) then
          waveRetryUntil, lastWaveRetry, waveRetryCount = now + 3, now, 0
        end
      end
      if retryWave then lastWaveRetry, waveRetryCount = now, waveRetryCount + 1 end
      if lastTimeline:find("|W~", 1, true) then waveRetryUntil = 0 end
      if waveRetryCount >= 3 then waveRetryUntil = 0 end
      lastTimelineProject, lastTimelineState = project, state
    end
  end
  if now - lastTimelinePosScan > 0.08 then
    lastTimelinePosScan = now
    local pos = timelinePosition()
    if pos ~= lastTimelinePos then
      lastTimelinePos = pos
      reaper.SetExtState(SEC, "timelinepos", pos, false)
    end
  end
  if now - lastPublish > 0.05 then
    lastPublish = now
    local ok = pcall(publish)
    if not ok then pub.list, pub.params, pub.mapKey = "", "", "" end
    local ok2 = pcall(publishRouting)
    if not ok2 then rpub.info, rpub.hw = "", "" end
  end

  reaper.defer(loop)
end

-- descarta órdenes viejas que hayan quedado pendientes de una sesión anterior
reaper.DeleteExtState(SEC, "cmd", false)
reaper.DeleteExtState(SEC, "upload", false)
reaper.DeleteExtState(SEC, "coverset", false)
reaper.DeleteExtState(SEC, "uploadack", false)
reaper.DeleteExtState(SEC, "set", false)
reaper.DeleteExtState(SEC, "watch", false)
reaper.DeleteExtState(SEC, "rwatch", false)
reaper.DeleteExtState(SEC, "rset", false)
reaper.DeleteExtState(SEC, "rtinfo", false)
reaper.DeleteExtState(SEC, "rthw", false)
do
  local saved = reaper.GetExtState(SEC, "recent")
  local seen = {}
  for h in saved:gmatch("[^|]+") do
    local path = hexdecode(h)
    if path ~= "" and reaper.file_exists(path) then recentProjects[#recentProjects + 1] = path; seen[path] = true end
  end
  local ini = io.open(reaper.get_ini_file(), "r")
  if ini then
    for line in ini:lines() do
      local path = line:match("^recent%d+=(.+)$")
      if path and not seen[path] and reaper.file_exists(path) and #recentProjects < 20 then
        recentProjects[#recentProjects + 1] = path
        seen[path] = true
      end
    end
    ini:close()
  end
end
reaper.SetExtState(SEC, "helper", HELPER_VERSION, false)
reaper.atexit(function() reaper.DeleteExtState(SEC, "helper", false) end)
clearPublished()
loop()
