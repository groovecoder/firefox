#!/usr/bin/env python3
"""EXIF strip test server.

Run:  python3 server.py
Then open http://localhost:8742/ in Firefox Nightly.
Uploaded files are saved to ./uploads/ for inspection with exiftool.
"""

import cgi
import io
import os
import struct
import zlib
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = 8742
UPLOAD_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "uploads")


# ---------------------------------------------------------------------------
# Test image generation
# ---------------------------------------------------------------------------


def _build_exif_tiff():
    BYTE, ASCII, LONG, RATIONAL = 1, 2, 4, 5
    ifd0_offset = 8
    gps_ifd_offset = 26
    lat_offset = 92
    lon_offset = 116
    gps_entries = (
        struct.pack("<HHI", 0x0000, BYTE, 4)
        + b"\x02\x02\x00\x00"
        + struct.pack("<HHI", 0x0001, ASCII, 2)
        + b"N\x00\x00\x00"
        + struct.pack("<HHII", 0x0002, RATIONAL, 3, lat_offset)
        + struct.pack("<HHI", 0x0003, ASCII, 2)
        + b"W\x00\x00\x00"
        + struct.pack("<HHII", 0x0004, RATIONAL, 3, lon_offset)
    )
    gps_ifd = struct.pack("<H", 5) + gps_entries + struct.pack("<I", 0)
    ifd0 = (
        struct.pack("<H", 1)
        + struct.pack("<HHII", 0x8825, LONG, 1, gps_ifd_offset)
        + struct.pack("<I", 0)
    )
    tiff_header = b"II" + struct.pack("<HI", 42, ifd0_offset)
    lat_data = struct.pack("<IIIIII", 37, 1, 46, 1, 30, 1)
    lon_data = struct.pack("<IIIIII", 122, 1, 25, 1, 0, 1)
    return tiff_header + ifd0 + gps_ifd + lat_data + lon_data


def _png_chunk(chunk_type, data):
    crc = zlib.crc32(chunk_type + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + chunk_type + data + struct.pack(">I", crc)


JPEG_BYTES = (
    b"\xff\xd8"
    + b"\xff\xe1"
    + struct.pack(">H", 2 + 6 + len(_build_exif_tiff()))
    + b"Exif\x00\x00"
    + _build_exif_tiff()
    + b"\xff\xd9"
)

PNG_BYTES = (
    b"\x89PNG\r\n\x1a\n"
    + _png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
    + _png_chunk(b"eXIf", _build_exif_tiff())
    + _png_chunk(b"IDAT", zlib.compress(b"\x00\xff\xff\xff"))
    + _png_chunk(b"IEND", b"")
)


# ---------------------------------------------------------------------------
# HTML page
# ---------------------------------------------------------------------------


def _build_page():
    return """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Firefox Photo Privacy</title>
<style>
  *, *::before, *::after { box-sizing: border-box; }
  html, body { height: 100%; margin: 0; }
  body { font-family: system-ui, sans-serif; display: flex; flex-direction: column;
          background: #210340; color: #e8e0f0; }
  .page-header { padding: 0.7em 1.5em; border-bottom: 1px solid #3a0560;
                  background: #180228; display: flex; align-items: center;
                  gap: 1.2em; flex-wrap: wrap; }
  .page-header h1 { margin: 0; font-size: 1.3em; color: #fff; flex: 1; }
  .page-header p { margin: 0.15em 0 0; font-size: 0.88em; color: #b09ac0; flex-basis: 100%; }
  .protection-badge { display: inline-flex; align-items: center; gap: 0.4em;
                       padding: 0.3em 0.9em; border-radius: 999px; font-size: 0.82em;
                       font-weight: 600; white-space: nowrap; cursor: pointer;
                       text-decoration: none; border: 2px solid transparent; }
  .protection-badge.unknown { background: #3a0560; color: #b09ac0; border-color: #5a1080; }
  .protection-badge.off { background: #FF9400; color: #210340; border-color: #e07800; }
  .protection-badge.ask { background: #0090ed; color: #fff; border-color: #0070c0; }
  .protection-badge.on  { background: #00d230; color: #003a0d; border-color: #00a825; }
  .badge-dot { width: 8px; height: 8px; border-radius: 50%; background: currentColor; }
  #layout { flex: 1; display: flex; min-height: 0; }
  #steps-col { width: 540px; min-width: 300px; overflow-y: auto; padding: 1em 1.5em 2em; }
  #map-col { flex: 1; display: flex; flex-direction: column; padding: 1em;
              border-left: 2px solid #3a0560; background: #1a0230; gap: 0.5em; min-width: 0; }
  #map-col h2 { margin-top: 0; color: #e8e0f0; }
  #map { flex: 1; min-height: 0; border-radius: 6px; display: none; }
  .card { background: #2e0550; border-radius: 8px; padding: 1em 1.2em;
           box-shadow: 0 1px 6px rgba(0,0,0,0.4); margin-bottom: 1em; }
  .card h3 { margin: 0 0 0.4em; font-size: 1em; color: #f0eaf8; }
  .card p { margin: 0.3em 0 0; font-size: 0.9em; color: #b09ac0; }
  .test-images { display: flex; gap: 0.6em; margin-top: 0.6em; }
  .test-images a { display: inline-block; padding: 0.35em 0.8em; background: #5a1080;
                    color: #e8e0f0; border-radius: 5px; font-size: 0.82em;
                    text-decoration: none; font-weight: 600; }
  .test-images a:hover { background: #7a30a0; }
  .upload-area { border: 2px dashed #5a1080; border-radius: 8px; padding: 1.5em;
                  text-align: center; color: #9080a8; background: #2a0448;
                  cursor: pointer; transition: background 0.15s, border-color 0.15s; }
  .upload-area:hover, .upload-area.over { background: #3a0560; border-color: #b060e0; color: #e8e0f0; }
  .upload-area p { margin: 0.4em 0 0; font-size: 0.85em; }
  .upload-btn { display: inline-block; margin-top: 0.8em; padding: 0.5em 1.2em;
                 background: #FF9400; color: #210340; border-radius: 6px; font-size: 0.9em;
                 font-weight: 600; cursor: pointer; border: none; }
  .upload-btn:hover { background: #ffaa30; }
  #file-input { display: none; }
  .result-card { display: flex; align-items: flex-start; gap: 0.8em;
                  border-radius: 8px; padding: 1em 1.2em; margin-bottom: 0;
                  border-left: 5px solid #5a1080; background: #2a0448; }
  .result-card.danger { border-left-color: #FF9400; background: #3a1a00; }
  .result-card.success { border-left-color: #00d230; background: #003a14; }
  .result-card.pending { border-left-color: #5a1080; color: #9080a8; }
  .result-card.waiting { border-left-color: #0090ed; background: #001a3a; }
  .result-icon { font-size: 2em; line-height: 1; flex-shrink: 0; }
  .result-card strong { display: block; margin-bottom: 0.2em; color: #f0eaf8; }
  .result-card p { margin: 0; font-size: 0.88em; color: #b09ac0; }
  .action-card { border-left: 4px solid #FF9400; }
  .action-card h3 { color: #f0eaf8; }
  .settings-btn { display: inline-block; margin-top: 0.7em; padding: 0.5em 1.1em;
                   background: #FF9400; color: #210340; border-radius: 6px;
                   text-decoration: none; font-size: 0.9em; font-weight: 600; }
  .settings-btn:hover { background: #ffaa30; }
  .action-card small { display: block; margin-top: 0.6em; font-size: 0.82em; color: #9080a8; }
  #map-status { padding: 0.7em 1em; border-radius: 6px; font-size: 0.92em; }
  #map-status.danger { background: #3a1a00; border-left: 4px solid #FF9400; color: #ffb347; }
  #map-status.success { background: #003a14; border-left: 4px solid #00d230; color: #40ff80; }
  #map-status.pending { background: #2a0448; border-left: 4px solid #5a1080; color: #9080a8; }
  #event-log { margin-top: 1em; font-family: monospace; font-size: 0.82em;
               background: #180228; border-radius: 6px; padding: 0.8em 1em;
               max-height: 300px; overflow-y: auto; border: 1px solid #3a0560; }
  #event-log h4 { margin: 0 0 0.5em; color: #b09ac0; font-size: 0.95em; }
  .log-entry { padding: 0.25em 0; border-bottom: 1px solid #2e0550; }
  .log-entry:last-child { border-bottom: none; }
  .log-time { color: #5a1080; }
  .log-leaked { color: #FF9400; font-weight: 600; }
  .log-safe { color: #00d230; font-weight: 600; }
  .log-wait { color: #0090ed; font-weight: 600; }
  @keyframes pulse { 0%,100% { opacity: 1; } 50% { opacity: 0.4; } }
  .pulse { animation: pulse 1.5s infinite; }
</style>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
</head>
<body>
<div class="page-header">
  <h1>Firefox Photo Privacy</h1>
  <a id="protection-badge" class="protection-badge unknown"
     href="about:preferences#privacy" target="_blank">
    <span class="badge-dot"></span>
    <span id="badge-label">Protection: --</span>
  </a>
  <p>Photos can secretly share your location. Firefox can remove that before upload.</p>
</div>
<div id="layout">
  <div id="steps-col">
    <div class="card">
      <h3>Upload a photo</h3>
      <p>Drop or choose any photo from your device, or use a test image:</p>
      <div class="test-images">
        <a href="/test-jpeg" download="gps-test.jpg">Download test JPEG (with GPS)</a>
        <a href="/test-png" download="gps-test.png">Download test PNG (with GPS)</a>
      </div>
      <div id="upload-area" class="upload-area" style="margin-top:0.8em">
        <div>Drag a photo here, or paste (Ctrl+V / Cmd+V)</div>
        <p>or</p>
        <button class="upload-btn" id="upload-btn">Choose a photo</button>
        <input type="file" id="file-input" accept="image/jpeg,image/png">
      </div>
    </div>

    <div id="upload-result" style="display:none" class="result-card pending"></div>

    <div id="action-card" class="card action-card" style="display:none">
      <h3>Enable Firefox protection</h3>
      <p>To toggle Firefox protection:</p>
      <a class="settings-btn" href="about:preferences#privacy" target="_blank">Open Privacy Settings</a>
      <small>Set <code>privacy.removeExifOnUpload</code> in about:config (0=Never, 1=Ask, 2=Always)</small>
    </div>

    <div id="event-log">
      <h4>Event timeline</h4>
      <div id="log-entries"><div class="log-entry" style="color:#9080a8">Waiting for file input ...</div></div>
    </div>
  </div>
  <div id="map-col">
    <h2>Location</h2>
    <div id="map-status" class="pending">No photo uploaded yet.</div>
    <div id="map"></div>
  </div>
</div>

<script>
function findExifInJpeg(u8) {
  if (u8.length < 2 || u8[0] !== 0xFF || u8[1] !== 0xD8) return null;
  let pos = 2;
  while (pos + 3 < u8.length) {
    if (u8[pos] !== 0xFF) break;
    const marker = u8[pos+1];
    if (marker === 0xD9 || marker === 0xDA) break;
    const len = (u8[pos+2] << 8) | u8[pos+3];
    if (len < 2) break;
    if (marker === 0xE1 && len >= 8) {
      const d = u8.subarray(pos+4, pos+4+len-2);
      if (d[0]===69&&d[1]===120&&d[2]===105&&d[3]===102&&d[4]===0&&d[5]===0) return d.slice(6);
    }
    pos += 2 + len;
  }
  return null;
}

function findExifInPng(u8) {
  if (u8.length < 8) return null;
  const sig = [0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A];
  for (let i = 0; i < 8; i++) if (u8[i] !== sig[i]) return null;
  let pos = 8;
  while (pos + 12 <= u8.length) {
    const len = (u8[pos]<<24|u8[pos+1]<<16|u8[pos+2]<<8|u8[pos+3]) >>> 0;
    const type = String.fromCharCode(u8[pos+4],u8[pos+5],u8[pos+6],u8[pos+7]);
    if (type === 'eXIf') return u8.slice(pos+8, pos+8+len);
    if (type === 'IEND') break;
    pos += 12 + len;
  }
  return null;
}

function parseGps(tiff) {
  if (!tiff || tiff.length < 8) return null;
  const le = tiff[0] === 0x49;
  const r16 = o => le ? tiff[o]|tiff[o+1]<<8 : tiff[o]<<8|tiff[o+1];
  const r32 = o => (le
    ? tiff[o]|tiff[o+1]<<8|tiff[o+2]<<16|tiff[o+3]<<24
    : tiff[o]<<24|tiff[o+1]<<16|tiff[o+2]<<8|tiff[o+3]) >>> 0;
  const rat = o => { const n=r32(o),d=r32(o+4); return d?n/d:0; };
  if (r16(2) !== 42) return null;
  const ifd0 = r32(4);
  const n0 = r16(ifd0);
  let gps = null;
  for (let i = 0; i < n0; i++) {
    const b = ifd0+2+i*12;
    if (r16(b) === 0x8825) { gps = r32(b+8); break; }
  }
  if (gps === null) return null;
  const ng = r16(gps);
  let latRef='?', lonRef='?', lat=null, lon=null;
  for (let i = 0; i < ng; i++) {
    const b = gps+2+i*12;
    const tag=r16(b), type=r16(b+2), cnt=r32(b+4), voff=r32(b+8);
    if (tag===1) latRef=String.fromCharCode(tiff[b+8]);
    else if (tag===3) lonRef=String.fromCharCode(tiff[b+8]);
    else if (tag===2&&type===5&&cnt===3) lat=rat(voff)+rat(voff+8)/60+rat(voff+16)/3600;
    else if (tag===4&&type===5&&cnt===3) lon=rat(voff)+rat(voff+8)/60+rat(voff+16)/3600;
  }
  return lat!==null&&lon!==null ? {lat,latRef,lon,lonRef} : null;
}

function extractGps(buf) {
  const u8 = new Uint8Array(buf);
  const tiff = findExifInJpeg(u8) || findExifInPng(u8);
  return tiff ? parseGps(tiff) : null;
}

async function reverseGeocode(lat, lon) {
  try {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 2000);
    const r = await fetch(
      `https://nominatim.openstreetmap.org/reverse?lat=${lat}&lon=${lon}&format=json`,
      { signal: controller.signal, headers: { 'Accept-Language': 'en' } }
    );
    clearTimeout(timer);
    const j = await r.json();
    const a = j.address || {};
    const city = a.city || a.town || a.village || a.county || '';
    const state = a.state || '';
    return city && state ? `${city}, ${state}` : city || state || null;
  } catch (_) {
    return null;
  }
}

function updateProtectionBadge(mode) {
  const badge = document.getElementById('protection-badge');
  const label = document.getElementById('badge-label');
  if (mode === 'leaked') {
    badge.className = 'protection-badge off';
    label.textContent = 'Location: LEAKED';
  } else if (mode === 'stripped') {
    badge.className = 'protection-badge on';
    label.textContent = 'Location: REMOVED';
  } else if (mode === 'ask') {
    badge.className = 'protection-badge ask';
    label.textContent = 'Waiting for decision ...';
  } else {
    badge.className = 'protection-badge on';
    label.textContent = 'Clean (no GPS)';
  }
}

let _map = null, _marker = null;
async function updateMap(gps) {
  const statusEl = document.getElementById('map-status');
  const mapEl = document.getElementById('map');
  if (!gps) {
    statusEl.className = 'success';
    statusEl.textContent = 'No location data found in delivered file';
    mapEl.style.display = 'none';
    return;
  }
  const lat = gps.latRef === 'S' ? -gps.lat : gps.lat;
  const lon = gps.lonRef === 'W' ? -gps.lon : gps.lon;
  statusEl.className = 'danger';
  statusEl.textContent = `Location detected: ${lat.toFixed(5)}, ${lon.toFixed(5)}`;
  mapEl.style.display = 'block';
  if (!_map) {
    _map = L.map('map').setView([lat, lon], 13);
    L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
      attribution: '\\xa9 <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors'
    }).addTo(_map);
    _marker = L.marker([lat, lon]).addTo(_map);
  } else {
    _map.setView([lat, lon], 13);
    _marker.setLatLng([lat, lon]);
  }
  const city = await reverseGeocode(lat, lon);
  if (city) statusEl.textContent = `Location detected: ${city}`;
}

let _clickTime = null;

function showWaiting() {
  const el = document.getElementById('upload-result');
  el.style.display = 'flex';
  el.className = 'result-card waiting pulse';
  el.innerHTML = `<span class="result-icon">&#9201;</span>
    <div><strong>Waiting for your decision in Firefox dialog ...</strong>
    <p>Firefox detected GPS location data. The file is held back until you choose.</p>
    <p style="margin-top:0.4em;color:#6ab0e8">The page has NOT received any file data yet.</p></div>`;
  document.getElementById('action-card').style.display = 'none';
}

function showResult(gps, fileSize) {
  const el = document.getElementById('upload-result');
  el.style.display = 'flex';
  el.classList.remove('pulse');
  const sizeNote = fileSize ? ` (${fileSize} bytes delivered)` : '';
  const elapsed = _clickTime ? ((performance.now() - _clickTime) / 1000).toFixed(1) : '?';
  if (gps) {
    el.className = 'result-card danger';
    el.innerHTML = `<span class="result-icon">&#9888;&#65039;</span>
      <div><strong>Your location was shared</strong>
      <p>GPS coordinates found in the delivered file${sizeNote}.</p>
      <p style="margin-top:0.3em;font-size:0.82em;color:#9080a8">File arrived ${elapsed}s after picker click.</p></div>`;
    document.getElementById('action-card').style.display = 'block';
    updateProtectionBadge('leaked');
  } else {
    el.className = 'result-card success';
    el.innerHTML = `<span class="result-icon">&#128737;&#65039;</span>
      <div><strong>Firefox protected this upload</strong>
      <p>No GPS location data in the delivered file${sizeNote}.</p>
      <p style="margin-top:0.3em;font-size:0.82em;color:#9080a8">File arrived ${elapsed}s after picker click.</p></div>`;
    document.getElementById('action-card').style.display = 'none';
    updateProtectionBadge('stripped');
  }
  updateMap(gps);
}

const _t0 = performance.now();
function ts() { return ((performance.now() - _t0) / 1000).toFixed(3) + 's'; }

function log(source, msg, cls) {
  const el = document.getElementById('log-entries');
  if (el.querySelector('[style]')) el.innerHTML = '';
  const span = cls ? `<span class="${cls}">` : '<span>';
  const div = document.createElement('div');
  div.className = 'log-entry';
  div.innerHTML = `<span class="log-time">[${ts()}]</span> <b>${source}</b>: ${span}${msg}</span>`;
  el.appendChild(div);
  el.parentElement.scrollTop = el.parentElement.scrollHeight;
}

async function greedyExtract(file, source) {
  log(source, `got file: ${file.name} (${file.size} bytes, ${file.type})`);
  const buf = await file.arrayBuffer();
  log(source, `read ${buf.byteLength} bytes into page memory`);
  const gps = extractGps(buf);
  if (gps) {
    const lat = (gps.latRef === 'S' ? -1 : 1) * gps.lat;
    const lon = (gps.lonRef === 'W' ? -1 : 1) * gps.lon;
    log(source, `LOCATION LEAKED: ${lat.toFixed(5)}, ${lon.toFixed(5)}`, 'log-leaked');
  } else {
    log(source, 'no GPS data found in delivered file bytes', 'log-safe');
  }
  showResult(gps, buf.byteLength);
  log(source, 'uploading to server ...');
  const fd = new FormData();
  fd.append('file', file, file.name);
  const resp = await fetch('/upload', {method:'POST', body:fd});
  const json = await resp.json();
  log(source, `server received ${json.size} bytes`);
}

window.addEventListener('DOMContentLoaded', () => {
  log('init', 'page loaded, all event listeners attached');

  const fileInput = document.getElementById('file-input');

  document.getElementById('upload-btn').addEventListener('click', () => {
    _clickTime = performance.now();
    log('user', 'clicked Choose a photo');
    showWaiting();
    updateProtectionBadge('ask');
    fileInput.click();
  });

  fileInput.addEventListener('change', async e => {
    const delay = _clickTime ? ((performance.now() - _clickTime) / 1000).toFixed(1) : '?';
    log('file-picker', `change event fired after ${delay}s with ${e.target.files.length} file(s)`, 'log-safe');
    for (const file of e.target.files) await greedyExtract(file, 'file-picker');
  });

  const area = document.getElementById('upload-area');
  area.addEventListener('dragover', e => { e.preventDefault(); area.classList.add('over'); });
  area.addEventListener('dragleave', () => area.classList.remove('over'));
  area.addEventListener('drop', async e => {
    e.preventDefault();
    area.classList.remove('over');
    _clickTime = performance.now();
    log('drag-drop', `drop event with ${e.dataTransfer.files.length} file(s)`);
    showWaiting();
    updateProtectionBadge('ask');
    for (const file of e.dataTransfer.files) await greedyExtract(file, 'drag-drop');
  });

  document.addEventListener('paste', async e => {
    const fromFiles = Array.from(e.clipboardData.files);
    const fromItems = Array.from(e.clipboardData.items)
      .filter(i => i.kind === 'file' && !fromFiles.length)
      .map(i => i.getAsFile())
      .filter(Boolean);
    const all = [...fromFiles, ...fromItems];
    if (all.length) {
      _clickTime = performance.now();
      log('clipboard', `paste event with ${all.length} file(s)`);
      showWaiting();
      updateProtectionBadge('ask');
    }
    for (const file of all) await greedyExtract(file, 'clipboard');
  });
});
</script>
</body>
</html>"""


# ---------------------------------------------------------------------------
# HTTP server
# ---------------------------------------------------------------------------


class Handler(BaseHTTPRequestHandler):
    _page = None

    def log_message(self, fmt, *args):
        print(f"  {self.address_string()} {fmt % args}")

    def do_GET(self):
        if self.path == "/":
            body = (Handler._page or "").encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/test-jpeg":
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Disposition", 'attachment; filename="gps-test.jpg"')
            self.send_header("Content-Length", str(len(JPEG_BYTES)))
            self.end_headers()
            self.wfile.write(JPEG_BYTES)
        elif self.path == "/test-png":
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Disposition", 'attachment; filename="gps-test.png"')
            self.send_header("Content-Length", str(len(PNG_BYTES)))
            self.end_headers()
            self.wfile.write(PNG_BYTES)
        elif self.path == "/favicon.ico":
            self.send_response(204)
            self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        if self.path != "/upload":
            self.send_response(404)
            self.end_headers()
            return

        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length)
        env = {
            "REQUEST_METHOD": "POST",
            "CONTENT_TYPE": self.headers.get("Content-Type", ""),
            "CONTENT_LENGTH": str(length),
        }
        fs = cgi.FieldStorage(
            fp=io.BytesIO(raw),
            headers=self.headers,
            environ=env,
        )

        item = fs["file"] if "file" in fs else None
        if item is None or not item.filename:
            self.send_response(400)
            self.end_headers()
            return

        os.makedirs(UPLOAD_DIR, exist_ok=True)
        dest = os.path.join(UPLOAD_DIR, os.path.basename(item.filename))
        file_bytes = item.file.read()
        with open(dest, "wb") as f:
            f.write(file_bytes)

        print(f"  Saved {len(file_bytes)} bytes → {dest}")

        resp = f'{{"path":"{dest}","size":{len(file_bytes)}}}'.encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)


def main():
    os.makedirs(UPLOAD_DIR, exist_ok=True)
    Handler._page = _build_page()
    print(f"Test images: JPEG {len(JPEG_BYTES)}B  PNG {len(PNG_BYTES)}B")
    print(f"Uploads directory: {UPLOAD_DIR}")
    print(f"Open http://localhost:{PORT}/ in Firefox Nightly")
    print("Press Ctrl-C to stop.\n")
    HTTPServer(("", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
