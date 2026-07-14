#!/usr/bin/env python3
"""Generador de envs y flasher de los bridges FraguaMesh.

Lee fragua-mesh.toml, genera platformio.fragua.ini (un env por nodo) y flashea el nodo
elegido en el puerto elegido. Solo stdlib (tomllib requiere Python 3.11+, que PlatformIO
ya necesita).

Subcomandos:
  list                    nodos del toml + puertos serie detectados
  gen [nodo ...]          escribe platformio.fragua.ini (todos por defecto)
  build <nodo>            gen + pio run -e fragua_<nodo> (compila, sin hardware)
  flash <nodo> [opts]     gen + upload. Sin nodo -> interactivo.
     --port PORT          puerto serie (si no, se detecta/pregunta)
     --fresh              borra el flash antes (pio run -t erase). Necesario cuando
                          cambia nombre/coords/region: esos valores se persisten en
                          /com_prefs en el primer boot y un reflash normal NO los pisa.
     --monitor            abre el monitor serie tras el upload
"""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CONFIG_PATH = ROOT / "fragua-mesh.toml"
OUTPUT_PATH = ROOT / "platformio.fragua.ini"
STATE_PATH = ROOT / ".fragua-flash-state.json"

BOARDS = {
    "heltec_v2": {
        "base": "Heltec_lora32_v2",
        "wg_lib": "https://github.com/ciniml/WireGuard-ESP32-Arduino.git",
    },
    "heltec_v3": {
        "base": "Heltec_lora32_v3",
        # El registry PlatformIO ya no resuelve 'ciniml/WireGuard-ESP32 @ ^1.0.0'
        # (la ref del env V3 original del repo). El git URL, que si resuelve y es el
        # mismo que usa V2, soporta ESP32-S3.
        "wg_lib": "https://github.com/ciniml/WireGuard-ESP32-Arduino.git",
    },
}

# Flags que el firmware persiste en /com_prefs en el primer boot (MyMesh.cpp). Si cambian,
# un flash normal no los aplica: hace falta --fresh (erase). Se hashean para avisar.
PERSISTED_KEYS = ("name", "lat", "lon", "freq", "bw", "sf", "cr")


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def env_name(node_key: str) -> str:
    return "fragua_" + re.sub(r"[^A-Za-z0-9_]", "_", node_key)


def cstr(value: str) -> str:
    """Devuelve el value como literal de string C para un -D de build_flags.

    Forma normal: -D X='"value"'. Si el value tiene comilla simple, esa forma rompe el
    parseo de PlatformIO, asi que usamos la forma escapada con comillas dobles.
    """
    if "'" in value:
        return '"\\"' + value + '\\""'
    return "'\"" + value + "\"'"


def load_config() -> dict:
    if not CONFIG_PATH.exists():
        die(f"falta {CONFIG_PATH.name}. Copiá fragua-mesh.toml.example y completalo.")
    with CONFIG_PATH.open("rb") as f:
        cfg = tomllib.load(f)
    validate(cfg)
    return cfg


def validate(cfg: dict) -> None:
    shared = cfg.get("shared")
    if not shared:
        die("falta la seccion [shared]")
    for k in ("admin_password", "udp_port", "reflector_ip"):
        if k not in shared:
            die(f"[shared] sin '{k}'")
    lora = shared.get("lora")
    if not lora or not all(k in lora for k in ("freq", "bw", "sf", "cr")):
        die("[shared.lora] debe tener freq, bw, sf, cr")
    if not (902.0 <= float(lora["freq"]) <= 928.0):
        print(
            f"aviso: LORA_FREQ {lora['freq']} MHz fuera de la banda US 902-928. "
            "Seguí solo si sabés lo que hacés.",
            file=sys.stderr,
        )
    wg = shared.get("wireguard")
    if not wg or not all(k in wg for k in ("peer_endpoint", "peer_port", "peer_public_key")):
        die("[shared.wireguard] debe tener peer_endpoint, peer_port, peer_public_key")

    nodes = cfg.get("nodes")
    if not nodes:
        die("no hay nodos definidos ([nodes.<id>])")
    reflector_ip = shared["reflector_ip"]
    seen_wg = {}
    for key, n in nodes.items():
        req = ("board", "name", "lat", "lon", "wifi_ssid", "wifi_password", "wg_address", "wg_private_key")
        for r in req:
            if r not in n:
                die(f"[nodes.{key}] sin '{r}'")
        if n["board"] not in BOARDS:
            die(f"[nodes.{key}] board '{n['board']}' desconocido. Opciones: {', '.join(BOARDS)}")
        if not (-90 <= float(n["lat"]) <= 90) or not (-180 <= float(n["lon"]) <= 180):
            die(f"[nodes.{key}] lat/lon fuera de rango")
        if n["wg_address"] == reflector_ip:
            die(f"[nodes.{key}] wg_address no puede ser igual a reflector_ip ({reflector_ip})")
        if n["wg_address"] in seen_wg:
            die(f"[nodes.{key}] wg_address {n['wg_address']} repetida (tambien en {seen_wg[n['wg_address']]})")
        seen_wg[n["wg_address"]] = key
        pk = str(n["wg_private_key"])
        if pk != "CHANGEME" and not re.fullmatch(r"[A-Za-z0-9+/]{43}=", pk):
            print(f"aviso: [nodes.{key}] wg_private_key no parece base64 de 44 chars", file=sys.stderr)


def build_flags(cfg: dict, key: str) -> list[str]:
    shared, lora, wg = cfg["shared"], cfg["shared"]["lora"], cfg["shared"]["wireguard"]
    n = cfg["nodes"][key]
    base = BOARDS[n["board"]]["base"]
    flags = [
        f"${{{base}.build_flags}}",
        f"-D ADVERT_NAME={cstr(str(n['name']))}",
        f"-D ADVERT_LAT={n['lat']}",
        f"-D ADVERT_LON={n['lon']}",
        f"-D LORA_FREQ={lora['freq']}",
        f"-D LORA_BW={lora['bw']}",
        f"-D LORA_SF={lora['sf']}",
        f"-D LORA_CR={lora['cr']}",
        f"-D ADMIN_PASSWORD={cstr(str(shared['admin_password']))}",
        f"-D MAX_NEIGHBOURS={shared.get('max_neighbours', 50)}",
        f"-D ESP32_CPU_FREQ={shared.get('cpu_freq', 240)}",
        "-D WITH_UDP_BRIDGE=1",
        f"-D UDP_REMOTE_IP={cstr(str(shared['reflector_ip']))}",
        f"-D UDP_REMOTE_PORT={shared['udp_port']}",
        f"-D UDP_LOCAL_PORT={shared['udp_port']}",
        f"-D WIFI_SSID={cstr(str(n['wifi_ssid']))}",
        f"-D WIFI_PWD={cstr(str(n['wifi_password']))}",
        "-D WIFI_DEBUG_LOGGING=1",
    ]
    if shared.get("bridge_debug"):
        flags.append("-D BRIDGE_DEBUG=1")
    flags += [
        f"-D WG_ADDRESS={cstr(str(n['wg_address']))}",
        f"-D WG_PRIVATE_KEY={cstr(str(n['wg_private_key']))}",
        f"-D WG_PEER_PUBLIC_KEY={cstr(str(wg['peer_public_key']))}",
        f"-D WG_PEER_ENDPOINT={cstr(str(wg['peer_endpoint']))}",
        f"-D WG_PEER_PORT={wg['peer_port']}",
        f"-D WG_KEEPALIVE={wg.get('keepalive', 25)}",
        f"-D WG_WATCHDOG_SOFT_SECS={wg.get('watchdog_soft_secs', 180)}",
    ]
    return flags


def render_env(cfg: dict, key: str) -> str:
    n = cfg["nodes"][key]
    board = BOARDS[n["board"]]
    base = board["base"]
    flags = build_flags(cfg, key)
    lines = [f"[env:{env_name(key)}]", f"extends = {base}", "build_flags ="]
    lines += [f"  {fl}" for fl in flags]
    lines.append(f"build_src_filter = ${{{base}.build_src_filter}}")
    lines.append("  +<helpers/bridges/UdpBridge.cpp>")
    lines.append("  +<../examples/simple_repeater>")
    lines.append("lib_deps =")
    lines.append(f"  ${{{base}.lib_deps}}")
    lines.append("  ${esp32_ota.lib_deps}")
    lines.append(f"  {board['wg_lib']}")
    return "\n".join(lines) + "\n"


def persisted_hash(cfg: dict, key: str) -> str:
    n = cfg["nodes"][key]
    lora = cfg["shared"]["lora"]
    src = {k: (n.get(k) if k in n else lora.get(k)) for k in PERSISTED_KEYS}
    return hashlib.sha256(json.dumps(src, sort_keys=True).encode()).hexdigest()[:16]


def read_state() -> dict:
    if STATE_PATH.exists():
        try:
            return json.loads(STATE_PATH.read_text())
        except (json.JSONDecodeError, OSError):
            return {}
    return {}


def cmd_gen(cfg: dict, node_keys: list[str]) -> None:
    keys = node_keys or list(cfg["nodes"])
    for k in keys:
        if k not in cfg["nodes"]:
            die(f"nodo '{k}' no esta en {CONFIG_PATH.name}")
    header = (
        "; Generado por flash.py desde fragua-mesh.toml. NO editar a mano.\n"
        "; Regenerá con: ./flash.py gen\n\n"
    )
    body = "\n".join(render_env(cfg, k) for k in keys)
    OUTPUT_PATH.write_text(header + body)
    print(f"escrito {OUTPUT_PATH.name} con envs: {', '.join(env_name(k) for k in keys)}")


def pio() -> str:
    exe = shutil.which("pio") or shutil.which("platformio")
    if not exe:
        die("no encontré 'pio' en el PATH. Instalá PlatformIO Core.")
    return exe


def list_ports() -> list[dict]:
    try:
        out = subprocess.run(
            [pio(), "device", "list", "--json-output"],
            capture_output=True, text=True, check=True,
        ).stdout
        ports = json.loads(out)
    except (subprocess.CalledProcessError, json.JSONDecodeError):
        return []
    likely = []
    for p in ports:
        blob = f"{p.get('port', '')} {p.get('hwid', '')} {p.get('description', '')}".lower()
        if any(t in blob for t in ("usb", "ttyusb", "ttyacm", "usbserial", "wchusb", "slab")):
            likely.append(p)
    return likely or ports


def cmd_list(cfg: dict) -> None:
    print("Nodos:")
    for k, n in cfg["nodes"].items():
        print(f"  {k:20s} board={n['board']:10s} name={n['name']!r} wg={n['wg_address']}")
    print("\nPuertos serie detectados:")
    ports = list_ports()
    if not ports:
        print("  (ninguno)")
    for p in ports:
        print(f"  {p.get('port'):24s} {p.get('description', '')}")


def prompt_choice(title: str, options: list[str]) -> str:
    print(title)
    for i, o in enumerate(options, 1):
        print(f"  {i}) {o}")
    while True:
        raw = input("> ").strip()
        if raw.isdigit() and 1 <= int(raw) <= len(options):
            return options[int(raw) - 1]
        if raw in options:
            return raw
        print("opcion invalida")


def resolve_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    ports = [p.get("port") for p in list_ports() if p.get("port")]
    if not ports:
        die("no detecté puertos serie. Pasá --port explícito.")
    if len(ports) == 1:
        print(f"usando puerto {ports[0]}")
        return ports[0]
    return prompt_choice("Elegí puerto:", ports)


def run_pio(args: list[str]) -> None:
    cmd = [pio()] + args
    print("+ " + " ".join(cmd))
    subprocess.run(cmd, check=True)


def cmd_build(cfg: dict, key: str) -> None:
    cmd_gen(cfg, [key])
    run_pio(["run", "-e", env_name(key)])


def cmd_flash(cfg: dict, args: argparse.Namespace) -> None:
    key = args.node
    interactive = key is None
    if interactive:
        key = prompt_choice("Elegí nodo:", list(cfg["nodes"]))
    if key not in cfg["nodes"]:
        die(f"nodo '{key}' no esta en {CONFIG_PATH.name}")

    cmd_gen(cfg, [key])
    env = env_name(key)
    n = cfg["nodes"][key]
    lora = cfg["shared"]["lora"]
    port = resolve_port(args.port)

    # Deteccion de cambios en flags persistidos (nombre/coords/region).
    state = read_state()
    cur_hash = persisted_hash(cfg, key)
    changed = state.get(env) is not None and state.get(env) != cur_hash

    fresh = args.fresh
    if interactive and not fresh:
        default_yes = changed or state.get(env) is None
        hint = "Y/n" if default_yes else "y/N"
        ans = input(f"¿Nodo nuevo o reconfigurado (erase completo)? [{hint}] ").strip().lower()
        fresh = (ans in ("", "y") if default_yes else ans == "y")
    elif changed and not fresh:
        print(
            "\n*** AVISO: cambiaron nombre/coords/region respecto al ultimo flash de este "
            "nodo. El firmware los persiste en el primer boot y un reflash normal NO los "
            "aplica. Reflasheá con --fresh para que tomen efecto. ***\n",
            file=sys.stderr,
        )

    print(
        f"\nNodo:   {key}  (env {env}, board {n['board']})\n"
        f"Name:   {n['name']}\n"
        f"Coords: {n['lat']}, {n['lon']}\n"
        f"LoRa:   {lora['freq']} MHz  BW{lora['bw']} SF{lora['sf']} CR{lora['cr']}\n"
        f"WG:     {n['wg_address']} -> reflector {cfg['shared']['reflector_ip']}\n"
        f"Puerto: {port}   Fresh(erase): {fresh}\n"
    )
    if interactive:
        if input("Confirmar flash? [y/N] ").strip().lower() != "y":
            die("cancelado")

    if fresh:
        run_pio(["run", "-e", env, "-t", "erase", "--upload-port", port])
        print(
            "\n*** El erase regenera la identidad del nodo: NUEVO pubkey. Actualizá "
            "MESHCORE_BRIDGE_PUBKEYS en el .env de fragua-board con el pubkey nuevo. ***\n"
        )
    run_pio(["run", "-e", env, "-t", "upload", "--upload-port", port])

    state[env] = cur_hash
    STATE_PATH.write_text(json.dumps(state, indent=2))

    if args.monitor:
        run_pio(["device", "monitor", "-p", port, "-b", "115200"])


def main() -> None:
    ap = argparse.ArgumentParser(description="Flasher de bridges FraguaMesh")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    g = sub.add_parser("gen")
    g.add_argument("nodes", nargs="*")
    b = sub.add_parser("build")
    b.add_argument("node")
    f = sub.add_parser("flash")
    f.add_argument("node", nargs="?")
    f.add_argument("--port")
    f.add_argument("--fresh", action="store_true")
    f.add_argument("--monitor", action="store_true")
    args = ap.parse_args()

    cfg = load_config()
    if args.cmd == "list":
        cmd_list(cfg)
    elif args.cmd == "gen":
        cmd_gen(cfg, args.nodes)
    elif args.cmd == "build":
        cmd_build(cfg, args.node)
    elif args.cmd == "flash":
        cmd_flash(cfg, args)


if __name__ == "__main__":
    main()
