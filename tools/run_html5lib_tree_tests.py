#!/usr/bin/env python3
"""Compare MLBrowser's supported HTML document tree against html5lib .dat fixtures.

This is intentionally a structural subset comparator: the current MLBrowser DOM
has no fragment parser, parse-error reporting, namespaces or comment/doctype
nodes. Unsupported fixtures are counted as skipped rather than silently treated
as passes. The comparator is diagnostic and does not claim full HTML5 conformance.
"""
from __future__ import annotations
import argparse
import ast
import re
import subprocess
import sys
from pathlib import Path


def source_cases(path: Path):
    raw = path.read_bytes().decode("utf-8", errors="replace")
    # Each html5lib fixture starts with a line-oriented #data marker.
    chunks = re.split(r"(?m)^#data\n", raw)[1:]
    for ordinal, chunk in enumerate(chunks, 1):
        if "\n#errors\n" not in chunk:
            continue
        data, rest = chunk.split("\n#errors\n", 1)
        if data.endswith("\n"):
            data = data[:-1]  # delimiter newline before #errors, not input data
        input_bytes = data.encode("utf-8")
        # Check fragment marker before splitting at #document: the latter is
        # also a prefix of #document-fragment.
        if "\n#document-fragment\n" in rest:
            yield ordinal, input_bytes, None, "fragment parsing is not exposed by html_build"
            continue
        header = rest.split("#document", 1)[0]
        if "#script-off" in header or "#script-on" in header:
            yield ordinal, input_bytes, None, "fixture requires a scripting-mode toggle"
            continue
        if "\n#document\n" not in rest:
            yield ordinal, input_bytes, None, "fixture has no document tree expectation"
            continue
        expected_block = rest.split("\n#document\n", 1)[1]
        # Cases are separated with a blank line; tree output itself has no blank lines.
        expected_block = expected_block.split("\n\n", 1)[0]
        expected = parse_expected_tree(expected_block)
        if expected is None:
            yield ordinal, input_bytes, None, "fixture contains unsupported tree nodes or text encoding"
        else:
            yield ordinal, input_bytes, expected, ""


def parse_expected_tree(block: str):
    result = []
    for raw_line in block.splitlines():
        line = raw_line.rstrip("\r")
        if not line.startswith("| "):
            continue
        rest = line[2:]
        indent = len(rest) - len(rest.lstrip(" "))
        if indent % 2:
            return None
        depth = indent // 2
        content = rest[indent:]
        if not content:
            continue
        if content.startswith("<"):
            if content.startswith("<!") or content.startswith("<?"):
                continue  # comments/doctypes are absent from the MLBrowser DOM
            m = re.match(r"<(?:svg |math )?([^\s>]+)", content)
            if m:
                tag = m.group(1)
                if tag == "svg" or tag.startswith("svg") or tag.startswith("math"):
                    return None  # namespace-aware SVG/MathML trees are unsupported here
                if tag == "template":
                    return None  # template-content fragments are not represented here
                result.append(("E", depth, tag))
            continue  # attribute lines are intentionally excluded
        if content.startswith('"'):
            try:
                text_value = ast.literal_eval(content)
            except (SyntaxError, ValueError):
                return None
            if not isinstance(text_value, str):
                return None
            result.append(("T", depth, text_value.encode("utf-8").hex()))
        if content.startswith("#document-fragment") or content.startswith("#shadow-root"):
            return None
    return result


def actual_tree(executable: str, data: bytes):
    proc = subprocess.run([executable, "--dump"], input=data, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=False)
    if proc.returncode:
        return None, proc.stderr.decode("utf-8", errors="replace") or f"exit={proc.returncode}"
    result = []
    try:
        for line in proc.stdout.decode("ascii").splitlines():
            kind, depth, value = line.split("\t", 2)
            result.append((kind, int(depth), value))
    except Exception as exc:  # malformed output should be reported as a harness failure
        return None, f"invalid parser dump: {exc}"
    return result, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fixtures", type=Path, help="path to html5lib-tests/tree-construction")
    ap.add_argument("--parser", default="build/html5_tree_builder_test", help="compiled parser test binary")
    ap.add_argument("--limit", type=int, default=0, help="run only the first N supported/skipped cases (0 = all)")
    ap.add_argument("--show", type=int, default=12, help="number of initial failures to print")
    args = ap.parse_args()
    if not args.fixtures.is_dir():
        print(f"fixture directory not found: {args.fixtures}", file=sys.stderr)
        return 2
    if not Path(args.parser).exists():
        print(f"parser binary not found: {args.parser}; run make test-html5-tree-builder", file=sys.stderr)
        return 2

    files = sorted(p for p in args.fixtures.rglob("*.dat") if "scripted" not in p.parts)
    if not files:
        print(f"no .dat fixture files found under {args.fixtures}", file=sys.stderr)
        return 2
    checked = skipped = failed = cases_seen = 0
    skip_reasons: dict[str, int] = {}
    for path in files:
        for ordinal, data, expected, reason in source_cases(path):
            cases_seen += 1
            if expected is None:
                skipped += 1
                skip_reasons[reason] = skip_reasons.get(reason, 0) + 1
            else:
                actual, error = actual_tree(args.parser, data)
                checked += 1
                if actual is None:
                    failed += 1
                    if failed <= args.show:
                        print(f"CRASH {path.name} case {ordinal}: {error}")
                elif actual != expected:
                    failed += 1
                    if failed <= args.show:
                        upto = min(len(actual), len(expected))
                        diff = next((i for i in range(upto) if actual[i] != expected[i]), upto)
                        got = actual[diff] if diff < len(actual) else "<end>"
                        want = expected[diff] if diff < len(expected) else "<end>"
                        print(f"FAIL {path.name} case {ordinal}: at node {diff}; expected {want!r}, got {got!r}; lengths {len(expected)}/{len(actual)}")
            if args.limit and cases_seen >= args.limit:
                break
        if args.limit and cases_seen >= args.limit:
            break
    if cases_seen == 0:
        print(f"no #data/#errors fixtures found under {args.fixtures}", file=sys.stderr)
        return 2
    print(f"html5lib structural subset: cases={cases_seen}, checked={checked}, failed={failed}, skipped={skipped}")
    for reason, count in sorted(skip_reasons.items()):
        print(f"  skipped {count}: {reason}")
    print("NOTE: attributes, comments, doctypes, namespaces, parse errors, fragments and scripting modes are not compared.")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
