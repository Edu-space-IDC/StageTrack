import express from 'express';
import multer from 'multer';
import yauzl from 'yauzl';
import { randomUUID, timingSafeEqual } from 'node:crypto';
import { createReadStream, createWriteStream } from 'node:fs';
import { appendFile, mkdir, readFile, rename, stat, unlink, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');
const dataDir = path.resolve(process.env.COMMUNITY_DATA_DIR || path.join(here, 'data'));
const packageDir = path.join(dataDir, 'packages');
const coverDir = path.join(dataDir, 'covers');
const incomingDir = path.join(dataDir, 'incoming');
const indexFile = path.join(dataDir, 'sequences.json');
const feedbackFile = path.join(dataDir, 'feedback.json');
const feedbackAppsScriptUrl = String(process.env.FEEDBACK_APPS_SCRIPT_URL || 'https://script.google.com/macros/s/AKfycbzeM_yfegYr6_wtJ9KvNf0g4VF8PL25DnuV-hQFx0tw9ANIlPmejGYOCADnyywmPRnp6Q/exec').trim();
const googleDriveAppsScriptUrl = String(process.env.GOOGLE_DRIVE_APPS_SCRIPT_URL || feedbackAppsScriptUrl).trim();
const googleDriveAppsScriptToken = String(process.env.GOOGLE_DRIVE_APPS_SCRIPT_TOKEN || '').trim();
const driveStorageEnabled = Boolean(googleDriveAppsScriptUrl && googleDriveAppsScriptToken);
const port = Number(process.env.PORT || 47822);
const host = process.env.HOST || '0.0.0.0';
const maxUploadBytes = Number(process.env.MAX_UPLOAD_MB || 1024) * 1024 * 1024;
const maxChunkedUploadBytes = Number(process.env.MAX_CHUNKED_UPLOAD_MB || 1024) * 1024 * 1024;
const uploadChunkBytes = 8 * 1024 * 1024;
const sequenceUploadSessions = new Map();
const allowedAudio = new Set(['mp3','wav','wave','flac','ogg','m4a','aif','aiff','wma']);
const uploadsPerMinute = new Map();
const feedbackPerMinute = new Map();

await Promise.all([mkdir(packageDir, { recursive: true }), mkdir(coverDir, { recursive: true }), mkdir(incomingDir, { recursive: true })]);
let entries = [];
try { entries = JSON.parse(await readFile(indexFile, 'utf8')); if (!Array.isArray(entries)) entries = []; } catch {}
let feedbackEntries = [];
try { feedbackEntries = JSON.parse(await readFile(feedbackFile, 'utf8')); if (!Array.isArray(feedbackEntries)) feedbackEntries = []; } catch {}

async function driveStoreCall(action, data = {}) {
  if (!driveStorageEnabled) throw new Error('Google Drive todavía no está configurado en la API.');
  const response = await fetch(googleDriveAppsScriptUrl, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ action, token: googleDriveAppsScriptToken, ...data }),
    signal: AbortSignal.timeout(30_000), redirect: 'follow',
  });
  let result = {};
  try { result = await response.json(); } catch {}
  if (!response.ok || result.ok !== true) throw new Error(result.error || `Google Drive respondió ${response.status}.`);
  return result;
}

async function uploadFileToDrive(filePath, name, mimeType) {
  const size = (await stat(filePath)).size;
  const { uploadUrl } = await driveStoreCall('begin-upload', { name, mimeType, size });
  if (!uploadUrl) throw new Error('Apps Script no devolvió una sesión de carga a Google Drive.');
  const stream = createReadStream(filePath);
  let response;
  try {
    response = await fetch(uploadUrl, {
      method: 'PUT', headers: { 'Content-Length': String(size) }, body: stream, duplex: 'half',
      signal: AbortSignal.timeout(90 * 60_000),
    });
  } catch (error) {
    stream.destroy();
    throw new Error(`No se pudo subir ${name} a Google Drive: ${error.message}`);
  }
  let uploaded = {};
  try { uploaded = await response.json(); } catch {}
  if (!response.ok || !uploaded.id) throw new Error(`Google Drive rechazó ${name} (${response.status}).`);
  await driveStoreCall('finish-upload', { fileId: uploaded.id });
  return uploaded.id;
}

async function removeStoredFile(fileId) {
  if (!fileId) return;
  if (driveStorageEnabled) await driveStoreCall('delete-file', { fileId });
}

if (driveStorageEnabled) {
  const remoteState = await driveStoreCall('load-state');
  entries = Array.isArray(remoteState.sequences) ? remoteState.sequences : [];
  // Las cargas anteriores quedaron guardadas como pendientes y la API solo
  // muestra las aprobadas. Publicarlas al iniciar recupera esas secuencias.
  const pendingEntries = entries.filter((item) => item.status === 'pending');
  if (pendingEntries.length) {
    for (const item of pendingEntries) {
      await driveStoreCall('publish-files', { fileIds: [item.packageDriveFileId, item.coverDriveFileId].filter(Boolean) });
      item.status = 'approved';
    }
    await driveStoreCall('save-sequences', { sequences: entries });
  }
  feedbackEntries = Array.isArray(remoteState.feedback) ? remoteState.feedback : [];
}
let writes = Promise.resolve();
const persist = () => {
  writes = writes.then(async () => {
    if (driveStorageEnabled) await driveStoreCall('save-sequences', { sequences: entries });
    else {
      const tmp = indexFile + '.tmp';
      await writeFile(tmp, JSON.stringify(entries, null, 2));
      await rename(tmp, indexFile);
    }
  });
  return writes;
};
let feedbackWrites = Promise.resolve();
const persistFeedback = () => {
  feedbackWrites = feedbackWrites.then(async () => {
    if (driveStorageEnabled) await driveStoreCall('save-feedback', { feedback: feedbackEntries });
    else {
      const tmp = feedbackFile + '.tmp';
      await writeFile(tmp, JSON.stringify(feedbackEntries, null, 2));
      await rename(tmp, feedbackFile);
    }
  });
  return feedbackWrites;
};

function rateLimit(req, res, next) {
  const now = Date.now(), key = req.ip || req.socket.remoteAddress || 'unknown';
  const recent = (uploadsPerMinute.get(key) || []).filter((t) => now - t < 60_000);
  if (recent.length >= 8) return res.status(429).json({ error: 'Límite de publicaciones alcanzado. Intenta en un minuto.' });
  recent.push(now); uploadsPerMinute.set(key, recent); next();
}

const upload = multer({
  dest: incomingDir,
  limits: { fileSize: maxUploadBytes, files: 2, fields: 12, fieldSize: 16_384 },
  fileFilter(_req, file, cb) {
    if (file.fieldname === 'package' && /\.(zip|rpseq)$/i.test(file.originalname)) return cb(null, true);
    if (file.fieldname === 'cover' && /^image\/(png|jpeg|webp)$/.test(file.mimetype)) return cb(null, true);
    cb(new Error('Sube un paquete .rpseq/.zip y una carátula PNG, JPG o WebP.'));
  },
});

function inspectArchive(filePath) {
  return new Promise((resolve, reject) => {
    yauzl.open(filePath, { lazyEntries: true, autoClose: true, validateEntrySizes: true }, (error, zip) => {
      if (error) return reject(new Error('El archivo no es un paquete ZIP válido.'));
      const names = new Set(); let manifest = null, count = 0, total = 0;
      const fail = (message) => { try { zip.close(); } catch {} reject(new Error(message)); };
      zip.on('error', () => reject(new Error('El paquete está dañado.')));
      zip.on('entry', (entry) => {
        count++; total += entry.uncompressedSize;
        const name = entry.fileName.replaceAll('\\', '/');
        const clean = path.posix.normalize(name);
        if (count > 40_000 || total > 8 * 1024 ** 3) return fail('El paquete supera los límites de seguridad.');
        if (name.startsWith('/') || /^[A-Za-z]:/.test(name) || clean === '..' || clean.startsWith('../') || clean.includes('/../')) return fail('El paquete contiene una ruta no permitida.');
        if (!entry.fileName.endsWith('/')) names.add(clean);
        if (path.posix.extname(name).toLowerCase() === '.rpseq') {
          if (manifest) return fail('El paquete debe contener exactamente un archivo .rpseq.');
          if (entry.uncompressedSize > 8 * 1024 * 1024) return fail('El manifiesto de secuencia es demasiado grande.');
          zip.openReadStream(entry, (streamError, stream) => {
            if (streamError) return fail('No se pudo leer el manifiesto de secuencia.');
            const chunks = []; let bytes = 0;
            stream.on('data', (chunk) => { bytes += chunk.length; if (bytes > 8 * 1024 * 1024) return fail('El manifiesto de secuencia es demasiado grande.'); chunks.push(chunk); });
            stream.on('error', () => fail('No se pudo leer el manifiesto de secuencia.'));
            stream.on('end', () => { try { manifest = JSON.parse(Buffer.concat(chunks).toString('utf8')); zip.readEntry(); } catch { fail('El archivo .rpseq no contiene JSON válido.'); } });
          });
          return;
        }
        zip.readEntry();
      });
      zip.on('end', () => {
        try {
          if (!manifest || !Array.isArray(manifest.songs) || manifest.songs.length === 0) throw new Error('El paquete no incluye canciones en el archivo .rpseq.');
          const referenced = [];
          for (const song of manifest.songs) {
            if (song.cover) {
              const cover = String(song.cover).replaceAll('\\\\', '/');
              if (path.posix.isAbsolute(cover) || /^[A-Za-z]:/.test(cover) || cover.split('/').includes('..')) throw new Error('La carátula debe usar una ruta relativa dentro del paquete.');
              referenced.push(path.posix.normalize(cover));
            }
            if (!song || typeof song.name !== 'string' || !Array.isArray(song.tracks)) throw new Error('El paquete contiene una secuencia con estructura inválida.');
            for (const track of song.tracks) for (const item of (track.items || [])) {
              const file = item.file || item.originalFile;
              if (!file) continue;
              if (path.posix.isAbsolute(file) || /^[A-Za-z]:/.test(file) || file.replaceAll('\\','/').split('/').includes('..')) throw new Error('Los audios deben usar rutas relativas dentro del paquete.');
              if (!allowedAudio.has(path.extname(file).slice(1).toLowerCase())) throw new Error('El paquete incluye una pista en un formato no compatible.');
              referenced.push(path.posix.normalize(file.replaceAll('\\','/')));
            }
          }
          const missing = referenced.find((file) => !names.has(file));
          if (missing) throw new Error(`Falta el audio ${missing}. Incluye todas las pistas referenciadas en el ZIP.`);
          const manifestTempo = Number(manifest.songs[0]?.tempo);
          const manifestMeter = `${manifest.songs[0]?.timeSigNum || 4}/${manifest.songs[0]?.timeSigDen || 4}`;
          resolve({ songCount: manifest.songs.length, totalBytes: total, bpm: manifestTempo >= 40 && manifestTempo <= 240 ? manifestTempo : null, meter: manifestMeter });
        } catch (e) { reject(e); }
      });
      zip.readEntry();
    });
  });
}

const app = express();
app.disable('x-powered-by');
app.use(express.json({ limit: '8kb' }));
const allowedCorsOrigins = new Set([
  ...String(process.env.CORS_ORIGIN || '').split(','),
  ...String(process.env.CORS_ORIGINS || '').split(','),
  'https://stagetrack-project.vercel.app',
].map((origin) => origin.trim()).filter(Boolean));
function isStageTracksLoopbackOrigin(origin) {
  try {
    const url = new URL(origin);
    const loopback = ['127.0.0.1', 'localhost', '[::1]'].includes(url.hostname.toLowerCase());
    const port = Number(url.port);
    return url.protocol === 'http:' && loopback && port >= 47821 && port <= 47840;
  } catch { return false; }
}
app.use((req, res, next) => {
  const origin = req.get('Origin');
  if (!origin || allowedCorsOrigins.has('*')) res.setHeader('Access-Control-Allow-Origin', '*');
  else if (allowedCorsOrigins.has(origin) || isStageTracksLoopbackOrigin(origin)) res.setHeader('Access-Control-Allow-Origin', origin);
  res.vary('Origin');
  res.setHeader('Access-Control-Allow-Methods', 'GET,POST,PUT,DELETE,OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type,Authorization,Range');
  res.setHeader('Access-Control-Expose-Headers', 'Content-Length,Content-Disposition,Content-Range,Accept-Ranges');
  if (req.method === 'OPTIONS') return res.sendStatus(204);
  next();
});
if (driveStorageEnabled) {
  app.get('/media/:fileId', (req, res) => {
    const fileId = String(req.params.fileId || '');
    if (!entries.some((item) => item.coverDriveFileId === fileId)) return res.status(404).json({ error: 'No encontramos esa carátula.' });
    res.redirect(302, `https://drive.google.com/uc?export=view&id=${encodeURIComponent(fileId)}`);
  });
} else {
  app.use('/media', express.static(coverDir, { fallthrough: false, maxAge: '1d' }));
}
app.get('/api/health', (_req, res) => res.json({ ok: true, service: 'StageTracks Community', version: 1 }));
app.post('/api/feedback', async (req, res, next) => {
  const now = Date.now(), key = req.ip || req.socket.remoteAddress || 'unknown';
  const recent = (feedbackPerMinute.get(key) || []).filter((time) => now - time < 60_000);
  if (recent.length >= 3) return res.status(429).json({ error: 'Espera un minuto antes de enviar otra sugerencia.' });
  recent.push(now); feedbackPerMinute.set(key, recent);
  // Honeypot for simple bots; never store visitor identifiers or contact details.
  if (String(req.body?.website || '').trim()) return res.status(201).json({ ok: true });
  const message = String(req.body?.message || '').trim();
  if (message.length < 10 || message.length > 2000) return res.status(400).json({ error: 'Escribe una sugerencia de entre 10 y 2000 caracteres.' });
  const item = { id: randomUUID(), message, createdAt: new Date().toISOString() };
  try {
    feedbackEntries.unshift(item);
    await persistFeedback();
    const scriptToken = String(process.env.FEEDBACK_APPS_SCRIPT_TOKEN || '');
    if (!scriptToken) return res.status(201).json({ ok: true, emailed: false, message: 'Gracias. Guardamos tu sugerencia de forma anonima. Falta configurar el token privado para activar el correo.' });
    let mailResponse;
    try {
      mailResponse = await fetch(feedbackAppsScriptUrl, {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ token: scriptToken, id: item.id, message: item.message, createdAt: item.createdAt }),
        signal: AbortSignal.timeout(12_000), redirect: 'follow',
      });
    } catch { return res.status(502).json({ error: 'La sugerencia quedo guardada, pero no pudimos enviarla al correo. Puedes consultarla en el archivo de sugerencias.' }); }
    let mailResult = {};
    try { mailResult = await mailResponse.json(); } catch {}
    if (!mailResponse.ok || mailResult.ok !== true) return res.status(502).json({ error: 'La sugerencia quedo guardada, pero Apps Script no confirmo el envio del correo. Revisa la configuracion y puedes consultarla en el archivo de sugerencias.' });
    res.status(201).json({ ok: true, emailed: true, message: 'Gracias. Recibimos tu sugerencia y te la enviamos por correo.' });
  } catch (error) { feedbackEntries = feedbackEntries.filter((entry) => entry.id !== item.id); next(error); }
});
app.get('/api/sequences', (req, res) => {
  const query = String(req.query.q || '').trim().toLocaleLowerCase();
  const limit = Math.min(50, Math.max(1, Number(req.query.limit) || 20));
  const results = entries.filter((item) => item.status === 'approved' && (!query || `${item.name} ${item.artist} ${item.key} ${item.genre}`.toLocaleLowerCase().includes(query)))
    .sort((a, b) => b.downloads - a.downloads || a.name.localeCompare(b.name)).slice(0, limit)
    .map((item) => ({ id: item.id, name: item.name, artist: item.artist, bpm: item.bpm, meter: item.meter, key: item.key, genre: item.genre, cover: item.coverDriveFileId ? `/media/${item.coverDriveFileId}` : (item.cover ? `/media/${item.cover}` : ''), downloads: item.downloads, fileSize: item.fileSize, createdAt: item.createdAt }));
  res.json({ results, total: entries.filter((item) => item.status === 'approved' && (!query || `${item.name} ${item.artist} ${item.key} ${item.genre}`.toLocaleLowerCase().includes(query))).length });
});
app.get('/api/sequences/:id/download', async (req, res, next) => {
  const item = entries.find((row) => row.id === req.params.id && row.status === 'approved');
  if (!item) return res.status(404).json({ error: 'No encontramos esa secuencia.' });
  try {
    if (driveStorageEnabled) {
      if (!item.packageDriveFileId) return res.status(404).json({ error: 'El archivo de la secuencia ya no está disponible.' });
      item.downloads++; await persist();
      return res.redirect(302, `https://drive.google.com/uc?export=download&id=${encodeURIComponent(item.packageDriveFileId)}`);
    }
    const file = path.join(packageDir, item.packageFile);
    await stat(file);
    item.downloads++; await persist();
    res.download(file, item.downloadName, (error) => { if (error && !res.headersSent) next(error); });
  } catch { res.status(404).json({ error: 'El archivo de la secuencia ya no está disponible.' }); }
});
async function processSequenceUpload(req, res, next, packageUpload, coverUpload) {
  let packageDriveFileId = '', coverDriveFileId = '', tempCoverPath = '', itemId = '';
  try {
    if (!packageUpload) return res.status(400).json({ error: 'Adjunta el paquete de secuencia.' });
    const name = String(req.body.name || '').trim(), artist = String(req.body.artist || '').trim();
    const requestedBpm = Number(req.body.bpm), requestedMeter = String(req.body.meter || '4/4');
    if (name.length < 2 || name.length > 120 || artist.length > 120) throw new Error('Escribe el nombre de la canción y un artista de máximo 120 caracteres.');
    const info = await inspectArchive(packageUpload.path);
    const bpm = info.bpm || requestedBpm, meter = info.bpm ? info.meter : requestedMeter;
    if (!Number.isFinite(bpm) || bpm < 40 || bpm > 240 || !/^\d{1,2}\/(1|2|4|8|16|32)$/.test(meter)) throw new Error('Revisa el BPM y el compás.');
    const id = randomUUID(), ext = path.extname(packageUpload.originalname).toLowerCase() === '.zip' ? '.zip' : '.rpseq';
    itemId = id;
    const packageFile = `${id}${ext}`, downloadName = `${name.replace(/[\\/:*?"<>|]/g, '-').trim()}${ext}`;
    if (driveStorageEnabled) packageDriveFileId = await uploadFileToDrive(packageUpload.path, downloadName, 'application/octet-stream');
    else await rename(packageUpload.path, path.join(packageDir, packageFile));
    let cover = '';
    if (coverUpload) {
      const coverExt = ({ 'image/jpeg': '.jpg', 'image/png': '.png', 'image/webp': '.webp' })[coverUpload.mimetype];
      if (!coverExt || coverUpload.size > 8 * 1024 * 1024) throw new Error('La carátula debe ser PNG/JPG/WebP de hasta 8 MB.');
      if (driveStorageEnabled) coverDriveFileId = await uploadFileToDrive(coverUpload.path, `${id}${coverExt}`, coverUpload.mimetype);
      else { cover = `${id}${coverExt}`; await rename(coverUpload.path, path.join(coverDir, cover)); }
    } else if (req.body.coverUrl) {
      let imageUrl;
      try { imageUrl = new URL(String(req.body.coverUrl)); } catch { throw new Error('La URL de la carátula no es válida.'); }
      if (imageUrl.protocol !== 'https:' || !/^is\d+-ssl\.mzstatic\.com$/i.test(imageUrl.hostname)) throw new Error('La carátula automática debe provenir del catálogo de Apple Music.');
      const response = await fetch(imageUrl, { signal: AbortSignal.timeout(8_000), headers: { Accept: 'image/jpeg,image/png,image/webp' } });
      const mime = (response.headers.get('content-type') || '').split(';')[0].toLowerCase();
      const coverExt = ({ 'image/jpeg': '.jpg', 'image/png': '.png', 'image/webp': '.webp' })[mime];
      if (!response.ok || !coverExt) throw new Error('No se pudo descargar la carátula seleccionada.');
      const bytes = Buffer.from(await response.arrayBuffer());
      if (bytes.length > 8 * 1024 * 1024) throw new Error('La carátula supera el límite de 8 MB.');
      tempCoverPath = path.join(incomingDir, `${id}${coverExt}`);
      await writeFile(tempCoverPath, bytes);
      if (driveStorageEnabled) coverDriveFileId = await uploadFileToDrive(tempCoverPath, `${id}${coverExt}`, mime);
      else { cover = `${id}${coverExt}`; await rename(tempCoverPath, path.join(coverDir, cover)); tempCoverPath = ''; }
    }
    const item = { id, name, artist, bpm, meter, key: String(req.body.key || '').slice(0, 8), genre: String(req.body.genre || '').slice(0, 40), cover, packageFile, packageDriveFileId, coverDriveFileId, downloadName, fileSize: packageUpload.size, songCount: info.songCount, downloads: 0, status: 'approved', createdAt: new Date().toISOString() };
    if (driveStorageEnabled) await driveStoreCall('publish-files', { fileIds: [packageDriveFileId, coverDriveFileId].filter(Boolean) });
    entries.unshift(item); await persist();
    await Promise.all([packageUpload.path, coverUpload?.path].filter(Boolean).map((filePath) => unlink(filePath).catch(() => {})));
    res.status(201).json({ id, status: item.status, message: item.status === 'approved' ? 'Secuencia publicada.' : 'Recibimos la secuencia. Quedará visible cuando la apruebe el equipo.' });
  } catch (error) {
    if (itemId) entries = entries.filter((entry) => entry.id !== itemId);
    await Promise.all([removeStoredFile(packageDriveFileId).catch(() => {}), removeStoredFile(coverDriveFileId).catch(() => {})]);
    if (packageUpload?.path) await unlink(packageUpload.path).catch(() => {});
    if (coverUpload?.path) await unlink(coverUpload.path).catch(() => {});
    if (tempCoverPath) await unlink(tempCoverPath).catch(() => {});
    next(error);
  }
}

app.post('/api/sequences', rateLimit, upload.fields([{ name: 'package', maxCount: 1 }, { name: 'cover', maxCount: 1 }]), (req, res, next) =>
  processSequenceUpload(req, res, next, req.files?.package?.[0], req.files?.cover?.[0])
);

function removeSequenceUploadSession(id) {
  const session = sequenceUploadSessions.get(id);
  if (!session) return;
  sequenceUploadSessions.delete(id);
  for (const file of Object.values(session.files)) unlink(file.path).catch(() => {});
}

setInterval(() => {
  const expiredBefore = Date.now() - 2 * 60 * 60_000;
  for (const [id, session] of sequenceUploadSessions) if (session.createdAt < expiredBefore) removeSequenceUploadSession(id);
}, 10 * 60_000).unref();

app.post('/api/sequences/uploads', rateLimit, (req, res) => {
  const packageSize = Number(req.body?.packageSize), coverSize = Number(req.body?.coverSize || 0);
  if (!Number.isSafeInteger(packageSize) || packageSize < 1 || packageSize > maxChunkedUploadBytes) return res.status(413).json({ error: `El paquete debe pesar hasta ${Math.floor(maxChunkedUploadBytes / 1024 ** 3)} GB.` });
  if (!Number.isSafeInteger(coverSize) || coverSize < 0 || coverSize > 8 * 1024 * 1024) return res.status(413).json({ error: 'La carátula debe pesar hasta 8 MB.' });
  const metadata = { ...req.body };
  delete metadata.packageSize; delete metadata.coverSize;
  const id = randomUUID();
  const files = {
    package: { name: String(req.body.packageName || 'sequence.rpseq').slice(0, 180), size: packageSize, path: path.join(incomingDir, `${id}.package.part`), received: 0, nextPart: 0 },
  };
  if (coverSize) files.cover = { name: String(req.body.coverName || 'cover.png').slice(0, 180), size: coverSize, path: path.join(incomingDir, `${id}.cover.part`), received: 0, nextPart: 0 };
  sequenceUploadSessions.set(id, { id, metadata, files, createdAt: Date.now() });
  Promise.all(Object.values(files).map((file) => writeFile(file.path, Buffer.alloc(0)))).then(() => {
    if (!res.headersSent) res.status(201).json({ uploadId: id, chunkSize: uploadChunkBytes });
  }).catch((error) => { removeSequenceUploadSession(id); if (!res.headersSent) res.status(500).json({ error: `No se pudo preparar la carga: ${error.message}` }); });
});

app.put('/api/sequences/uploads/:id/:field/:part', express.raw({ type: () => true, limit: `${uploadChunkBytes}b` }), async (req, res) => {
  const session = sequenceUploadSessions.get(req.params.id), field = req.params.field, file = session?.files[field], part = Number(req.params.part);
  if (!session || !file) return res.status(404).json({ error: 'La sesión de carga venció. Inicia la publicación otra vez.' });
  if (!Number.isSafeInteger(part) || part !== file.nextPart) return res.status(409).json({ error: 'El bloque no llegó en el orden esperado.' });
  if (!Buffer.isBuffer(req.body) || req.body.length < 1 || req.body.length > uploadChunkBytes || file.received + req.body.length > file.size) return res.status(400).json({ error: 'El bloque de archivo no es válido.' });
  try {
    await appendFile(file.path, req.body);
    file.received += req.body.length; file.nextPart++;
    res.json({ received: file.received, total: file.size });
  } catch (error) { res.status(500).json({ error: `No se pudo guardar un bloque: ${error.message}` }); }
});

app.post('/api/sequences/uploads/:id/complete', rateLimit, async (req, res, next) => {
  const session = sequenceUploadSessions.get(req.params.id);
  if (!session) return res.status(404).json({ error: 'La sesión de carga venció. Inicia la publicación otra vez.' });
  const incomplete = Object.values(session.files).find((file) => file.received !== file.size);
  if (incomplete) return res.status(400).json({ error: `La carga de ${incomplete.name} está incompleta.` });
  sequenceUploadSessions.delete(session.id);
  const packageFile = session.files.package;
  const packageUpload = { path: packageFile.path, originalname: packageFile.name, size: packageFile.size, mimetype: 'application/octet-stream' };
  const coverFile = session.files.cover;
  const coverUpload = coverFile ? { path: coverFile.path, originalname: coverFile.name, size: coverFile.size, mimetype: ({ '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.png': 'image/png', '.webp': 'image/webp' })[path.extname(coverFile.name).toLowerCase()] || 'application/octet-stream' } : undefined;
  req.body = session.metadata;
  await processSequenceUpload(req, res, (error) => {
    for (const file of Object.values(session.files)) unlink(file.path).catch(() => {});
    next(error);
  }, packageUpload, coverUpload);
});

function adminAuth(req, res, next) {
  const expected = process.env.COMMUNITY_ADMIN_TOKEN || '';
  const supplied = String(req.headers.authorization || '').replace(/^Bearer\s+/i, '');
  if (!expected || supplied.length !== expected.length || !timingSafeEqual(Buffer.from(supplied), Buffer.from(expected))) return res.status(401).json({ error: 'Token de moderación inválido.' });
  next();
}
app.get('/api/admin/pending', adminAuth, (_req, res) => res.json({ results: entries.filter((item) => item.status === 'pending') }));
app.get('/api/admin/feedback', adminAuth, (_req, res) => res.json({ results: feedbackEntries }));
app.delete('/api/admin/feedback/:id', adminAuth, async (req, res) => {
  const index = feedbackEntries.findIndex((item) => item.id === req.params.id);
  if (index < 0) return res.status(404).json({ error: 'No encontramos esa sugerencia.' });
  const [item] = feedbackEntries.splice(index, 1);
  try { await persistFeedback(); res.json({ ok: true }); }
  catch (error) { feedbackEntries.splice(index, 0, item); res.status(500).json({ error: 'No se pudo eliminar la sugerencia.' }); }
});
app.post('/api/admin/sequences/:id/approve', adminAuth, async (req, res) => {
  const item = entries.find((row) => row.id === req.params.id);
  if (!item) return res.status(404).json({ error: 'No encontramos esa secuencia.' });
  if (driveStorageEnabled) await driveStoreCall('publish-files', { fileIds: [item.packageDriveFileId, item.coverDriveFileId].filter(Boolean) });
  item.status = 'approved'; await persist(); res.json({ ok: true });
});
app.delete('/api/admin/sequences/:id', adminAuth, async (req, res) => {
  const index = entries.findIndex((row) => row.id === req.params.id);
  if (index < 0) return res.status(404).json({ error: 'No encontramos esa secuencia.' });
  const [item] = entries.splice(index, 1); await persist();
  if (driveStorageEnabled) await Promise.all([removeStoredFile(item.packageDriveFileId), removeStoredFile(item.coverDriveFileId)]);
  else await Promise.all([unlink(path.join(packageDir, item.packageFile)).catch(() => {}), item.cover ? unlink(path.join(coverDir, item.cover)).catch(() => {}) : Promise.resolve()]);
  res.json({ ok: true });
});

app.use('/api', (req, res) => res.status(404).json({ error: `Ruta API desconocida: ${req.path}` }));
app.use((error, _req, res, _next) => {
  if (error instanceof multer.MulterError && error.code === 'LIMIT_FILE_SIZE') return res.status(413).json({ error: `El paquete supera el límite de ${process.env.MAX_UPLOAD_MB || 1024} MB.` });
  res.status(400).json({ error: error.message || 'No se pudo procesar la solicitud.' });
});
if (await import('node:fs/promises').then(({ access }) => access(path.join(root, 'dist')).then(() => true).catch(() => false))) {
  app.use(express.static(path.join(root, 'dist')));
  app.get('*path', (_req, res) => res.sendFile(path.join(root, 'dist', 'index.html')));
}
app.listen(port, host, () => console.log(`StageTracks Community API escuchando en http://${host}:${port}`));
