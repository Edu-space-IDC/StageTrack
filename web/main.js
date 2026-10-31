(function () {
  var queue = [], recur = [], inflight = false, started = false, timer = 0;
  function schedule(ms) { clearTimeout(timer); timer = setTimeout(tick, ms); }
  window.wwr_req = function (x) { queue.push(String(x)); if (started && !inflight) schedule(0); };
  window.wwr_req_recur = function (x, ms) { recur.push({ cmd: String(x), ms: ms || 500, last: 0 }); };
  window.wwr_start = function () { if (started) return; started = true; tick(); };
  // si hay varios SET de volumen/pan de la misma pista en la cola, solo importa el último
  function coalesce(parts) {
    var re = /^SET\/TRACK\/(\d+)\/(VOL|PAN)\//, last = {}, out = [], i, m;
    for (i = 0; i < parts.length; i++) { m = re.exec(parts[i]); if (m) last[m[1] + m[2]] = i; }
    for (i = 0; i < parts.length; i++) { m = re.exec(parts[i]); if (m && last[m[1] + m[2]] !== i) continue; out.push(parts[i]); }
    return out;
  }
  function tick() {
    timer = 0;
    if (inflight) return;                       // se reprograma al terminar la petición en curso
    var now = Date.now(), parts = queue.splice(0, queue.length);
    for (var i = 0; i < recur.length; i++) {
      var r = recur[i];
      if (now - r.last >= r.ms) { r.last = now; parts.push(r.cmd); }
    }
    if (parts.length) send(coalesce(parts).join(';')); else schedule(8);
  }
  function send(body) {
    inflight = true;
    var done = false;
    function finish() { done = true; inflight = false; schedule(queue.length ? 0 : 8); }
    var guard = setTimeout(function () { if (!done) finish(); }, 8000);
    fetch('/_/', { method: 'POST', body: body, cache: 'no-store', headers: { 'Content-Type': 'text/plain' } })
      .then(function (r) { return r.text(); })
      .then(function (t) {
        if (done) return; clearTimeout(guard); finish();
        try { if (typeof window.wwr_onreply === 'function') window.wwr_onreply(t); } catch (e) { console.error(e); }
      })
      .catch(function () { if (!done) { clearTimeout(guard); finish(); } });
  }
  window.mkvolstr = function (vol) {
    vol = parseFloat(vol);
    if (!(vol > 0.0000000298023223876953125)) return '-inf';
    var v = Math.log(vol) * 8.68588963806504;
    return (Math.abs(v) < 10 ? v.toFixed(2) : v.toFixed(1));
  };
  window.dbToLin = window.dbToLin || function (db) { return Math.pow(10, db / 20); };
  window.linToDb = window.linToDb || function (lin) { return 20 * Math.log10(lin); };
})();
