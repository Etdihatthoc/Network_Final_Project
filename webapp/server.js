// Simple gateway: serves static HTML/CSS/JS and bridges WebSocket <-> TCP length-prefixed JSON.
const http = require("http");
const fs = require("fs");
const path = require("path");
const net = require("net");
const WebSocket = require("ws");

const STATIC_DIR = path.join(__dirname, "public");
const TCP_HOST = process.env.BACKEND_HOST || "127.0.0.1";
const TCP_PORT = parseInt(process.env.BACKEND_PORT || "5555", 10);
const HTTP_PORT = parseInt(process.env.HTTP_PORT || "8080", 10);

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

function encodeFrame(obj) {
  const json = JSON.stringify(obj);
  const payload = Buffer.from(json, "utf8");
  const len = Buffer.alloc(4);
  len.writeUInt32BE(payload.length, 0);
  return Buffer.concat([len, payload]);
}

function decodeFrames(buffer, onMessage) {
  let offset = 0;
  while (offset + 4 <= buffer.length) {
    const len = buffer.readUInt32BE(offset);
    if (offset + 4 + len > buffer.length) break;
    const payload = buffer.slice(offset + 4, offset + 4 + len).toString("utf8");
    try {
      onMessage(JSON.parse(payload));
    } catch (e) {
      // ignore malformed
    }
    offset += 4 + len;
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
