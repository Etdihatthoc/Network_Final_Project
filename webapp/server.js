// Simple gateway: serves static HTML/CSS/JS and bridges WebSocket <-> TCP length-prefixed JSON.
const http = require("http");
const fs = require("fs");
const path = require("path");
const net = require("net");
const WebSocket = require("ws");
const crypto = require("crypto");

const STATIC_DIR = path.join(__dirname, "public");
const TCP_HOST = process.env.BACKEND_HOST || "127.0.0.1";
const TCP_PORT = parseInt(process.env.BACKEND_PORT || "5555", 10);
const HTTP_PORT = parseInt(process.env.HTTP_PORT || "8080", 10);

// AES-256-CBC encryption - MUST match C++ backend exactly
// DO NOT use in production - this is for educational purposes only!
const AES_KEY = Buffer.from([
  0x4a,0x2f,0x5b,0x6c,0x9d,0x11,0x23,0x34,
  0x45,0x56,0x67,0x78,0x89,0x9a,0xab,0xbc,
  0xcd,0xde,0xef,0xf1,0x12,0x24,0x35,0x46,
  0x57,0x68,0x79,0x8a,0x9b,0xac,0xbd,0xce
]);

const AES_IV = Buffer.from([
  0x10,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
  0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x01
]);

function serveStatic(req, res) {
  const urlPath = req.url === "/" ? "/index.html" : req.url;
  const safePath = path.normalize(urlPath).replace(/^(\.\.[/\\])+/, "");
  const filePath = path.join(STATIC_DIR, safePath);
  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404);
      res.end("Not found");
      return;
    }
    const ext = path.extname(filePath);
    const mime =
      ext === ".html"
        ? "text/html"
        : ext === ".js"
        ? "application/javascript"
        : ext === ".css"
        ? "text/css"
        : "text/plain";
    res.writeHead(200, { "Content-Type": mime });
    res.end(data);
  });
}

const server = http.createServer(serveStatic);
const wss = new WebSocket.Server({ noServer: true });

server.on("upgrade", (req, socket, head) => {
  if (req.url === "/ws") {
    wss.handleUpgrade(req, socket, head, (ws) => {
      wss.emit("connection", ws, req);
    });
  } else {
    socket.destroy();
  }
});

// Encrypt buffer using AES-256-CBC
function encryptAES(plaintext) {
  const cipher = crypto.createCipheriv("aes-256-cbc", AES_KEY, AES_IV);
  return Buffer.concat([cipher.update(plaintext), cipher.final()]);
}

// Decrypt buffer using AES-256-CBC
function decryptAES(ciphertext) {
  try {
    const decipher = crypto.createDecipheriv("aes-256-cbc", AES_KEY, AES_IV);
    return Buffer.concat([decipher.update(ciphertext), decipher.final()]);
  } catch (err) {
    console.error("AES decryption failed:", err.message);
    return null;
  }
}

function encodeFrame(obj) {
  const json = JSON.stringify(obj);
  const plaintext = Buffer.from(json, "utf8");

  // Encrypt the JSON payload
  const encrypted = encryptAES(plaintext);

  // Build length-prefixed frame with encrypted payload
  const len = Buffer.alloc(4);
  len.writeUInt32BE(encrypted.length, 0);
  return Buffer.concat([len, encrypted]);
}

function decodeFrames(buffer, onMessage) {
  let offset = 0;
  while (offset + 4 <= buffer.length) {
    const encryptedLen = buffer.readUInt32BE(offset);
    if (offset + 4 + encryptedLen > buffer.length) break;

    // Extract encrypted payload
    const encrypted = buffer.slice(offset + 4, offset + 4 + encryptedLen);

    // Decrypt the payload
    const decrypted = decryptAES(encrypted);
    if (decrypted === null) {
      console.error("Failed to decrypt frame, skipping");
      offset += 4 + encryptedLen;
      continue;
    }

    // Parse decrypted JSON
    const payload = decrypted.toString("utf8");
    try {
      onMessage(JSON.parse(payload));
    } catch (e) {
      console.error("Failed to parse JSON after decryption:", e.message);
      // ignore malformed
    }
    offset += 4 + encryptedLen;
  }
  return buffer.slice(offset);
}

wss.on("connection", (ws) => {
  const tcp = net.createConnection({ host: TCP_HOST, port: TCP_PORT });
  let recvBuf = Buffer.alloc(0);

  tcp.on("data", (chunk) => {
    recvBuf = Buffer.concat([recvBuf, chunk]);
    recvBuf = decodeFrames(recvBuf, (msg) => {
      if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(msg));
    });
  });
  tcp.on("error", (err) => {
    if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ error: err.message }));
  });
  tcp.on("close", () => {
    if (ws.readyState === WebSocket.OPEN) ws.close();
  });

  ws.on("message", (data) => {
    try {
      const obj = JSON.parse(data);
      const frame = encodeFrame(obj);
      tcp.write(frame);
    } catch (e) {
      ws.send(JSON.stringify({ error: "Invalid JSON" }));
    }
  });
  ws.on("close", () => tcp.destroy());
});

server.listen(HTTP_PORT, () => {
  console.log(`Web UI at http://localhost:${HTTP_PORT} (WS -> ${TCP_HOST}:${TCP_PORT})`);
});
