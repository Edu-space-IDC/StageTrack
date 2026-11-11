const $ = (selector) => document.querySelector(selector);
const PAD_KEYS = "C Db D Eb E F Gb G Ab A Bb B".split(" ");
const KEYS = "C C# Db D D# Eb E F F# Gb G G# Ab A A# Bb B Cm C#m Dbm Dm D#m Ebm Em Fm F#m Gbm Gm G#m Abm Am A#m Bbm Bm".split(" ");
const API = String(window.STAGETRACKS_COMMUNITY_API || window.location.origin).replace(/\/+$/, "");
const MAX_COMMUNITY_UPLOAD_BYTES = 2 * 1024 * 1024 * 1024;
let sequences = [];
let searchTimer = 0;
let selectedCoverUrl = "";

$(".eq").innerHTML = Array.from({ length: 28 }, (_, i) => `<i style="animation-delay:${(i * .09).toFixed(2)}s;animation-duration:${1 + i % 5 * .2}s"></i>`).join("");
const FEATURES = ["Pads incorporados", "Ruteo de salidas", "Crea tus secuencias", "Importa desde Reaper", "Solo y Mute", "Buses", "Master"];
$("#mq").innerHTML = [...FEATURES, ...FEATURES].map((feature) => `<span><b>●</b> ${feature}</span>`).join("");
$("#keys").innerHTML = PAD_KEYS.map((key, i) => `<button data-i="${i}">${key}</button>`).join("");

// Demo de pads de la página original.
let audioContext, masterGain, padBus, currentPad, lastPad, padVolume = .6;
$("#pv").oninput = (event) => {
  padVolume = event.target.value / 100;
  if (masterGain) masterGain.gain.setTargetAtTime(padVolume * .6, audioContext.currentTime, .05);
};
function startAudio() {
  audioContext = new AudioContext();
  masterGain = audioContext.createGain(); masterGain.gain.value = padVolume * .6; masterGain.connect(audioContext.destination);
  padBus = audioContext.createGain();
  const buffer = audioContext.createBuffer(2, audioContext.sampleRate * 5, audioContext.sampleRate);
  for (let channel = 0; channel < 2; channel++) {
    const data = buffer.getChannelData(channel);
    for (let i = 0; i < data.length; i++) data[i] = (Math.random() * 2 - 1) * (1 - i / data.length) ** 3;
  }
  const convolver = audioContext.createConvolver(), wet = audioContext.createGain();
  convolver.buffer = buffer; wet.gain.value = 1.1; padBus.connect(convolver); convolver.connect(wet); wet.connect(masterGain);
  const dry = audioContext.createGain(); dry.gain.value = .35; padBus.connect(dry); dry.connect(masterGain);
}
function fadePad(pad) {
  const time = audioContext.currentTime;
  pad.gain.gain.cancelScheduledValues(time); pad.gain.gain.setValueAtTime(pad.gain.gain.value, time);
  pad.gain.gain.linearRampToValueAtTime(0, time + 3.5); pad.oscillators.forEach((oscillator) => oscillator.stop(time + 4));
}
$("#keys").onclick = (event) => {
  const button = event.target.closest("button"); if (!button) return;
  document.querySelectorAll(".keys .on").forEach((key) => key.classList.remove("on"));
  try { audioContext || startAudio(); audioContext.resume(); } catch { return; }
  if (currentPad) { fadePad(currentPad); currentPad = null; }
  if (button === lastPad) { lastPad = null; return; }
  lastPad = button; button.classList.add("on");
  const time = audioContext.currentTime, gain = audioContext.createGain(), filter = audioContext.createBiquadFilter();
  const lfo = audioContext.createOscillator(), lfoGain = audioContext.createGain(), root = 65.41 * 2 ** (Number(button.dataset.i) / 12);
  gain.gain.setValueAtTime(0, time); gain.gain.linearRampToValueAtTime(.5, time + 3.5);
  filter.type = "lowpass"; filter.frequency.value = 650; filter.Q.value = .7; lfo.frequency.value = .12; lfoGain.gain.value = 280;
  lfo.connect(lfoGain); lfoGain.connect(filter.frequency); gain.connect(filter); filter.connect(padBus);
  const oscillators = [[1,"sine",0],[2,"triangle",-7],[2,"triangle",7],[3,"sine",4],[4,"triangle",-5],[6,"sine",6]].map(([multiple,type,detune], index) => {
    const oscillator = audioContext.createOscillator(), voice = audioContext.createGain();
    oscillator.type = type; oscillator.frequency.value = root * multiple; oscillator.detune.value = detune;
    voice.gain.value = [.7,.3,.3,.18,.12,.07][index]; oscillator.connect(voice); voice.connect(gain); oscillator.start(time); return oscillator;
  });
  lfo.start(time); oscillators.push(lfo); currentPad = { gain, oscillators };
};

const windowPreview = $("#win");
windowPreview.onmousemove = (event) => {
  const rect = windowPreview.getBoundingClientRect(), x = (event.clientX - rect.left) / rect.width - .5, y = (event.clientY - rect.top) / rect.height - .5;
  windowPreview.style.transform = `rotateY(${x * 10}deg) rotateX(${-y * 8}deg)`;
};
windowPreview.onmouseleave = () => windowPreview.style.transform = "";
const revealObserver = new IntersectionObserver((entries) => entries.forEach((entry) => {
  if (entry.isIntersecting) { entry.target.classList.add("on"); revealObserver.unobserve(entry.target); }
}), { threshold: .15 });
const reveal = () => document.querySelectorAll(".rv:not(.on)").forEach((element, index) => {
  element.style.transitionDelay = `${index % 4 * 90}ms`; revealObserver.observe(element);
});

function artworkLookup(title, artist) {
  const callback = `stageCover${Date.now()}${Math.floor(Math.random() * 9999)}`;
  return new Promise((resolve, reject) => {
    const script = document.createElement("script"), timeout = setTimeout(() => finish(new Error("timeout")), 7000);
    function finish(error, url) { clearTimeout(timeout); delete window[callback]; script.remove(); error ? reject(error) : resolve(url); }
    window[callback] = (data) => {
      const url = data?.results?.[0]?.artworkUrl100?.replace(/\d+x\d+bb/, "300x300bb");
      finish(url ? null : new Error("not found"), url);
    };
    script.onerror = () => finish(new Error("request failed"));
    script.src = `https://itunes.apple.com/search?media=music&entity=song&limit=1&term=${encodeURIComponent(`${title} ${artist}`)}&callback=${callback}`;
    document.head.appendChild(script);
  });
}

function sequenceCard(item) {
  const card = document.createElement("article"); card.className = "c card rv";
  const cover = document.createElement("div"); cover.className = "cv"; cover.style.backgroundColor = coverColor(item.name || "StageTracks");
  const initials = document.createElement("span"); initials.textContent = item.key || "ST"; cover.appendChild(initials);
  if (item.cover) setCoverImage(cover, item.cover);
  else artworkLookup(item.name || "", item.artist || "").then((url) => setCoverImage(cover, url)).catch(() => {});
  const info = document.createElement("div"); info.style.minWidth = "0";
  const title = document.createElement("h3"); title.style.cssText = "font-size:1.05rem;margin:0"; title.textContent = item.name || "Secuencia";
  const artist = document.createElement("div"); artist.className = "meta"; artist.textContent = `${item.artist || "Comunidad"} · Tono ${item.key || "—"} · ${item.bpm || "—"} BPM · ${item.meter || "4/4"}`;
  const downloads = document.createElement("div"); downloads.className = "meta"; downloads.textContent = `⬇ ${Number(item.downloads || 0).toLocaleString("es")}`;
  info.append(title, artist, downloads);
  const link = document.createElement("a"); link.className = "btn sm"; link.href = `${API}/api/sequences/${encodeURIComponent(item.id)}/download`;
  const downloadName = String(item.downloadName || `${item.name || "StageTracks"}.zip`).replace(/[\\/:*?"<>|]/g, "-");
  link.download = downloadName; link.target = "_blank"; link.rel = "noopener"; link.textContent = "Descargar"; link.setAttribute("aria-label", `Descargar ${item.name || "secuencia"}`);
  const downloadStatus = document.createElement("small"); downloadStatus.className = "meta"; downloadStatus.setAttribute("role", "status"); downloadStatus.setAttribute("aria-live", "polite");
  link.addEventListener("click", async (event) => {
    if (typeof window.showSaveFilePicker !== "function") return;
    event.preventDefault();
    if (link.dataset.downloading === "1") return;
    let handle, writable, reader;
    try {
      handle = await window.showSaveFilePicker({ suggestedName: downloadName });
      link.dataset.downloading = "1"; link.textContent = "Descargando..."; downloadStatus.textContent = "Conectando con la comunidad...";
      const response = await fetch(link.href, { mode: "cors", credentials: "omit", cache: "no-store", headers: { Accept: "application/octet-stream" } });
      if (!response.ok) {
        let message = `La descarga respondio HTTP ${response.status}.`;
        try { message = (await response.json()).error || message; } catch {}
        throw new Error(message);
      }
      if (!response.body) throw new Error("El navegador no pudo abrir el flujo de descarga.");
      const total = Number(response.headers.get("Content-Length")) || Number(item.fileSize) || 0;
      writable = await handle.createWritable();
      reader = response.body.getReader(); let loaded = 0, lastUpdate = 0;
      while (true) {
        const part = await reader.read();
        if (part.done) break;
        await writable.write(part.value); loaded += part.value.byteLength;
        const now = Date.now();
        if (now - lastUpdate > 250) {
          downloadStatus.textContent = total ? `Descargando ${Math.min(100, Math.round(loaded / total * 100))} %` : `Descargando ${(loaded / 1048576).toFixed(0)} MB...`;
          lastUpdate = now;
        }
      }
      await writable.close(); writable = null;
      downloadStatus.textContent = "Descarga completa.";
    } catch (error) {
      if (reader) await reader.cancel().catch(() => {});
      if (writable) await writable.abort().catch(() => {});
      if (error?.name !== "AbortError") downloadStatus.textContent = error?.message || "No se pudo descargar la secuencia.";
    } finally {
      delete link.dataset.downloading; link.textContent = "Descargar";
    }
  });
  const actions = document.createElement("div"); actions.style.cssText = "display:flex;gap:8px;flex-wrap:wrap";
  actions.append(link, downloadStatus);
  card.append(cover, info, actions); return card;
}
const editDialog = $("#editSequenceDialog"), editForm = $("#editSequenceForm"), editStatus = $("#editSequenceStatus");
const editCoverInput = $("#editCover"), editCoverPreview = $("#editCoverPreview"), editCoverUrlInput = $("#editCoverUrl");
function openSequenceEditor(item) {
  editForm.dataset.id = item.id;
  editForm.elements.name.value = item.name || "";
  editForm.elements.artist.value = item.artist || "";
  editForm.elements.key.value = item.key || "C";
  editForm.elements.bpm.value = item.bpm || 120;
  editForm.elements.meter.value = item.meter || "4/4";
  editForm.elements.genre.value = item.genre || "Otro";
  editForm.elements.cover.value = "";
  editCoverUrlInput.value = "";
  editCoverPreview.hidden = !item.cover;
  editCoverPreview.src = item.cover ? (/^https?:/i.test(item.cover) ? item.cover : `${API}${item.cover}`) : "";
  editStatus.textContent = "";
  editDialog.showModal();
}
$("#editSequenceCancel").onclick = () => editDialog.close();
$("#editCoverSearch").onclick = async () => {
  const title = editForm.elements.name.value.trim(), artist = editForm.elements.artist.value.trim(), button = $("#editCoverSearch");
  if (!title) { editStatus.textContent = "Escribe primero el nombre de la canción para buscar su carátula."; return; }
  button.disabled = true; button.textContent = "Buscando carátula…";
  try {
    const url = await artworkLookup(title, artist);
    editCoverUrlInput.value = url; editCoverInput.value = ""; editCoverPreview.src = url; editCoverPreview.hidden = false;
    editStatus.textContent = "Carátula seleccionada. Guarda los cambios para publicarla.";
  } catch { editStatus.textContent = "No encontramos carátula. Puedes subir una imagen."; }
  finally { button.disabled = false; button.textContent = "Buscar carátula automáticamente"; }
};
editCoverInput.onchange = () => {
  const file = editCoverInput.files[0]; if (!file) return;
  editCoverUrlInput.value = ""; editCoverPreview.src = URL.createObjectURL(file); editCoverPreview.hidden = false;
};
editForm.onsubmit = async (event) => {
  event.preventDefault();
  const submit = editForm.querySelector('button[type="submit"]');
  const body = new FormData(editForm);
  if (!editForm.elements.cover.files[0]) body.delete("cover");
  submit.disabled = true; editStatus.textContent = "Guardando cambios…";
  try {
    const response = await fetch(`${API}/api/sequences/${encodeURIComponent(editForm.dataset.id)}`, { method: "PUT", body });
    const result = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(result.error || "No se pudieron guardar los cambios.");
    editStatus.textContent = result.message || "Cambios guardados y visibles para todos.";
    await refreshCommunity($("#q").value.trim());
    setTimeout(() => editDialog.close(), 800);
  } catch (error) { editStatus.textContent = error.message || "No se pudo conectar con la comunidad."; }
  finally { submit.disabled = false; }
};
function setCoverImage(element, url) {
  const resolved = /^https?:/i.test(String(url)) ? url : `${API}${url}`;
  element.style.backgroundImage = `url("${String(resolved).replace(/["\\]/g, "")}")`;
  element.dataset.ok = "1"; const fallback = element.querySelector("span"); if (fallback) fallback.hidden = true;
}
function coverColor(name) { return `hsl(${[...name].reduce((sum, char) => sum + char.charCodeAt(0), 0) * 7 % 60 + 195} 55% 42%)`; }
function renderCards(container, items, emptyMessage) {
  container.replaceChildren();
  if (!items.length) { const empty = document.createElement("p"); empty.className = "meta"; empty.textContent = emptyMessage; container.appendChild(empty); return; }
  items.forEach((item) => container.appendChild(sequenceCard(item))); reveal();
}
async function fetchCommunity(query = "", limit = 50) {
  const response = await fetch(`${API}/api/sequences?q=${encodeURIComponent(query)}&limit=${limit}`, { headers: { Accept: "application/json" }, cache: "no-store" });
  if (!response.ok) throw new Error("La comunidad no respondió. Intenta de nuevo en unos momentos.");
  const data = await response.json(); return data.results || [];
}
function sortSequences(items) {
  const sort = $("#o").value;
  return [...items].sort((a, b) => sort === "d" ? (b.downloads || 0) - (a.downloads || 0) : sort === "n" ? new Date(b.createdAt) - new Date(a.createdAt) : a.name.localeCompare(b.name));
}
function renderCommunity() {
  const query = $("#q").value.trim().toLocaleLowerCase(), key = $("#k").value, genre = $("#gn").value;
  const items = sortSequences(sequences.filter((item) => `${item.name} ${item.artist}`.toLocaleLowerCase().includes(query) && (!key || item.key === key) && (!genre || item.genre === genre)));
  $("#tt").textContent = !query && !key && !genre && $("#o").value === "d" ? "Más descargados" : `${items.length} resultado(s)`;
  renderCards($("#res"), items, "Sin resultados. Prueba otros filtros.");
}
async function refreshCommunity(query = "") {
  try {
    sequences = await fetchCommunity(query, 50);
    if (!$("#k").dataset.ready) {
      $("#k").innerHTML = '<option value="">Tonalidad</option>' + KEYS.map((key) => `<option>${key}</option>`).join("");
      $("#k").dataset.ready = "1";
    }
    renderCommunity();
    renderCards($("#top"), [...sequences].sort((a, b) => (b.downloads || 0) - (a.downloads || 0)).slice(0, 4), "Las secuencias más descargadas aparecerán aquí.");
  } catch (error) {
    sequences = []; $("#tt").textContent = "Comunidad no disponible";
    renderCards($("#res"), [], error.message); renderCards($("#top"), [], "Conecta la API de comunidad para ver secuencias.");
  }
}
function scheduleSearch() {
  clearTimeout(searchTimer);
  searchTimer = setTimeout(async () => {
    try { sequences = await fetchCommunity($("#q").value.trim(), 50); renderCommunity(); }
    catch (error) { $("#tt").textContent = "Comunidad no disponible"; renderCards($("#res"), [], error.message); }
  }, 220);
}
$("#k").innerHTML = '<option value="">Tonalidad</option>' + KEYS.map((key) => `<option>${key}</option>`).join("");
$("#gn").innerHTML = '<option value="">Género</option><option>Adoración</option><option>Gospel</option><option>Contemporáneo</option><option>Acústico</option><option>Rock</option><option>Otro</option>';
$("#q").addEventListener("input", scheduleSearch);
["#k", "#gn", "#o"].forEach((selector) => $(selector).addEventListener("change", renderCommunity));
$("#hs").onsubmit = (event) => { event.preventDefault(); $("#q").value = $("#hq").value; location.hash = "secuencias"; };
function route() {
  const community = location.hash === "#secuencias"; document.body.classList.toggle("vs", community);
  if (community) { refreshCommunity($("#q").value.trim()); scrollTo(0, 0); }
  else { const target = location.hash && $(location.hash); target ? target.scrollIntoView() : scrollTo(0, 0); }
  reveal();
}
addEventListener("hashchange", route);

const themeButton = $("#th");
function setTheme(theme) { document.documentElement.dataset.theme = theme; themeButton.textContent = theme === "dark" ? "☀️" : "🌙"; try { localStorage.setItem("th", theme); } catch {} }
let theme = "light"; try { theme = localStorage.getItem("th") || "light"; } catch {} setTheme(theme);
themeButton.onclick = () => setTheme(document.documentElement.dataset.theme === "dark" ? "light" : "dark");

const navMenu = $("#navMenu"), navMore = $("#navMore");
function closeNavMenu() { navMenu.classList.remove("nav-open"); navMore.setAttribute("aria-expanded", "false"); }
navMore.onclick = () => {
  const open = !navMenu.classList.contains("nav-open");
  navMenu.classList.toggle("nav-open", open); navMore.setAttribute("aria-expanded", String(open));
};
navMenu.querySelectorAll("a").forEach((link) => link.addEventListener("click", closeNavMenu));
document.addEventListener("click", (event) => { if (!event.target.closest("nav")) closeNavMenu(); });
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape" && navMenu.classList.contains("nav-open")) { closeNavMenu(); navMore.focus(); }
});

const dialog = $("#dg"), form = $("#fm"), coverInput = $("#im"), coverPreview = $("#pvw"), status = $("#uploadStatus");
const feedbackDialog = $("#feedbackDialog"), feedbackForm = $("#feedbackForm"), feedbackStatus = $("#feedbackStatus");
$("#feedbackOpen").onclick = () => { feedbackStatus.textContent = ""; feedbackStatus.className = "meta"; feedbackDialog.showModal(); };
if (location.hash === "#feedback" || new URLSearchParams(location.search).get("feedback") === "1") {
  setTimeout(() => { if (!feedbackDialog.open) $("#feedbackOpen").click(); }, 0);
}
$("#feedbackCancel").onclick = () => feedbackDialog.close();
feedbackForm.onsubmit = async (event) => {
  event.preventDefault();
  const submit = feedbackForm.querySelector('button[type="submit"]');
  const body = Object.fromEntries(new FormData(feedbackForm).entries());
  submit.disabled = true; feedbackStatus.textContent = "Enviando…"; feedbackStatus.className = "meta";
  try {
    const response = await fetch(`${API}/api/feedback`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || "No se pudo enviar la sugerencia.");
    feedbackStatus.textContent = result.message || "Gracias. Recibimos tu sugerencia de forma anónima.";
    feedbackStatus.className = "meta success"; feedbackForm.reset();
    setTimeout(() => feedbackDialog.close(), 1400);
  } catch (error) { feedbackStatus.textContent = error.message || "No se pudo conectar con el servidor."; feedbackStatus.className = "meta error"; }
  finally { submit.disabled = false; }
};
$("#fk").innerHTML = KEYS.map((key) => `<option>${key}</option>`).join("");
$("#editKey").innerHTML = KEYS.map((key) => `<option>${key}</option>`).join("");
$("#up").onclick = () => { status.textContent = ""; dialog.showModal(); };
$("#cx").onclick = () => dialog.close();
document.querySelectorAll('a[href="#"]').forEach((link) => link.addEventListener("click", (event) => event.preventDefault()));
coverInput.onchange = (event) => {
  const file = event.target.files[0]; if (!file) return;
  selectedCoverUrl = ""; coverPreview.src = URL.createObjectURL(file); coverPreview.hidden = false;
};
$("#coverSearch").onclick = async () => {
  const title = $("#n").value.trim(), artist = $("#a").value.trim();
  if (!title) { status.textContent = "Escribe primero el nombre de la canción para buscar su carátula."; return; }
  const button = $("#coverSearch"), choices = $("#coverChoices"); button.disabled = true; button.textContent = "Buscando carátulas…"; choices.replaceChildren();
  try {
    const results = await new Promise((resolve, reject) => {
      const callback = `stageCoverPick${Date.now()}`, script = document.createElement("script"), timeout = setTimeout(() => done(new Error("timeout")), 8000);
      function done(error, value) { clearTimeout(timeout); delete window[callback]; script.remove(); error ? reject(error) : resolve(value); }
      window[callback] = (data) => done(null, data?.results || []); script.onerror = () => done(new Error("request"));
      script.src = `https://itunes.apple.com/search?media=music&entity=song&limit=8&term=${encodeURIComponent(`${title} ${artist}`)}&callback=${callback}`; document.head.appendChild(script);
    });
    if (!results.length) { status.textContent = "No encontramos carátulas; puedes subir una imagen."; return; }
    status.textContent = "Elige la carátula que corresponde a la canción.";
    results.forEach((result) => {
      if (!result.artworkUrl100) return;
      const url = result.artworkUrl100.replace(/\d+x\d+bb/, "600x600bb"), option = document.createElement("button"); option.type = "button"; option.className = "cover-choice";
      const image = document.createElement("img"); image.src = url; image.alt = result.trackName || result.collectionName || "Carátula";
      const name = document.createElement("span"); name.textContent = result.trackName || result.collectionName || "Elegir"; option.append(image, name);
      option.onclick = () => { selectedCoverUrl = url; coverInput.value = ""; coverPreview.src = url; coverPreview.hidden = false; choices.replaceChildren(); status.textContent = "Carátula seleccionada."; };
      choices.appendChild(option);
    });
  } catch { status.textContent = "No se pudo buscar la carátula ahora. Puedes subir una imagen."; }
  finally { button.disabled = false; button.textContent = "Buscar carátula automáticamente"; }
};
async function uploadCommunitySequence(metadata, packageFile, coverFile, status) {
  const totalBytes = packageFile.size + (coverFile?.size || 0);
  const start = await fetch(`${API}/api/sequences/uploads`, {
    method: "POST", mode: "cors", credentials: "omit", cache: "no-store", headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ ...metadata, packageSize: packageFile.size, packageName: packageFile.name, coverSize: coverFile?.size || 0, coverName: coverFile?.name || "" }),
  }).catch((error) => { throw new Error(`No se pudo conectar con la API para iniciar la subida. ${error?.message || "Comprueba internet e inténtalo de nuevo."}`); });
  const session = await start.json().catch(() => ({}));
  if (!start.ok) throw new Error(session.error || `La API respondió HTTP ${start.status}.`);
  let sentBytes = 0;
  const files = [{ field: "package", file: packageFile }, ...(coverFile ? [{ field: "cover", file: coverFile }] : [])];
  for (const { field, file } of files) {
    const parts = Math.ceil(file.size / session.chunkSize);
    for (let part = 0; part < parts; part++) {
      const blob = file.slice(part * session.chunkSize, Math.min(file.size, (part + 1) * session.chunkSize));
      await new Promise((resolve, reject) => {
        const request = new XMLHttpRequest();
        request.open("PUT", `${API}/api/sequences/uploads/${session.uploadId}/${field}/${part}`);
        request.setRequestHeader("Content-Type", "application/octet-stream");
        request.timeout = 10 * 60 * 1000;
        request.upload.onprogress = (event) => {
          const current = sentBytes + (event.lengthComputable ? event.loaded : 0);
          status.textContent = `Subiendo ${Math.min(100, Math.round(current / totalBytes * 100))} % (${field === "package" ? "paquete" : "carátula"})…`;
        };
        request.onload = () => {
          let result = {};
          try { result = JSON.parse(request.responseText || "{}"); } catch {}
          if (request.status < 200 || request.status >= 300) { reject(new Error(result.error || `La API respondió HTTP ${request.status}.`)); return; }
          sentBytes += blob.size; resolve();
        };
        request.onerror = () => reject(new Error("Se perdió la conexión con la API. Revisa internet e inténtalo otra vez."));
        request.ontimeout = () => reject(new Error("Un bloque tardó demasiado. Revisa tu conexión y vuelve a intentarlo."));
        request.send(blob);
      });
    }
  }
  status.textContent = "Archivo recibido. Guardándolo en Drive en segundo plano…";
  const complete = await fetch(`${API}/api/sequences/uploads/${session.uploadId}/complete`, { method: "POST", mode: "cors", credentials: "omit", cache: "no-store" })
    .catch((error) => { throw new Error(`La secuencia se transfirió, pero se perdió la conexión al iniciar el guardado. Revisa la comunidad antes de volver a subirla. ${error?.message || ""}`); });
  let result = await complete.json().catch(() => ({}));
  if (!complete.ok) throw new Error(result.error || `La API respondió HTTP ${complete.status}.`);
  if (result.status === "processing") {
    const deadline = Date.now() + 30 * 60 * 1000;
    while (Date.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, 2000));
      const response = await fetch(`${API}/api/sequences/uploads/${session.uploadId}/status`, { mode: "cors", credentials: "omit", cache: "no-store" }).catch(() => null);
      if (!response) { status.textContent = "Drive sigue guardando la secuencia. Reintentando conexión…"; continue; }
      result = await response.json().catch(() => ({}));
      if (response.ok && result.jobStatus === "complete") return result;
      if (!response.ok) throw new Error(result.error || `La API respondió HTTP ${response.status}.`);
      status.textContent = "Drive sigue guardando la secuencia…";
    }
    throw new Error("La transferencia terminó, pero Drive sigue procesando el archivo. Revisa la comunidad antes de volver a subirlo.");
  }
  return result;
}

async function publishSelectedSequence({ background = false } = {}) {
  const file = $("#sq").files[0], publish = form.querySelector('button[type="submit"]');
  const progressStatus = background ? $("#backgroundUploadStatus") : status;
  if (!file) { status.textContent = "Adjunta un paquete de secuencia .rpseq o .zip."; return; }
  if (file.size > MAX_COMMUNITY_UPLOAD_BYTES) { status.textContent = "El paquete supera el límite de 2 GB."; return; }
  const coverFile = coverInput.files[0] || null;
  if (coverFile?.size > 8 * 1024 * 1024) { status.textContent = "La carátula debe pesar hasta 8 MB."; return; }
  const metadata = { name: $("#n").value.trim(), artist: $("#a").value.trim(), key: $("#fk").value, bpm: String(Math.round(Number($("#b").value))), meter: $("#meter").value, genre: $("#fg").value };
  if (!coverFile && selectedCoverUrl) metadata.coverUrl = selectedCoverUrl;
  publish.disabled = true; progressStatus.textContent = "Preparando la carga…";
  if (background) progressStatus.hidden = false;
  if (background) dialog.close();
  try {
    const result = await uploadCommunitySequence(metadata, file, coverFile, progressStatus);
    progressStatus.textContent = result.message; await refreshCommunity($("#q").value.trim());
    if (result.status === "approved") { form.reset(); coverPreview.hidden = true; $("#coverChoices").replaceChildren(); selectedCoverUrl = ""; if (dialog.open) setTimeout(() => dialog.close(), 900); }
  } catch (error) { progressStatus.textContent = error.message || "No se pudo conectar con la comunidad."; }
  finally { publish.disabled = false; if (background) setTimeout(() => { progressStatus.hidden = true; }, 9000); }
}
form.onsubmit = (event) => { event.preventDefault(); publishSelectedSequence(); };
$("#continueUpload").onclick = () => publishSelectedSequence({ background: true });

document.addEventListener("pointermove", (event) => {
  const card = event.target.closest?.(".c"); if (!card) return;
  const rect = card.getBoundingClientRect(); card.style.setProperty("--mx", `${event.clientX - rect.left}px`); card.style.setProperty("--my", `${event.clientY - rect.top}px`);
});
addEventListener("scroll", () => { $("#pg").style.transform = `scaleX(${scrollY / Math.max(1, document.documentElement.scrollHeight - innerHeight)})`; }, { passive: true });
fetch("/downloads/release.json").then((response) => response.ok ? response.json() : null).then((release) => {
  const configured = window.STAGETRACKS_DOWNLOADS || {};
  const local = location.hostname === "localhost" || location.hostname === "127.0.0.1";
  const applyDownload = (selector, url) => {
    const link = $(selector);
    if (url) { link.href = url.startsWith("/") ? new URL(url, window.location.origin).href : url; link.removeAttribute("aria-disabled"); }
    else { link.removeAttribute("href"); link.setAttribute("aria-disabled", "true"); link.title = "Configura el enlace de descarga en community-config.js"; }
  };
  applyDownload("#downloadStage", release?.installer || configured.installer || (local ? release?.installer : ""));
  applyDownload("#downloadPortable", release?.portable || configured.portable || (local ? release?.portable : ""));
  applyDownload("#downloadStudioExe", release?.reaperStudioExe || configured.reaperStudioExe || (local ? release?.reaperStudioExe : ""));
}).catch(() => {});
refreshCommunity(""); route();
