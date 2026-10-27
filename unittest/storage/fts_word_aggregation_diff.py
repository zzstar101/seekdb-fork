#!/usr/bin/env python3
"""Compare TOKENIZE output bytes against a reference seekdb build.

Capture before replacing the server binary, then compare on the candidate:
  python3 unittest/storage/fts_word_aggregation_diff.py \
    --mysql 'mysql -h127.0.0.1 -P2881 -uroot' --save-reference /tmp/fts-map.json
  python3 unittest/storage/fts_word_aggregation_diff.py \
    --mysql 'mysql -h127.0.0.1 -P2881 -uroot' --reference /tmp/fts-map.json

The corpus uses only SELECT statements; no database or table is modified.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


TEXTS = [
    None,
    "",
    "   ,.;!?\t\n",
    "alpha alpha beta gamma beta alpha",
    "ALPHA alpha Alpha beta BETA gamma",
    "the THE and AND is in with a to",
    "cafe café CAFE CAFÉ resume résumé RESUME Résumé",
    "i İ ı I Straße STRASSE straße",
    "数据库 数据库 全文索引 分词器 分词器 内存管理",
    "数据库 OceanBase oceanbase TOKENIZER tokenizer 全文索引 2026 10.27",
    "alpha😀beta 😀 \U00020000全文索引 🍣 Tokyo東京 カタカナ 한국어",
    "alpha\x00beta gamma\talpha delta\ngamma",
    "xx " + "a" * 84 + " " + "b" * 85 + " alpha",
    " ".join(f"token{i:03}" for i in range(160)),
    " ".join(["repeat", "alpha", "beta", "repeat"] * 128),
]
PARSERS = ["ik", "beng", "space", "ngram", "ngram2"]
COLLATIONS = ["utf8mb4_bin", "utf8mb4_general_ci", "utf8mb4_unicode_ci"]
# Uppercase non-ASCII inside lowercase endpoints must still be normalized.
BENG_TEXTS = ["abÉcd abécd abΩcd abωcd abcZ abcy abc0 0123"]


def text_expression(value, collation):
    if value is None:
        return f"CAST(NULL AS CHAR CHARACTER SET utf8mb4) COLLATE {collation}"
    return f"CONVERT(X'{value.encode('utf-8').hex()}' USING utf8mb4) COLLATE {collation}"


def corpus():
    statements = ["SET ob_query_timeout = 120000000;"]
    keys = []
    for parser in PARSERS:
        texts = TEXTS + BENG_TEXTS if parser == "beng" else TEXTS
        for collation in COLLATIONS:
            for index, value in enumerate(texts):
                for mode in ("default", "all"):
                    key = f"{parser}/{collation}/{index}/{mode}"
                    keys.append(key)
                    expr = text_expression(value, collation)
                    props = json.dumps([{"output": mode}], separators=(",", ":"))
                    statements.append(
                        f"SELECT '{key}', HEX(CAST(TOKENIZE({expr}, '{parser}', '{props}') AS CHAR));"
                    )
            # Evaluate one expression over alternating populated/empty rows. A
            # document-local state leak must not affect the following document.
            rows = []
            for index, value in enumerate([TEXTS[3], "", TEXTS[6], " ", "fresh fresh"]):
                expr = text_expression(value, collation)
                rows.append(f"SELECT {index} AS n, {expr} AS txt")
                keys.append(f"rows/{parser}/{collation}/{index}")
            statements.append(
                f"SELECT CONCAT('rows/{parser}/{collation}/', n), "
                f"HEX(CAST(TOKENIZE(txt, '{parser}', '[{{\"output\":\"all\"}}]') AS CHAR)) "
                f"FROM ({' UNION ALL '.join(rows)}) AS documents ORDER BY n;"
            )
    # Binary is supported but must retain the original case conversion path.
    binary_text = "ALPHA alpha Alpha Zz9 Zz9"
    binary_expr = f"CONVERT(X'{binary_text.encode().hex()}' USING binary)"
    for mode in ("default", "all"):
        key = f"beng/binary/fallback/{mode}"
        keys.append(key)
        props = json.dumps([{"output": mode}], separators=(",", ":"))
        statements.append(
            f"SELECT '{key}', HEX(CAST(TOKENIZE({binary_expr}, 'beng', '{props}') AS CHAR));"
        )
    return "\n".join(statements) + "\n", keys


def run_mysql(command, sql, keys):
    args = shlex.split(command)
    if not args:
        raise RuntimeError("--mysql must name a MySQL-compatible client")
    args += ["--batch", "--raw", "--skip-column-names", "--default-character-set=utf8mb4"]
    result = subprocess.run(args, input=sql.encode(), capture_output=True, timeout=300, check=False)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    outputs = {}
    for line in result.stdout.decode().splitlines():
        key, value = line.split("\t", 1)
        if key in outputs:
            raise RuntimeError(f"duplicate result: {key}")
        # JSON decoding also catches malformed or NULL responses. Keep the hex
        # bytes for comparison so token order and first spelling are checked.
        json.loads(bytes.fromhex(value).decode())
        outputs[key] = value
    if set(outputs) != set(keys):
        raise RuntimeError("result keys do not match the regression corpus")
    return outputs


def check_invariants(outputs):
    for parser in PARSERS:
        for collation in COLLATIONS:
            for index in (0, 1):
                default = json.loads(bytes.fromhex(outputs[f"{parser}/{collation}/{index}/default"]))
                detail = json.loads(bytes.fromhex(outputs[f"{parser}/{collation}/{index}/all"]))
                if default != [] or detail != {"tokens": [], "doc_len": 0}:
                    raise RuntimeError(f"nonempty NULL/empty result: {parser}/{collation}/{index}")
    for parser in ("beng", "space"):
        for collation in COLLATIONS:
            detail = json.loads(bytes.fromhex(outputs[f"{parser}/{collation}/3/all"]))
            counts = {word: count for entry in detail["tokens"] for word, count in entry.items()}
            if counts != {"alpha": 3, "beta": 2, "gamma": 1} or detail["doc_len"] != 6:
                raise RuntimeError(f"wrong grouped frequencies: {parser}/{collation}")
    binary_detail = json.loads(bytes.fromhex(outputs["beng/binary/fallback/all"]))
    binary_counts = {word: count for entry in binary_detail["tokens"] for word, count in entry.items()}
    if binary_counts != {"ALPHA": 1, "alpha": 1, "Alpha": 1, "Zz9": 2} or binary_detail["doc_len"] != 5:
        raise RuntimeError("binary BENG normalization changed case or frequency")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mysql", help="client command and connection options; executed without a shell")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--save-reference", type=Path)
    mode.add_argument("--reference", type=Path)
    mode.add_argument("--emit-sql", action="store_true")
    args = parser.parse_args()
    sql, keys = corpus()
    if args.emit_sql:
        print(sql, end="")
        return
    if not args.mysql:
        parser.error("--mysql is required when running a server")
    outputs = run_mysql(args.mysql, sql, keys)
    check_invariants(outputs)
    snapshot = {"format_version": 1, "corpus_sha256": hashlib.sha256(sql.encode()).hexdigest(), "outputs": outputs}
    if args.save_reference:
        args.save_reference.write_text(json.dumps(snapshot, indent=2, ensure_ascii=False) + "\n")
        print(f"Captured {len(outputs)} TOKENIZE results; frequency and empty-input invariants passed.")
        return
    reference = json.loads(args.reference.read_text())
    if reference.get("format_version") != 1 or reference.get("corpus_sha256") != snapshot["corpus_sha256"]:
        raise RuntimeError("reference uses a different regression corpus")
    mismatches = [key for key in keys if outputs[key] != reference["outputs"].get(key)]
    if mismatches:
        raise RuntimeError(f"{len(mismatches)} TOKENIZE byte differences: " + ", ".join(mismatches[:12]))
    print(f"PASS: {len(outputs)} TOKENIZE results are byte-identical; frequency and empty-input invariants passed.")


if __name__ == "__main__":
    main()
