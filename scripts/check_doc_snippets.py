#!/usr/bin/env python3
"""Run the SQL in the docs and the Claude Code skills against the build, and check that every
function name the skills mention exists. Adapted from anofox-forecast's checker.

Usage (from the repository root, after `make release`):
    python3 scripts/check_doc_snippets.py [FILE ...]
    python3 scripts/check_doc_snippets.py --changed-since REF
    python3 scripts/check_doc_snippets.py --out PATH [FILE ...]
    make check-docs

Default scan: README.md and plugins/anofox-similarity/skills/**/*.md, whose blocks are written to
run as they stand. docs/ pages mostly run against the reader's own tables, so they are checked only
when passed as FILE args. --changed-since REF scans the default set's files changed since REF.

Each ```sql block runs in a fresh in-memory database of ./build/release/duckdb (the extension is
linked in), with a temporary directory as cwd and HOME (test/ and examples/ linked in, so relative data paths
resolve), autoinstall off and telemetry off. The
block gets scripts/doc_snippets/fixtures.sql plus the earlier setup blocks of the same file that
create a table it names. A block with INSTALL or a remote read (read_*/ATTACH/FROM '<url>') is not run; a URL used as a
value (an AAS semantic id) is fine.

Setup blocks are matched case-insensitively by every name they CREATE or INSERT INTO.

Function names: in a SKILL.md, every backticked identifier in the frontmatter description, and
every lower-case `name(` call outside a ```sql block, must be a registered function, unless it is
listed in scripts/doc_snippets/removed_names.txt (name<TAB>reason). SQL blocks are checked by
running them. With --skills-coverage (implied by a default scan), every function the extension
registers must be named in at least one SKILL.md (needs a plain `duckdb` on PATH, or
DUCKDB_BASELINE, to tell the extension's functions from DuckDB's own).

Expected output: lines `-- => a | b | c` in a block are the rows its last statement must print
(-list output, cells trimmed), in order; a mismatch is a FAIL. Use them for the headline numbers
a reader will quote; leave run-to-run-variable values (bootstrap bounds) unasserted.

Status per block: PASS, FAIL, EXPECTED-ERROR (keyed in expected_errors.txt by
"file<TAB>first SQL line"; such a block must fail), SYNOPSIS (keyed in synopsis.txt; a signature
or placeholder, not run), SKIP-NETWORK, SKIP-VERSION (first line `-- DuckDB 2.0+` on an older
binary). A default scan also reports STALE keys in the support files.

Writes a TSV (status, file, line, first SQL line, detail) to --out (default
build/doc-snippets.tsv). Exit 0 clean; 1 on any FAIL or STALE; 2 when the binary is missing.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = subprocess.run(
    ["git", "rev-parse", "--show-toplevel"], cwd=SCRIPT_DIR, capture_output=True, text=True, check=True
).stdout.strip()

SUPPORT_DIR = os.path.join(SCRIPT_DIR, "doc_snippets")
FIXTURES_FILE = os.path.join(SUPPORT_DIR, "fixtures.sql")
EXPECTED_ERRORS_FILE = os.path.join(SUPPORT_DIR, "expected_errors.txt")
SYNOPSIS_FILE = os.path.join(SUPPORT_DIR, "synopsis.txt")
REMOVED_NAMES_FILE = os.path.join(SUPPORT_DIR, "removed_names.txt")
# The built shell has the extension linked in. In CI, DUCKDB names a stock DuckDB CLI and
# ANOFOX_EXTENSION the .duckdb_extension artifact to load into it.
DUCKDB_BIN = os.environ.get("DUCKDB") or os.path.join(REPO_ROOT, "build", "release", "duckdb")
EXTENSION = os.environ.get("ANOFOX_EXTENSION")
SHELL_ARGS = [DUCKDB_BIN, "-unsigned"] + (["-cmd", f"LOAD '{EXTENSION}'"] if EXTENSION else [])
DEFAULT_OUT = os.path.join(REPO_ROOT, "build", "doc-snippets.tsv")

SKILLS_DIR = os.path.join("plugins", "anofox-similarity", "skills")
# docs/ is not in the default scan: most of its blocks run against the reader's own tables
# (bom_items, tree_nodes, ...) or are signatures. Pass docs files explicitly to check them.
DEFAULT_SCAN_DIRS = [SKILLS_DIR]

FENCE_RE = re.compile(r"^```[ \t]*([A-Za-z0-9_+-]*)[ \t]*$")
SETUP_RE = re.compile(
    r"\b(CREATE\s+(OR\s+REPLACE\s+)?(TEMP\s+|TEMPORARY\s+)?(TABLE|VIEW|MACRO)|INSERT)\b",
    re.IGNORECASE,
)
CREATE_NAME_RE = re.compile(
    r"\bCREATE\s+(?:OR\s+REPLACE\s+)?(?:TEMP\s+|TEMPORARY\s+)?(?:TABLE|VIEW|MACRO)\s+"
    r"(?:IF\s+NOT\s+EXISTS\s+)?([A-Za-z_][A-Za-z0-9_]*)",
    re.IGNORECASE,
)
INSERT_NAME_RE = re.compile(r"\bINSERT\s+INTO\s+([A-Za-z_][A-Za-z0-9_]*)", re.IGNORECASE)
PLACEHOLDER_ELLIPSIS_RE = re.compile(r"(?<!\d)\.\.\.(?!\d)")
# INSTALL, or a URL read as a file (read_*/…_scan/ATTACH/FROM '<url>'). A URL as a value, such
# as an AAS semantic id 'https://admin-shell.io/...', is data and does not make a block networked.
NETWORK_RE = re.compile(
    r"^\s*(?:FORCE\s+)?INSTALL\b"
    r"|\b(?:read_\w+|\w*_scan|ATTACH|FROM)\s*\(?\s*\[?\s*'(?:https?|s3|gcs|r2|hf)://",
    re.IGNORECASE | re.MULTILINE)

BACKTICK_IDENT_RE = re.compile(r"`([a-z_][a-z0-9_]*)(?:\(\))?`")
CALL_IDENT_RE = re.compile(r"(?<![\w.])([a-z_][a-z0-9_]*)\(")
# lower-case words that read like calls in prose but are SQL syntax, not functions
NOT_FUNCTIONS = {"in", "values", "exists", "over", "filter", "using", "as", "and", "or", "not", "struct", "map", "list",
                 # placeholders in formulas and signatures inside text fences
                 "attr", "a", "p", "score", "req", "column"}


def read_text(abs_path):
    with open(abs_path, encoding="utf-8") as f:
        return f.read()


def git_show(ref, path):
    result = subprocess.run(
        ["git", "show", f"{ref}:{path}"], cwd=REPO_ROOT, capture_output=True, text=True
    )
    if result.returncode != 0:
        return None
    return result.stdout


def changed_md_files(ref):
    result = subprocess.run(
        ["git", "diff", "--name-only", ref, "--", "README.md"] + DEFAULT_SCAN_DIRS,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    files = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return [f for f in files if f.endswith(".md") and os.path.exists(os.path.join(REPO_ROOT, f))]


def default_scan_files():
    files = []
    if os.path.exists(os.path.join(REPO_ROOT, "README.md")):
        files.append("README.md")
    for scan_dir in DEFAULT_SCAN_DIRS:
        abs_dir = os.path.join(REPO_ROOT, scan_dir)
        if not os.path.isdir(abs_dir):
            continue
        for root, dirs, fnames in os.walk(abs_dir):
            dirs.sort()
            for fn in sorted(fnames):
                if fn.endswith(".md"):
                    files.append(os.path.relpath(os.path.join(root, fn), REPO_ROOT))
    return files


def extract_sql_blocks(text):
    """Return list of (block_text, start_line_idx) for ```sql fenced blocks."""
    if text is None:
        return []
    lines = text.splitlines()
    blocks = []
    i = 0
    n = len(lines)
    while i < n:
        m = FENCE_RE.match(lines[i].strip())
        if m and m.group(1).lower() == "sql":
            start = i
            body_lines = []
            i += 1
            while i < n and lines[i].strip() != "```":
                body_lines.append(lines[i])
                i += 1
            blocks.append(("\n".join(body_lines), start))
        i += 1
    return blocks


def first_non_comment_line(block_text):
    for line in block_text.splitlines():
        stripped = line.strip()
        if stripped and not stripped.startswith("--"):
            return stripped
    for line in block_text.splitlines():
        if line.strip():
            return line.strip()
    return ""


def load_key_set(path):
    if not os.path.exists(path):
        return set()
    with open(path) as f:
        return {line.rstrip("\n") for line in f if line.strip()}


def load_removed_names(path):
    """name<TAB>reason, one per line; blank lines and '#'-comment lines ignored."""
    names = {}
    if not os.path.exists(path):
        return names
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.strip().startswith("#"):
                continue
            parts = line.split("\t", 1)
            name = parts[0].strip()
            reason = parts[1].strip() if len(parts) > 1 else ""
            if name:
                names[name] = reason
    return names


def strip_sql_comments(text):
    return "\n".join(line.split("--", 1)[0] for line in text.splitlines())


def self_created_names(text):
    return {n for n in CREATE_NAME_RE.findall(text) if n}


def select_required_setup(target_text, setup_accum, fixture_names=frozenset()):
    """Transitive closure of the setup blocks (in doc order) whose created names the target
    mentions, excluding names the target creates itself and names fixtures.sql provides.
    Every block that registered a matched name is included (a CREATE and a later INSERT)."""
    shadowed = {n.lower() for n in self_created_names(strip_sql_comments(target_text)) | set(fixture_names)}
    needed_text = [strip_sql_comments(target_text)]
    included = []
    changed = True
    while changed:
        changed = False
        combined = "\n".join(needed_text)
        for name, block_text in setup_accum:
            if name in shadowed or any(bt is block_text for bt in included):
                continue
            if re.search(r"\b" + re.escape(name) + r"\b", combined, re.IGNORECASE):
                included.append(block_text)
                needed_text.append(strip_sql_comments(block_text))
                changed = True
    order = {id(bt): i for i, (_, bt) in enumerate(setup_accum)}
    included.sort(key=lambda bt: order.get(id(bt), 0))
    return included


def first_error_line(stderr, stdout):
    for stream in (stderr, stdout):
        for line in stream.splitlines():
            if line.strip():
                return line.strip()[:200]
    return ""


def run_block_safe(sql_text, timeout=120):
    """Execute sql_text against DUCKDB_BIN in :memory:, with a fresh temp dir as
    both cwd and HOME (removed afterward), autoinstall disabled."""
    tmp_dir = tempfile.mkdtemp(prefix="doc_snippets_")
    try:
        # relative paths like test/data/... resolve as from the repository root
        for shared in ("test", "examples"):
            os.symlink(os.path.join(REPO_ROOT, shared), os.path.join(tmp_dir, shared))
        env = dict(os.environ)
        if not EXTENSION:  # a loaded artifact may need the real extension cache (vss)
            env["HOME"] = tmp_dir
        env["DATAZOO_DISABLE_TELEMETRY"] = "1"
        env.pop("AI_AGENT", None)
        env.pop("CLAUDECODE", None)
        # the temporary HOME has no extension cache: point at the real one, so extensions that are
        # already installed (json, icu) autoload as they would for a user; nothing is downloaded
        real_cache = os.path.join(os.path.expanduser("~"), ".duckdb", "extensions")
        # (the dev build has autoload off; a stock DuckDB has it on and json built in)
        cache = f"SET extension_directory = '{real_cache}';\nSET autoload_known_extensions = true;\n" if os.path.isdir(real_cache) and not EXTENSION else ""
        full_sql = "SET autoinstall_known_extensions = false;\n" + cache + sql_text
        try:
            result = subprocess.run(
                SHELL_ARGS + ["-bail", "-list", "-noheader", ":memory:"],
                input=full_sql,
                cwd=tmp_dir,
                capture_output=True,
                text=True,
                timeout=timeout,
                env=env,
            )
            return result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            return 124, "", f"TIMEOUT after {timeout}s"
    finally:
        shutil.rmtree(tmp_dir, ignore_errors=True)


def query_registered_function_names():
    rc, out, err = run_block_safe(
        "SELECT DISTINCT function_name FROM duckdb_functions();\n"
    )
    if rc != 0:
        print(
            f"ERROR: failed to query duckdb_functions(): {(err.strip() or out.strip())[:500]}",
            file=sys.stderr,
        )
        sys.exit(2)
    return {line.strip() for line in out.splitlines() if line.strip()}


def query_duckdb_major():
    rc, out, err = run_block_safe("SELECT version();\n")
    m = re.search(r"v(\d+)\.", out or "")
    return int(m.group(1)) if rc == 0 and m else 0


V2_ONLY_RE = re.compile(r"\A\s*--\s*DuckDB 2\.0\+")
EXPECT_RE = re.compile(r"^\s*--\s*=>\s?(.*)$", re.MULTILINE)


def expected_rows(block_text):
    """`-- => a | b | c` lines: the rows the block's last statement must print, in order."""
    return [" | ".join(c.strip() for c in row.split("|")) for row in EXPECT_RE.findall(block_text)]


LAST_MARKER = "__anofox_last_statement__"
WRITES_REPO_RE = re.compile(r"\bTO\s+'(?:\./)?(?:test|examples)/", re.IGNORECASE)


def statement_ends(text):
    """Offsets just after each top-level `;`, skipping strings ('..', ".."), -- and /* */ comments."""
    ends, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in "'\"":
            j = i + 1
            while j < n:
                if text[j] == c:
                    if j + 1 < n and text[j + 1] == c:  # '' / "" escape
                        j += 2
                        continue
                    break
                j += 1
            i = j + 1
        elif text.startswith("--", i):
            nl = text.find("\n", i)
            i = n if nl < 0 else nl
        elif text.startswith("/*", i):
            close = text.find("*/", i + 2)
            i = n if close < 0 else close + 2
        else:
            if c == ";":
                ends.append(i + 1)
            i += 1
    return ends


def has_code(text):
    """True when text holds anything besides blanks and comments."""
    return bool(re.sub(r"--[^\n]*|/\*.*?\*/|\s+", "", text, flags=re.DOTALL))


def with_last_marker(block_text):
    """The block with `SELECT '<marker>';` before its last statement, so the rows after the
    marker are exactly what the last statement printed. An unterminated last statement counts
    (the shell runs it), and several statements may share a line."""
    ends = [e for e in statement_ends(block_text)]
    if ends and has_code(block_text[ends[-1]:]):
        cut = ends[-1]
    elif len(ends) >= 2:
        cut = ends[-2]
    else:
        cut = 0
    return block_text[:cut] + f"\nSELECT '{LAST_MARKER}';\n" + block_text[cut:]


def output_mismatch(block_text, stdout):
    want = expected_rows(block_text)
    if not want:
        return ""
    lines = [" | ".join(c.strip() for c in l.split("|")) for l in stdout.splitlines() if l.strip()]
    if LAST_MARKER not in lines:
        return f"expected {want} but the last statement did not run"
    got = lines[len(lines) - 1 - lines[::-1].index(LAST_MARKER) + 1:]
    if got == want:
        return ""
    return f"expected {want} got {got}"


def collect_name_mentions(rel_path, text):
    """(name, line_no) mentions in a SKILL.md that must be registered functions: backticked
    identifiers in the frontmatter description, and lower-case `name(` calls in the body outside
    ```sql blocks (those are checked by running them)."""
    if not rel_path.endswith("SKILL.md"):
        return []
    mentions = []
    lines = text.splitlines()
    frontmatter_end = 0
    if lines and lines[0].strip() == "---":
        for i in range(1, len(lines)):
            if lines[i].strip() == "---":
                frontmatter_end = i
                break
        for i in range(1, frontmatter_end):
            for m in BACKTICK_IDENT_RE.finditer(lines[i]):
                mentions.append((m.group(1), i + 1))
    fence = None  # None outside a fence, else the fence's language
    for i in range(frontmatter_end + 1 if frontmatter_end else 0, len(lines)):
        line = lines[i]
        if line.strip().startswith("```"):
            fence = None if fence is not None else line.strip()[3:].strip().lower()
            continue
        if fence == "sql":
            continue
        for m in CALL_IDENT_RE.finditer(line):
            if m.group(1) not in NOT_FUNCTIONS:
                mentions.append((m.group(1), i + 1))
    return mentions


def extension_function_names(registered):
    """The extension's own functions: the build's minus a plain DuckDB's (and minus vss, which
    the build links). None when there is no plain DuckDB to compare with."""
    baseline = os.environ.get("DUCKDB_BASELINE") or shutil.which("duckdb")
    if not baseline:
        return None
    env = dict(os.environ, DATAZOO_DISABLE_TELEMETRY="1")
    env.pop("AI_AGENT", None)
    env.pop("CLAUDECODE", None)
    out = subprocess.run([baseline, "-noheader", "-list", "-c", "SELECT DISTINCT function_name FROM duckdb_functions()"],
                         capture_output=True, text=True, env=env, check=True).stdout
    return {n for n in registered - set(out.splitlines()) if not n.startswith(("hnsw_", "pragma_hnsw_", "vss_"))}


def skill_coverage_rows(registered):
    ours = extension_function_names(registered)
    if ours is None:
        print("note: no plain DuckDB shell found (set DUCKDB_BASELINE); skipped the skills coverage check")
        return []
    text = ""
    for root, _, fnames in os.walk(os.path.join(REPO_ROOT, SKILLS_DIR)):
        for fn in fnames:
            if fn == "SKILL.md":
                text += read_text(os.path.join(root, fn))
    return [("FAIL", SKILLS_DIR, "", "", f"{name} is named in no SKILL.md")
            for name in sorted(ours) if not re.search(r"\b" + re.escape(name) + r"\b", text)]


def plugin_version_rows():
    """plugin.json's version must equal every SKILL.md's frontmatter version."""
    manifest = os.path.join(REPO_ROOT, "plugins", "anofox-similarity", ".claude-plugin", "plugin.json")
    if not os.path.exists(manifest):
        return []
    import json
    version = json.load(open(manifest))["version"]
    rows = []
    for root, _, fnames in os.walk(os.path.join(REPO_ROOT, SKILLS_DIR)):
        for fn in fnames:
            if fn != "SKILL.md":
                continue
            path = os.path.join(root, fn)
            m = re.search(r"^version:\s*(\S+)", read_text(path), re.MULTILINE)
            if not m or m.group(1) != version:
                rows.append(("FAIL", os.path.relpath(path, REPO_ROOT), "", "",
                             f"version {m.group(1) if m else '(none)'} != plugin.json {version}"))
    return rows


def main():
    args = sys.argv[1:]
    files_arg = []
    changed_since = None
    out_path = DEFAULT_OUT
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--changed-since":
            i += 1
            if i >= len(args):
                print("ERROR: --changed-since requires a REF argument", file=sys.stderr)
                return 1
            changed_since = args[i]
        elif a == "--out":
            i += 1
            if i >= len(args):
                print("ERROR: --out requires a PATH argument", file=sys.stderr)
                return 1
            out_path = args[i]
        elif a == "--skills-coverage":
            pass
        elif a.startswith("--"):
            print(f"ERROR: unknown option {a}", file=sys.stderr)
            return 1
        elif not os.path.exists(os.path.join(REPO_ROOT, a)):
            print(f"ERROR: no such file: {a}", file=sys.stderr)
            return 1
        else:
            files_arg.append(a)
        i += 1

    if not os.path.exists(DUCKDB_BIN):
        print(f"ERROR: duckdb binary not found at {DUCKDB_BIN} -- build it first (make release)", file=sys.stderr)
        return 2

    is_full_scan = not files_arg and not changed_since

    if files_arg:
        files = files_arg
    elif changed_since:
        files = changed_md_files(changed_since)
    else:
        files = default_scan_files()

    fixtures = ""
    if os.path.exists(FIXTURES_FILE):
        fixtures = read_text(FIXTURES_FILE)
    fixture_names = set(CREATE_NAME_RE.findall(strip_sql_comments(fixtures)))

    expected_errors = load_key_set(EXPECTED_ERRORS_FILE)
    synopsis_keys = load_key_set(SYNOPSIS_FILE)
    removed_names = load_removed_names(REMOVED_NAMES_FILE)
    registered_names = query_registered_function_names()
    duckdb_major = query_duckdb_major()

    rows = []  # (status, file, line, first_sql_line, detail)
    status_counts = {"PASS": 0, "FAIL": 0, "EXPECTED-ERROR": 0, "SYNOPSIS": 0, "SKIP-NETWORK": 0, "SKIP-VERSION": 0, "STALE": 0}

    all_block_keys = set()
    all_mentioned_names = set()
    seen_unknown = set()  # (file, name) already reported as FAIL

    for rel_path in files:
        abs_path = os.path.join(REPO_ROOT, rel_path)
        if not os.path.exists(abs_path):
            continue
        text = read_text(abs_path)

        # --- Function-name resolution (frontmatter + backtick/call-syntax bodies) ---
        for name, line_no in collect_name_mentions(rel_path, text):
            all_mentioned_names.add(name)
            if name in registered_names or name in removed_names:
                continue
            dedup_key = (rel_path, name)
            if dedup_key in seen_unknown:
                continue
            seen_unknown.add(dedup_key)
            rows.append(("FAIL", rel_path, str(line_no), "", f"unknown function name {name}"))
            status_counts["FAIL"] += 1

        # --- SQL fence execution ---
        blocks = extract_sql_blocks(text)
        setup_accum = []  # list of (created_name, block_text)

        for block_text, start_line in blocks:
            key = f"{rel_path}\t{first_non_comment_line(block_text)}"
            all_block_keys.add(key)
            is_placeholder = bool(PLACEHOLDER_ELLIPSIS_RE.search(block_text))
            is_setup_only = bool(SETUP_RE.search(block_text)) and not is_placeholder
            first_line_trunc = first_non_comment_line(block_text)[:100]

            if WRITES_REPO_RE.search(strip_sql_comments(block_text)):
                status = "FAIL"
                detail = "writes into test/ or examples/ (shared with the repository); write to the cwd"
            elif NETWORK_RE.search(strip_sql_comments(block_text)):
                status = "SKIP-NETWORK"
                detail = "network-looking block (INSTALL/URL), not executed"
            elif V2_ONLY_RE.search(block_text) and duckdb_major < 2:
                status = "SKIP-VERSION"
                detail = "DuckDB 2.0+ block, binary is older"
            elif key in synopsis_keys:
                status = "SYNOPSIS"
                detail = "narrative placeholder, not executed"
            else:
                required_setup = select_required_setup(block_text, setup_accum, fixture_names)
                full_sql = fixtures
                for setup_sql in required_setup:
                    full_sql += "\n" + setup_sql
                full_sql += "\n" + (with_last_marker(block_text) if expected_rows(block_text) else block_text)

                rc, stdout, stderr = run_block_safe(full_sql)
                raw_pass = rc == 0

                mismatch = output_mismatch(block_text, stdout) if raw_pass else ""
                if raw_pass:
                    if key in expected_errors:
                        status = "FAIL"
                        detail = "expected error but passed"
                    elif mismatch:
                        status = "FAIL"
                        detail = mismatch[:300]
                    else:
                        status = "PASS"
                        detail = ""
                else:
                    if key in expected_errors:
                        status = "EXPECTED-ERROR"
                        detail = ""
                    else:
                        status = "FAIL"
                        detail = first_error_line(stderr, stdout)

            rows.append((status, rel_path, str(start_line + 1), first_line_trunc, detail))
            status_counts[status] = status_counts.get(status, 0) + 1

            if is_setup_only:
                # every table/view/macro the block creates (comments ignored), so a later block
                # naming any of them gets this block prepended (once)
                code = strip_sql_comments(block_text)
                names = CREATE_NAME_RE.findall(code) + INSERT_NAME_RE.findall(code)
                for name in dict.fromkeys(names):
                    setup_accum.append((name.lower(), block_text))

    if is_full_scan or "--skills-coverage" in sys.argv:
        for row in skill_coverage_rows(registered_names) + plugin_version_rows():
            rows.append(row)
            status_counts["FAIL"] += 1

    # --- Stale support-file entries (default full scan only) ---
    if is_full_scan:
        for key in sorted(expected_errors):
            if key not in all_block_keys:
                file_part = key.split("\t", 1)[0]
                rows.append(("STALE", file_part, "", "", f"stale expected_errors.txt entry: {key}"))
                status_counts["STALE"] += 1
        for key in sorted(synopsis_keys):
            if key not in all_block_keys:
                file_part = key.split("\t", 1)[0]
                rows.append(("STALE", file_part, "", "", f"stale synopsis.txt entry: {key}"))
                status_counts["STALE"] += 1
        for name in sorted(removed_names):
            if name in registered_names:
                rows.append(("STALE", "scripts/doc_snippets/removed_names.txt", "", "", f"{name} is registered (no longer removed)"))
                status_counts["STALE"] += 1
            elif name not in all_mentioned_names:
                rows.append(("STALE", "scripts/doc_snippets/removed_names.txt", "", "", f"{name} is no longer mentioned anywhere"))
                status_counts["STALE"] += 1

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w") as f:
        for row in rows:
            f.write("\t".join(row) + "\n")

    p = status_counts.get("PASS", 0)
    fcount = status_counts.get("FAIL", 0)
    e = status_counts.get("EXPECTED-ERROR", 0)
    s = status_counts.get("SYNOPSIS", 0)
    n = status_counts.get("SKIP-NETWORK", 0)
    v = status_counts.get("SKIP-VERSION", 0)
    t = status_counts.get("STALE", 0)
    print(f"check_doc_snippets: {p} PASS, {fcount} FAIL, {e} EXPECTED-ERROR, {s} SYNOPSIS, {n} SKIP-NETWORK, {v} SKIP-VERSION, {t} STALE")

    return 1 if (fcount or t) else 0


if __name__ == "__main__":
    sys.exit(main())
