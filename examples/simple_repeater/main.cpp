#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "MyMesh.h"

#ifdef WITH_UDP_BRIDGE
#include <WiFi.h>

#ifdef WIFI_DEBUG_LOGGING
#define WIFI_DEBUG_PRINTLN(F, ...) Serial.printf("WiFi: " F "\n", ##__VA_ARGS__)
#else
#define WIFI_DEBUG_PRINTLN(...) {}
#endif

#ifdef WG_ADDRESS
#include <WireGuard-ESP32.h>
#include <time.h>

WireGuard wg;
bool wg_initialized = false;
bool ntp_synced = false;

// Watchdog del tunel: si no llega nada del peer (ni keepalives, que van cada
// UDP_BRIDGE_KEEPALIVE_SECS) primero reinicia WireGuard (soft) y si sigue muerto
// reinicia el ESP32 entero (hard). Tiempos en segundos, configurables por build flag.
#ifndef WG_WATCHDOG_SOFT_SECS
  #define WG_WATCHDOG_SOFT_SECS 180    // 3 min sin vida -> re-handshake de WG
#endif
#ifndef WG_WATCHDOG_HARD_SECS
  #define WG_WATCHDOG_HARD_SECS 600    // 10 min sin vida -> ESP.restart()
#endif
unsigned long wg_last_alive = 0;         // ultima senal de vida conocida (o inicio de WG)
unsigned long wg_last_soft_restart = 0;

// Accessors para que el comando CLI "bridge" (en MyMesh) reporte el estado de WireGuard
// sin acoplar MyMesh a los globales de este archivo.
bool fraguaWgUp() { return wg_initialized; }
uint32_t fraguaWgAgeSecs() { return wg_last_alive ? (millis() - wg_last_alive) / 1000 : 0; }

void setupTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  int retry = 0;
  time_t now;
  time(&now);
  while (now < 1000000000 && retry < 100) {
    delay(100);
    time(&now);
    retry++;
  }
  ntp_synced = (now >= 1000000000);
}
#endif

bool wifi_needs_reconnect = false;
unsigned long last_wifi_reconnect_attempt = 0;
#endif

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

static char command[160];

// For power saving
unsigned long POWERSAVING_FIRSTSLEEP_SECS = 120; // The first sleep (if enabled) from boot

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
static unsigned long userBtnDownAt = 0;
#define USER_BTN_HOLD_OFF_MILLIS 1500
#endif

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#if defined(MESH_DEBUG) && defined(NRF52_PLATFORM)
  // give some extra time for serial to settle so
  // boot debug messages can be seen on terminal
  delay(5000);
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) {
    MESH_DEBUG_PRINTLN("Radio init failed!");
    halt();
  }

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Repeater ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;

  sensors.begin();

#ifdef WITH_UDP_BRIDGE
  board.setInhibitSleep(true);
  WiFi.setAutoReconnect(true);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      WIFI_DEBUG_PRINTLN("WiFi disconnected. Flagging for reconnect...\n");
      wifi_needs_reconnect = true;
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      WIFI_DEBUG_PRINTLN("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
      wifi_needs_reconnect = false;
#ifdef WG_ADDRESS
      setupTime();
#endif
    }
  });

  WiFi.begin(WIFI_SSID, WIFI_PWD);
  delay(500);  // let lwIP stack init before bridge touches it
#endif

  the_mesh.begin(fs);

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif

  board.onBootComplete();
}

void loop() {
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
  // Hold the user button to power off the SenseCAP Solar repeater.
  int btnState = digitalRead(PIN_USER_BTN);
  if (btnState == LOW) {
    if (userBtnDownAt == 0) {
      userBtnDownAt = millis();
    } else if ((unsigned long)(millis() - userBtnDownAt) >= USER_BTN_HOLD_OFF_MILLIS) {
      Serial.println("Powering off...");
      board.powerOff();  // does not return
    }
  } else {
    userBtnDownAt = 0;
  }
#endif

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();

#ifdef WITH_UDP_BRIDGE
  if (wifi_needs_reconnect && (millis() - last_wifi_reconnect_attempt > 10000)) {
    WIFI_DEBUG_PRINTLN("Attempting WiFi reconnect...\n");
    WiFi.disconnect();
    WiFi.reconnect();
    last_wifi_reconnect_attempt = millis();
  }

#ifdef WG_ADDRESS
  if (ntp_synced && !wg_initialized) {
    IPAddress local_ip;
    if (local_ip.fromString(WG_ADDRESS)) {
      wg.begin(local_ip, WG_PRIVATE_KEY, WG_PEER_ENDPOINT, WG_PEER_PUBLIC_KEY, WG_PEER_PORT);
      wg_initialized = true;
      wg_last_alive = millis();  // arranca el periodo de gracia del watchdog
      WIFI_DEBUG_PRINTLN("WireGuard initialized: %s\n", WG_ADDRESS);
    }
  }

  // Watchdog del tunel, alimentado por los datagramas del peer (bridge keepalives).
  if (wg_initialized) {
    unsigned long peer_rx = the_mesh.getBridgeLastPeerRx();
    if (peer_rx > wg_last_alive) wg_last_alive = peer_rx;

    unsigned long silent_ms = millis() - wg_last_alive;
    if (silent_ms > (WG_WATCHDOG_HARD_SECS * 1000UL)) {
      WIFI_DEBUG_PRINTLN("WG watchdog: sin vida del peer hace %lus, reinicio HARD\n", silent_ms / 1000);
      delay(200);      // dejar salir el log por serial
      ESP.restart();   // no retorna
    }
    if (silent_ms > (WG_WATCHDOG_SOFT_SECS * 1000UL) &&
        (millis() - wg_last_soft_restart) > (WG_WATCHDOG_SOFT_SECS * 1000UL)) {
      WIFI_DEBUG_PRINTLN("WG watchdog: sin vida del peer hace %lus, reinicio de WireGuard\n", silent_ms / 1000);
      wg_last_soft_restart = millis();
      wg.end();
      wg_initialized = false;  // el bloque de arriba lo re-inicializa en el proximo loop
    }
  }
#endif
#endif

  if (the_mesh.getNodePrefs()->powersaving_enabled && !the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#else
    if (the_mesh.millisHasNowPassed(POWERSAVING_FIRSTSLEEP_SECS * 1000)) { // To check if it is time to sleep
      board.sleep(30); // Sleep. Wake up after a while or when receiving a LoRa packet
    }
#endif
  }
}
