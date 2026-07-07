#include "UdpBridge.h"

#ifdef WITH_UDP_BRIDGE

UdpBridge::UdpBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc)
    : BridgeBase(prefs, mgr, rtc), _remote_port(0), _local_port(0) {
}

void UdpBridge::begin() {
  BRIDGE_DEBUG_PRINTLN("Initializing...\n");

  _remote_port = UDP_REMOTE_PORT;
  _local_port = UDP_LOCAL_PORT;

  if (!_remote_ip.fromString(UDP_REMOTE_IP)) {
    BRIDGE_DEBUG_PRINTLN("Invalid remote IP: %s\n", UDP_REMOTE_IP);
    _remote_port = 0; // desactiva el reintento de bind en loop()
    return;
  }

  if (_udp.begin(_local_port)) {
    _initialized = true;
    BRIDGE_DEBUG_PRINTLN("Listening on :%d, remote %s:%d\n",
                         _local_port, _remote_ip.toString().c_str(), _remote_port);
  } else {
    // WiFi todavía no asoció (begin corre dentro de the_mesh.begin()); loop() reintenta.
    BRIDGE_DEBUG_PRINTLN("UDP bind diferido (WiFi no listo), reintentaré en loop()\n");
  }
}

void UdpBridge::end() {
  BRIDGE_DEBUG_PRINTLN("Stopping...\n");
  _udp.stop();
  _initialized = false;
}

void UdpBridge::loop() {
  if (!_initialized) {
    // Reintento de bind: begin() puede haber corrido antes de que el WiFi asociara.
    if (_remote_port != 0 && WiFi.status() == WL_CONNECTED &&
        (millis() - _last_bind_attempt) > 3000) {
      _last_bind_attempt = millis();
      if (_udp.begin(_local_port)) {
        _initialized = true;
        BRIDGE_DEBUG_PRINTLN("UDP bind OK (diferido) en :%d\n", _local_port);
      }
    }
    return;
  }

  // WiFi asociado y socket bindeado: primero drenamos lo que quedo en el buffer.
  if (WiFi.status() == WL_CONNECTED) {
    if (_tx_count > 0) flushBuffer();
#if UDP_BRIDGE_KEEPALIVE_SECS > 0
    // Keepalive: datagrama de 2 bytes (solo el magic). Mantiene vivo el mapeo NAT
    // del tunel y le da al peer una senal de vida periodica.
    if ((millis() - _last_keepalive_tx) > (UDP_BRIDGE_KEEPALIVE_SECS * 1000UL)) {
      _last_keepalive_tx = millis();
      uint8_t ka[BRIDGE_MAGIC_SIZE];
      ka[0] = (BRIDGE_PACKET_MAGIC >> 8) & 0xFF;
      ka[1] = BRIDGE_PACKET_MAGIC & 0xFF;
      _udp.beginPacket(_remote_ip, _remote_port);
      _udp.write(ka, sizeof(ka));
      _udp.endPacket();
    }
#endif
  } else {
    // Enlace caido: no leemos RX (parsePacket no daria nada util); sendPacket va al buffer.
    return;
  }

  int packetSize = _udp.parsePacket();
  if (packetSize <= 0) return;

  if (packetSize < (int)BRIDGE_MAGIC_SIZE) {
    BRIDGE_DEBUG_PRINTLN("RX packet too small, len=%d\n", packetSize);
    return;
  }

  if (packetSize > (int)MAX_UDP_PACKET_SIZE) {
    BRIDGE_DEBUG_PRINTLN("RX packet too large, len=%d\n", packetSize);
    return;
  }

  int len = _udp.read(_rx_buffer, packetSize);
  if (len != packetSize) {
    BRIDGE_DEBUG_PRINTLN("RX read mismatch, expected=%d got=%d\n", packetSize, len);
    return;
  }

  uint16_t received_magic = (_rx_buffer[0] << 8) | _rx_buffer[1];
  if (received_magic != BRIDGE_PACKET_MAGIC) {
    BRIDGE_DEBUG_PRINTLN("RX invalid magic 0x%04X\n", received_magic);
    return;
  }

  // Magic valido: el peer esta vivo (keepalive o paquete real).
  {
    unsigned long now = millis();
    // Nueva conexion o reconexion tras una caida (mas de ~3 keepalives sin señal).
    if (_last_peer_rx == 0 || (now - _last_peer_rx) > 70000UL) {
      _peer_up_since = now;
    }
    _last_peer_rx = now;
  }

  if (packetSize == (int)BRIDGE_MAGIC_SIZE) {
    return; // keepalive del peer, nada mas que hacer
  }

  if (packetSize < (int)(BRIDGE_MAGIC_SIZE + BRIDGE_CHECKSUM_SIZE)) {
    BRIDGE_DEBUG_PRINTLN("RX packet too small, len=%d\n", packetSize);
    return;
  }

  const size_t encryptedDataLen = packetSize - BRIDGE_MAGIC_SIZE;
  uint8_t decrypted[MAX_UDP_PACKET_SIZE];
  memcpy(decrypted, _rx_buffer + BRIDGE_MAGIC_SIZE, encryptedDataLen);
  xorCrypt(decrypted, encryptedDataLen);

  uint16_t received_checksum = (decrypted[0] << 8) | decrypted[1];
  const size_t payloadLen = encryptedDataLen - BRIDGE_CHECKSUM_SIZE;

  if (!validateChecksum(decrypted + BRIDGE_CHECKSUM_SIZE, payloadLen, received_checksum)) {
    BRIDGE_DEBUG_PRINTLN("RX checksum mismatch\n");
    return;
  }

  BRIDGE_DEBUG_PRINTLN("RX, payload_len=%d\n", payloadLen);

  mesh::Packet *pkt = _mgr->allocNew();
  if (!pkt) return;

  if (pkt->readFrom(decrypted + BRIDGE_CHECKSUM_SIZE, (uint8_t)payloadLen)) {
    if (_bridged_rx < UINT32_MAX) _bridged_rx++;
    onPacketReceived(pkt);
  } else {
    _mgr->free(pkt);
  }
}

void UdpBridge::sendPacket(mesh::Packet *packet) {
  if (!packet) return;

  // hasSeen() ademas de consultar, marca el paquete como visto (dedupe / anti-loop).
  // Se llama una sola vez aca, aunque el envio real quede diferido en el buffer.
  if (_seen_packets.hasSeen(packet)) return;

  uint8_t meshPacket[255];
  uint8_t meshPacketLen = packet->writeTo(meshPacket); // uint8_t: <=255, entra siempre
  if (meshPacketLen == 0) return;

  if (linkReady()) {
    txFrame(meshPacket, meshPacketLen);
  } else {
    // Enlace caido (WiFi/WG): guardo el paquete serializado para reenviarlo despues.
    bufferPush(meshPacket, meshPacketLen);
  }
}

// Enmarca (magic + checksum + XOR) un paquete mesh ya serializado y lo manda por UDP.
void UdpBridge::txFrame(const uint8_t *meshPacket, uint8_t len) {
  uint8_t buffer[MAX_UDP_PACKET_SIZE];

  buffer[0] = (BRIDGE_PACKET_MAGIC >> 8) & 0xFF;
  buffer[1] = BRIDGE_PACKET_MAGIC & 0xFF;

  const size_t packetOffset = BRIDGE_MAGIC_SIZE + BRIDGE_CHECKSUM_SIZE;
  memcpy(buffer + packetOffset, meshPacket, len);

  uint16_t checksum = fletcher16(buffer + packetOffset, len);
  buffer[2] = (checksum >> 8) & 0xFF;
  buffer[3] = checksum & 0xFF;

  xorCrypt(buffer + BRIDGE_MAGIC_SIZE, len + BRIDGE_CHECKSUM_SIZE);

  const size_t totalPacketSize = packetOffset + len;

  _udp.beginPacket(_remote_ip, _remote_port);
  _udp.write(buffer, totalPacketSize);
  if (_udp.endPacket()) {
    if (_bridged_tx < UINT32_MAX) _bridged_tx++;
    BRIDGE_DEBUG_PRINTLN("TX, len=%d\n", len);
  } else {
    BRIDGE_DEBUG_PRINTLN("TX FAILED!\n");
  }
}

bool UdpBridge::linkReady() const {
  return _initialized && WiFi.status() == WL_CONNECTED;
}

// Guarda un paquete en el ring. Si esta lleno, pisa el mas viejo (drop-oldest).
void UdpBridge::bufferPush(const uint8_t *meshPacket, uint8_t len) {
  TxEntry &e = _txbuf[_tx_head];
  e.len = len;
  memcpy(e.data, meshPacket, len);
  _tx_head = (_tx_head + 1) % UDP_BRIDGE_BUFFER_SIZE;

  if (_tx_count < UDP_BRIDGE_BUFFER_SIZE) {
    _tx_count++;
  } else {
    // estaba lleno: _tx_head apuntaba al mas viejo, que acabamos de sobrescribir.
    _tx_dropped++;
  }
  BRIDGE_DEBUG_PRINTLN("Buffered (enlace caido), pend=%d drop=%lu\n", _tx_count, (unsigned long)_tx_dropped);
}

// Reenvia los paquetes acumulados. Acotado por vuelta para no bloquear el loop.
void UdpBridge::flushBuffer() {
  int budget = 8;
  while (_tx_count > 0 && budget-- > 0) {
    uint8_t tail = (_tx_head - _tx_count + UDP_BRIDGE_BUFFER_SIZE) % UDP_BRIDGE_BUFFER_SIZE;
    txFrame(_txbuf[tail].data, _txbuf[tail].len);
    _tx_count--;
  }
}

void UdpBridge::onPacketReceived(mesh::Packet *packet) {
  handleReceivedPacket(packet);
}

void UdpBridge::xorCrypt(uint8_t *data, size_t len) {
  size_t keyLen = strlen(_prefs->bridge_secret);
  if (keyLen == 0) return; // sin secreto (p.ej. confiando en WireGuard): sin XOR, evita %0
  for (size_t i = 0; i < len; i++) {
    data[i] ^= _prefs->bridge_secret[i % keyLen];
  }
}

#endif
