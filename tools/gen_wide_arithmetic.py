#!/usr/bin/env python3
"""wide-arithmetic proposal の testsuite (wasm + json) を生成する。

wabt 1.0.x は wide-arithmetic 構文を parse できないため、proposal repo の
wast 内 (module binary ...) を直接 .wasm にエクスポートし、assert_return を
JSON 形式に変換する。

使い方:
    python gen_wide_arithmetic.py <input.wast> <output_dir>

出力:
    <output_dir>/wide-arithmetic.wasm
    <output_dir>/wide-arithmetic.json
"""

import os
import re
import struct
import sys


def build_wasm():
    """proposal の wast に書かれている (module binary ...) と同等の wasm を生成する。"""
    parts = []
    # magic + version
    parts.append(b"\x00asm\x01\x00\x00\x00")
    # type section: id=1, payload=17 bytes
    type_payload = b"".join([
        bytes([2]),                        # 2 types
        b"\x60", bytes([4]) + b"\x7e\x7e\x7e\x7e", bytes([2]) + b"\x7e\x7e",
        b"\x60", bytes([2]) + b"\x7e\x7e", bytes([2]) + b"\x7e\x7e",
    ])
    parts.append(b"\x01" + bytes([len(type_payload)]) + type_payload)
    # function section: id=3
    func_payload = bytes([4]) + b"\x00\x00\x01\x01"
    parts.append(b"\x03" + bytes([len(func_payload)]) + func_payload)
    # export section: id=7
    def export_entry(name, idx):
        return bytes([len(name)]) + name.encode() + b"\x00" + bytes([idx])
    export_payload = bytes([4]) + b"".join([
        export_entry("i64.add128", 0),
        export_entry("i64.sub128", 1),
        export_entry("i64.mul_wide_s", 2),
        export_entry("i64.mul_wide_u", 3),
    ])
    parts.append(b"\x07" + bytes([len(export_payload)]) + export_payload)
    # code section: id=10
    def code_entry(args, opcode_bytes):
        # body = no locals + local.get 0..(args-1) + opcode + end
        body = b"\x00"
        for i in range(args):
            body += b"\x20" + bytes([i])
        body += opcode_bytes + b"\x0b"
        return bytes([len(body)]) + body
    code_payload = bytes([4]) + b"".join([
        code_entry(4, b"\xfc\x13"),  # i64.add128
        code_entry(4, b"\xfc\x14"),  # i64.sub128
        code_entry(2, b"\xfc\x15"),  # i64.mul_wide_s
        code_entry(2, b"\xfc\x16"),  # i64.mul_wide_u
    ])
    parts.append(b"\x0a" + bytes([len(code_payload)]) + code_payload)
    return b"".join(parts)


_NUM_RE = re.compile(r"-?\d+")


def parse_i64_const(token):
    """`(i64.const N)` を解析して 64bit 符号付き値を 0..2^64-1 に正規化する。"""
    m = _NUM_RE.search(token)
    if not m:
        raise ValueError(f"cannot parse i64: {token}")
    v = int(m.group())
    return v & 0xFFFFFFFFFFFFFFFF


def parse_assertions(wast_text):
    """wast から assert_return をすべて抜き出して JSON 用 dict のリストを返す。"""
    assertions = []
    line_no = 0
    in_assert = False
    buf = ""
    start_line = 0
    for raw in wast_text.splitlines(True):
        line_no += 1
        if not in_assert:
            if "(assert_return" in raw:
                in_assert = True
                buf = raw[raw.index("(assert_return"):]
                start_line = line_no
                # 念のため一行で完結している場合も処理
                depth = buf.count("(") - buf.count(")")
                if depth == 0 and buf.strip().endswith(")"):
                    item = parse_one(buf, start_line)
                    if item is not None:
                        assertions.append(item)
                    in_assert = False
                    buf = ""
        else:
            buf += raw
            depth = buf.count("(") - buf.count(")")
            if depth == 0:
                item = parse_one(buf, start_line)
                if item is not None:
                    assertions.append(item)
                in_assert = False
                buf = ""
    return assertions


def parse_one(text, line):
    """1 件の `(assert_return (invoke "name" args...) results...)` を parse する。"""
    inv_m = re.search(r'\(invoke\s+"([^"]+)"((?:\s*\(i64\.const\s+-?\d+\))*)\s*\)', text)
    if not inv_m:
        return None
    field = inv_m.group(1)
    args_str = inv_m.group(2)
    args = []
    for m in re.finditer(r"\(i64\.const\s+(-?\d+)\)", args_str):
        args.append({"type": "i64", "value": str(parse_i64_const(m.group(1)))})
    # results: invoke 句の後ろに続く (i64.const ...) を全部拾う
    after = text[inv_m.end():]
    # 末尾の ) を 1 つ削って中身を見る
    expected = []
    for m in re.finditer(r"\(i64\.const\s+(-?\d+)\)", after):
        expected.append({"type": "i64", "value": str(parse_i64_const(m.group(1)))})
    return {
        "type": "assert_return",
        "line": line,
        "action": {"type": "invoke", "field": field, "args": args},
        "expected": expected,
    }


def build_json(wast_path, wasm_filename, assertions):
    import json
    commands = [{"type": "module", "line": 1, "filename": wasm_filename}] + assertions
    return json.dumps({
        "source_filename": wast_path.replace("\\", "/"),
        "commands": commands,
    }, indent=1)


def main():
    if len(sys.argv) != 3:
        print("usage: gen_wide_arithmetic.py <input.wast> <output_dir>", file=sys.stderr)
        sys.exit(1)
    wast_path = sys.argv[1]
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    with open(wast_path, "r", encoding="utf-8") as f:
        wast_text = f.read()
    wasm_bytes = build_wasm()
    wasm_path = os.path.join(out_dir, "wide-arithmetic.wasm")
    json_path = os.path.join(out_dir, "wide-arithmetic.json")
    with open(wasm_path, "wb") as f:
        f.write(wasm_bytes)
    assertions = parse_assertions(wast_text)
    with open(json_path, "w", encoding="utf-8") as f:
        f.write(build_json(wast_path, "wide-arithmetic.wasm", assertions))
    print(f"wrote {wasm_path} ({len(wasm_bytes)} bytes), {json_path} ({len(assertions)} assertions)")


if __name__ == "__main__":
    main()
