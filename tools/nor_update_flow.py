# =============================================================================
# HYDRA-UMC - tools/nor_update_flow.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the backup of the application slot before an update (the C twin is src/mcu_stm32h757/Common/nor_update_flow.h) and the reader of the [previous] and [update] sections of
config/nor_backup.cfg: the two regions of the NOR, the rule that they may not overlap, and the choice made at start-up. Evidence: simulator."""
from __future__ import annotations

import configparser
from pathlib import Path

import nor_backup_plan as nb

CFG_PATH = nb.CFG_PATH
NONE, RESTORE_CURRENT, ROLLBACK_PREVIOUS, LOST = 0, 1, 2, 3


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    current = nb.load_config(path)
    previous = dict(current, nor_offset=int(parser["previous"]["offset_bytes"]))
    cfg = {"current": current, "previous": previous, "require_backup": int(parser["update"]["require_backup"])}
    if cfg["require_backup"] not in (0, 1):
        raise ValueError("config/nor_backup.cfg: require_backup is 0 or 1")
    plan(cfg)
    return cfg


def plan(cfg: dict) -> tuple[dict, dict]:
    """The layouts of both regions; ValueError when one does not fit, the slots differ or the regions overlap."""
    current, previous = nb.plan(cfg["current"]), nb.plan(cfg["previous"])
    if cfg["current"]["slot_bytes"] != cfg["previous"]["slot_bytes"]:
        raise ValueError("the two regions must hold the same slot")
    if previous["header_a"] < current["end"] and current["header_a"] < previous["end"]:
        raise ValueError("the regions overlap")
    return current, previous


def decide(app_good: bool, current_copy: bool, previous_copy: bool) -> int:
    if app_good:
        return NONE
    if current_copy:
        return RESTORE_CURRENT
    return ROLLBACK_PREVIOUS if previous_copy else LOST


def update_allowed(app_good: bool, backup_ok: bool, require_backup: int) -> bool:
    """Whether an update may start: nothing to protect, or the backup worked, or the file lets it go on without."""
    return (not app_good) or backup_ok or not require_backup
