const WebSocket = require('ws');
const server = new WebSocket.Server({ port: 8080 });

let connCount = 0;

server.on('connection', (ws, req) => {
  connCount++;
  const id = connCount;
  console.log(`[CONNECT] ${new Date().toISOString()} #${id} from ${req.socket.remoteAddress} (total: ${server.clients.size})`);

  ws.on('message', (data) => {
    // 上行音频接收 - 仅统计
  });

  ws.on('close', () => {
    console.log(`[DISCONNECT] ${new Date().toISOString()} #${id} (total: ${server.clients.size})`);
  });

  ws.on('error', (err) => {
    console.error(`[ERROR] #${id} ${err.message}`);
  });
});

console.log('WebSocket test server listening on port 8080');
