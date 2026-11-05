#!/usr/bin/env python3
import sys
import os
import re

MAX_ALLOWED_TOTAL_BUDGET = 1_258_291  # 1.2 MiB: the VOCR6.2 blob lives in POOL_OCR_WEIGHTS

BANNED_PATTERNS = [
    (re.compile(r'\bmalloc\s*\('), "malloc()"),
    (re.compile(r'\bcalloc\s*\('), "calloc()"),
    (re.compile(r'\brealloc\s*\('), "realloc()"),
    (re.compile(r'(?<![A-Za-z0-9_])free\s*\('), "free()"),
    (re.compile(r'\bnew\s+'), "operator new"),
    (re.compile(r'\bnew\['), "operator new[]"),
    (re.compile(r'\bdelete\s+'), "operator delete"),
    (re.compile(r'\bdelete\['), "operator delete[]"),
]

def strip_comments_and_strings(code: str) -> str:
    """Strip C/C++ comments and string literals to prevent false positives."""
    def replacer(match):
        s = match.group(0)
        if s.startswith('/'):
            return " " * len(s) # replace comment with spaces to preserve line offsets
        else:
            return '""'
    pattern = re.compile(
        r'//.*?$|/\*.*?\*/|\'(?:\\.|[^\\\'])*\'|"(?:\\.|[^\\"])*"',
        re.DOTALL | re.MULTILINE
    )
    return re.sub(pattern, replacer, code)

def verify_pool_budgets(header_path: str):
    print(f"[CHECK] Verifying memory ledger budgets in {header_path}...")
    if not os.path.exists(header_path):
        print(f"ERROR: Header not found: {header_path}", file=sys.stderr)
        sys.exit(1)

    with open(header_path, "r", encoding="utf-8") as f:
        content = f.read()

    pool_sizes = {}
    pool_regex = re.compile(r'#define\s+(SAFIR_POOL_[A-Z0-9_]+_MAX_BYTES)\s+([0-9]+)(?:ULL|UL|U|LL)?', re.MULTILINE)
    for match in pool_regex.finditer(content):
        name = match.group(1)
        val = int(match.group(2))
        pool_sizes[name] = val

    total_regex = re.compile(r'#define\s+SAFIR_TOTAL_BUDGET_MAX_BYTES\s+([0-9]+)(?:ULL|UL|U|LL)?', re.MULTILINE)
    total_match = total_regex.search(content)
    if not total_match:
        print("ERROR: SAFIR_TOTAL_BUDGET_MAX_BYTES not defined in header!", file=sys.stderr)
        sys.exit(1)

    defined_total = int(total_match.group(1))
    summed_pools = sum(pool_sizes.values())

    print(f"  Found {len(pool_sizes)} static pools:")
    for k, v in pool_sizes.items():
        print(f"    - {k}: {v:,} bytes ({v / (1024 * 1024):.2f} MB)")
    print(f"  Sum of static pools: {summed_pools:,} bytes")
    print(f"  Defined budget limit: {defined_total:,} bytes")

    if summed_pools > MAX_ALLOWED_TOTAL_BUDGET:
        print(f"ERROR: Summed pool sizes ({summed_pools} bytes) exceed budget limit ({MAX_ALLOWED_TOTAL_BUDGET} bytes)!", file=sys.stderr)
        sys.exit(1)

    if defined_total > MAX_ALLOWED_TOTAL_BUDGET:
        print(f"ERROR: SAFIR_TOTAL_BUDGET_MAX_BYTES ({defined_total}) exceeds {MAX_ALLOWED_TOTAL_BUDGET} bytes!", file=sys.stderr)
        sys.exit(1)

    print(f"[PASS] Static pool budgets are strictly within {MAX_ALLOWED_TOTAL_BUDGET:,} bytes headroom limit.")

def scan_for_banned_allocations(root_dir: str):
    print(f"[CHECK] Scanning for banned dynamic allocations in core/ and switch/...")
    violations = []
    scanned_files = 0

    scan_dirs = ["core", "switch"]
    allowed_files = {"core/safir_ledger.c", "core\\safir_ledger.c", "switch/main_ovl.cpp", "switch\\main_ovl.cpp", "switch/main.cpp", "switch\\main.cpp"}

    for s_dir in scan_dirs:
        full_path = os.path.join(root_dir, s_dir)
        if not os.path.exists(full_path):
            continue

        for root, _, files in os.walk(full_path):
            for file in files:
                if not file.endswith((".c", ".cpp", ".cc", ".cxx", ".h", ".hpp")):
                    continue

                rel_path = os.path.relpath(os.path.join(root, file), root_dir)
                norm_path = rel_path.replace("\\", "/")
                if norm_path in ["core/safir_ledger.c", "switch/main_ovl.cpp", "switch/main.cpp"]:
                    continue

                scanned_files += 1
                file_full = os.path.join(root, file)
                with open(file_full, "r", encoding="utf-8", errors="replace") as f:
                    raw_lines = f.readlines()

                code_clean = strip_comments_and_strings("".join(raw_lines))
                clean_lines = code_clean.splitlines()

                for line_idx, clean_line in enumerate(clean_lines):
                    for pattern, desc in BANNED_PATTERNS:
                        if pattern.search(clean_line):
                            violations.append((rel_path, line_idx + 1, desc, raw_lines[line_idx].strip()))

    print(f"  Scanned {scanned_files} source files.")
    if violations:
        print("\nERROR: Found banned dynamic allocation(s):", file=sys.stderr)
        for path, line_no, desc, snippet in violations:
            print(f"  {path}:{line_no} uses '{desc}': {snippet}", file=sys.stderr)
        print("Rule 2 Violation: All allocations must use safir_alloc() via include/safir_ledger.h!", file=sys.stderr)
        sys.exit(1)

    print("[PASS] Zero banned dynamic allocations found.")

def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    header_path = os.path.join(repo_root, "include", "safir_ledger.h")

    verify_pool_budgets(header_path)
    scan_for_banned_allocations(repo_root)
    print("\n[ALL CHECKS PASSED] Memory Ledger is compliant.")

if __name__ == "__main__":
    main()
