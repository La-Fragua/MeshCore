# FraguaMesh: despliegue de bridges con flash.py

Herramienta para configurar y flashear los repetidores puente (bridges UDP/WireGuard)
de FraguaMesh sin editar PlatformIO a mano. Un archivo de config (`fragua-mesh.toml`)
tiene todo (WiFi, claves WireGuard, nombre, coordenadas, region LoRa); `flash.py` genera
el env de PlatformIO y flashea el nodo elegido en el puerto elegido.

## Topologia

Los bridges ya no se enlazan de a dos. Todos apuntan su `UDP_REMOTE_IP` al **reflector**
(hub) que corre en el server (contenedor `mesh-reflector` del compose de fragua-board). El
reflector aprende los peers y reenvia los paquetes entre todas las islas. Ver
`fragua-board/reflector/README.md`.

## Setup

1. Requisitos: PlatformIO Core (`pio` en el PATH) y Python 3.11+.
2. Copiá la plantilla y completá con tus valores reales:

   ```
   cp fragua-mesh.toml.example fragua-mesh.toml
   ```

   `fragua-mesh.toml`, `platformio.fragua.ini` y `.fragua-flash-state.json` estan
   gitignored (tienen secretos o son generados).

## Config

- `[shared]`: hub y comunes. `reflector_ip` = IP WireGuard del server (el hub). `udp_port`
  debe coincidir con `REFLECTOR_UDP_PORT` del reflector.
- `[shared.lora]`: region. **Siempre se emite** (`LORA_FREQ/BW/SF/CR`), asi la region
  queda poblada en cada build. Preset US/America: `freq = 910.525`, banda 902-928.
- `[shared.wireguard]`: peer comun (endpoint/puerto/pubkey del server).
- `[nodes.<id>]`: un bridge fisico. `board` es `heltec_v2` o `heltec_v3`. Cada `wg_address`
  debe ser unica y distinta de `reflector_ip`. `name`/`lat`/`lon` son el advert del nodo.

## Comandos

```
./flash.py list                       # nodos + puertos serie detectados
./flash.py gen [nodo ...]             # escribe platformio.fragua.ini (todos por defecto)
./flash.py build <nodo>               # compila el env (sin hardware)
./flash.py flash <nodo> [opts]        # gen + flash. Sin nodo -> interactivo
       --port <PORT>                  # puerto serie (si no, se detecta o pregunta)
       --fresh                        # borra el flash antes (ver abajo)
       --monitor                      # abre el monitor serie tras el upload
```

Interactivo (`./flash.py flash`): elegis nodo, elegis puerto, te muestra un resumen
(nombre, coords, freq, WG) y confirmas antes de flashear.

## El gotcha de `--fresh`

`ADVERT_NAME`, `lat`/`lon` y todos los `LORA_*` se aplican **solo en el primer boot** y
quedan persistidos en `/com_prefs` del filesystem del nodo (ver `MyMesh.cpp`,
`CommonCLI.cpp`). Un reflash normal **no pisa** esos valores: si cambiaste el nombre, las
coordenadas o la region, tenes que borrar el flash primero.

- `--fresh` corre `pio run -t erase` antes del upload.
- En modo interactivo, si es un nodo nuevo o cambiaron esos campos, se ofrece el erase por
  defecto.
- En modo no interactivo, si detecta que cambiaron esos campos desde el ultimo flash,
  avisa fuerte para que uses `--fresh`.

**Efecto secundario del erase**: el nodo regenera su identidad -> **nuevo pubkey**.
Actualizá `MESHCORE_BRIDGE_PUBKEYS` en el `.env` de fragua-board con el pubkey nuevo, o el
dashboard no reconocera al nodo como bridge.

## Verificar tras flashear

Con `--monitor` (o `pio device monitor -b 115200`) deberias ver: conexion WiFi, sync NTP,
`WireGuard initialized`, `UDP bind OK`, y los keepalive TX (con `bridge_debug = true`). Por
la malla, el comando CLI `bridge` del firmware reporta `wg=1 peer=1` cuando el tunel esta
arriba y el hub responde.

## Nota sobre Heltec V3

El env V3 usa el git de WireGuard-ESP32 (el mismo que V2). El paquete de registry
`ciniml/WireGuard-ESP32` que tenia el env V3 original del repo ya no resuelve. Los dos
nodos actuales son V2.
