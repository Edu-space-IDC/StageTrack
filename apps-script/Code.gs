const COMMUNITY_FOLDER_NAME = 'StageTracks Community';
const FILES_FOLDER_NAME = 'files';

function doPost(e) {
  try {
    const data = JSON.parse((e && e.postData && e.postData.contents) || '{}');
    requireToken_(data.token);
    const action = String(data.action || 'email-feedback');
    let result;
    switch (action) {
      case 'load-state': result = loadState_(); break;
      case 'save-sequences': saveJson_('sequences.json', data.sequences, true); result = {}; break;
      case 'save-feedback': saveJson_('feedback.json', data.feedback, true); result = {}; break;
      case 'begin-upload': result = beginUpload_(data); break;
      case 'finish-upload': result = finishUpload_(data.fileId); break;
      case 'publish-files': result = publishFiles_(data.fileIds); break;
      case 'delete-file': result = deleteFile_(data.fileId); break;
      case 'email-feedback': result = emailFeedback_(data); break;
      default: throw new Error('Acción de Apps Script desconocida.');
    }
    return output_({ ok: true, ...result });
  } catch (error) {
    return output_({ ok: false, error: String(error && error.message ? error.message : error) });
  }
}

// Ejecuta esta función una vez desde el editor para conceder acceso a Drive,
// solicitudes HTTPS y envío de correo antes de publicar la aplicación web.
function authorizeServices() {
  DriveApp.getRootFolder().getId();
  MailApp.getRemainingDailyQuota();
  UrlFetchApp.fetch('https://www.googleapis.com/drive/v3/about?fields=user', {
    headers: { Authorization: 'Bearer ' + ScriptApp.getOAuthToken() },
  });
}

function requireToken_(supplied) {
  const properties = PropertiesService.getScriptProperties();
  const expected = properties.getProperty('API_TOKEN') || properties.getProperty('FEEDBACK_TOKEN') || '';
  if (!expected || String(supplied || '') !== expected) throw new Error('No autorizado.');
}

function loadState_() {
  return {
    sequences: readJson_('sequences.json', []),
    feedback: readJson_('feedback.json', []),
  };
}

function readJson_(name, fallback) {
  const folder = communityFolder_();
  const files = folder.getFilesByName(name);
  if (!files.hasNext()) return fallback;
  try { return JSON.parse(files.next().getBlob().getDataAsString('UTF-8')); }
  catch (error) { throw new Error('No se pudo leer ' + name + ' en Google Drive.'); }
}

function saveJson_(name, value, mustBeArray) {
  if (mustBeArray && !Array.isArray(value)) throw new Error('El contenido de ' + name + ' no es válido.');
  const text = JSON.stringify(value);
  if (text.length > 8 * 1024 * 1024) throw new Error(name + ' superó el límite seguro de 8 MB.');
  const lock = LockService.getScriptLock();
  lock.waitLock(20000);
  try {
    const folder = communityFolder_();
    const files = folder.getFilesByName(name);
    if (files.hasNext()) files.next().setContent(text);
    else folder.createFile(name, text, MimeType.PLAIN_TEXT);
  } finally { lock.releaseLock(); }
}

function beginUpload_(data) {
  // Apps Script solo inicia la sesión resumible; el archivo se transmite desde
  // la API directamente a Drive, por lo que este límite no es una carga POST de 1 GB.
  const maxDriveUploadBytes = 1024 * 1024 * 1024;
  const size = Number(data.size);
  if (!Number.isFinite(size) || size <= 0 || size > maxDriveUploadBytes) throw new Error('El archivo supera el límite de carga configurado (1 GB).');
  const name = String(data.name || '').replace(/[\\/:*?"<>|]/g, '-').slice(0, 180);
  const mimeType = String(data.mimeType || 'application/octet-stream').slice(0, 120);
  if (!name) throw new Error('Falta el nombre del archivo.');
  const url = 'https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable';
  const response = UrlFetchApp.fetch(url, {
    method: 'post', contentType: 'application/json', muteHttpExceptions: true,
    headers: {
      Authorization: 'Bearer ' + ScriptApp.getOAuthToken(),
      'X-Upload-Content-Type': mimeType,
      'X-Upload-Content-Length': String(size),
    },
    payload: JSON.stringify({ name: name, mimeType: mimeType, parents: [filesFolder_().getId()] }),
  });
  const code = response.getResponseCode();
  const headers = response.getAllHeaders();
  const uploadUrl = headers.Location || headers.location;
  if (code < 200 || code >= 300 || !uploadUrl) throw new Error('Drive no pudo iniciar la carga resumible (' + code + ').');
  return { uploadUrl: String(uploadUrl) };
}

function finishUpload_(fileId) {
  const file = managedFile_(fileId);
  return { fileId: file.getId() };
}

function publishFiles_(fileIds) {
  if (!Array.isArray(fileIds)) throw new Error('La lista de archivos no es válida.');
  fileIds.filter(Boolean).forEach(function (fileId) {
    managedFile_(fileId).setSharing(DriveApp.Access.ANYONE_WITH_LINK, DriveApp.Permission.VIEW);
  });
  return {};
}

function deleteFile_(fileId) {
  managedFile_(fileId).setTrashed(true);
  return {};
}

function managedFile_(fileId) {
  const file = DriveApp.getFileById(String(fileId || ''));
  const parents = file.getParents();
  while (parents.hasNext()) if (parents.next().getId() === filesFolder_().getId()) return file;
  throw new Error('El archivo no pertenece a StageTracks Community.');
}

function emailFeedback_(data) {
  const message = String(data.message || '').trim();
  if (message.length < 10 || message.length > 2000) throw new Error('La sugerencia no tiene una longitud válida.');
  const recipient = PropertiesService.getScriptProperties().getProperty('FEEDBACK_RECIPIENT') || 'jminaq26@gmail.com';
  MailApp.sendEmail({
    to: recipient,
    subject: 'Nueva sugerencia para StageTracks',
    body: 'Recibiste una sugerencia anónima para StageTracks.\n\n' + message + '\n\nFecha: ' + String(data.createdAt || new Date().toISOString()) + '\nID: ' + String(data.id || ''),
  });
  return { emailed: true };
}

function communityFolder_() {
  const properties = PropertiesService.getScriptProperties();
  const folderId = properties.getProperty('COMMUNITY_FOLDER_ID');
  if (folderId) {
    try { return DriveApp.getFolderById(folderId); } catch (error) { properties.deleteProperty('COMMUNITY_FOLDER_ID'); }
  }
  const matches = DriveApp.getFoldersByName(COMMUNITY_FOLDER_NAME);
  const folder = matches.hasNext() ? matches.next() : DriveApp.createFolder(COMMUNITY_FOLDER_NAME);
  properties.setProperty('COMMUNITY_FOLDER_ID', folder.getId());
  return folder;
}

function filesFolder_() {
  const folder = communityFolder_();
  const matches = folder.getFoldersByName(FILES_FOLDER_NAME);
  return matches.hasNext() ? matches.next() : folder.createFolder(FILES_FOLDER_NAME);
}

function output_(value) {
  return ContentService.createTextOutput(JSON.stringify(value)).setMimeType(ContentService.MimeType.JSON);
}
