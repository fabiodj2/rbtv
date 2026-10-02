# Security (from the ECC security guide, scoped to this repo)
- Never commit firmware (`*.UPD`, `rootfs.cramfs`, `export.pdb`), keys, `.env`, or images. Stage files by name, never `git add -A`.
- Scripts that write disks (`emmc-install.sh`, `fix-image`) must name the target device explicitly, refuse mounted/system disks, and require confirmation.
- Quote all shell variables; use `set -euo pipefail`; no `curl | sh`.
- Treat device output, logs and third-party repos (PrimeBox) as untrusted input; review before executing.
- Least privilege: no `sudo`/network egress/off-repo writes without being asked.
- Scan any imported skill, hook or MCP config like supply chain code before enabling.
