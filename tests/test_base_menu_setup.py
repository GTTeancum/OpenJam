"""Regression tests for PC menu setup, using no real game assets."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
import frontend
import modkit


def synthetic_rewrite(sources, outputs, mods, active):
    for source, output in zip(sources, outputs):
        output.write_bytes(source.read_bytes() + b" / PC menu")
    return []


class BaseMenuSetup(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.game = Path(folder.name)
        (self.game / "default.xex").touch()
        for rel in frontend.asset_paths():
            path = self.game / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(rel.encode())
        (self.game / "ui").mkdir()
        for name in modkit.UI_FILES:
            (self.game / "ui" / name).touch()

    @patch.object(modkit.subprocess, "Popen")
    @patch.object(frontend, "rewrite", side_effect=synthetic_rewrite)
    def test_unpack_prepares_base_and_repeat_setup_does_not_rewrite(self, rewrite, popen):
        popen.return_value.poll.return_value = 0
        popen.return_value.returncode = 0
        self.assertTrue(modkit.unpack_container("game", "container", self.game,
                                               say=lambda _line: None))
        self.assertTrue(frontend.is_ready(self.game))
        self.assertEqual(rewrite.call_args.args[2:], ([], None))
        with patch.object(modkit, "stage_ui") as stage_ui:
            modkit.prepare_base_game(self.game, refresh_art=False)
            stage_ui.assert_not_called()
        rewrite.assert_called_once()
        for rel in frontend.asset_paths():
            self.assertEqual((self.game / rel).read_bytes(),
                             rel.encode() + b" / PC menu")
            self.assertEqual(frontend.original_path(self.game, rel).read_bytes(),
                             rel.encode())

    def test_menu_parse_failure_preserves_stock_and_can_retry(self):
        def failed_rewrite(_sources, outputs, _mods, _active):
            outputs[0].write_bytes(b"partial rewrite")
            raise ValueError("unrecognized panel")

        warnings = []
        with patch.object(frontend, "rewrite", side_effect=failed_rewrite):
            modkit.prepare_base_game(self.game, warnings.append)
        self.assertFalse(frontend.is_ready(self.game))
        self.assertIn("unrecognized panel", " ".join(warnings))
        for rel in frontend.asset_paths():
            self.assertEqual((self.game / rel).read_bytes(), rel.encode())
        with patch.object(frontend, "rewrite", side_effect=synthetic_rewrite):
            modkit.prepare_base_game(self.game)
        self.assertTrue(frontend.is_ready(self.game))

    @patch.object(frontend, "rewrite", side_effect=synthetic_rewrite)
    def test_mod_keeps_its_override_and_uses_stock_for_inherited_assets(self, _rewrite):
        modkit.prepare_base_game(self.game)
        source = self.game / "mods" / "fixture-source"
        overridden = source / frontend.MENU.removeprefix("data/")
        overridden.parent.mkdir(parents=True)
        overridden.write_bytes(b"mod menu")
        with patch.object(modkit, "_convert", return_value=False):
            mod_id = modkit.install(self.game, source, name="Fixture",
                                    say=lambda _line: None)
        mod = self.game / "mods" / mod_id
        self.assertEqual((mod / frontend.MENU).read_bytes(), b"mod menu / PC menu")
        self.assertEqual(frontend.original_path(mod, frontend.MENU).read_bytes(),
                         b"mod menu")
        for rel in frontend.asset_paths():
            self.assertEqual((self.game / rel).read_bytes(),
                             rel.encode() + b" / PC menu")
            if rel != frontend.MENU:
                self.assertEqual((mod / rel).read_bytes(),
                                 rel.encode() + b" / PC menu")
                self.assertEqual(frontend.original_path(mod, rel).read_bytes(),
                                 rel.encode())


@unittest.skipUnless(os.name == "posix", "Linux shell launcher")
class LinuxLauncher(unittest.TestCase):
    def test_prepare_runs_after_unpack_and_on_later_launch(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            launcher = folder / "nbajam.sh"
            shutil.copy2(REPO / "res/linux/nbajam.sh", launcher)
            (folder / "1234567890abcdef").touch()
            stub = folder / "nbajam_ofe"
            stub.write_text('#!/bin/sh\nif [ "$1" = --unpack ]; then\n'
                            '  touch default.xex; mkdir data\n'
                            '  echo unpack >> steps\nelse\n'
                            '  echo launch >> steps\nfi\n', encoding="utf-8")
            stub.chmod(0o755)
            setup = folder / "setup"
            setup.mkdir()
            (setup / "modkit.py").write_text(
                'from pathlib import Path\n'
                'def prepare_base_game(game, say, *, refresh_art):\n'
                '    game = Path(game)\n'
                '    assert not refresh_art\n'
                '    assert (game / "default.xex").is_file()\n'
                '    assert (game / "data").is_dir()\n'
                '    with (game / "steps").open("a") as f:\n'
                '        f.write("prepare\\n")\n', encoding="utf-8")
            tools = folder / "bin"
            tools.mkdir()
            (tools / "python3").symlink_to(sys.executable)
            notify = tools / "notify-send"
            notify.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            notify.chmod(0o755)
            env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"])
            for _ in range(2):
                subprocess.run(["sh", str(launcher)], check=True, env=env,
                               capture_output=True, timeout=10)
            self.assertEqual((folder / "steps").read_text(encoding="utf-8").splitlines(),
                             ["unpack", "prepare", "launch", "prepare", "launch"])


if __name__ == "__main__":
    unittest.main()
