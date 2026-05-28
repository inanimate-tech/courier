// Binary WebSocket relay: one fixed Durable Object instance fans binary audio
// frames from the device out to every browser viewer. No agents SDK; the DO is
// hand-rolled on the WebSocket Hibernation API.

export class AudioRelay implements DurableObject {
  constructor(private ctx: DurableObjectState, private env: Env) {}

  async fetch(request: Request): Promise<Response> {
    if (request.headers.get("Upgrade") !== "websocket") {
      return new Response("expected websocket", { status: 426 });
    }
    const url = new URL(request.url);
    const role = url.searchParams.get("monitor") === "1" ? "monitor" : "device";

    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];

    this.ctx.acceptWebSocket(server);
    // Role survives hibernation — used to pick broadcast targets.
    server.serializeAttachment({ role });

    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws: WebSocket, message: ArrayBuffer | string): Promise<void> {
    // This demo only relays binary audio; ignore any text.
    if (typeof message === "string") return;
    for (const sock of this.ctx.getWebSockets()) {
      const att = sock.deserializeAttachment() as { role?: string } | null;
      if (att && att.role === "monitor") {
        try {
          sock.send(message);
        } catch {
          /* viewer went away mid-send; drop */
        }
      }
    }
  }

  async webSocketClose(ws: WebSocket, code: number, reason: string): Promise<void> {
    try {
      ws.close(code, reason);
    } catch {
      /* already closed */
    }
  }
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);

    if (url.pathname === "/ws") {
      const id = env.AudioRelay.idFromName("audio"); // single fixed instance
      return env.AudioRelay.get(id).fetch(request);
    }

    if (url.pathname === "/" || url.pathname === "") {
      return new Response(PAGE, {
        headers: { "content-type": "text/html; charset=utf-8" },
      });
    }

    return new Response("Not found", { status: 404 });
  },
} satisfies ExportedHandler<Env>;

const PAGE = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Binary WebSocket - live audio FFT</title>
<style>
  :root { color-scheme: dark; }
  body { margin:0; background:#0b0b10; color:#e6e6f0;
         font:14px/1.4 system-ui, sans-serif; display:flex;
         flex-direction:column; height:100vh; }
  header { padding:12px 16px; }
  h1 { font-size:16px; margin:0 0 4px; }
  #status { opacity:.7; }
  #wrap { flex:1; position:relative; }
  canvas { width:100%; height:100%; display:block; }
</style>
</head>
<body>
<header>
  <h1>Binary WebSocket - live audio FFT</h1>
  <div id="status">connecting...</div>
</header>
<div id="wrap"><canvas id="c"></canvas></div>
<script>
(function () {
  var N = 512;        // FFT size / samples per binary frame
  var NUM_BARS = 48;
  var statusEl = document.getElementById("status");
  var canvas = document.getElementById("c");
  var ctx = canvas.getContext("2d");

  // Hann window
  var win = new Float32Array(N);
  for (var i = 0; i < N; i++) win[i] = 0.5 * (1 - Math.cos(2 * Math.PI * i / (N - 1)));

  var re = new Float32Array(N), im = new Float32Array(N);
  var mags = new Float32Array(NUM_BARS);  // latest computed (0..1)
  var bars = new Float32Array(NUM_BARS);  // smoothed for display

  // Log-spaced bin ranges over [1, N/2)
  var ranges = [], half = N / 2, minBin = 1, maxBin = half;
  for (var b = 0; b < NUM_BARS; b++) {
    var lo = Math.floor(minBin * Math.pow(maxBin / minBin, b / NUM_BARS));
    var hi = Math.floor(minBin * Math.pow(maxBin / minBin, (b + 1) / NUM_BARS));
    if (hi <= lo) hi = lo + 1;
    if (hi > half) hi = half;
    ranges.push([lo, hi]);
  }

  // In-place iterative radix-2 FFT.
  function fft(re, im) {
    var n = re.length, j = 0, i, bit, len, k, ang, wr, wi, cr, ci;
    var ur, ui, vr, vi, ncr, tr, ti, hlen;
    for (i = 1; i < n; i++) {
      bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) { tr = re[i]; re[i] = re[j]; re[j] = tr;
                   ti = im[i]; im[i] = im[j]; im[j] = ti; }
    }
    for (len = 2; len <= n; len <<= 1) {
      hlen = len >> 1;
      ang = -2 * Math.PI / len; wr = Math.cos(ang); wi = Math.sin(ang);
      for (i = 0; i < n; i += len) {
        cr = 1; ci = 0;
        for (k = 0; k < hlen; k++) {
          ur = re[i + k]; ui = im[i + k];
          vr = re[i + k + hlen] * cr - im[i + k + hlen] * ci;
          vi = re[i + k + hlen] * ci + im[i + k + hlen] * cr;
          re[i + k] = ur + vr; im[i + k] = ui + vi;
          re[i + k + hlen] = ur - vr; im[i + k + hlen] = ui - vi;
          ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
        }
      }
    }
  }

  function onFrame(buf) {
    // Device sends little-endian int16 PCM; browsers are little-endian.
    var pcm = new Int16Array(buf);
    if (pcm.length < N) return;
    for (var i = 0; i < N; i++) { re[i] = (pcm[i] / 32768) * win[i]; im[i] = 0; }
    fft(re, im);
    var scale = N / 4;  // full-scale tone -> ~1.0 at its bin
    for (var b = 0; b < NUM_BARS; b++) {
      var lo = ranges[b][0], hi = ranges[b][1], peak = 0;
      for (var k = lo; k < hi; k++) {
        var m = Math.sqrt(re[k] * re[k] + im[k] * im[k]);
        if (m > peak) peak = m;
      }
      var v = peak / scale;
      var db = 20 * Math.log10(v + 1e-9);   // ~ -180..0 dB
      var norm = (db + 60) / 60;            // -60 dB -> 0, 0 dB -> 1
      mags[b] = norm < 0 ? 0 : (norm > 1 ? 1 : norm);
    }
  }

  function resize() {
    var dpr = window.devicePixelRatio || 1;
    canvas.width = canvas.clientWidth * dpr;
    canvas.height = canvas.clientHeight * dpr;
  }
  window.addEventListener("resize", resize);
  resize();

  function draw() {
    var w = canvas.width, h = canvas.height, dpr = window.devicePixelRatio || 1;
    ctx.clearRect(0, 0, w, h);
    var gap = 2 * dpr;
    var bw = (w - gap * (NUM_BARS + 1)) / NUM_BARS;
    for (var b = 0; b < NUM_BARS; b++) {
      var target = mags[b];
      bars[b] += (target - bars[b]) * (target > bars[b] ? 0.6 : 0.12);
      var bh = bars[b] * h;
      var x = gap + b * (bw + gap);
      var hue = 200 - 160 * (b / NUM_BARS);
      ctx.fillStyle = "hsl(" + hue + ",80%," + (40 + 30 * bars[b]) + "%)";
      ctx.fillRect(x, h - bh, bw, bh);
    }
    requestAnimationFrame(draw);
  }
  requestAnimationFrame(draw);

  function connect() {
    var proto = location.protocol === "https:" ? "wss:" : "ws:";
    var ws = new WebSocket(proto + "//" + location.host + "/ws?monitor=1");
    ws.binaryType = "arraybuffer";
    ws.onopen = function () { statusEl.textContent = "connected - waiting for stream..."; };
    ws.onmessage = function (ev) {
      if (typeof ev.data === "string") return;
      statusEl.textContent = "streaming";
      onFrame(ev.data);
    };
    ws.onclose = function () {
      statusEl.textContent = "disconnected - retrying...";
      setTimeout(connect, 1000);
    };
    ws.onerror = function () { try { ws.close(); } catch (e) {} };
  }
  connect();
})();
</script>
</body>
</html>`;
