import hashlib
import tempfile
import unittest
from pathlib import Path

from tools import m5burner_post as publisher


class FakeResponse:
    def __init__(self, payload=None, status_code=200):
        self._payload = payload
        self.status_code = status_code

    def json(self):
        if isinstance(self._payload, Exception):
            raise self._payload
        return self._payload


class FakeSession:
    def __init__(self, responses=()):
        self.responses = list(responses)
        self.calls = []
        self.cookies = {"m5_auth_token": "test-token"}
        self.headers = {}
        self.hooks = {"response": []}

    def request(self, method, url, **kwargs):
        self.calls.append((method, url, kwargs))
        if not self.responses:
            raise AssertionError(f"unexpected request: {method} {url}")
        return self.responses.pop(0)


def release_fixture(directory: Path):
    artifact = directory / "hotspot-arcade-cardputer.full.bin"
    artifact.write_bytes(b"firmware fixture")
    digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
    (directory / "SHA256SUMS").write_text(
        f"{digest}  {artifact.name}\n", encoding="utf-8"
    )
    return artifact


class PublisherValidationTests(unittest.TestCase):
    def test_rejects_cleartext_api_url(self):
        with self.assertRaises(publisher.ValidationError):
            publisher.M5BurnerClient(
                "http://m5burner-api.m5stack.com", session=FakeSession()
            )

    def test_rejects_checksum_mismatch(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            release_fixture(directory)
            (directory / "SHA256SUMS").write_text(
                f"{'0' * 64}  hotspot-arcade-cardputer.full.bin\n",
                encoding="utf-8",
            )
            with self.assertRaises(publisher.ValidationError):
                publisher.validate_artifacts(directory, directory / "SHA256SUMS")

    def test_catalog_repository_must_match_release_owner(self):
        session = FakeSession(
            [
                FakeResponse(
                    [
                        {
                            "fid": next(iter(publisher.FIRMWARE_MAP)),
                            "github": "https://github.com/genkigenki/hotspot-arcade-cardputer",
                            "versions": [],
                        }
                    ]
                )
            ]
        )
        client = publisher.M5BurnerClient(session=session)
        with self.assertRaises(publisher.ValidationError):
            client.ensure_firmware_published(
                next(iter(publisher.FIRMWARE_MAP)),
                "v0.6.0",
                Path("unused.bin"),
                dry_run=True,
            )

    def test_dry_run_never_posts_or_puts(self):
        firmware_id = next(iter(publisher.FIRMWARE_MAP))
        session = FakeSession(
            [
                FakeResponse(
                    [
                        {
                            "fid": firmware_id,
                            "github": publisher.EXPECTED_REPOSITORY,
                            "versions": [],
                        }
                    ]
                )
            ]
        )
        client = publisher.M5BurnerClient(session=session)
        result = client.ensure_firmware_published(
            firmware_id, "v0.6.0", Path("unused.bin"), dry_run=True
        )
        self.assertEqual(result, "would upload and publish")
        self.assertEqual([call[0] for call in session.calls], ["GET"])
        self.assertEqual(session.calls[0][2]["timeout"], publisher.REQUEST_TIMEOUT)
        self.assertFalse(session.calls[0][2]["allow_redirects"])

    def test_authentication_http_failure_has_auth_exit_class(self):
        session = FakeSession([FakeResponse(status_code=401)])
        client = publisher.M5BurnerClient(session=session)
        with self.assertRaises(publisher.AuthenticationError):
            client.login("user", "wrong-password")

    def test_existing_upload_is_published_and_refetched(self):
        firmware_id = next(iter(publisher.FIRMWARE_MAP))
        unpublished = {
            "fid": firmware_id,
            "github": publisher.EXPECTED_REPOSITORY,
            "versions": [{"version": "v0.6.0", "file": "file-id", "published": 0}],
        }
        published = {
            "fid": firmware_id,
            "github": publisher.EXPECTED_REPOSITORY,
            "versions": [{"version": "v0.6.0", "file": "file-id", "published": 1}],
        }
        session = FakeSession(
            [FakeResponse([unpublished]), FakeResponse({"status": 1}), FakeResponse([published])]
        )
        client = publisher.M5BurnerClient(session=session)
        result = client.ensure_firmware_published(
            firmware_id, "v0.6.0", Path("unused.bin")
        )
        self.assertEqual(result, "published")
        self.assertEqual([call[0] for call in session.calls], ["GET", "PUT", "GET"])


class PublisherExitStatusTests(unittest.TestCase):
    def test_upload_failure_returns_nonzero(self):
        class FailingClient:
            def __init__(self, api_base_url, logger=None):
                del api_base_url, logger

            def login(self, username, password):
                del username, password

            def ensure_firmware_published(self, *args, **kwargs):
                del args, kwargs
                raise publisher.UploadError("simulated upload failure")

        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            release_fixture(directory)
            result = publisher.main(
                ["--tag", "v0.6.0", "--artifacts-dir", str(directory)],
                {"M5BURNER_USER": "user", "M5BURNER_PWD": "password"},
                FailingClient,
            )
        self.assertEqual(result, publisher.UploadError.exit_code)

    def test_success_and_already_published_return_zero(self):
        class SuccessfulClient:
            def __init__(self, api_base_url, logger=None):
                del api_base_url, logger

            def login(self, username, password):
                self.credentials_present = bool(username and password)

            def ensure_firmware_published(self, *args, **kwargs):
                del args, kwargs
                return "already published"

        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            release_fixture(directory)
            result = publisher.main(
                ["--tag", "v0.6.0", "--artifacts-dir", str(directory)],
                {"M5BURNER_USER": "user", "M5BURNER_PWD": "password"},
                SuccessfulClient,
            )
        self.assertEqual(result, 0)

    def test_missing_credentials_returns_validation_error(self):
        result = publisher.main(["--tag", "v0.6.0"], {})
        self.assertEqual(result, publisher.ValidationError.exit_code)

    def test_tag_must_match_checked_in_version(self):
        result = publisher.main(
            ["--tag", "v0.5.0"],
            {"M5BURNER_USER": "user", "M5BURNER_PWD": "password"},
        )
        self.assertEqual(result, publisher.ValidationError.exit_code)


if __name__ == "__main__":
    unittest.main()
