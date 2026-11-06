#include "WebControlServer.h"
#include "BinaryData.h"

namespace {
// Se inserta al inicio de <head>: marca el modo "Playback" (sin FX), enlaza la pagina con el anfitrion nativo
// (mismo papel que chrome.webview.postMessage en REAPER Playback) y aplica los mismos ajustes de interfaz.
const char* shim = R"JS(<script>
window.RP_PLAYBACK=true;
document.documentElement.classList.add('stagetracks-native');
(function(){
  function post(m){try{fetch('/host/msg',{method:'POST',body:String(m),cache:'no-store'});}catch(e){}}
  var wv={postMessage:post,postMessageWithAdditionalObjects:function(){}};
  try{window.chrome=window.chrome||{};Object.defineProperty(window.chrome,'webview',{value:wv,configurable:true,writable:true});}
  catch(e){try{window.chrome={webview:wv};}catch(e2){}}
  var busy=false;
  setInterval(function(){
    if(location.hostname!=='localhost'&&location.hostname!=='127.0.0.1'&&location.hostname!=='::1')return;
    if(busy)return;busy=true;
    fetch('/host/poll',{cache:'no-store'}).then(function(r){return r.text();})
      .then(function(t){busy=false;if(t){try{(0,eval)(t);}catch(e){console.error(e);}}})
      .catch(function(){busy=false;});
  },300);
  window.addEventListener('DOMContentLoaded',function(){
    var h=location.hostname;
    if(h==='localhost'||h==='127.0.0.1'||h==='::1')return;
    var badge=document.createElement('div');badge.style.cssText='position:fixed;top:8px;left:50%;transform:translateX(-50%);z-index:20000;display:flex;align-items:center;gap:10px;padding:6px 13px;border:1px solid #50d89066;border-radius:20px;background:#10251bef;color:#75e6a9;font:600 12px system-ui,sans-serif;box-shadow:0 4px 18px #0008';
    var label=document.createElement('span');label.textContent='Control web · Sesión remota';badge.appendChild(label);
    var disconnect=document.createElement('button');disconnect.type='button';disconnect.textContent='Desconectarse';disconnect.style.cssText='border:1px solid #75e6a955;border-radius:14px;padding:4px 9px;background:#183b2a;color:#d8ffe8;font:600 12px system-ui,sans-serif;cursor:pointer';disconnect.onclick=function(){if(history.length>1)history.back();else location.href='/';};badge.appendChild(disconnect);
    badge.setAttribute('role','status');
    document.body.appendChild(badge);
  });
  window.addEventListener('dragover',function(e){e.preventDefault();});
  window.addEventListener('drop',function(e){e.preventDefault();});
  window.addEventListener('load',function(){
    var b=document.getElementById('pExportSeq');if(b){if(window.STAGETRACKS_NATIVE||window.chrome&&window.chrome.webview){b.hidden=false;b.style.setProperty('display','inline-flex','important');}else b.style.display='none';}
    var l=document.getElementById('pLogout');if(l){l.textContent='Audio';l.title='Configuración de audio';}
    if(!window.STAGETRACKS_NATIVE&&l)l.style.display='none';
  });
})();
</script>)JS";

struct Request { juce::String method, path, body, range; bool ok = false; };

Request readRequest(juce::StreamingSocket& client)
{
    Request r;
    juce::MemoryBlock data;
    const auto deadline = juce::Time::getMillisecondCounter() + 8000;
    int headerEnd = -1;
    char buffer[16384];
    auto find = [&](const char* needle) -> int {
        const auto* p = (const char*) data.getData();
        const int n = (int) data.getSize(), m = (int) std::strlen(needle);
        for (int i = 0; i + m <= n; ++i) if (std::memcmp(p + i, needle, (size_t) m) == 0) return i;
        return -1;
    };
    while (headerEnd < 0 && juce::Time::getMillisecondCounter() < deadline && data.getSize() < 65536) {
        if (client.waitUntilReady(true, 100) <= 0) continue;
        const int n = client.read(buffer, (int) sizeof(buffer), false);
        if (n <= 0) return r;
        data.append(buffer, (size_t) n);
        headerEnd = find("\r\n\r\n");
    }
    if (headerEnd < 0) return r;
    const auto head = juce::String::fromUTF8((const char*) data.getData(), headerEnd);
    const auto first = head.upToFirstOccurrenceOf("\r\n", false, false);
    r.method = first.upToFirstOccurrenceOf(" ", false, false);
    const auto target = first.fromFirstOccurrenceOf(" ", false, false).upToFirstOccurrenceOf(" ", false, false);
    r.path = target;
    int contentLength = 0;
    for (const auto& line : juce::StringArray::fromLines(head)) {
        if (line.startsWithIgnoreCase("content-length:")) contentLength = line.fromFirstOccurrenceOf(":", false, false).trim().getIntValue();
        else if (line.startsWithIgnoreCase("range:")) r.range = line.fromFirstOccurrenceOf(":", false, false).trim();
    }
    contentLength = juce::jlimit(0, 16 * 1024 * 1024, contentLength);
    const size_t bodyStart = (size_t) headerEnd + 4;
    while (data.getSize() < bodyStart + (size_t) contentLength && juce::Time::getMillisecondCounter() < deadline) {
        if (client.waitUntilReady(true, 100) <= 0) continue;
        const int n = client.read(buffer, (int) sizeof(buffer), false);
        if (n <= 0) break;
        data.append(buffer, (size_t) n);
    }
    if (data.getSize() < bodyStart + (size_t) contentLength) return r;
    r.body = juce::String::fromUTF8((const char*) data.getData() + bodyStart, contentLength);
    r.ok = true;
    return r;
}

void sendResponse(juce::StreamingSocket& client, int status, const juce::String& type, const void* body, int length)
{
    const auto reason = status == 200 ? "OK" : status == 206 ? "Partial Content" : status == 416 ? "Range Not Satisfiable" : status == 404 ? "Not Found" : "Error";
    const auto head = "HTTP/1.1 " + juce::String(status) + " " + reason
        + "\r\nContent-Type: " + type + "\r\nContent-Length: " + juce::String(length)
        + "\r\nCache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n";
    auto write = [&](const void* p, int n) {
        int sent = 0;
        while (sent < n) {
            const int w = client.write((const char*) p + sent, n - sent);
            if (w <= 0) return;
            sent += w;
        }
    };
    write(head.toRawUTF8(), (int) head.getNumBytesAsUTF8());
    write(body, length);
}

void sendText(juce::StreamingSocket& client, int status, const juce::String& type, const juce::String& text)
{
    sendResponse(client, status, type, text.toRawUTF8(), (int) text.getNumBytesAsUTF8());
}

void sendFileStream(juce::StreamingSocket& client, const juce::File& file, const juce::String& range)
{
    auto input = file.createInputStream();
    if (input == nullptr) { sendText(client, 404, "text/plain; charset=utf-8", "No encontramos la secuencia exportada."); return; }
    const auto fileLength = file.getSize();
    int64_t start = 0, end = fileLength - 1;
    const bool partial = range.startsWithIgnoreCase("bytes=");
    if (partial) {
        const auto bounds = juce::StringArray::fromTokens(range.fromFirstOccurrenceOf("=", false, false), "-", "");
        if (bounds.size() != 2 || !bounds[0].containsOnly("0123456789")) { sendText(client, 416, "text/plain; charset=utf-8", "Rango no válido"); return; }
        start = bounds[0].getLargeIntValue();
        end = bounds[1].isEmpty() ? end : bounds[1].getLargeIntValue();
        if (start < 0 || start >= fileLength || end < start) { sendText(client, 416, "text/plain; charset=utf-8", "Rango fuera del archivo"); return; }
        end = juce::jmin(end, fileLength - 1);
    }
    const auto length = end - start + 1;
    juce::MemoryBlock rangeBody;
    if (partial) {
        if (length > 16 * 1024 * 1024) { sendText(client, 413, "text/plain; charset=utf-8", "El bloque solicitado es demasiado grande."); return; }
        rangeBody.setSize((size_t) length, false);
        input->setPosition(start);
        int64_t loaded = 0;
        while (loaded < length) {
            const int amount = input->read(static_cast<char*>(rangeBody.getData()) + loaded, (int) juce::jmin<int64_t>(64 * 1024, length - loaded));
            if (amount <= 0) { sendText(client, 500, "text/plain; charset=utf-8", "No se pudo leer completo el bloque de la secuencia."); return; }
            loaded += amount;
        }
    }
    const auto status = partial ? 206 : 200;
    const auto head = "HTTP/1.1 " + juce::String(status) + (partial ? " Partial Content" : " OK")
        + "\r\nContent-Type: application/octet-stream\r\nContent-Length: " + juce::String(length)
        + "\r\nAccept-Ranges: bytes\r\n"
        + (partial ? "Content-Range: bytes " + juce::String(start) + "-" + juce::String(end) + "/" + juce::String(fileLength) + "\r\n" : juce::String())
        + "Content-Disposition: attachment; filename=\"StageTracks-community.rpseq\"\r\nCache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n";
    auto writeAll = [&](const void* data, int bytes) {
        int sent = 0;
        int stalledMs = 0;
        while (sent < bytes) {
            const int written = client.write(static_cast<const char*>(data) + sent, bytes - sent);
            if (written > 0) { sent += written; stalledMs = 0; continue; }
            if (client.waitUntilReady(false, 250) < 0 || (stalledMs += 250) >= 30000) return false;
        }
        return true;
    };
    bool connected = writeAll(head.toRawUTF8(), (int) head.getNumBytesAsUTF8());
    if (partial) {
        if (connected) connected = writeAll(rangeBody.getData(), (int) rangeBody.getSize());
        input.reset();
        return;
    }
    input->setPosition(start);
    char buffer[64 * 1024];
    int64_t remaining = length;
    while (connected && remaining > 0) {
        const int read = input->read(buffer, (int) juce::jmin<int64_t>(sizeof(buffer), remaining));
        if (read <= 0) break;
        connected = writeAll(buffer, read);
        remaining -= read;
    }
    input.reset();
}
}

WebControlServer::WebControlServer(PlayerBridge& b) : juce::Thread("StageTracks web"), bridge(b)
{
    for (int candidate = 47821; candidate < 47841; ++candidate) {
        auto socket = std::make_unique<juce::StreamingSocket>();
        if (socket->createListener(candidate, "127.0.0.1")) {
            port = candidate;
            listener = std::move(socket);
            available = startThread();
            break;
        }
    }
}

WebControlServer::~WebControlServer()
{
    signalThreadShouldExit();
    if (listener) listener->close();
    stopThread(5000);
    workers.removeAllJobs(true, 5000);
}

bool WebControlServer::setRemoteEnabled(bool enabled)
{
    if (remoteEnabled == enabled) return available;
    signalThreadShouldExit();
    if (listener) listener->close();
    stopThread(5000);
    listener.reset();
    available = false;
    const auto address = enabled ? juce::String("0.0.0.0") : juce::String("127.0.0.1");
    auto socket = std::make_unique<juce::StreamingSocket>();
    if (!socket->createListener(port, address)) {
        // Preserve the previous state if rebinding fails.
        auto fallback = std::make_unique<juce::StreamingSocket>();
        const auto previousAddress = remoteEnabled ? juce::String("0.0.0.0") : juce::String("127.0.0.1");
        if (fallback->createListener(port, previousAddress)) {
            listener = std::move(fallback);
            available = startThread();
        }
        return false;
    }
    listener = std::move(socket);
    remoteEnabled = enabled;
    available = startThread();
    return available;
}

juce::String WebControlServer::getRemoteInfo() const
{
    juce::var info = new juce::DynamicObject();
    auto* object = info.getDynamicObject();
    const bool enabled = remoteEnabled.load();
    object->setProperty("enabled", enabled);
    const bool availableNow = available.load();
    object->setProperty("available", availableNow);
    object->setProperty("port", port);
    juce::Array<juce::var> urls;
    if (enabled && availableNow) {
        for (const auto& address : juce::IPAddress::getAllAddresses()) {
            const auto ip = address.toString();
            if (!ip.containsChar('.') || ip == "127.0.0.1" || ip.startsWith("169.254.") || ip == "0.0.0.0") continue;
            urls.add("http://" + ip + ":" + juce::String(port) + "/");
        }
    }
    object->setProperty("urls", urls);
    return juce::JSON::toString(info);
}

void WebControlServer::run()
{
    struct ClientJob final : juce::ThreadPoolJob
    {
        ClientJob(WebControlServer& s, juce::StreamingSocket* c) : ThreadPoolJob("StageTracks http"), server(s), client(c) {}
        JobStatus runJob() override { server.serve(*client); return jobHasFinished; }
        WebControlServer& server;
        std::unique_ptr<juce::StreamingSocket> client;
    };
    while (!threadShouldExit() && listener) {
        auto* client = listener->waitForNextConnection();
        if (client == nullptr) continue;
        if (threadShouldExit()) { delete client; break; }
        workers.addJob(new ClientJob(*this, client), true);
    }
}

juce::String WebControlServer::page() const
{
    auto html = juce::String::fromUTF8(BinaryData::studio_html, BinaryData::studio_htmlSize);
    auto communityApi = juce::String("http://127.0.0.1:47822");
    const auto configFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                                .getParentDirectory().getChildFile("community-config.json");
    if (configFile.existsAsFile()) {
        const auto config = juce::JSON::parse(configFile.loadFileAsString());
        const auto configuredApi = config.getProperty("apiBase", {}).toString().trim();
        if (configuredApi.startsWith("https://") || configuredApi.startsWith("http://")) communityApi = configuredApi.trimCharactersAtEnd("/");
    }
    const int at = html.indexOf("<head>");
    const auto communityConfig = "<script>window.STAGETRACKS_COMMUNITY_API=" + juce::JSON::toString(juce::var(communityApi)) + ";window.STAGETRACKS_APP_VERSION=" + juce::JSON::toString(juce::var(STAGETRACKS_APP_VERSION)) + ";</script>";
    if (at >= 0) html = html.substring(0, at + 6) + communityConfig + shim + html.substring(at + 6);
    return html;
}

void WebControlServer::serve(juce::StreamingSocket& client)
{
    const auto req = readRequest(client);
    if (!req.ok) return;
    const auto path = req.path.upToFirstOccurrenceOf("?", false, false);
    if (path == "/" || path == "/index.html") { sendText(client, 200, "text/html; charset=utf-8", page()); return; }
    if (path == "/main.js") { sendResponse(client, 200, "application/javascript; charset=utf-8", BinaryData::main_js, BinaryData::main_jsSize); return; }
    if (path == "/app-icon.png") {
        juce::MemoryBlock icon;
        const auto iconFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                                  .getParentDirectory().getChildFile("app-icon.png");
        if (iconFile.loadFileAsData(icon) && icon.getSize() > 0)
            sendResponse(client, 200, "image/png", static_cast<const char*>(icon.getData()), (int) icon.getSize());
        else
            sendText(client, 404, "text/plain; charset=utf-8", "Icon not found");
        return;
    }
    if (path.startsWith("/_/")) {
        // POST: el cuerpo son las ordenes separadas por ';'. GET: van en la ruta.
        const auto body = req.method == "POST" ? req.body : path.substring(3);
        sendText(client, 200, "text/plain; charset=utf-8", bridge.handle(body));
        return;
    }
    if (path == "/host/poll") { sendText(client, 200, "text/plain; charset=utf-8", bridge.takeHostScript()); return; }
    if (path == "/host/info") { sendText(client, 200, "application/json; charset=utf-8", getRemoteInfo()); return; }
    if (path == "/host/community/export") {
        const auto file = onCommunityExportFile ? onCommunityExportFile() : juce::File();
        if (!file.existsAsFile()) { sendText(client, 404, "application/json; charset=utf-8", "{\"error\":\"Primero exporta una secuencia desde Mi biblioteca.\"}"); return; }
        sendFileStream(client, file, req.range);
        return;
    }
    if (path == "/host/community/export/finish") {
        if (onCommunityExportCleanup) onCommunityExportCleanup();
        sendText(client, 200, "text/plain; charset=utf-8", "ok");
        return;
    }
    if (path == "/host/msg") {
        if (onHostMessage) onHostMessage(req.body);
        sendText(client, 200, "text/plain; charset=utf-8", "ok");
        return;
    }
    if (path == "/host/community/start") {
        const auto data = juce::JSON::parse(req.body);
        const auto rawName = data.getProperty("fileName", juce::String("StageTracks-community.rpseq")).toString();
        auto name = rawName.replaceCharacters("\\/:*?\"<>|", "---------").trim().substring(0, 120);
        if (name.isEmpty()) name = "StageTracks-community.rpseq";
        if (!name.endsWithIgnoreCase(".rpseq") && !name.endsWithIgnoreCase(".zip")) name << ".rpseq";
        const int64_t expected = (int64_t) data.getProperty("bytes", 0);
        if (expected < 0 || expected > (int64_t) (8ULL << 30)) { sendText(client, 413, "application/json; charset=utf-8", "{\"error\":\"El paquete excede el límite de 8 GB.\"}"); return; }
        const auto token = juce::Uuid().toString().removeCharacters("{}");
        const auto folder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("StageTracks").getChildFile("CommunityDownloads");
        folder.createDirectory();
        CommunityDownload state;
        state.name = name; state.expected = expected;
        state.metadata = data.getProperty("metadata", juce::var());
        state.file = folder.getChildFile(token + (name.endsWithIgnoreCase(".zip") ? ".zip" : ".rpseq"));
        state.stream = std::make_unique<juce::FileOutputStream>(state.file);
        if (!state.stream->openedOk()) { sendText(client, 500, "application/json; charset=utf-8", "{\"error\":\"No se pudo preparar el archivo descargado.\"}"); return; }
        { const juce::ScopedLock sl(downloadLock); communityDownloads[token] = std::move(state); }
        juce::var response = new juce::DynamicObject();
        response.getDynamicObject()->setProperty("token", token);
        sendText(client, 200, "application/json; charset=utf-8", juce::JSON::toString(response));
        return;
    }
    if (path.startsWith("/host/community/chunk/")) {
        const auto token = path.fromFirstOccurrenceOf("/host/community/chunk/", false, false);
        juce::MemoryOutputStream decoded;
        // Chunks de 4 MiB codificados en base64 ocupan aproximadamente 5.6 MiB.
        // readRequest limita el cuerpo completo a 16 MiB, así que aceptamos hasta 8 MiB aquí.
        if (req.body.length() > 8 * 1024 * 1024 || !juce::Base64::convertFromBase64(decoded, req.body)) { sendText(client, 400, "text/plain; charset=utf-8", "Chunk no válido"); return; }
        const juce::ScopedLock sl(downloadLock);
        auto it = communityDownloads.find(token);
        if (it == communityDownloads.end() || it->second.stream == nullptr) { sendText(client, 404, "text/plain; charset=utf-8", "Descarga no encontrada"); return; }
        auto& state = it->second;
        if (state.written + (int64_t) decoded.getDataSize() > (int64_t) (8ULL << 30) || (state.expected > 0 && state.written + (int64_t) decoded.getDataSize() > state.expected) || !state.stream->write(decoded.getData(), decoded.getDataSize())) {
            sendText(client, 413, "text/plain; charset=utf-8", "El chunk supera el tamaño permitido"); return;
        }
        state.written += (int64_t) decoded.getDataSize();
        sendText(client, 200, "text/plain; charset=utf-8", "ok");
        return;
    }
    if (path.startsWith("/host/community/finish/")) {
        const auto token = path.fromFirstOccurrenceOf("/host/community/finish/", false, false);
        juce::File completed;
        juce::var metadata;
        {
            const juce::ScopedLock sl(downloadLock);
            auto it = communityDownloads.find(token);
            if (it == communityDownloads.end()) { sendText(client, 404, "text/plain; charset=utf-8", "Descarga no encontrada"); return; }
            auto& state = it->second;
            if ((state.expected > 0 && state.written != state.expected) || state.written == 0) {
                state.stream.reset(); state.file.deleteFile(); communityDownloads.erase(it);
                sendText(client, 400, "text/plain; charset=utf-8", "La descarga llegó incompleta"); return;
            }
            state.stream->flush(); state.stream.reset(); completed = state.file;
            metadata = state.metadata;
            communityDownloads.erase(it);
        }
        if (onCommunityDownload) onCommunityDownload(completed, metadata);
        sendText(client, 200, "text/plain; charset=utf-8", "ok");
        return;
    }
    if (path.startsWith("/host/community/abort/")) {
        const auto token = path.fromFirstOccurrenceOf("/host/community/abort/", false, false);
        const juce::ScopedLock sl(downloadLock);
        auto it = communityDownloads.find(token);
        if (it != communityDownloads.end()) { it->second.stream.reset(); it->second.file.deleteFile(); communityDownloads.erase(it); }
        sendText(client, 200, "text/plain; charset=utf-8", "ok");
        return;
    }
    sendText(client, 404, "text/plain; charset=utf-8", "Not found");
}
