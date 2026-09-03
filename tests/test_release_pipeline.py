import hashlib
import importlib.util
import io
import json
import struct
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "release_integrity", ROOT / "packaging" / "release_integrity.py"
)
integrity = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(integrity)


def metadata(path: Path) -> dict:
    return {
        "file": path.name,
        "size": path.stat().st_size,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
    }


class ReleasePipelineTests(unittest.TestCase):
    commit = "1234567890ab" + "c" * 28

    def application(self) -> bytes:
        version = (ROOT / "VERSION").read_text().strip()
        descriptor = (
            f"version={version};project=tiny_touch_unified;build={self.commit[:12]};"
            "board=rp2040-zero;protocol=6;"
        ).encode()
        payload = bytearray(b"\xff" * 300)
        payload[16:16 + len(integrity.DESCRIPTOR_MAGIC) + len(descriptor) + 1] = (
            integrity.DESCRIPTOR_MAGIC + descriptor + b"\0"
        )
        return bytes(payload)

    def make_app(self, path: Path) -> None:
        path.write_bytes(self.application() + b"s" * integrity.SIGNATURE_BYTES)

    def make_uf2(self, path: Path) -> None:
        application = self.application()
        blocks = []
        count = (len(application) + 255) // 256
        for number in range(count):
            chunk = application[number * 256:(number + 1) * 256].ljust(256, b"\0")
            block = struct.pack(
                "<8I", integrity.UF2_MAGIC_START0, integrity.UF2_MAGIC_START1, 0x2000,
                integrity.UF2_FLASH_BASE + number * 256, 256, number, count, 0xE48BFF56,
            ) + chunk.ljust(476, b"\0") + struct.pack("<I", integrity.UF2_MAGIC_END)
            blocks.append(block)
        path.write_bytes(b"".join(blocks))

    def make_cli(self, path: Path) -> None:
        with tarfile.open(path, "w:gz") as archive:
            for name, value in (
                ("tinytouch/tinytouch", b"executable"),
                ("tinytouch/_internal/runtime", b"runtime"),
            ):
                info = tarfile.TarInfo(name)
                info.size = len(value)
                info.mode = 0o755
                archive.addfile(info, io.BytesIO(value))

    def make_release(self, root: Path) -> None:
        version = (ROOT / "VERSION").read_text().strip()
        layouts = {}
        for kind, images in integrity.EXPECTED_IMAGES.items():
            directory = root / kind
            directory.mkdir(parents=True)
            entries = []
            for address, name in images.items():
                path = directory / name
                self.make_uf2(path)
                entries.append({"name": name, "address": address, **metadata(path)})
            layouts[kind] = {
                "version": version,
                "protocol": integrity.PROTOCOL,
                "secureVersion": integrity.SECURE_VERSION,
                "board": integrity.BOARDS[0],
                "images": entries,
            }
            (directory / "manifest.json").write_text(json.dumps(layouts[kind]))
        self.make_app(root / "tiny_touch_unified.bin")
        cli = {}
        for key, name in (
            ("macos-arm64", "tinytouch-macos-arm64.tar.gz"),
        ):
            path = root / name
            self.make_cli(path)
            cli[key] = {**metadata(path), "format": "tar.gz"}
        manifest = {
            "version": version,
            "build": self.commit[:12],
            "protocol": integrity.PROTOCOL,
            "secureVersion": integrity.SECURE_VERSION,
            "boards": integrity.BOARDS,
            "firmware": layouts,
            "ota": metadata(root / "tiny_touch_unified.bin"),
            "cli": cli,
        }
        (root / "release-manifest.json").write_text(json.dumps(manifest))

    def test_finalizer_produces_complete_flat_release(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            release = root / "release"
            release.mkdir()
            self.make_release(release)
            output = root / "publish"
            subprocess.run(
                [
                    "python3", str(ROOT / "packaging" / "finalize-release.py"),
                    str(release), "--output", str(output), "--commit", self.commit,
                ],
                check=True,
            )
            integrity.validate_release(output, self.commit, flat=True)
            integrity.validate_checksums(output)
            self.assertTrue((output / "tiny_touch_unified.uf2").is_file())
            self.assertTrue((output / "tiny_touch_unified.bin").is_file())
            self.assertFalse((output / "ota_data_initial.bin").exists())
            self.assertFalse((output / "tinytouch-web-flashers.tar.gz").exists())

            public = root / "public"
            public.mkdir()
            subprocess.run(
                [
                    "python3", str(ROOT / "packaging" / "sync-docs-release.py"),
                    str(output), str(public), "--commit", self.commit,
                ],
                check=True,
            )
            release_manifest = json.loads((output / "release-manifest.json").read_text())
            self.assertEqual(
                json.loads((public / "flash" / "factory" / "manifest.json").read_text()),
                release_manifest["firmware"]["factory"],
            )
            self.assertEqual(
                json.loads((public / "release.json").read_text()), release_manifest
            )
            self.assertTrue((public / "flash" / "recovery" / "manifest.json").is_file())
            self.assertEqual(
                json.loads((public / "flash" / "recovery" / "manifest.json").read_text()),
                release_manifest["firmware"]["factory"],
            )
            self.assertTrue((public / "cli" / "tinytouch-macos-arm64.tar.gz").is_file())
            (output / "unexpected.bin").write_bytes(b"unexpected")
            with self.assertRaisesRegex(integrity.IntegrityError, "published asset set mismatch"):
                integrity.validate_release(output, self.commit, flat=True)

    def test_descriptor_version_mismatch_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_release(root)
            path = root / "tiny_touch_unified.bin"
            data = path.read_bytes().replace(b"version=", b"version=stale-")
            path.write_bytes(data)
            manifest_path = root / "release-manifest.json"
            manifest = json.loads(manifest_path.read_text())
            manifest["ota"].update(metadata(path))
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(integrity.IntegrityError, "embedded version mismatch"):
                integrity.validate_release(root, self.commit)

    def test_factory_uf2_must_match_the_ota_application(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_release(root)
            path = root / "tiny_touch_unified.bin"
            data = bytearray(path.read_bytes())
            data[0] ^= 0xFF
            path.write_bytes(data)
            manifest_path = root / "release-manifest.json"
            manifest = json.loads(manifest_path.read_text())
            manifest["ota"].update(metadata(path))
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(integrity.IntegrityError, "factory UF2 differs"):
                integrity.validate_release(root, self.commit)

    def test_candidate_extraction_rejects_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive_path = root / "candidate.tar.gz"
            with tarfile.open(archive_path, "w:gz") as archive:
                info = tarfile.TarInfo("publish/link")
                info.type = tarfile.SYMTYPE
                info.linkname = "../../outside"
                archive.addfile(info)
            with self.assertRaisesRegex(integrity.IntegrityError, "links are not allowed"):
                integrity.safe_extract(archive_path, root / "output")

    def test_tag_workflow_promotes_without_rebuilding(self):
        workflow = (ROOT / ".github" / "workflows" / "release.yml").read_text()
        self.assertNotIn("idf.py", workflow)
        self.assertNotIn("build-standalone-macos", workflow)
        self.assertNotIn("--clobber", workflow)
        self.assertIn('workflows: ["Release candidate"]', workflow)
        self.assertIn("AUTOMATIC_COMMIT", workflow)
        self.assertIn("AUTOMATIC_RUN_ID", workflow)
        self.assertIn("Create automatic release tag", workflow)
        self.assertIn("Version $release_tag is already published and active", workflow)
        self.assertGreaterEqual(workflow.count("git/ref/heads/$RELEASE_BRANCH"), 1)
        self.assertIn('test "$tag_type" = tag', workflow)
        self.assertIn('if [[ "$tag_sha" = "$release_commit" ]]', workflow)
        self.assertIn("already published and active", workflow)
        self.assertIn("Confirm automatic candidate is still current", workflow)
        self.assertIn("--signer-workflow", workflow)
        self.assertIn("--source-digest", workflow)
        self.assertNotIn("Activate verified CLI update channel", workflow)
        self.assertIn("group: release-promotion", workflow)
        self.assertIn("Verify published GitHub release", workflow)
        self.assertNotIn("Commit verified docs release assets", workflow)
        self.assertNotIn("alpacaengineer/dispatches", workflow)
        self.assertNotIn("PUBLIC_SITE_ORIGIN", workflow)
        self.assertNotIn("base=https://alpacaengineer.ing/tinytouch", workflow)
        self.assertIn("releases/latest/download", workflow)
        self.assertNotIn("packaging/sync-docs-release.py", workflow)
        self.assertIn("sha256sum --check --strict", workflow)
        self.assertIn("--json tagName,isDraft", workflow)
        self.assertNotIn("releases/tags/$GITHUB_REF_NAME", workflow)
        self.assertIn("release immutability is a configured server-side prerequisite", workflow)
        self.assertNotIn("TINYTOUCH_RELEASE_ADMIN_TOKEN", workflow)
        self.assertIn("CANDIDATE_WAIT_SECONDS", workflow)
        self.assertIn("release_state=published", workflow)
        self.assertIn('git rev-parse "$release_tag^{commit}"', workflow)
        self.assertNotIn("release_target", workflow)
        candidate = (ROOT / ".github" / "workflows" / "release-candidate.yml").read_text()
        self.assertIn("paths-ignore:", candidate)
        self.assertIn("channels/**", candidate)
        self.assertIn("docs/**", candidate)
        self.assertNotIn("docs/public/release.json", candidate)
        self.assertNotIn("docs/public/flash/factory/**", candidate)
        self.assertNotIn("workflow_dispatch:", candidate)
        self.assertIn('branches: [main, "beta/**"]', candidate)
        self.assertIn("group: release-candidate-${{ github.ref }}", candidate)
        self.assertIn("cancel-in-progress: true", candidate)
        self.assertIn('refs/heads/beta/*', candidate)
        self.assertNotIn("tinytouch-web-flashers.tar.gz", workflow)
        self.assertNotIn("web/flash", workflow)
        docs_workflow = (ROOT / ".github" / "workflows" / "docs.yml").read_text()
        self.assertIn("group: production-documentation", docs_workflow)
        self.assertIn("environment: release-publishing", docs_workflow)
        self.assertIn("api/github-release?file=tiny_touch_unified.bin", docs_workflow)
        candidate_workflow = (ROOT / ".github" / "workflows" / "release-candidate.yml").read_text()
        self.assertNotIn("build-recovery", candidate_workflow)
        self.assertNotIn("--recovery-build", candidate_workflow)

        build_script = (ROOT / "packaging" / "build-standalone-macos.sh").read_text()
        self.assertIn("--require-hashes", build_script)
        self.assertIn("--no-build-isolation", build_script)
        self.assertIn("requirements-bootstrap.txt", build_script)
        self.assertIn("requirements-release.txt", build_script)
        tag_script = (ROOT / "packaging" / "tag-release").read_text()
        self.assertIn("git diff --cached --quiet", tag_script)
        self.assertNotIn("git status --porcelain", tag_script)
        self.assertIn("Timed out after 600s", tag_script)
        self.assertLess(tag_script.index("release-candidate.yml"), tag_script.index("git tag -a"))
        self.assertIn("release.yml/runs", tag_script)
        self.assertIn("Release promotion ended with", tag_script)
        self.assertNotIn("\n  status=", tag_script)
        release_script = (ROOT / "packaging" / "release").read_text()
        self.assertIn('git push origin "$release_branch"', release_script)
        self.assertIn('beta/*', release_script)
        self.assertIn("GitHub Actions is handling the release", release_script)
        self.assertNotIn("tag-release", release_script)

    def test_browser_serves_the_verified_uf2_for_bootsel(self):
        source = (ROOT / "docs" / ".vitepress" / "theme" / "FlashTool.vue").read_text()
        self.assertIn("const UPDATE_PROTOCOL = 6", source)
        self.assertIn("function releaseAsset(file: string, tag?: string)", source)
        self.assertNotIn("/firmware/${image.file}", source)
        self.assertIn("<option value=\"beta\">Beta firmware</option>", source)
        self.assertIn("release.prerelease", source)
        self.assertIn("RPI-RP2", source)
        self.assertNotIn("esptool", source)
        self.assertNotIn("navigator.serial", source)
        proxy = (ROOT / "docs" / "api" / "github-release.js").read_text()
        self.assertIn("redirect: 'follow'", proxy)
        self.assertIn("RELEASE_ASSETS.has(file)", proxy)
        self.assertIn("'tiny_touch_unified.uf2'", proxy)
        self.assertNotIn("bootloader.bin", proxy)
        self.assertNotIn('"rewrites"', (ROOT / "docs" / "vercel.json").read_text())
        self.assertNotIn("/flash/recovery", source)


if __name__ == "__main__":
    unittest.main()
