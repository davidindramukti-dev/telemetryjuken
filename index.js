/*
  ============================================================
  JUKEN TELEMETRY SERVER
  - Express HTTP server
  - SQLite database (file lokal, Railway persist via volume)
  - REST API untuk ESP32 POST data
  - WebSocket untuk push real-time ke browser
  - Serve frontend dashboard statis
  ============================================================
  Deploy ke Railway:
    1. Push folder ini ke GitHub repo
    2. New Project di Railway → Deploy from GitHub
    3. Tambah variable: PORT (otomatis dari Railway)
    4. Tambah volume di /app/data agar DB tidak hilang saat redeploy
  ============================================================
*/

const express    = require('express');
const http       = require('http');
const WebSocket  = require('ws');
const Database   = require('better-sqlite3');
const path       = require('path');
const fs         = require('fs');

const app    = express();
const server = http.createServer(app);
const wss    = new WebSocket.Server({ server });

const PORT   = process.env.PORT || 3000;
const DB_DIR = process.env.DB_DIR || path.join(__dirname, '..', 'data');
const DB_PATH = path.join(DB_DIR, 'juken.db');

// Pastikan folder data ada
if (!fs.existsSync(DB_DIR)) fs.mkdirSync(DB_DIR, { recursive: true });

// ================== DATABASE ==================
const db = new Database(DB_PATH);

db.exec(`
  CREATE TABLE IF NOT EXISTS sessions (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    started_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    ended_at  DATETIME,
    note      TEXT
  );

  CREATE TABLE IF NOT EXISTS telemetry (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL,
    ts         DATETIME DEFAULT CURRENT_TIMESTAMP,
    rpm        INTEGER,
    tps        REAL,
    voltage    REAL,
    ect        REAL,
    o2         REAL,
    map_kpa    REAL,
    fuel_pump  INTEGER,
    FOREIGN KEY (session_id) REFERENCES sessions(id)
  );

  CREATE INDEX IF NOT EXISTS idx_tel_session ON telemetry(session_id);
  CREATE INDEX IF NOT EXISTS idx_tel_ts      ON telemetry(ts);
`);

// Prepared statements
const stmtNewSession  = db.prepare(`INSERT INTO sessions DEFAULT VALUES`);
const stmtEndSession  = db.prepare(`UPDATE sessions SET ended_at = CURRENT_TIMESTAMP WHERE id = ?`);
const stmtInsertTel   = db.prepare(`
  INSERT INTO telemetry (session_id, rpm, tps, voltage, ect, o2, map_kpa, fuel_pump)
  VALUES (@session_id, @rpm, @tps, @voltage, @ect, @o2, @map_kpa, @fuel_pump)
`);
const stmtGetSessions = db.prepare(`
  SELECT id, started_at, ended_at,
    (SELECT COUNT(*) FROM telemetry WHERE session_id = sessions.id) AS points
  FROM sessions ORDER BY started_at DESC LIMIT 50
`);
const stmtGetTelemetry = db.prepare(`
  SELECT ts, rpm, tps, voltage, ect, o2, map_kpa, fuel_pump
  FROM telemetry WHERE session_id = ? ORDER BY ts ASC
`);

// ================== STATE ==================
let currentSessionId = null;
let lastDataTs       = 0;
const SESSION_TIMEOUT_MS = 10000; // 10 detik tanpa data → session dianggap selesai

function ensureSession() {
  if (!currentSessionId) {
    const info = stmtNewSession.run();
    currentSessionId = info.lastInsertRowid;
    console.log(`[session] New session #${currentSessionId}`);
    broadcastWs({ type: 'session_start', sessionId: currentSessionId });
  }
  lastDataTs = Date.now();
}

function checkSessionTimeout() {
  if (currentSessionId && Date.now() - lastDataTs > SESSION_TIMEOUT_MS) {
    stmtEndSession.run(currentSessionId);
    console.log(`[session] Session #${currentSessionId} ended (timeout)`);
    broadcastWs({ type: 'session_end', sessionId: currentSessionId });
    currentSessionId = null;
  }
}
setInterval(checkSessionTimeout, 2000);

// ================== WEBSOCKET ==================
const wsClients = new Set();

wss.on('connection', (ws) => {
  wsClients.add(ws);
  ws.on('close', () => wsClients.delete(ws));
  // Kirim session aktif saat client baru connect
  if (currentSessionId) {
    ws.send(JSON.stringify({ type: 'session_start', sessionId: currentSessionId }));
  }
});

function broadcastWs(obj) {
  const msg = JSON.stringify(obj);
  for (const ws of wsClients) {
    if (ws.readyState === WebSocket.OPEN) ws.send(msg);
  }
}

// ================== MIDDLEWARE ==================
app.use(express.json());
app.use(express.static(path.join(__dirname, '..', 'public')));

// ================== API ==================

// POST /api/data — dari ESP32
app.post('/api/data', (req, res) => {
  const d = req.body;
  if (!d || d.rpm === undefined) return res.status(400).json({ error: 'invalid' });

  ensureSession();

  const row = {
    session_id: currentSessionId,
    rpm:        d.rpm       || 0,
    tps:        d.tps       || 0,
    voltage:    d.voltage   || 0,
    ect:        d.ect       || 0,
    o2:         d.o2        || 0,
    map_kpa:    d.map       || 0,
    fuel_pump:  d.fp        || 0,
  };

  stmtInsertTel.run(row);

  // Push ke semua browser
  broadcastWs({ type: 'data', ...d, sessionId: currentSessionId });

  res.json({ ok: true, sessionId: currentSessionId });
});

// GET /api/sessions — list semua sesi
app.get('/api/sessions', (req, res) => {
  res.json(stmtGetSessions.all());
});

// GET /api/sessions/:id/data — data satu sesi (untuk replay)
app.get('/api/sessions/:id/data', (req, res) => {
  const rows = stmtGetTelemetry.all(req.params.id);
  res.json(rows);
});

// GET /api/sessions/:id/csv — download CSV
app.get('/api/sessions/:id/csv', (req, res) => {
  const rows = stmtGetTelemetry.all(req.params.id);
  if (!rows.length) return res.status(404).send('No data');

  const header = 'timestamp,rpm,tps,voltage,ect,o2,map_kpa,fuel_pump\n';
  const csv = header + rows.map(r =>
    `${r.ts},${r.rpm},${r.tps},${r.voltage},${r.ect},${r.o2},${r.map_kpa},${r.fuel_pump}`
  ).join('\n');

  res.setHeader('Content-Type', 'text/csv');
  res.setHeader('Content-Disposition', `attachment; filename="session_${req.params.id}.csv"`);
  res.send(csv);
});

// ================== START ==================
server.listen(PORT, () => {
  console.log(`Juken Telemetry Server running on port ${PORT}`);
  console.log(`DB: ${DB_PATH}`);
});
