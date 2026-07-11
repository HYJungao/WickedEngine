'use strict';

const WebSocket = require('ws');

const port = Number(process.env.PORT || 39876);
if (!Number.isInteger(port) || port < 1 || port > 65535) {
  console.error(`[NewPipeline WebRTC signaling] invalid PORT: ${process.env.PORT || ''}`);
  process.exit(2);
}
const wss = new WebSocket.Server({ port });

const rooms = new Map();
let nextPeerId = 1;

function nowIso() {
  return new Date().toISOString();
}

function logInfo(message) {
  console.log(`[${nowIso()}] [INFO] ${message}`);
}

function logWarn(message) {
  console.warn(`[${nowIso()}] [WARN] ${message}`);
}

function tryDecodeBase64Utf8(payload) {
  try {
    return Buffer.from(payload, 'base64').toString('utf8');
  } catch (_) {
    return payload;
  }
}

function getOrCreateRoom(roomId) {
  let room = rooms.get(roomId);
  if (!room) {
    room = {
      peers: new Set()
    };
    rooms.set(roomId, room);
  }
  return room;
}

function removePeerFromRoom(peer) {
  if (!peer.roomId) {
    return;
  }
  const room = rooms.get(peer.roomId);
  if (!room) {
    return;
  }
  room.peers.delete(peer);
  if (room.peers.size === 0) {
    rooms.delete(peer.roomId);
  }
  peer.roomId = '';
  peer.role = '';
}

function sendText(peer, text) {
  if (peer.ws.readyState === WebSocket.OPEN) {
    peer.ws.send(text);
  }
}

function broadcastRoom(room, sender, text) {
  for (const peer of room.peers) {
    if (peer !== sender) {
      sendText(peer, text);
    }
  }
}

wss.on('connection', (ws) => {
  const peer = {
    id: nextPeerId++,
    ws,
    roomId: '',
    role: ''
  };

  const remote = ws._socket ?
    `${ws._socket.remoteAddress || 'unknown'}:${ws._socket.remotePort || 'unknown'}` :
    'unknown';
  logInfo(`peer#${peer.id} connected from ${remote}`);

  ws.on('message', (raw) => {
    const message = String(raw);
    const tokens = message.split('|');
    if (tokens.length === 0) {
      logWarn(`peer#${peer.id} sent empty message`);
      return;
    }

    if (tokens[0] === 'join' && tokens.length >= 3) {
      const roomId = tokens[1];
      const role = tokens[2];
      if (!roomId || (role !== 'client' && role !== 'server')) {
        logWarn(`peer#${peer.id} sent invalid join: "${message}"`);
        sendText(peer, 'error|invalid-join');
        return;
      }

      const candidateRoom = getOrCreateRoom(roomId);
      const duplicate = [...candidateRoom.peers].find((other) => other !== peer && other.role === role);
      if (duplicate) {
        logWarn(`peer#${peer.id} rejected duplicate role room="${roomId}" role="${role}"`);
        sendText(peer, `error|duplicate-role|${role}`);
        return;
      }

      const prevRoomId = peer.roomId;
      const prevRole = peer.role;
      removePeerFromRoom(peer);
      if (prevRoomId) {
        logInfo(`peer#${peer.id} left room="${prevRoomId}" role="${prevRole || 'unknown'}"`);
      }

      peer.roomId = roomId;
      peer.role = role;
      const room = getOrCreateRoom(roomId);

      let peerAnnounceCount = 0;
      for (const other of room.peers) {
        if (other.role) {
          sendText(peer, `peer|${other.role}`);
          peerAnnounceCount += 1;
        }
      }

      room.peers.add(peer);
      broadcastRoom(room, peer, `peer|${peer.role}`);
      logInfo(`peer#${peer.id} joined room="${roomId}" role="${role}" peers=${room.peers.size} announced=${peerAnnounceCount}`);
      return;
    }

    if (tokens[0] === 'signal' && tokens.length >= 5) {
      const roomId = tokens[1];
      const fromRole = tokens[2];
      const signalType = tokens[3];
      const payload = tokens.slice(4).join('|');

      if (!peer.roomId || peer.roomId !== roomId || peer.role !== fromRole) {
        logWarn(`peer#${peer.id} rejected signal: room/role mismatch (peer room="${peer.roomId}" role="${peer.role}" msg room="${roomId}" role="${fromRole}")`);
        return;
      }
      const room = rooms.get(roomId);
      if (!room) {
        logWarn(`peer#${peer.id} signal dropped: room="${roomId}" not found`);
        return;
      }
      const recipients = Math.max(0, room.peers.size - 1);
      broadcastRoom(room, peer, `signal|${fromRole}|${signalType}|${payload}`);
      logInfo(`peer#${peer.id} forwarded signal room="${roomId}" role="${fromRole}" type="${signalType}" recipients=${recipients} payloadBytes=${Buffer.byteLength(payload, 'utf8')}`);
      return;
    }

    if (tokens[0] === 'status' && tokens.length >= 5) {
      const roomId = tokens[1];
      const fromRole = tokens[2];
      const kind = tokens[3];
      const payload = tokens.slice(4).join('|');

      if (!peer.roomId || peer.roomId !== roomId || peer.role !== fromRole) {
        logWarn(`peer#${peer.id} rejected status: room/role mismatch (peer room="${peer.roomId}" role="${peer.role}" msg room="${roomId}" role="${fromRole}")`);
        return;
      }
      const detail = tryDecodeBase64Utf8(payload);
      logInfo(`peer#${peer.id} status room="${roomId}" role="${fromRole}" kind="${kind}" detail="${detail}"`);
      return;
    }

    logWarn(`peer#${peer.id} sent unsupported message: "${message}"`);
  });

  ws.on('error', (err) => {
    logWarn(`peer#${peer.id} socket error: ${err && err.message ? err.message : String(err)}`);
  });

  ws.on('close', (code, reasonBuffer) => {
    const reason = reasonBuffer ? reasonBuffer.toString() : '';
    const roomBeforeClose = peer.roomId;
    const roleBeforeClose = peer.role;
    removePeerFromRoom(peer);
    logInfo(`peer#${peer.id} disconnected code=${code} reason="${reason}" room="${roomBeforeClose}" role="${roleBeforeClose}"`);
  });
});

logInfo(`[NewPipeline WebRTC signaling] ws://127.0.0.1:${port}`);

wss.on('error', (err) => {
  logWarn(`server error: ${err && err.message ? err.message : String(err)}`);
  process.exitCode = 1;
});
