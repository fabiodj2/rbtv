#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Static guards for the stems module."""

from pathlib import Path
import json


ROOT = Path(__file__).resolve().parent
REPOSITORY = ROOT.parents[3]
MODULE = (ROOT / "module.sh").read_text()
MANIFEST = json.loads((ROOT / "manifest.json").read_text())
HOOK = (REPOSITORY / "mod/modules/core/1.19/rx3_core_hook.c").read_text()
FEATURE = (ROOT / "rx3_stems_feature.h").read_text()
PANEL = (ROOT / "rx3_stems_panel.h").read_text()
SOURCE = HOOK + FEATURE + PANEL


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require(
    "register_prepare_hook stems_prepare" in MODULE
    and "register_after_launch_hook stems_after_launch" in MODULE
    and "export RX3_STEMS_DIR" in MODULE,
    "stem lifecycle logic must remain owned by the stems module",
)
require(
    MANIFEST["requires"] == ["core"] and MANIFEST["conflicts"] == [],
    "the stems dependency must be expressed by its manifest",
)
require(
    set(MANIFEST["build_files"]) == {
        "rx3_stems_decl.h", "rx3_stems_feature.h", "rx3_stems_panel.h"
    },
    "the module must own and publish its lifecycle and panel sources",
)
require(
    '[ -r "$CORE_OBJECT" ]' in MODULE,
    "the module must decline when the performance core is not selected",
)
require(
    "librx3" not in MODULE,
    "the stems module must no longer install a shared object of its own; the "
    "core it depends on is named through CORE_OBJECT, not by path",
)
require(
    "STEMS_PAD_STATE_FILE" in HOOK
    and "state.active[deck]" in HOOK
    and "state.armed[deck]" in HOOK,
    "STEMS LED state must be published through the dedicated state transport",
)
require(
    'memcmp(header.magic, "RX3STM2", 7)' in HOOK
    and "header.sample_rate != 44100" in HOOK,
    "the RX3STM2 sidecar must be validated against PcmReader's time domain",
)
require(
    "(void)mprotect(drums, payload, PROT_READ);" in HOOK
    and "(void)mprotect(vocal, payload, PROT_READ);" in HOOK,
    "both resident RX3STM2 payloads must become read-only before playback",
)
require(
    "if (output[i].left == 0.0f && output[i].right == 0.0f)" in HOOK,
    "a partially zero-filled block must never become inverted vocal audio",
)
require(
    "key_code < 0x411du || key_code > 0x411eu" not in FEATURE
    and "command.deck < 2u && command.control < 3u" in HOOK
    and "stems_panel_activate(command.deck, command.control);" in HOOK,
    "only the three dedicated DDJ STEMS controls may be accepted",
)
stream_body = FEATURE.split("static unsigned long hooked_get_stream", 1)[1].split(
    "\nstatic ", 1
)[0]
require(
    "apply_mix(context, position, output, frames, selected)" in stream_body,
    "stems must be mixed inside getStreamAt, where the read is addressed by "
    "position and therefore indifferent to the reader's out-of-order access",
)
require(
    '#include "../../stems/1.19/rx3_stems_feature.h"' in HOOK
    and "stems_feature_install" in FEATURE
    and "keyshift" not in FEATURE.lower(),
    "stems must implement only its own core lifecycle and hook group",
)

print("Stems regression guards: OK")
