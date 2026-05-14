const WebSocket = require('ws');
const fs = require('fs');

const PORT = process.env.WS_PORT || 8080;
const AUDIO_FILE = process.env.AUDIO_FILE || 'test_audio.pcm';

const server = new WebSocket.Server({ port: PORT });
let connCount = 0;

server.on('connection', (ws) => {
  connCount++;
  const id = connCount;
  console.log(`[CONNECT] ${new Date().toISOString()} #${id} (total: ${server.clients.size})`);

  let audioData;
  try {
    audioData = fs.readFileSync(AUDIO_FILE);
  } catch (e) {
    console.error(`[ERROR] #${id} Cannot read audio file: ${AUDIO_FILE}`);
    ws.close();
    return;
  }

  let offset = 0;
  const frameSize = 320; // 20ms @ 8kHz 16bit = 320 bytes

  const interval = setInterval(() => {
    if (ws.readyState === WebSocket.OPEN) {
      const chunk = audioData.slice(offset, offset + frameSize);
      if (chunk.length < frameSize) {
        offset = 0;
      } else {
        ws.send(chunk);
      }
      offset += frameSize;
    }
  }, 20);

  ws.on('message', (data) => {
    // 接收上行音频
  });

  ws.on('close', () => {
    clearInterval(interval);
    console.log(`[DISCONNECT] ${new Date().toISOString()} #${id} (total: ${server.clients.size})`);
  });

  ws.on('error', (err) => {
    clearInterval(interval);
    console.error(`[ERROR] #${id} ${err.message}`);
  });
});

console.log(`WebSocket bidir test server listening on port ${PORT}, audio: ${AUDIO_FILE}`);
