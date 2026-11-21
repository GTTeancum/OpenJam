import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import modkit


@unittest.skipIf(os.name == "nt", "Linux guest VFS alias regression")
class ModAudioAliasTest(unittest.TestCase):
    def test_alias_is_a_guest_readable_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bank = root / "data/xenon/bigs/audio/aemsdata.ast"
            bank.parent.mkdir(parents=True)
            bank.write_bytes(b"stock test bank")

            modkit._stage_ps3_alias(root)

            alias = root / "data/ps3"
            self.assertTrue(alias.is_dir())
            self.assertFalse(alias.is_symlink())
            other = alias / "bigs/audio/aemsdata.ast"
            self.assertEqual(other.read_bytes(), bank.read_bytes())
            self.assertTrue(other.samefile(bank))

    def test_alias_can_fall_back_to_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bank = root / "data/xenon/audio/stock.bin"
            bank.parent.mkdir(parents=True)
            bank.write_bytes(b"stock test bank")
            with patch.object(modkit.os, "link", side_effect=OSError):
                modkit._stage_ps3_alias(root)
            alias_bank = root / "data/ps3/audio/stock.bin"
            self.assertEqual(alias_bank.read_bytes(), bank.read_bytes())


if __name__ == "__main__":
    unittest.main()
