import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class RepositoryLayoutTests(unittest.TestCase):
    def test_live_runner_uses_tracked_fixture(self):
        runner = (ROOT / "scripts" / "run-live-tests.ps1").read_text(encoding="utf-8")
        self.assertIn("'tests\\fixtures\\crunk-demo.mmp'", runner)
        self.assertNotIn("'runtime\\test\\crunk-demo.mmp'", runner)

    def test_groundtruth_matches_project_fixture(self):
        fixtures = ROOT / "tests" / "fixtures"
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "groundtruth.json"
            subprocess.run(
                [sys.executable, str(ROOT / "scripts" / "parse_groundtruth.py"),
                 str(fixtures / "crunk-demo.mmp"), str(output)],
                check=True, capture_output=True, text=True,
            )
            expected = json.loads((fixtures / "crunk-groundtruth.json").read_text(encoding="utf-8"))
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), expected)

    def test_helper_sources_are_outside_generated_directories(self):
        for name in ("scripts/compare.py", "scripts/parse_groundtruth.py",
                     "scripts/capture.ps1", "sdk/tools/seh_probe.cpp"):
            with self.subTest(path=name):
                self.assertTrue((ROOT / name).is_file(), name)

    def test_gitignore_includes_release_files_and_excludes_private_artifacts(self):
        public = {
            "AGENTS.md", "README.md", "LICENSE", "requirements.txt",
            "start-lmms-with-bridge.bat", "docs/development.md",
            "scripts/lmms_mcp.py", "plugin/agentbridge/AgentBridge.cpp",
            "plugin/agentbridge/Makefile", "sdk/def/lmms.def",
            "sdk/include/QtCore/qconfig.h", "sdk/tools/gen_qfeatures.pl",
            "tests/fixtures/crunk-demo.mmp", "tests/fixtures/LICENSES.TXT",
            ".agents/skills/lmms-operation-instructions/SKILL.md",
            ".agents/skills/lmms-operation-instructions/agents/openai.yaml",
            ".agents/skills/lmms-operation-instructions/references/connection.md",
            ".github/workflows/checks.yml", "scripts/.env.example",
        }
        private = {
            "personal-notes.md", "conversation.txt", "---Prompt---.txt",
            "BACKUP-MANIFEST.json", "build/lmms-plugins/agentbridge.dll",
            "tools/downloads/source.zip", "third_party/lmms/private.txt",
            "runtime/backup/settings.xml", ".venv/pyvenv.cfg",
            ".venv-codex/pyvenv.cfg", ".devin/devingui.local.json",
            ".idea/workspace.xml", ".agents/local-settings.json",
            ".agents/skills/private-skill/SKILL.md", "sdk/lib/liblmms.a",
            "sdk/analysis/report.txt", "docs/verification-evidence.md",
            "scripts/.env", "scripts/.env.production", "scripts/private.key",
            "scripts/settings.local.json", "scripts/__pycache__/client.pyc",
            "plugin/agentbridge/AgentBridge.o", "plugin/agentbridge/debug.pdb",
        }
        with tempfile.TemporaryDirectory() as directory:
            checkout = Path(directory)
            subprocess.run(["git", "init", "--quiet", str(checkout)], check=True)
            (checkout / ".gitignore").write_bytes((ROOT / ".gitignore").read_bytes())
            for name in public | private:
                target = checkout / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("fixture", encoding="utf-8")
            visible = set(subprocess.check_output(
                ["git", "-C", str(checkout), "ls-files", "--others", "--exclude-standard"],
                text=True,
            ).splitlines())
            self.assertEqual(visible, public | {".gitignore"})


if __name__ == "__main__":
    unittest.main()
