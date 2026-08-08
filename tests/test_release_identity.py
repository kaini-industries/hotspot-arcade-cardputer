import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CANONICAL_REPOSITORY = "https://github.com/kaini-industries/hotspot-arcade-cardputer"
PINNED_ACTION = re.compile(r"^\s*- uses: [^\s@]+@[0-9a-f]{40}(?:\s+#.*)?$", re.MULTILINE)
UNPINNED_ACTION = re.compile(r"^\s*- uses: [^\s@]+@(?![0-9a-f]{40}(?:\s|$))", re.MULTILINE)


class ReleaseIdentityTests(unittest.TestCase):
    def test_version_and_catalog_identity_are_canonical(self):
        self.assertEqual((ROOT / "VERSION").read_text(encoding="utf-8").strip(), "0.6.0")
        metadata = json.loads((ROOT / "m5burner.json").read_text(encoding="utf-8"))
        self.assertEqual(metadata["version"], "0.6.0")
        self.assertEqual(metadata["author"], "kaini-industries")
        self.assertEqual(metadata["repository"], CANONICAL_REPOSITORY)

    def test_downstream_identity_preserves_both_attributions(self):
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        license_text = (ROOT / "LICENSE").read_text(encoding="utf-8")
        notices = (ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        self.assertIn(CANONICAL_REPOSITORY, readme)
        self.assertIn("genkigenki", readme)
        self.assertIn("tarikbc/hotspot-arcade", readme)
        self.assertIn("Kaini Industries", license_text)
        self.assertIn("genkigenki", notices)
        self.assertRegex(notices, r"Tarik\s+Caramanico")

    def test_workflows_are_read_only_and_publishing_is_manual_and_frozen(self):
        self.assertFalse((ROOT / ".github/workflows/build.yml").exists())
        workflows = sorted((ROOT / ".github/workflows").glob("*.yml"))
        self.assertEqual({path.name for path in workflows}, {"ci.yml", "release.yml"})
        for path in workflows:
            text = path.read_text(encoding="utf-8")
            self.assertIn("permissions:\n  contents: read", text)
            self.assertIn("persist-credentials: false", text)
            self.assertIsNone(UNPINNED_ACTION.search(text), path.name)
            self.assertIsNotNone(PINNED_ACTION.search(text), path.name)
            self.assertNotIn("secrets.", text)

        release = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertIn("workflow_dispatch:", release)
        self.assertNotRegex(release, r"(?m)^\s{2}(?:push|release):")
        self.assertIn("publishing frozen", release.lower())
        self.assertNotIn("M5BURNER", release)
        self.assertNotIn("gh release", release)


if __name__ == "__main__":
    unittest.main()
