# Repo guidance for Claude

This is a **public** repository. Anything committed is world-readable forever.

## Before every commit: scan for sensitive data

Every time you are about to commit, review the staged diff (`git diff --cached`)
and refuse to commit if it contains any real secret or private data. Check for:

- Credentials: passwords, API tokens, admin passwords, private keys (WireGuard
  `wg_private_key`, SSH keys, TLS keys), `.env` values.
- WireGuard details: real server endpoints/hostnames, peer public keys, real
  tunnel IPs tied to a deployment.
- Network identifiers: real WiFi SSIDs and passwords, internal/LAN IPs, server
  hostnames, deploy paths, SSH `user@host`.
- Personal data: real GPS coordinates, home/site locations, names, emails.

Example/template files (`*.example`, docs) must use obvious placeholders
(`CHANGEME`, `vpn.example.com`, `0.0`), never real values. Real config lives in
gitignored files (`fragua-mesh.toml`, `platformio.local.ini`, `platformio.fragua.ini`).

If a commit that already happened leaked data, scrub the file, rewrite history
(amend/rebase), and force-push with `--force-with-lease`.
