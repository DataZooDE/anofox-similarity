"""Pure-function checks of scripts/check_doc_snippets.py: the `-- =>` assertions compare exactly
the rows of a block's last statement, signatures in text fences are checked, and blocks that
write into the shared test/ or examples/ trees are rejected. No DuckDB shell is needed."""

import importlib.util
import re
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "check_doc_snippets.py"
spec = importlib.util.spec_from_file_location("check_doc_snippets", SCRIPT)
cds = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cds)

M = cds.LAST_MARKER


def test_marker_goes_before_the_last_statement():
    block = "CREATE TABLE t AS SELECT 1 AS a; -- setup\nSELECT a\nFROM t;\n-- => 1\n"
    marked = cds.with_last_marker(block)
    assert marked.index("AS a;") < marked.index(f"SELECT '{M}';") < marked.index("SELECT a\nFROM t;")


def test_single_statement_block_gets_the_marker_first():
    assert cds.with_last_marker("SELECT 1;\n-- => 1").strip().startswith(f"SELECT '{M}';")


def test_rows_before_the_marker_do_not_count():
    # an earlier statement printed the expected row; the last one printed nothing
    assert cds.output_mismatch("SELECT 1;\nSELECT 2 WHERE false;\n-- => 1", f"1\n{M}\n")


def test_extra_rows_fail():
    assert cds.output_mismatch("SELECT 1;\n-- => 1", f"{M}\n0\n1\n")


def test_exact_rows_pass():
    assert cds.output_mismatch("SELECT 1, 2;\n-- => 1 | 2", f"{M}\n1|2\n") == ""


def test_missing_marker_fails():
    assert "did not run" in cds.output_mismatch("SELECT 1;\n-- => 1", "1\n")


def test_text_fences_are_checked_sql_fences_are_not():
    text = "---\nname: x\n---\n```text\nfoo_bar(a)\n```\n```sql\nbaz_qux(1);\n```\n"
    names = [n for n, _ in cds.collect_name_mentions("skills/x/SKILL.md", text)]
    assert "foo_bar" in names and "baz_qux" not in names


def test_writes_into_the_repository_are_rejected():
    assert cds.WRITES_REPO_RE.search("COPY t TO 'test/data/x.csv';")
    assert cds.WRITES_REPO_RE.search("COPY t TO './examples/x.parquet' (FORMAT parquet);")
    assert not cds.WRITES_REPO_RE.search("COPY t TO 'out.csv';")
    assert not cds.WRITES_REPO_RE.search("SELECT * FROM 'test/data/x.csv';")


def test_unterminated_last_statement():
    # the shell runs `SELECT 2 WHERE false` although it has no `;`
    assert cds.output_mismatch("SELECT 1;\nSELECT 2 WHERE false\n-- => 1",
                               cds_run(cds.with_last_marker("SELECT 1;\nSELECT 2 WHERE false\n-- => 1")))


def test_two_statements_on_one_line():
    block = "SELECT 1; SELECT 2 WHERE false;\n-- => 1"
    assert cds.output_mismatch(block, cds_run(cds.with_last_marker(block)))


def test_semicolons_and_dashes_inside_strings_and_comments():
    block = "SELECT 'a;--b' AS x; -- note; here\n/* c; */ SELECT 'it''s; ok';\n-- => it's; ok"
    marked = cds.with_last_marker(block)
    assert marked.index(f"SELECT '{M}'") > marked.index("AS x;")
    assert marked.index(f"SELECT '{M}'") < marked.index("SELECT 'it''s; ok'")


def cds_run(marked):
    """Simulate the shell: print each SELECT's literal row, nothing for `WHERE false`."""
    out = []
    for stmt in re.split(r";\s*|\n(?=SELECT)", re.sub(r"--[^\n]*", "", marked)):
        stmt = stmt.strip()
        if not stmt or "WHERE false" in stmt:
            continue
        m = re.match(r"SELECT '?([^']*)'?$", stmt)
        if m:
            out.append(m.group(1))
    return "\n".join(out) + "\n"
