# Orange Pi backups
- Backups reach git only through `scripts/orangepi/backup-pull.sh` run on the MacBook (docs/20). Never push from the board, never install GitHub tokens on it.
- Target is a clone of fabiodj2/RX3-Orange-PI-4-LTS (`BACKUP_REPO`); one branch per backup: `backup/orangepi-<date>-<label>`; never the default branch.
- Never store firmware, `rbp` binaries, keys or credentials. Show the script's summary and wait for the user's explicit yes before the push step; never pass `--yes` on the user's behalf.
- Do not weaken the blocklist or secret scan to make a backup pass; fix the source or ask.
