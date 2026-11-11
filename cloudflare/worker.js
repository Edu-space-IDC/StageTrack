const CHUNK_BYTES = 4 * 1024 * 1024;
const MAX_PACKAGE_BYTES = 2 * 1024 * 1024 * 1024;
const MAX_COVER_BYTES = 8 * 1024 * 1024;
const SESSION_TTL = 7200;

function json(data, status = 200, origin = "*") {
  return new Response(JSON.stringify(data), { status, headers: corsHeaders(origin, { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" }) });
}
function corsHeaders(origin, extra = {}) {
  return { "Access-Control-Allow-Origin": origin, "Access-Control-Allow-Methods": "GET,POST,PUT,DELETE,OPTIONS", "Access-Control-Allow-Headers": "Content-Type,Authorization,Range,X-Upload-Offset", "Access-Control-Expose-Headers": "Content-Length,Content-Range,Accept-Ranges,Content-Disposition", "Access-Control-Max-Age": "86400", Vary: "Origin", ...extra };
}
function allowedOrigin(request, env) {
  const origin = request.headers.get("Origin");
  if (!origin) return "*";
  const allowed = String(env.CORS_ORIGINS || "https://stagetrack-project.vercel.app,https://stagetrack-ten.vercel.app")
    .split(",").map((value) => value.trim()).filter(Boolean);
  try {
    const url = new URL(origin);
    if (url.protocol === "http:" && ["localhost", "127.0.0.1", "[::1]"].includes(url.hostname) && Number(url.port) >= 47821 && Number(url.port) <= 47840) return origin;
  } catch {}
  return allowed.includes(origin) ? origin : "null";
}
async function scriptCall(env, action, data = {}) {
  if (!env.APPS_SCRIPT_URL || !env.API_TOKEN) throw new Error("Falta configurar Apps Script en Cloudflare.");
  const response = await fetch(env.APPS_SCRIPT_URL, {
    method: "POST", headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ action, token: env.API_TOKEN, ...data }),
    redirect: "follow",
  });
  const result = await response.json();
  if (!response.ok || result.ok !== true) throw new Error(result.error || `Apps Script respondió ${response.status}.`);
  return result;
}
function stateCacheKey(env) {
  const endpoint = new URL(env.APPS_SCRIPT_URL);
  return new Request(`${endpoint.origin}/__stagetracks_state_cache?script=${encodeURIComponent(endpoint.pathname)}`);
}
async function getState(env) {
  if (!env.APPS_SCRIPT_URL || !env.API_TOKEN) return scriptCall(env, "load-state");
  const cache = caches.default, key = stateCacheKey(env);
  const cached = await cache.match(key);
  const state = cached ? await cached.json() : await scriptCall(env, "load-state");
  // Older community records already contain ZIP archives, but some were
  // published with a .rpseq download name. Migrate their index entries lazily.
  let migrated = false;
  for (const item of state.sequences || []) {
    const name = String(item.downloadName || `${item.name || "StageTracks"}.rpseq`);
    if (!/\.zip$/i.test(name)) {
      item.downloadName = name.replace(/\.[^.]*$/, "") + ".zip";
      migrated = true;
    }
  }
  if (migrated) await scriptCall(env, "save-sequences", { sequences: state.sequences });
  if (!cached || migrated) await cache.put(key, new Response(JSON.stringify(state), { headers: { "Cache-Control": "public, max-age=60" } }));
  return state;
}
async function saveState(env, state) {
  await scriptCall(env, "save-sequences", { sequences: state.sequences });
  await caches.default.delete(stateCacheKey(env));
}
function normalized(value) { return String(value || "").normalize("NFD").replace(/[\u0300-\u036f]/g, "").toLowerCase().replace(/[^a-z0-9]+/g, " ").trim(); }
function duplicate(entries, name, artist, key, ignore = "") {
  const n = normalized(name), a = normalized(artist), k = normalized(key);
  return entries.find((item) => item.id !== ignore && item.status === "approved" && normalized(item.name) === n && (normalized(item.artist) === a || normalized(item.key) === k));
}
async function drivePublicFetch(request, fileId) {
  const headers = new Headers();
  const range = request.headers.get("Range");
  if (range) headers.set("Range", range);
  headers.set("Accept", "application/octet-stream,image/*,*/*");
  return fetch(`https://drive.usercontent.google.com/download?id=${encodeURIComponent(fileId)}&export=download&confirm=t`, { headers, redirect: "follow" });
}
async function driveRange(fileId, start, end) {
  let response;
  for (let attempt = 0; attempt < 6; attempt++) {
    response = await fetch(`https://drive.usercontent.google.com/download?id=${encodeURIComponent(fileId)}&export=download&confirm=t`, { headers: { Accept: "application/octet-stream,*/*", Range: `bytes=${start}-${end}` }, redirect: "follow" });
    if (response.status === 206) return new Uint8Array(await response.arrayBuffer());
    try { await response.body?.cancel(); } catch {}
    if (![401, 403, 404].includes(response.status) || attempt === 5) break;
    await new Promise((resolve) => setTimeout(resolve, 500 * (attempt + 1)));
  }
  throw new Error(`Google Drive no permitió validar el paquete por rangos (${response?.status || "sin respuesta"}).`);
}
function u16(view, offset) { return view.getUint16(offset, true); }
function u32(view, offset) { return view.getUint32(offset, true); }
async function readDriveZipEntry(fileId, entry) {
  const header = await driveRange(fileId, entry.localOffset, entry.localOffset + 29), headerView = new DataView(header.buffer, header.byteOffset, header.byteLength);
  if (u32(headerView, 0) !== 0x04034b50) throw new Error("Un archivo del paquete ZIP está dañado.");
  const start = entry.localOffset + 30 + u16(headerView, 26) + u16(headerView, 28), packed = await driveRange(fileId, start, start + entry.compressed - 1);
  if (entry.method === 0) return packed;
  if (entry.method !== 8 || typeof DecompressionStream === "undefined") throw new Error("El paquete usa una compresión no compatible.");
  const reader = new Blob([packed]).stream().pipeThrough(new DecompressionStream("deflate-raw")).getReader(), chunks = []; let total = 0;
  while (true) { const part = await reader.read(); if (part.done) break; total += part.value.byteLength; if (total > entry.expanded) { await reader.cancel(); throw new Error("Un archivo del paquete ZIP excedió su tamaño declarado."); } chunks.push(part.value); }
  const bytes = new Uint8Array(total); let offset = 0; for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.byteLength; }
  if (bytes.byteLength !== entry.expanded) throw new Error("Un archivo del paquete ZIP está incompleto.");
  return bytes;
}
async function inspectDriveZip(fileId, size) {
  const tailLength = Math.min(size, 65_557), tail = await driveRange(fileId, size - tailLength, size - 1), tailView = new DataView(tail.buffer, tail.byteOffset, tail.byteLength);
  let eocd = -1;
  for (let i = tail.length - 22; i >= 0; i--) if (u32(tailView, i) === 0x06054b50) { eocd = i; break; }
  if (eocd < 0) throw new Error("El archivo no es un paquete ZIP válido.");
  const entryCount = u16(tailView, eocd + 10), directorySize = u32(tailView, eocd + 12), directoryOffset = u32(tailView, eocd + 16);
  if (entryCount < 1 || entryCount > 40_000 || directorySize > 16 * 1024 * 1024 || directoryOffset + directorySize > size) throw new Error("El paquete supera los límites de seguridad o usa un ZIP no compatible.");
  const directoryBytes = await driveRange(fileId, directoryOffset, directoryOffset + directorySize - 1), view = new DataView(directoryBytes.buffer, directoryBytes.byteOffset, directoryBytes.byteLength), decoder = new TextDecoder("utf-8", { fatal: false });
  const entries = new Map(); let pos = 0, expandedTotal = 0, manifestEntry = null;
  for (let i = 0; i < entryCount; i++) {
    if (pos + 46 > directoryBytes.length || u32(view, pos) !== 0x02014b50) throw new Error("El directorio ZIP está dañado.");
    const flags = u16(view, pos + 8), method = u16(view, pos + 10), compressed = u32(view, pos + 20), expanded = u32(view, pos + 24), nameLength = u16(view, pos + 28), extraLength = u16(view, pos + 30), commentLength = u16(view, pos + 32), localOffset = u32(view, pos + 42);
    if (flags & 1) throw new Error("No se admiten paquetes ZIP cifrados.");
    if ([compressed, expanded, localOffset].includes(0xffffffff)) throw new Error("Este ZIP de 64 bits no es compatible.");
    const name = decoder.decode(directoryBytes.subarray(pos + 46, pos + 46 + nameLength)).replaceAll("\\", "/"), clean = name.split("/").filter((part) => part && part !== ".").join("/");
    if (!name || name.startsWith("/") || /^[A-Za-z]:/.test(name) || name.split("/").includes("..")) throw new Error("El paquete contiene una ruta no permitida.");
    if (clean && !name.endsWith("/")) entries.set(clean, { flags, method, compressed, expanded, localOffset });
    expandedTotal += expanded;
    if (expandedTotal > 8 * 1024 ** 3) throw new Error("El paquete supera los límites de seguridad.");
    if (/\.rpseq$/i.test(name)) { if (manifestEntry) throw new Error("El paquete debe contener exactamente un archivo .rpseq."); manifestEntry = entries.get(clean); }
    pos += 46 + nameLength + extraLength + commentLength;
  }
  if (pos !== directoryBytes.length || !manifestEntry || manifestEntry.expanded > 8 * 1024 * 1024 || manifestEntry.compressed > 16 * 1024 * 1024) throw new Error("El paquete debe incluir un archivo .rpseq válido de hasta 8 MB.");
  const manifestBytes = await readDriveZipEntry(fileId, manifestEntry);
  if (manifestBytes.byteLength > 8 * 1024 * 1024 || manifestBytes.byteLength !== manifestEntry.expanded) throw new Error("El manifiesto de secuencia está dañado o supera 8 MB.");
  let manifest;
  try { manifest = JSON.parse(new TextDecoder().decode(manifestBytes)); } catch { throw new Error("El archivo .rpseq no contiene JSON válido."); }
  if (!Array.isArray(manifest.songs) || !manifest.songs.length) throw new Error("El paquete no incluye canciones en el archivo .rpseq.");
  const referenced = [];
  for (const song of manifest.songs) {
    if (!song || typeof song.name !== "string" || !Array.isArray(song.tracks)) throw new Error("El paquete contiene una secuencia con estructura inválida.");
    if (song.cover) {
      const coverPath = String(song.cover).replaceAll("\\", "/");
      if (coverPath.startsWith("/") || /^[A-Za-z]:/.test(coverPath) || coverPath.split("/").includes("..")) throw new Error("La carátula debe usar una ruta relativa dentro del paquete.");
      referenced.push(coverPath);
    }
    for (const track of song.tracks) for (const item of track.items || []) {
      const file = item.file || item.originalFile;
      if (!file) continue;
      const normalized = String(file).replaceAll("\\", "/");
      if (normalized.startsWith("/") || /^[A-Za-z]:/.test(normalized) || normalized.split("/").includes("..")) throw new Error("Los audios deben usar rutas relativas dentro del paquete.");
      if (!/\.(mp3|wav|wave|flac|ogg|m4a|aif|aiff|wma)$/i.test(normalized)) throw new Error("El paquete incluye una pista en un formato no compatible.");
      referenced.push(normalized);
    }
  }
  const missing = referenced.find((file) => !entries.has(file.split("/").filter((part) => part && part !== ".").join("/")));
  if (missing) throw new Error(`Falta el audio ${missing}. Incluye todas las pistas referenciadas en el ZIP.`);
  const first = manifest.songs[0], tempo = Number(first.tempo), coverPath = String(first.cover || "").replaceAll("\\", "/");
  let embeddedCover = null;
  if (coverPath) {
    const coverEntry = entries.get(coverPath.split("/").filter((part) => part && part !== ".").join("/"));
    if (coverEntry && coverEntry.expanded > MAX_COVER_BYTES) throw new Error("La carátula del paquete supera el límite de 8 MB.");
    if (coverEntry) {
      const bytes = await readDriveZipEntry(fileId, coverEntry);
      const mime = bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff ? "image/jpeg" : bytes.subarray(0, 8).join(",") === "137,80,78,71,13,10,26,10" ? "image/png" : String.fromCharCode(...bytes.subarray(0, 4)) === "RIFF" && String.fromCharCode(...bytes.subarray(8, 12)) === "WEBP" ? "image/webp" : "";
      if (!mime) throw new Error("La carátula dentro del paquete no es una imagen válida.");
      embeddedCover = { bytes, mime };
    }
  }
  return { songCount: manifest.songs.length, bpm: tempo >= 40 && tempo <= 240 ? Math.round(tempo) : null, meter: `${first.timeSigNum || 4}/${first.timeSigDen || 4}`, embeddedCover };
}
async function beginDriveUpload(env, name, mimeType, size) {
  const result = await scriptCall(env, "begin-upload", { name, mimeType, size });
  if (!result.uploadUrl) throw new Error("Apps Script no devolvió la sesión de Google Drive.");
  return { uploadUrl: result.uploadUrl, size, received: 0, fileId: "", name, mimeType };
}
async function beginUpload(request, env, origin) {
  const body = await request.json();
  const updateSequenceId = String(body.updateSequenceId || "");
  const adminPublish = body.adminPublish === true;
  if (updateSequenceId || adminPublish) {
    const access = await adminSession(request, env);
    if (!access) return json({ error: "Inicia sesión como administrador para subir archivos." }, 401, origin);
    if (access.mustChange) return json({ error: "Cambia el PIN inicial antes de usar el panel." , code: "PIN_CHANGE_REQUIRED" }, 403, origin);
    if (updateSequenceId) {
      const state = await getState(env);
      if (!state.sequences.some((item) => item.id === updateSequenceId)) return json({ error: "No encontramos esa secuencia." }, 404, origin);
    }
  }
  const packageSize = Number(body.packageSize), coverSize = Number(body.coverSize || 0);
  if (!Number.isSafeInteger(packageSize) || packageSize < 1 || packageSize > MAX_PACKAGE_BYTES) return json({ error: "El paquete debe pesar hasta 2 GB." }, 413, origin);
  if (!Number.isSafeInteger(coverSize) || coverSize < 0 || coverSize > MAX_COVER_BYTES) return json({ error: "La carátula debe pesar hasta 8 MB." }, 413, origin);
  if (!/\.(zip|rpseq)$/i.test(String(body.packageName || ""))) return json({ error: "Adjunta un paquete .rpseq o .zip." }, 400, origin);
  const id = crypto.randomUUID();
  const files = { package: await beginDriveUpload(env, String(body.packageName).slice(0, 180), "application/octet-stream", packageSize) };
  if (coverSize) {
    const name = String(body.coverName || "cover.png").slice(0, 180);
    const mime = /\.jpe?g$/i.test(name) ? "image/jpeg" : /\.webp$/i.test(name) ? "image/webp" : "image/png";
    files.cover = await beginDriveUpload(env, name, mime, coverSize);
  }
  const metadata = { ...body };
  for (const field of ["packageSize", "coverSize", "packageName", "coverName"]) delete metadata[field];
  metadata.adminAction = Boolean(updateSequenceId || adminPublish);
  await env.COMMUNITY_KV.put(`upload:${id}`, JSON.stringify({ id, metadata, files, createdAt: Date.now() }), { expirationTtl: SESSION_TTL });
  return json({ uploadId: id, chunkSize: CHUNK_BYTES }, 201, origin);
}
async function uploadChunk(request, env, id, field, part, origin) {
  const session = await env.COMMUNITY_KV.get(`upload:${id}`, "json");
  const file = session?.files?.[field], partNumber = Number(part);
  if (!session || !file) return json({ error: "La sesión de carga venció. Inicia la publicación otra vez." }, 404, origin);
  if (session.metadata?.adminAction) {
    const access = await adminSession(request, env);
    if (!access || access.mustChange) return json({ error: "La sesión de administrador venció. Inicia sesión de nuevo." }, 401, origin);
  }
  if (file.fileId) return json({ received: file.size, total: file.size }, 200, origin);
  if (!Number.isSafeInteger(partNumber) || partNumber < 0) return json({ error: "Número de bloque inválido." }, 400, origin);
  const offset = partNumber * CHUNK_BYTES;
  if (offset !== file.received) return json({ error: "El bloque no llegó en el orden esperado." }, 409, origin);
  const length = Number(request.headers.get("Content-Length"));
  if (!Number.isSafeInteger(length) || length < 1 || length > CHUNK_BYTES || offset + length > file.size) return json({ error: "El bloque debe pesar hasta 4 MB y caber en el archivo." }, 400, origin);
  const end = offset + length - 1;
  const upstream = await fetch(file.uploadUrl, {
    method: "PUT", headers: { "Content-Length": String(length), "Content-Range": `bytes ${offset}-${end}/${file.size}`, "Content-Type": file.mimeType },
    body: request.body, redirect: "manual",
  });
  if (upstream.status === 308) {
    const range = upstream.headers.get("Range") || "";
    const last = Number(range.match(/-(\d+)$/)?.[1]);
    if (!Number.isFinite(last) || last + 1 !== offset + length) return json({ error: "Google Drive confirmó un rango inesperado." }, 502, origin);
    file.received = last + 1;
    await env.COMMUNITY_KV.put(`upload:${id}`, JSON.stringify(session), { expirationTtl: SESSION_TTL });
    return json({ received: file.received, total: file.size }, 200, origin);
  }
  if (upstream.status >= 200 && upstream.status < 300) {
    const uploaded = await upstream.json();
    if (!uploaded.id) return json({ error: "Google Drive no devolvió el ID del archivo." }, 502, origin);
    await scriptCall(env, "finish-upload", { fileId: uploaded.id });
    file.fileId = uploaded.id; file.received = file.size; delete file.uploadUrl;
    await env.COMMUNITY_KV.put(`upload:${id}`, JSON.stringify(session), { expirationTtl: SESSION_TTL });
    return json({ received: file.size, total: file.size }, 200, origin);
  }
  const detail = await upstream.text();
  return json({ error: `Google Drive rechazó el bloque (${upstream.status}). ${detail.slice(0, 300)}` }, 502, origin);
}
async function completeUpload(request, env, id, origin) {
  const key = `upload:${id}`, session = await env.COMMUNITY_KV.get(key, "json");
  if (!session) return json({ error: "La sesión de carga venció." }, 404, origin);
  if (session.metadata?.adminAction) {
    const access = await adminSession(request, env);
    if (!access || access.mustChange) return json({ error: "La sesión de administrador venció. Inicia sesión de nuevo." }, 401, origin);
  }
  const pkg = session.files.package, cover = session.files.cover;
  if (!pkg.fileId || pkg.received !== pkg.size || (cover && (!cover.fileId || cover.received !== cover.size))) return json({ error: "La carga no terminó de llegar a Google Drive." }, 400, origin);
  const meta = session.metadata;
  const ownedFileIds = new Set([pkg.fileId, cover?.fileId].filter(Boolean));
  let item;
  try {
    // Drive's anonymous range endpoint only serves link-shared files. The file ID
    // stays in Worker KV; invalid uploads are trashed immediately in the catch.
    await scriptCall(env, "publish-files", { fileIds: [pkg.fileId] });
    const info = await inspectDriveZip(pkg.fileId, pkg.size);
    const name = String(meta.name || "").trim(), artist = String(meta.artist || "").trim(), keyValue = String(meta.key || "").slice(0, 8);
    if (name.length < 2 || name.length > 120 || artist.length > 120) throw new Error("Escribe el nombre de la canción y un artista de máximo 120 caracteres.");
    const bpm = info.bpm || Math.round(Number(meta.bpm));
    const meter = info.bpm ? info.meter : String(meta.meter || "4/4");
    if (!Number.isFinite(bpm) || bpm < 40 || bpm > 240 || !/^\d{1,2}\/(1|2|4|8|16|32)$/.test(meter)) throw new Error("Revisa el BPM y el compás.");
    const ext = ".zip", updateSequenceId = String(meta.updateSequenceId || "");
    const state = updateSequenceId ? await getState(env) : null;
    const existingIndex = updateSequenceId ? state.sequences.findIndex((row) => row.id === updateSequenceId) : -1;
    const existing = existingIndex >= 0 ? state.sequences[existingIndex] : null;
    if (updateSequenceId && !existing) throw new Error("No encontramos esa secuencia.");
    const duplicateEntry = duplicate(state?.sequences || [], name, artist, keyValue, updateSequenceId);
    if (duplicateEntry) throw new Error("Ya existe otra secuencia con ese nombre y autor o tonalidad.");
    let coverDriveFileId = cover?.fileId || "";
    if (!coverDriveFileId && info.embeddedCover) {
      const embedded = info.embeddedCover, coverExt = embedded.mime === "image/jpeg" ? ".jpg" : embedded.mime === "image/png" ? ".png" : ".webp";
      const upload = await beginDriveUpload(env, `${crypto.randomUUID()}${coverExt}`, embedded.mime, embedded.bytes.byteLength);
      const response = await fetch(upload.uploadUrl, { method: "PUT", headers: { "Content-Length": String(embedded.bytes.byteLength), "Content-Range": `bytes 0-${embedded.bytes.byteLength - 1}/${embedded.bytes.byteLength}`, "Content-Type": embedded.mime }, body: new Blob([embedded.bytes]).stream(), redirect: "manual" });
      if (response.status < 200 || response.status >= 300) throw new Error(`Google Drive no pudo guardar la carátula del paquete (${response.status}).`);
      const uploaded = await response.json();
      if (!uploaded.id) throw new Error("Google Drive no devolvió el ID de la carátula.");
      coverDriveFileId = uploaded.id; ownedFileIds.add(uploaded.id);
      await scriptCall(env, "finish-upload", { fileId: uploaded.id });
    }
    if (!coverDriveFileId && existing && !info.embeddedCover) coverDriveFileId = existing.coverDriveFileId || "";
    item = existing
      ? { ...existing, name, artist, bpm, meter, key: keyValue, genre: String(meta.genre || "Otro").slice(0, 40), packageDriveFileId: pkg.fileId, coverDriveFileId, downloadName: `${name.replace(/[\\/:*?"<>|]/g, "-")}${ext}`, fileSize: pkg.size, songCount: info.songCount }
      : { id: crypto.randomUUID(), name, artist, bpm, meter, key: keyValue, genre: String(meta.genre || "").slice(0, 40), cover: "", packageFile: "", packageDriveFileId: pkg.fileId, coverDriveFileId, downloadName: `${name.replace(/[\\/:*?"<>|]/g, "-")}${ext}`, fileSize: pkg.size, songCount: info.songCount, downloads: 0, status: "approved", createdAt: new Date().toISOString() };
    await scriptCall(env, "publish-files", { fileIds: [pkg.fileId, coverDriveFileId].filter(Boolean) });
    if (existing) {
      state.sequences[existingIndex] = item;
      await saveState(env, state);
      for (const oldId of [existing.packageDriveFileId, existing.coverDriveFileId]) {
        if (oldId && oldId !== pkg.fileId && oldId !== coverDriveFileId) await scriptCall(env, "delete-file", { fileId: oldId }).catch(() => {});
      }
    } else {
      await scriptCall(env, "publish-sequence", { item });
    }
  } catch (error) {
    await Promise.all([...ownedFileIds].map((fileId) => scriptCall(env, "delete-file", { fileId }).catch(() => {})));
    await env.COMMUNITY_KV.delete(key).catch(() => {});
    throw error;
  }
  await caches.default.delete(stateCacheKey(env)).catch(() => {});
  await env.COMMUNITY_KV.delete(key).catch(() => {});
  return json({ id: item.id, status: item.status, message: "Secuencia publicada." }, 201, origin);
}
async function updateSequence(request, env, id, origin) {
  const state = await getState(env), item = state.sequences.find((row) => row.id === id && row.status === "approved");
  if (!item) return json({ error: "No encontramos esa secuencia." }, 404, origin);
  const form = await request.formData(), name = String(form.get("name") || "").trim(), artist = String(form.get("artist") || "").trim(), keyValue = String(form.get("key") || "").slice(0, 8), bpm = Math.round(Number(form.get("bpm"))), meter = String(form.get("meter") || "4/4");
  if (name.length < 2 || name.length > 120 || artist.length > 120 || !Number.isFinite(bpm) || bpm < 40 || bpm > 240 || !/^\d{1,2}\/(1|2|4|8|16|32)$/.test(meter)) return json({ error: "Revisa el título, artista, BPM y compás." }, 400, origin);
  const found = duplicate(state.sequences, name, artist, keyValue, id);
  if (found) return json({ error: "Ya existe una secuencia con ese nombre y autor o tonalidad." }, 409, origin);
  let coverFile = form.get("cover"), oldCover = "";
  if ((!coverFile || !coverFile.size) && form.get("coverUrl")) {
    let coverUrl;
    try { coverUrl = new URL(String(form.get("coverUrl"))); } catch { return json({ error: "La URL de la carátula no es válida." }, 400, origin); }
    if (coverUrl.protocol !== "https:" || !/^is\d+-ssl\.mzstatic\.com$/i.test(coverUrl.hostname)) return json({ error: "La carátula automática debe provenir del catálogo de Apple Music." }, 400, origin);
    const response = await fetch(coverUrl, { headers: { Accept: "image/jpeg,image/png,image/webp" } }), mime = (response.headers.get("Content-Type") || "").split(";")[0].toLowerCase();
    if (!response.ok || !["image/jpeg", "image/png", "image/webp"].includes(mime)) return json({ error: "No se pudo descargar la carátula seleccionada." }, 400, origin);
    const bytes = await response.arrayBuffer();
    if (!bytes.byteLength || bytes.byteLength > MAX_COVER_BYTES) return json({ error: "La carátula debe pesar hasta 8 MB." }, 400, origin);
    coverFile = new File([bytes], "cover", { type: mime });
  }
  if (coverFile && typeof coverFile.arrayBuffer === "function" && coverFile.size) {
    if (coverFile.size > MAX_COVER_BYTES || !["image/jpeg", "image/png", "image/webp"].includes(coverFile.type)) return json({ error: "La carátula debe ser PNG, JPG o WebP de hasta 8 MB." }, 400, origin);
    const upload = await beginDriveUpload(env, `${id}-cover-${Date.now()}`, coverFile.type, coverFile.size);
    const saved = await fetch(upload.uploadUrl, { method: "PUT", headers: { "Content-Length": String(coverFile.size), "Content-Range": `bytes 0-${coverFile.size - 1}/${coverFile.size}`, "Content-Type": coverFile.type }, body: coverFile.stream(), redirect: "manual" });
    if (saved.status < 200 || saved.status >= 300) return json({ error: `Google Drive no pudo guardar la carátula (${saved.status}).` }, 502, origin);
    const uploaded = await saved.json(); await scriptCall(env, "finish-upload", { fileId: uploaded.id }); await scriptCall(env, "publish-files", { fileIds: [uploaded.id] });
    oldCover = item.coverDriveFileId;
    item.coverDriveFileId = uploaded.id; item.cover = "";
  }
  item.name = name; item.artist = artist; item.key = keyValue; item.bpm = bpm; item.meter = meter; item.genre = String(form.get("genre") || "Otro").slice(0, 40);
  await saveState(env, state);
  if (oldCover) await scriptCall(env, "delete-file", { fileId: oldCover }).catch(() => {});
  return json({ ok: true, message: "Cambios guardados y visibles para todos." }, 200, origin);
}
async function rateAllowed(request, env, name, maximum, windowSeconds) {
  const ip = request.headers.get("CF-Connecting-IP") || "unknown", key = `rate:${name}:${ip}`;
  const current = Number(await env.COMMUNITY_KV.get(key)) || 0;
  if (current >= maximum) return false;
  await env.COMMUNITY_KV.put(key, String(current + 1), { expirationTtl: windowSeconds });
  return true;
}
function base64Url(bytes) {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}
function fromBase64Url(value) {
  const base64 = String(value).replace(/-/g, "+").replace(/_/g, "/");
  return Uint8Array.from(atob(base64 + "=".repeat((4 - base64.length % 4) % 4)), (char) => char.charCodeAt(0));
}
async function hashAdminPin(pin, salt) {
  const key = await crypto.subtle.importKey("raw", new TextEncoder().encode(pin), "PBKDF2", false, ["deriveBits"]);
  const bits = await crypto.subtle.deriveBits({ name: "PBKDF2", salt: fromBase64Url(salt), iterations: 120000, hash: "SHA-256" }, key, 256);
  return base64Url(new Uint8Array(bits));
}
async function saveAdminPin(env, pin, version, mustChange = false) {
  const salt = base64Url(crypto.getRandomValues(new Uint8Array(16)));
  const hash = await hashAdminPin(pin, salt);
  const record = { salt, hash, version, mustChange };
  await env.COMMUNITY_KV.put("admin:pin", JSON.stringify(record));
  return record;
}
async function createAdminSession(env, version, mustChange) {
  const token = base64Url(crypto.getRandomValues(new Uint8Array(32)));
  await env.COMMUNITY_KV.put(`admin:session:${token}`, JSON.stringify({ version, mustChange }), { expirationTtl: 8 * 60 * 60 });
  return token;
}
async function adminSession(request, env, allowPinChange = false) {
  const supplied = String(request.headers.get("Authorization") || "").replace(/^Bearer\s+/i, "");
  const legacy = String(env.COMMUNITY_ADMIN_TOKEN || "");
  if (legacy && supplied === legacy) return { legacy: true, mustChange: false };
  if (!/^[A-Za-z0-9_-]{40,60}$/.test(supplied)) return null;
  const session = await env.COMMUNITY_KV.get(`admin:session:${supplied}`, "json");
  if (!session) return null;
  const pin = await env.COMMUNITY_KV.get("admin:pin", "json");
  if (!pin || Number(session.version) !== Number(pin.version)) return null;
  if (session.mustChange && !allowPinChange) return { ...session, mustChange: true };
  return session;
}
async function adminLogin(request, env, origin) {
  if (!await rateAllowed(request, env, "admin-login", 5, 60)) return json({ error: "Demasiados intentos. Espera un minuto." }, 429, origin);
  const body = await request.json(), pinValue = String(body.pin || "");
  if (!/^\d{4,12}$/.test(pinValue)) return json({ error: "El PIN debe tener entre 4 y 12 dígitos." }, 400, origin);
  let record = await env.COMMUNITY_KV.get("admin:pin", "json");
  if (!record) {
    if (pinValue !== "1234") return json({ error: "PIN incorrecto." }, 401, origin);
    record = await saveAdminPin(env, "1234", 1, true);
  } else if (await hashAdminPin(pinValue, record.salt) !== record.hash) {
    return json({ error: "PIN incorrecto." }, 401, origin);
  }
  const token = await createAdminSession(env, record.version, Boolean(record.mustChange));
  return json({ token, mustChangePin: Boolean(record.mustChange) }, 200, origin);
}
async function changeAdminPin(request, env, origin) {
  const current = await adminSession(request, env, true);
  if (!current || current.legacy) return json({ error: "Inicia sesión con el PIN del panel." }, 401, origin);
  const body = await request.json(), nextPin = String(body.pin || "");
  if (!/^\d{4,12}$/.test(nextPin) || nextPin === "1234") return json({ error: "El nuevo PIN debe tener entre 4 y 12 dígitos y ser distinto del PIN inicial." }, 400, origin);
  const record = await env.COMMUNITY_KV.get("admin:pin", "json");
  if (!record || Number(current.version) !== Number(record.version)) return json({ error: "La sesión venció. Inicia sesión de nuevo." }, 401, origin);
  const updated = await saveAdminPin(env, nextPin, Number(record.version) + 1, false);
  const token = await createAdminSession(env, updated.version, false);
  return json({ token, mustChangePin: false, message: "PIN actualizado." }, 200, origin);
}
async function handle(request, env) {
  const origin = allowedOrigin(request, env);
  if (origin === "null") return new Response("Origin not allowed", { status: 403 });
  if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: corsHeaders(origin) });
  const url = new URL(request.url), path = url.pathname;
  try {
    if (request.method === "GET" && path === "/api/health") return json({ ok: true, service: "StageTracks Community", storage: env.APPS_SCRIPT_URL && env.API_TOKEN ? "Google Drive" : "pending configuration" }, 200, origin);
    if (request.method === "GET" && path === "/api/sequences") {
      const state = await getState(env), query = String(url.searchParams.get("q") || "").trim().toLowerCase(), limit = Math.min(50, Math.max(1, Number(url.searchParams.get("limit")) || 20));
      const all = state.sequences.filter((item) => item.status === "approved" && (!query || `${item.name} ${item.artist} ${item.key} ${item.genre}`.toLowerCase().includes(query)));
      const results = all.sort((a, b) => Number(b.downloads || 0) - Number(a.downloads || 0) || String(a.name).localeCompare(String(b.name))).slice(0, limit).map((item) => ({ id: item.id, name: item.name, downloadName: item.downloadName, artist: item.artist, bpm: Math.round(Number(item.bpm)) || null, meter: item.meter, key: item.key, genre: item.genre, cover: item.coverDriveFileId ? `/media/${item.coverDriveFileId}` : item.cover ? `/media/${item.cover}` : "", downloads: item.downloads || 0, fileSize: item.fileSize, createdAt: item.createdAt }));
      return json({ results, total: all.length }, 200, origin);
    }
    if (request.method === "GET" && path.startsWith("/api/sequences/") && path.endsWith("/download")) {
      const id = decodeURIComponent(path.split("/").at(-2)), state = await getState(env), item = state.sequences.find((row) => row.id === id && row.status === "approved");
      if (!item?.packageDriveFileId) return json({ error: "No encontramos esa secuencia." }, 404, origin);
      const upstream = await drivePublicFetch(request, item.packageDriveFileId);
      if (!upstream.ok && upstream.status !== 206) return json({ error: `Google Drive no pudo entregar la secuencia (${upstream.status}).` }, 502, origin);
      if ((upstream.headers.get("Content-Type") || "").toLowerCase().includes("text/html")) return json({ error: "Google Drive devolvió una página de confirmación en lugar del archivo." }, 502, origin);
      const headers = new Headers(corsHeaders(origin, { "Cache-Control": "public, max-age=3600", "Accept-Ranges": "bytes", "Content-Disposition": `attachment; filename*=UTF-8''${encodeURIComponent(item.downloadName || `${item.name}.zip`)}` }));
      for (const key of ["Content-Type", "Content-Length", "Content-Range", "Accept-Ranges"]) if (upstream.headers.has(key)) headers.set(key, upstream.headers.get(key));
      return new Response(upstream.body, { status: upstream.status, headers });
    }
    const editMatch = path.match(/^\/api\/sequences\/([^/]+)$/);
    if (request.method === "PUT" && editMatch) {
      const access = await adminSession(request, env);
      if (!access) return json({ error: "Inicia sesión como administrador para editar secuencias." }, 401, origin);
      if (access.mustChange) return json({ error: "Cambia el PIN inicial antes de usar el panel.", code: "PIN_CHANGE_REQUIRED" }, 403, origin);
      return await updateSequence(request, env, decodeURIComponent(editMatch[1]), origin);
    }
    if (request.method === "GET" && path.startsWith("/media/")) {
      const fileId = decodeURIComponent(path.slice("/media/".length)), state = await getState(env);
      if (!state.sequences.some((item) => item.status === "approved" && item.coverDriveFileId === fileId)) return json({ error: "No encontramos esa carátula." }, 404, origin);
      const upstream = await drivePublicFetch(request, fileId);
      if (!upstream.ok) return json({ error: "No se pudo cargar la carátula." }, 502, origin);
      return new Response(upstream.body, { status: 200, headers: corsHeaders(origin, { "Content-Type": upstream.headers.get("Content-Type") || "image/jpeg", "Cache-Control": "public, max-age=86400" }) });
    }
    if (request.method === "POST" && path === "/api/sequences/uploads") {
      if (!await rateAllowed(request, env, "uploads", 8, 60)) return json({ error: "Límite de publicaciones alcanzado. Intenta en un minuto." }, 429, origin);
      return await beginUpload(request, env, origin);
    }
    const chunkMatch = path.match(/^\/api\/sequences\/uploads\/([^/]+)\/(package|cover)\/(\d+)$/);
    if (request.method === "PUT" && chunkMatch) return await uploadChunk(request, env, decodeURIComponent(chunkMatch[1]), chunkMatch[2], chunkMatch[3], origin);
    const completeMatch = path.match(/^\/api\/sequences\/uploads\/([^/]+)\/complete$/);
    if (request.method === "POST" && completeMatch) return await completeUpload(request, env, decodeURIComponent(completeMatch[1]), origin);
    const statusMatch = path.match(/^\/api\/sequences\/uploads\/([^/]+)\/status$/);
    if (request.method === "GET" && statusMatch) {
      const session = await env.COMMUNITY_KV.get(`upload:${decodeURIComponent(statusMatch[1])}`, "json");
      if (!session) return json({ error: "No encontramos el estado de esta publicación." }, 404, origin);
      return json({ jobStatus: "complete", ...(session.result || {}) }, 200, origin);
    }
    if (request.method === "POST" && path === "/api/feedback") {
      if (!await rateAllowed(request, env, "feedback", 3, 60)) return json({ error: "Espera un minuto antes de enviar otra sugerencia." }, 429, origin);
      const body = await request.json();
      const message = String(body.message || "").trim();
      if (message.length < 10 || message.length > 2000) return json({ error: "Escribe una sugerencia de entre 10 y 2000 caracteres." }, 400, origin);
      const state = await getState(env), feedback = { id: body.id || crypto.randomUUID(), message, createdAt: body.createdAt || new Date().toISOString() };
      state.feedback = Array.isArray(state.feedback) ? state.feedback : []; state.feedback.unshift(feedback);
      await scriptCall(env, "save-feedback", { feedback: state.feedback });
      await caches.default.delete(stateCacheKey(env));
      const result = await scriptCall(env, "email-feedback", { id: feedback.id, message, createdAt: feedback.createdAt });
      return json({ ok: true, emailed: result.emailed === true, message: "Gracias. Recibimos tu sugerencia." }, 201, origin);
    }
    if (request.method === "POST" && path === "/api/admin/login") return await adminLogin(request, env, origin);
    if (request.method === "POST" && path === "/api/admin/change-pin") return await changeAdminPin(request, env, origin);
    if (request.method === "POST" && path === "/api/admin/logout") {
      const access = await adminSession(request, env, true);
      if (!access) return json({ error: "La sesión ya venció." }, 401, origin);
      if (!access.legacy) {
        const supplied = String(request.headers.get("Authorization") || "").replace(/^Bearer\s+/i, "");
        await env.COMMUNITY_KV.delete(`admin:session:${supplied}`);
      }
      return json({ ok: true }, 200, origin);
    }
    if (path.startsWith("/api/admin/")) {
      const access = await adminSession(request, env);
      if (!access) return json({ error: "Inicia sesión como administrador." }, 401, origin);
      if (access.mustChange) return json({ error: "Cambia el PIN inicial antes de usar el panel.", code: "PIN_CHANGE_REQUIRED" }, 403, origin);
      const state = await getState(env);
      if (request.method === "GET" && path === "/api/admin/sequences") return json({ results: state.sequences }, 200, origin);
      if (request.method === "GET" && path === "/api/admin/pending") return json({ results: state.sequences.filter((item) => item.status === "pending") }, 200, origin);
      if (request.method === "GET" && path === "/api/admin/feedback") return json({ results: state.feedback || [] }, 200, origin);
      const editAdminSequence = path.match(/^\/api\/admin\/sequences\/([^/]+)$/);
      if (request.method === "PUT" && editAdminSequence) return await updateSequence(request, env, decodeURIComponent(editAdminSequence[1]), origin);
      const feedbackDelete = path.match(/^\/api\/admin\/feedback\/([^/]+)$/);
      if (request.method === "DELETE" && feedbackDelete) {
        const index = (state.feedback || []).findIndex((item) => item.id === decodeURIComponent(feedbackDelete[1]));
        if (index < 0) return json({ error: "No encontramos esa sugerencia." }, 404, origin);
        state.feedback.splice(index, 1); await scriptCall(env, "save-feedback", { feedback: state.feedback }); await caches.default.delete(stateCacheKey(env)); return json({ ok: true }, 200, origin);
      }
      if (request.method === "POST" && path.endsWith("/approve")) {
        const id = path.split("/").at(-2), item = state.sequences.find((row) => row.id === id);
        if (!item) return json({ error: "No encontramos esa secuencia." }, 404, origin);
        await scriptCall(env, "publish-files", { fileIds: [item.packageDriveFileId, item.coverDriveFileId].filter(Boolean) }); item.status = "approved"; await saveState(env, state); return json({ ok: true }, 200, origin);
      }
      const deleteSequence = path.match(/^\/api\/admin\/sequences\/([^/]+)$/);
      if (request.method === "DELETE" && deleteSequence) {
        const index = state.sequences.findIndex((item) => item.id === decodeURIComponent(deleteSequence[1]));
        if (index < 0) return json({ error: "No encontramos esa secuencia." }, 404, origin);
        const [item] = state.sequences.splice(index, 1); await saveState(env, state);
        await Promise.all([item.packageDriveFileId, item.coverDriveFileId].filter(Boolean).map((fileId) => scriptCall(env, "delete-file", { fileId })));
        return json({ ok: true }, 200, origin);
      }
    }
    return json({ error: "Ruta API desconocida." }, 404, origin);
  } catch (error) {
    return json({ error: error?.message || "Error interno de la API." }, 500, origin);
  }
}
export default { fetch: handle };
