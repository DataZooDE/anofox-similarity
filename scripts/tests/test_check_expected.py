"""check_expected.py: expected rows must be rows of the output."""
import importlib.util
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "check_expected.py"
spec = importlib.util.spec_from_file_location("check_expected", SCRIPT)
ce = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ce)


missing = ce.missing_rows


def test_single_digits_count():
    assert missing("-- Expected: 0 | 0\n", "40|88\n") == ["0 | 0"]
    assert missing("-- Expected: 40 | 88\n", "40|88\n") == []


def test_numbers_must_share_one_row():
    # 12 and 9 both appear, but not in one row
    assert missing("-- Expected: 12 | 9\n", "12|13\n9|10\n") == ["12 | 9"]


def test_prose_needs_each_number_somewhere():
    assert missing("-- Expected: 13 classes, 37 items\n", "13|37\n") == []
    assert missing("-- Expected: 13 classes, 37 items\n", "13|36\n") == ["13 classes, 37 items"]


def test_leading_label_and_trailing_columns_may_be_left_out():
    assert missing("-- Expected:\n--   surface | 0.6 | 2.0  | at_most   2 K too hot\n",
                   "surface max. 43 °C vs 45 °C|0.6|2.0|at_most\n") == []
    assert missing("-- Expected:\n--   1 | 0.0 | 0.4\n", "1|0.0|0.4|0.0324|400|0\n") == []


def test_explanations_and_box_output():
    assert missing("-- Expected:\n--   0.06 | 0.367   (6 x the overall error rate)\n",
                   "│           0.06 │               0.367 │\n") == []


def test_trailing_zeros_and_headers():
    assert missing("-- Expected:\n-- | k | mrr |\n-- |---|---|\n-- | 1 | 0.50 |\n", "1|0.5\n") == []
