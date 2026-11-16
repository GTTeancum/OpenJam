"""Run with Python; fixtures and cleanup are confined to this repository."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("build_release", REPO / "tools/build_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseChecksums(unittest.TestCase):
    def test_archive_and_file_checksums_match_shipped_bytes(self):
        fixtures = (REPO / ".local/tests").resolve()
        self.assertTrue(fixtures.is_relative_to(REPO.resolve()))
        fixtures.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="checksums-", dir=fixtures) as temp:
            root = Path(temp).resolve()
            self.assertTrue(root.is_relative_to(fixtures))
            output = root / "release"
            (root / "metadata").mkdir()
            (root / "metadata/achievements.toml").write_text("# fixture\n")
            (root / "metadata/icons").mkdir()
            (root / "metadata/icons/private.png").write_bytes(b"extracted game art")
            exe = root / "game.exe"
            dll = root / "TracyClientrd.dll"
            exe.write_bytes(b"test executable payload")
            dll.write_bytes(bytes(range(256)) * 5000)  # crosses hash chunk boundary
            (root / "private-save").write_text("must not ship")
            with patch.multiple(release, OUT=output, PORT=root,
                                SHIPPED=[("game.exe", exe, "fixture"),
                                         ("TracyClientrd.dll", dll, "fixture")]):
                self.assertEqual(release.windows("checksum-test"), 0)
                name = "OpenJam-vchecksum-test-win64"
                folder = output / name
                manifest = (folder / "SHA256SUMS.txt").read_bytes()
                self.assertEqual(manifest, (output / (name + ".files.sha256")).read_bytes())
                listed = dict(line.split("  ", 1)[::-1]
                              for line in manifest.decode().splitlines())
                self.assertEqual(set(listed), {"game.exe", "TracyClientrd.dll",
                                              "README.txt", "metadata/achievements.toml"})
                archive = output / (name + ".zip")
                with zipfile.ZipFile(archive) as zf:
                    for relative, expected in listed.items():
                        self.assertEqual(hashlib.sha256(zf.read(name + "/" + relative)).hexdigest(), expected)
                    self.assertEqual(zf.read(name + "/SHA256SUMS.txt"), manifest)
                    self.assertEqual(len(zf.namelist()), len(listed) + 1)
                self.assertEqual((output / (name + ".sha256")).read_text().split()[0],
                                 hashlib.sha256(archive.read_bytes()).hexdigest())
                release.write_file_checksums(folder, name)
                self.assertEqual((folder / "SHA256SUMS.txt").read_bytes(), manifest)
                (folder / "TracyClientrd.dll").write_bytes(b"changed")
                self.assertNotEqual(release.sha256_file(folder / "TracyClientrd.dll"),
                                    listed["TracyClientrd.dll"])


if __name__ == "__main__":
    unittest.main()
