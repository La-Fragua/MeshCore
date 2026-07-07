#pragma once

#include "MeshCore.h"
#include "helpers/bridges/BridgeBase.h"
#include <WiFi.h>
#include <WiFiUdp.h>

#ifdef WITH_UDP_BRIDGE

/**
 * @brief Bridge implementation using UDP for packet transport over IP networks.
 *
 * Unlike ESPNowBridge (local WiFi peer-to-peer) or RS232Bridge (wired serial),
 * this bridge sends mesh packets over UDP datagrams, enabling bridges across
 * the internet when combined with a VPN tunnel (WireGuard) or on a local LAN.
 *
 * Packet Structure (matches ESPNowBridge format for compatibility):
 * [2 bytes] Magic Header (BRIDGE_PACKET_MAGIC = 0xC03E)
 * [2 bytes] Fletcher-16 checksum of XOR-encrypted payload
 * [N bytes] XOR-encrypted payload containing the mesh packet
 *
 * Each UDP datagram carries exactly one bridge packet. No length framing is
 * needed since UDP preserves datagram boundaries.
 *
 * Configuration (build flags):
 * - WITH_UDP_BRIDGE=1          enables this bridge
 * - UDP_REMOTE_IP              remote IP address (e.g. "10.77.0.2")
 * - UDP_REMOTE_PORT            remote UDP port
 * - UDP_LOCAL_PORT             local UDP port to listen on
 * - WIFI_SSID / WIFI_PWD       WiFi credentials (set up in main.cpp)
 *
 * WireGuard integration:
 * This bridge is transport-agnostic. If WireGuard is set up in main.cpp
 * before begin() is called, UDP traffic automatically routes through the
 * tunnel. The bridge itself only speaks plain UDP to the specified IP:port.
 */
// Cuántos paquetes mesh guardar mientras el enlace (WiFi/WG) está caído.
// Cada entrada ~256 B → default 16 ≈ 4 KB estáticos. Subilo con -D UDP_BRIDGE_BUFFER_SIZE=N
// (cuesta N*256 B). El ESP32-S3 del Heltec V3 tolera unos KB de sobra sin problema.
#ifndef UDP_BRIDGE_BUFFER_SIZE
  #define UDP_BRIDGE_BUFFER_SIZE 16
#endif

// Cada cuantos segundos mandar un keepalive (datagrama de 2 bytes, solo el magic).
// Mantiene vivo el mapeo NAT del tunel (la lib WireGuard-ESP32 no manda keepalives
// propios) y le da al peer una senal de vida para su watchdog. 0 = deshabilitado.
#ifndef UDP_BRIDGE_KEEPALIVE_SECS
  #define UDP_BRIDGE_KEEPALIVE_SECS 25
#endif

class UdpBridge : public BridgeBase {
private:
  WiFiUDP _udp;
  IPAddress _remote_ip;
  uint16_t _remote_port;
  uint16_t _local_port;
  unsigned long _last_bind_attempt = 0;
  unsigned long _last_keepalive_tx = 0;
  unsigned long _last_peer_rx = 0;   // millis() del ultimo datagrama con magic valido del peer
  unsigned long _peer_up_since = 0;  // millis() en que arranco la racha de conexion actual con el peer

  static const size_t MAX_UDP_PACKET_SIZE = 512;
  static const size_t MAX_PAYLOAD_SIZE = MAX_UDP_PACKET_SIZE - (BRIDGE_MAGIC_SIZE + BRIDGE_CHECKSUM_SIZE);

  uint8_t _rx_buffer[MAX_UDP_PACKET_SIZE];

  // Buffer de retransmisión: cada entrada guarda el paquete mesh serializado
  // (salida de writeTo(), <=255 B por ser uint8_t), no el datagrama de 512.
  // Ring FIFO; si se llena, pisa el más viejo (drop-oldest).
  struct TxEntry {
    uint8_t len;
    uint8_t data[255];
  };
  TxEntry _txbuf[UDP_BRIDGE_BUFFER_SIZE];
  uint8_t _tx_head = 0;    // próxima posición de escritura
  uint8_t _tx_count = 0;   // paquetes pendientes en el buffer
  uint32_t _tx_dropped = 0; // total descartados por overflow (debug)

  void xorCrypt(uint8_t *data, size_t len);
  bool linkReady() const;                              // WiFi asociado + socket bindeado
  void txFrame(const uint8_t *meshPacket, uint8_t len); // enmarca + envía un datagrama
  void bufferPush(const uint8_t *meshPacket, uint8_t len);
  void flushBuffer();                                  // drena pendientes cuando vuelve el enlace

public:
  UdpBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc);

  void begin() override;
  void end() override;
  void loop() override;
  void onPacketReceived(mesh::Packet *packet) override;
  void sendPacket(mesh::Packet *packet) override;

  uint8_t pendingCount() const { return _tx_count; } // paquetes esperando a que vuelva el enlace

  // millis() del ultimo datagrama (keepalive o paquete) con magic valido recibido del peer.
  // 0 = nunca. Senal de vida para el watchdog de WireGuard en main.cpp.
  unsigned long lastPeerRx() const { return _last_peer_rx; }

  // millis() en que arranco la racha de conexion actual con el peer (para reportar
  // "conectado hace X"). 0 = nunca hubo conexion.
  unsigned long peerUpSince() const { return _peer_up_since; }

  // Packets bridged (mesh→UDP TX, UDP→mesh RX). uint32_t won't overflow at
  // typical rates (~1 pkt/s = 136 years). Caps at UINT32_MAX on wrap.
  uint32_t bridgedTx() const { return _bridged_tx; }
  uint32_t bridgedRx() const { return _bridged_rx; }
private:
  uint32_t _bridged_tx = 0;
  uint32_t _bridged_rx = 0;
};

#endif
