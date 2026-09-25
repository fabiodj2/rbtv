#!/usr/bin/env python3
"""Verify, install and roll back the RK3399 functional runtime baseline."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pwd
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path


REPO = Path(__file__).resolve().parents[3]


def operator_home() -> Path:
    sudo_user = os.environ.get("SUDO_USER")
    if sudo_user:
        return Path(pwd.getpwnam(sudo_user).pw_dir)
    return Path.home()


PRIVATE = operator_home() / "rx3-private/rk3399-1.20-f0777ec4"
RELEASES = PRIVATE / "releases"
BACKUPS = PRIVATE / "deployment-backups"

MAIN_SERVICE = "rx3-rk3399-ddj400.service"
TOGGLE_PATH = "rx3-interface-toggle.path"

FIXED_TARGETS = {
    "runtime/root/pdj/rbp":
        REPO / "work/runtime/rx3/root/pdj/rbp",
    "runtime/root/pdj/librx3_core.so":
        REPO / "work/runtime/rx3/root/pdj/librx3_core.so",
    "host/ddj400-bridge":
        REPO / "work/build/host/ddj400-bridge",
    "scripts/rb/start-rk3399-ddj400.sh":
        REPO / "scripts/rb/start-rk3399-ddj400.sh",
    "scripts/rb/toggle-rk3399-interface.sh":
        REPO / "scripts/rb/toggle-rk3399-interface.sh",
    "scripts/rb/systemd/rx3-rk3399-ddj400.service":
        REPO / "scripts/rb/systemd/rx3-rk3399-ddj400.service",
    "scripts/rb/systemd/rx3-interface-toggle.path":
        REPO / "scripts/rb/systemd/rx3-interface-toggle.path",
    "scripts/rb/systemd/rx3-interface-toggle.service":
        REPO / "scripts/rb/systemd/rx3-interface-toggle.service",
}

UNIT_NAMES = (
    "rx3-rk3399-ddj400.service",
    "rx3-interface-toggle.path",
    "rx3-interface-toggle.service",
)


def digest(path: Path, algorithm: str) -> str:
    value = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def service_state(name: str, operation: str) -> str:
    result = subprocess.run(
        ["systemctl", operation, name],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
    )
    return result.stdout.strip() or "unknown"


def newest_bundle() -> Path:
    candidates = sorted(RELEASES.glob("key-stems-functional-*"))
    if not candidates:
        raise SystemExit("ERRO: nenhum pacote funcional encontrado")
    return candidates[-1]


def resolve_bundle(value: str | None) -> Path:
    bundle = Path(value).expanduser().resolve() if value else newest_bundle()
    if not bundle.is_dir():
        raise SystemExit(f"ERRO: pacote não encontrado: {bundle}")
    return bundle


def read_manifest(bundle: Path) -> dict:
    manifest_path = bundle / "bundle.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"ERRO: manifesto inválido: {error}")

    if manifest.get("schema") != 1:
        raise SystemExit("ERRO: versão de manifesto incompatível")

    if manifest.get("name") != "rk3399-key-stems-functional":
        raise SystemExit("ERRO: pacote não é o baseline funcional esperado")

    return manifest


def safe_relative(value: str) -> Path:
    relative = Path(value)

    if relative.is_absolute() or ".." in relative.parts:
        raise SystemExit(f"ERRO: caminho inseguro no manifesto: {value}")

    return relative


def is_deployable(relative: str) -> bool:
    if relative in FIXED_TARGETS:
        return True

    return (
        relative.startswith("runtime/root/pdj/rx3-")
        and relative.endswith(".rgb565")
        and "/" not in relative[len("runtime/root/pdj/"):]
    )


def verify_bundle(bundle: Path, verbose: bool = True) -> dict:
    manifest = read_manifest(bundle)
    entries = manifest.get("files")

    if not isinstance(entries, list):
        raise SystemExit("ERRO: lista de arquivos ausente no manifesto")

    required = {
        "runtime/root/pdj/rbp",
        "runtime/root/pdj/librx3_core.so",
        "host/ddj400-bridge",
        "scripts/rb/start-rk3399-ddj400.sh",
        "scripts/rb/toggle-rk3399-interface.sh",
        "scripts/rb/systemd/rx3-rk3399-ddj400.service",
        "scripts/rb/systemd/rx3-interface-toggle.path",
        "scripts/rb/systemd/rx3-interface-toggle.service",
    }

    seen: set[str] = set()

    for entry in entries:
        relative_text = entry.get("path", "")
        relative = safe_relative(relative_text)

        if relative_text in seen:
            raise SystemExit(f"ERRO: arquivo duplicado: {relative_text}")
        seen.add(relative_text)

        source = bundle / relative
        if not source.is_file():
            raise SystemExit(f"ERRO: arquivo ausente: {source}")

        size = source.stat().st_size
        if size != entry.get("size"):
            raise SystemExit(f"ERRO: tamanho incorreto: {relative_text}")

        if digest(source, "sha1") != entry.get("sha1"):
            raise SystemExit(f"ERRO: SHA1 incorreto: {relative_text}")

        if digest(source, "sha256") != entry.get("sha256"):
            raise SystemExit(f"ERRO: SHA256 incorreto: {relative_text}")

    missing = sorted(required - seen)
    if missing:
        raise SystemExit("ERRO: arquivos obrigatórios ausentes: " + ", ".join(missing))

    if verbose:
        print(f"PASS: pacote verificado: {bundle}")
        print(f"PASS: {len(entries)} arquivos íntegros")

    return manifest


def deployment_targets(manifest: dict) -> list[tuple[str, Path]]:
    targets: list[tuple[str, Path]] = []

    for entry in manifest["files"]:
        relative = entry["path"]

        if relative in FIXED_TARGETS:
            targets.append((relative, FIXED_TARGETS[relative]))
            continue

        if is_deployable(relative):
            targets.append((
                relative,
                REPO / "work/runtime/rx3/root/pdj" / Path(relative).name,
            ))

    return targets


def all_install_targets(manifest: dict) -> list[tuple[str, str, Path]]:
    result: list[tuple[str, str, Path]] = []

    for relative, target in deployment_targets(manifest):
        result.append((f"repo/{relative}", relative, target))

    for unit in UNIT_NAMES:
        relative = f"scripts/rb/systemd/{unit}"
        result.append((f"systemd/{unit}", relative, Path("/etc/systemd/system") / unit))

    return result


def print_status(bundle: Path) -> int:
    manifest = verify_bundle(bundle, verbose=False)
    expected = {item["path"]: item for item in manifest["files"]}
    mismatch = False

    print(f"BUNDLE={bundle}")
    print()

    for relative, target in deployment_targets(manifest):
        entry = expected[relative]

        if not target.is_file():
            state = "FALTA"
            mismatch = True
        elif target.stat().st_size != entry["size"]:
            state = "TAMANHO_DIFERENTE"
            mismatch = True
        elif digest(target, "sha256") != entry["sha256"]:
            state = "HASH_DIFERENTE"
            mismatch = True
        else:
            state = "OK"

        print(f"{state:18} {target}")

    print()
    print(f"main enabled: {service_state(MAIN_SERVICE, 'is-enabled')}")
    print(f"main active: {service_state(MAIN_SERVICE, 'is-active')}")
    print(f"toggle path enabled: {service_state(TOGGLE_PATH, 'is-enabled')}")
    print(f"toggle path active: {service_state(TOGGLE_PATH, 'is-active')}")

    return 1 if mismatch else 0


def require_root() -> None:
    if os.geteuid() != 0:
        raise SystemExit("ERRO: esta operação exige sudo")


def capture_unit_states() -> dict:
    return {
        name: {
            "enabled": service_state(name, "is-enabled"),
            "active": service_state(name, "is-active"),
        }
        for name in (MAIN_SERVICE, TOGGLE_PATH)
    }


def run_systemctl(*arguments: str, check: bool = True) -> None:
    subprocess.run(["systemctl", *arguments], check=check)


def atomic_copy(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)

    old_stat = target.stat() if target.exists() else None
    temporary = target.parent / f".{target.name}.rx3-new-{os.getpid()}"

    try:
        shutil.copy2(source, temporary)

        if old_stat is not None:
            os.chmod(temporary, old_stat.st_mode & 0o7777)
            os.chown(temporary, old_stat.st_uid, old_stat.st_gid)
        elif str(target).startswith("/etc/"):
            os.chmod(temporary, 0o644)
            os.chown(temporary, 0, 0)

        os.replace(temporary, target)
    finally:
        if temporary.exists():
            temporary.unlink()


def create_backup(
    mappings: list[tuple[str, str, Path]],
    states: dict,
) -> Path:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = BACKUPS / f"deployment-{stamp}"
    files = backup / "files"
    files.mkdir(parents=True, exist_ok=False)

    records = []

    for key, _, target in mappings:
        destination = files / key
        existed = target.is_file()

        record = {
            "key": key,
            "target": str(target),
            "existed": existed,
        }

        if existed:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(target, destination)

        records.append(record)

    metadata = {
        "schema": 1,
        "created": datetime.now().isoformat(),
        "service_states": states,
        "files": records,
    }

    (backup / "backup.json").write_text(
        json.dumps(metadata, indent=2) + "\n",
        encoding="utf-8",
    )

    return backup


def restore_service_states(states: dict) -> None:
    for name, state in states.items():
        enabled = state.get("enabled")

        if enabled == "enabled":
            run_systemctl("enable", name)
        elif enabled == "disabled":
            run_systemctl("disable", name)

    if states.get(TOGGLE_PATH, {}).get("active") == "active":
        run_systemctl("start", TOGGLE_PATH)
    else:
        run_systemctl("stop", TOGGLE_PATH, check=False)

    if states.get(MAIN_SERVICE, {}).get("active") == "active":
        run_systemctl("start", MAIN_SERVICE)
    else:
        run_systemctl("stop", MAIN_SERVICE, check=False)


def restore_backup(backup: Path, restore_services: bool = True) -> None:
    metadata = json.loads(
        (backup / "backup.json").read_text(encoding="utf-8")
    )

    run_systemctl("stop", MAIN_SERVICE, check=False)
    run_systemctl("stop", TOGGLE_PATH, check=False)

    for record in metadata["files"]:
        target = Path(record["target"])
        saved = backup / "files" / record["key"]

        if record["existed"]:
            if not saved.is_file():
                raise RuntimeError(f"backup ausente: {saved}")
            atomic_copy(saved, target)
        elif target.exists():
            target.unlink()

    run_systemctl("daemon-reload")

    if restore_services:
        restore_service_states(metadata["service_states"])


def health_check(states: dict) -> None:
    if states[MAIN_SERVICE]["active"] != "active":
        return

    time.sleep(5)

    if service_state(MAIN_SERVICE, "is-active") != "active":
        raise RuntimeError("serviço principal não ficou ativo")

    checks = (
        r"^/lib/ld-linux\.so\.3 /root/pdj/rbp -a$",
        r"/work/build/host/ddj400-bridge",
    )

    for pattern in checks:
        result = subprocess.run(
            ["pgrep", "-f", pattern],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(f"processo obrigatório ausente: {pattern}")


def install_bundle(bundle: Path) -> None:
    require_root()
    manifest = verify_bundle(bundle)
    mappings = all_install_targets(manifest)
    states = capture_unit_states()
    backup = create_backup(mappings, states)

    print(f"BACKUP={backup}")

    try:
        run_systemctl("stop", MAIN_SERVICE, check=False)
        run_systemctl("stop", TOGGLE_PATH, check=False)

        for _, relative, target in mappings:
            source = bundle / relative
            atomic_copy(source, target)
            print(f"INSTALADO {target}")

        run_systemctl("daemon-reload")
        restore_service_states(states)
        health_check(states)

    except Exception as error:
        print(f"ERRO: instalação falhou: {error}", file=sys.stderr)
        print("Executando rollback automático...", file=sys.stderr)
        restore_backup(backup)
        raise SystemExit(1)

    print("PASS: baseline instalado e validado")
    print(f"ROLLBACK={backup}")


def newest_backup() -> Path:
    candidates = sorted(BACKUPS.glob("deployment-*"))
    if not candidates:
        raise SystemExit("ERRO: nenhum backup de instalação encontrado")
    return candidates[-1]


def rollback(value: str | None) -> None:
    require_root()
    backup = Path(value).expanduser().resolve() if value else newest_backup()

    if not (backup / "backup.json").is_file():
        raise SystemExit(f"ERRO: backup inválido: {backup}")

    restore_backup(backup)
    print(f"PASS: rollback restaurado: {backup}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Gerencia o baseline funcional RX3 RK3399"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    for name in ("verify", "status", "install"):
        subparser = subparsers.add_parser(name)
        subparser.add_argument("--bundle")

    rollback_parser = subparsers.add_parser("rollback")
    rollback_parser.add_argument("--backup")

    args = parser.parse_args()

    if args.command == "verify":
        verify_bundle(resolve_bundle(args.bundle))
        return 0

    if args.command == "status":
        return print_status(resolve_bundle(args.bundle))

    if args.command == "install":
        install_bundle(resolve_bundle(args.bundle))
        return 0

    if args.command == "rollback":
        rollback(args.backup)
        return 0

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
