#!/usr/bin/env python3
"""KinoWASM/operand.h の内部 opcode enum から名前テーブル (.inc) を生成する。

性能計測デバッグモード (KINOWASM_OPCODE_PROFILE / KINOWASM_OPCODE_COUNTER) の
レポートで opcode を数値ではなく名前で表示するため、debug.c の
debug_opcode_name() が #include する switch 本体を出力する。

operand.h の enum 値を変更・追加したら、本スクリプトを再実行して
debug_opcode_names.inc を再生成すること。

使い方:
    python tools/gen_opcode_names.py

出力:
    debug_opcode_names.inc (host/ 配下。debug.c と同じディレクトリに置く —
    debug.c の #include "debug_opcode_names.inc" は自ディレクトリを最優先で解決するため)
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OPERAND_H = os.path.join(ROOT, "KinoWASM", "operand.h")
OUT_INC = os.path.join(ROOT, "host/debug_opcode_names.inc")

# 例: "\tOP_I32_ADD = 0x6A,"  (行頭のインデント + OP名 + = + 16進値)
ENTRY_RE = re.compile(r"^\s*(OP_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*,")


def parse_entries(path):
    """operand.h を 1 行ずつ読み、(value, name) のリストを enum 出現順で返す。"""
    entries = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = ENTRY_RE.match(line)
            if not m:
                continue
            name = m.group(1)
            value = int(m.group(2), 0)
            entries.append((value, name))
    return entries


def main():
    if not os.path.isfile(OPERAND_H):
        sys.stderr.write("operand.h not found: %s\n" % OPERAND_H)
        return 1

    entries = parse_entries(OPERAND_H)
    if not entries:
        sys.stderr.write("no OP_ enum entries parsed from operand.h\n")
        return 1

    # case ラベルは重複不可。同一値の別名 (alias) は最初に現れた名前のみ採用する。
    seen = {}
    duplicates = []
    for value, name in entries:
        if value in seen:
            duplicates.append((value, seen[value], name))
            continue
        seen[value] = name

    lines = []
    lines.append("/* debug_opcode_names.inc")
    lines.append(" *")
    lines.append(" * 自動生成ファイル — 手で編集しないこと。")
    lines.append(" * KinoWASM/operand.h の内部 opcode enum から tools/gen_opcode_names.py で生成。")
    lines.append(" * enum を変更したら `python tools/gen_opcode_names.py` で再生成する。")
    lines.append(" *")
    lines.append(" * debug.c の debug_opcode_name() 内 switch 本体として #include される。")
    lines.append(" */")
    for value in sorted(seen):
        lines.append('case 0x%04X: return "%s";' % (value, seen[value]))

    # 既存ソースに合わせて UTF-8 (BOM 付き) + CRLF で出力する。
    # MSVC は BOM を見て UTF-8 と認識する (BOM が無いと CP932 と誤認し C4819)。
    with open(OUT_INC, "w", encoding="utf-8-sig", newline="\r\n") as f:
        f.write("\n".join(lines))
        f.write("\n")

    sys.stdout.write("generated %s (%d opcodes" % (OUT_INC, len(seen)))
    if duplicates:
        sys.stdout.write(", %d alias skipped" % len(duplicates))
    sys.stdout.write(")\n")
    for value, kept, skipped in duplicates:
        sys.stdout.write("  alias 0x%04X: kept %s, skipped %s\n" % (value, kept, skipped))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
