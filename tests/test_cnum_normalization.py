import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class CnumNormalizationRegressionTests(unittest.TestCase):
    def test_cnum_normalizes_toa_91_prefix_without_rewriting_nanp(self):
        modem = (ROOT / "components/idf_modem/idf_modem.cpp").read_text(encoding="utf-8")

        self.assertIn("normalize_msisdn", modem)
        self.assertIn('digits.rfind("19", 0) == 0', modem)
        self.assertIn("had_plus && digits.size() == 11 && digits[0] == '1'", modem)
        self.assertIn('strcmp(cc, "1") == 0', modem)
        self.assertRegex(modem, r"parse_cnum_phone[\s\S]*return normalize_msisdn\(phone\)")


if __name__ == "__main__":
    unittest.main()
