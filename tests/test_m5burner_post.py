import contextlib
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse

from tools import m5burner_post as publisher


FIRMWARE_ID = next(iter(publisher.FIRMWARE_MAP))
FIRMWARE_NAME = publisher.FIRMWARE_MAP[FIRMWARE_ID]
RELEASE_VERSION = "0.6.0"
RELEASE_TAG = f"v{RELEASE_VERSION}"


class FakeResponse:
    def __init__(self, payload=None, status_code=200, headers=None):
        self._payload = payload
        self.status_code = status_code
        self.headers = headers or {}
        self.request = None
        self.closed = False

    def json(self):
        if isinstance(self._payload, Exception):
            raise self._payload
        return self._payload

    def close(self):
        self.closed = True


class FakeSession:
    def __init__(self, responses=(), *, token="test-token"):
        self.responses = list(responses)
        self.calls = []
        self.cookies = {"m5_auth_token": token} if token else {}
        self.headers = {}
        self.hooks = {"response": []}

    def request(self, method, url, **kwargs):
        self.calls.append((method, url, kwargs))
        if not self.responses:
            raise AssertionError(f"unexpected request: {method} {url}")
        result = self.responses.pop(0)
        if isinstance(result, Exception):
            raise result
        result.request = SimpleNamespace(method=method, url=url)
        for hook in self.hooks.get("response", []):
            hook(result)
        return result


def firmware_info(*, versions, repository=publisher.EXPECTED_REPOSITORY):
    return {
        "fid": FIRMWARE_ID,
        "github": repository,
        "name": "Hotspot Arcade",
        "description": "fixture",
        "category": "Games",
        "versions": versions,
    }


def write_checksums(directory: Path):
    files = [directory / FIRMWARE_NAME, directory / publisher.BUILD_MANIFEST_NAME]
    text = "".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n" for path in files
    )
    (directory / "SHA256SUMS").write_text(text, encoding="utf-8")


def release_fixture(directory: Path):
    artifact = directory / FIRMWARE_NAME
    artifact.write_bytes(b"firmware fixture")
    manifest = {
        "schemaVersion": 1,
        "version": RELEASE_VERSION,
        "tag": RELEASE_TAG,
        "repository": publisher.EXPECTED_REPOSITORY,
        "artifacts": [
            {
                "filename": artifact.name,
                "bytes": artifact.stat().st_size,
                "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(),
            }
        ],
    }
    manifest_path = directory / publisher.BUILD_MANIFEST_NAME
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    write_checksums(directory)
    return artifact, manifest_path


def rewrite_manifest(path: Path, update):
    manifest = json.loads(path.read_text(encoding="utf-8"))
    update(manifest)
    path.write_text(json.dumps(manifest), encoding="utf-8")
    write_checksums(path.parent)


class PublisherValidationTests(unittest.TestCase):
    def test_accepts_artifact_bound_to_manifest_and_checksums(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            artifact, manifest = release_fixture(directory)
            result = publisher.validate_artifacts(
                directory,
                directory / "SHA256SUMS",
                manifest,
                expected_version=RELEASE_VERSION,
            )
        self.assertEqual(result, {FIRMWARE_ID: artifact})

    def test_rejects_unsafe_api_urls_without_echoing_secrets(self):
        for value in (
            "http://m5burner-api.m5stack.com",
            "https://uiflow2.m5stack.com",
            "https://user:do-not-print@m5burner-api.m5stack.com",
            "https://m5burner-api.m5stack.com:444",
        ):
            with self.subTest(value=value):
                with self.assertRaises(publisher.ValidationError) as raised:
                    publisher.M5BurnerClient(value, session=FakeSession())
                self.assertNotIn("do-not-print", str(raised.exception))

    def test_rejects_checksum_mismatch(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            release_fixture(directory)
            sums = (directory / "SHA256SUMS").read_text(encoding="utf-8")
            actual = hashlib.sha256((directory / FIRMWARE_NAME).read_bytes()).hexdigest()
            (directory / "SHA256SUMS").write_text(
                sums.replace(actual, "0" * 64), encoding="utf-8"
            )
            with self.assertRaisesRegex(publisher.ValidationError, "checksum mismatch"):
                publisher.validate_artifacts(directory, directory / "SHA256SUMS")

    def test_rejects_unchecksummed_manifest(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            _, manifest = release_fixture(directory)
            manifest.write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(publisher.ValidationError, "manifest checksum"):
                publisher.validate_artifacts(directory, directory / "SHA256SUMS")

    def test_manifest_version_and_repository_are_release_bound(self):
        cases = (
            (lambda value: value.update(version="9.9.9"), "version/tag"),
            (lambda value: value.update(tag="v9.9.9"), "version/tag"),
            (
                lambda value: value.update(repository="https://github.com/attacker/fork"),
                "canonical release repository",
            ),
        )
        for update, message in cases:
            with self.subTest(message=message), tempfile.TemporaryDirectory() as temp:
                directory = Path(temp)
                _, manifest = release_fixture(directory)
                rewrite_manifest(manifest, update)
                with self.assertRaisesRegex(publisher.ValidationError, message):
                    publisher.validate_artifacts(
                        directory,
                        directory / "SHA256SUMS",
                        expected_version=RELEASE_VERSION,
                    )

    def test_manifest_size_hash_and_presence_are_artifact_bound(self):
        cases = (
            (
                lambda value: value["artifacts"][0].update(
                    bytes=value["artifacts"][0]["bytes"] + 1
                ),
                "size does not match",
            ),
            (
                lambda value: value["artifacts"][0].update(sha256="0" * 64),
                "hash does not match",
            ),
            (lambda value: value.update(artifacts=[]), "missing from the build manifest"),
        )
        for update, message in cases:
            with self.subTest(message=message), tempfile.TemporaryDirectory() as temp:
                directory = Path(temp)
                _, manifest = release_fixture(directory)
                rewrite_manifest(manifest, update)
                with self.assertRaisesRegex(publisher.ValidationError, message):
                    publisher.validate_artifacts(directory, directory / "SHA256SUMS")

    def test_catalog_repository_must_match_release_owner(self):
        session = FakeSession(
            [FakeResponse([firmware_info(versions=[], repository="https://github.com/other/fork")])]
        )
        client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
        with self.assertRaises(publisher.ValidationError):
            client.ensure_firmware_published(
                FIRMWARE_ID, RELEASE_TAG, Path("unused.bin"), dry_run=True
            )


class PublisherTransportTests(unittest.TestCase):
    def test_login_uses_api_origin_and_disables_redirects(self):
        session = FakeSession([FakeResponse({"status": 1})])
        client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
        client.login("user@example.com", "password")
        method, url, kwargs = session.calls[0]
        self.assertEqual((method, url), ("POST", publisher.LOGIN_URL))
        self.assertEqual(urlparse(url).scheme, "https")
        self.assertEqual(urlparse(url).netloc, publisher.API_HOST)
        self.assertNotIn("uiflow2", url)
        self.assertEqual(kwargs["json"], {"email": "user@example.com", "password": "password"})
        self.assertEqual(kwargs["timeout"], publisher.REQUEST_TIMEOUT)
        self.assertFalse(kwargs["allow_redirects"])
        self.assertEqual(session.headers["m5_auth_token"], "test-token")

    def test_cross_origin_request_is_rejected_before_session(self):
        session = FakeSession()
        client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
        with self.assertRaises(publisher.ValidationError):
            client._request("GET", "https://uiflow2.m5stack.com/api/admin/firmware")
        self.assertEqual(session.calls, [])

    def test_caller_cannot_enable_redirects(self):
        session = FakeSession()
        client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
        with self.assertRaisesRegex(publisher.ValidationError, "redirect following"):
            client._request(
                "GET",
                f"{publisher.API_BASE_URL}/api/admin/firmware",
                allow_redirects=True,
            )
        self.assertEqual(session.calls, [])

    def test_redirect_is_never_followed(self):
        session = FakeSession(
            [FakeResponse(status_code=302, headers={"Location": "https://evil.example/steal"})]
        )
        client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
        with self.assertRaises(publisher.AuthenticationError):
            client.login("user", "password")
        self.assertEqual(len(session.calls), 1)
        self.assertFalse(session.calls[0][2]["allow_redirects"])

    def test_safe_get_retries_retryable_statuses_only_with_bound(self):
        first = FakeResponse(status_code=503)
        second = FakeResponse(status_code=429)
        sleeps = []
        session = FakeSession(
            [first, second, FakeResponse([firmware_info(versions=[])])]
        )
        client = publisher.M5BurnerClient(session=session, sleeper=sleeps.append)
        self.assertEqual(client.get_firmware_info(FIRMWARE_ID)["fid"], FIRMWARE_ID)
        self.assertEqual([call[0] for call in session.calls], ["GET", "GET", "GET"])
        self.assertEqual(sleeps, list(publisher.GET_RETRY_BACKOFF_SECONDS))
        self.assertTrue(first.closed)
        self.assertTrue(second.closed)

    def test_get_transport_retries_are_bounded_and_secret_safe(self):
        secret = "SENTINEL-PASSWORD"
        session = FakeSession([RuntimeError(secret), RuntimeError(secret), RuntimeError(secret)])
        client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
        with self.assertRaises(publisher.TransportError) as raised:
            client.get_firmware_info(FIRMWARE_ID)
        self.assertEqual(len(session.calls), publisher.GET_MAX_ATTEMPTS)
        self.assertNotIn(secret, str(raised.exception))

    def test_get_does_not_retry_nontransient_status(self):
        session = FakeSession([FakeResponse(status_code=404), FakeResponse([])])
        client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
        with self.assertRaises(publisher.TransportError):
            client.get_firmware_info(FIRMWARE_ID)
        self.assertEqual(len(session.calls), 1)

    def test_post_and_put_are_never_retried(self):
        login_session = FakeSession([FakeResponse(status_code=503), FakeResponse({"status": 1})])
        login = publisher.M5BurnerClient(session=login_session, sleeper=lambda _: None)
        with self.assertRaises(publisher.AuthenticationError):
            login.login("user", "password")
        self.assertEqual(len(login_session.calls), 1)

        publish_session = FakeSession([FakeResponse(status_code=503), FakeResponse({"status": 1})])
        publish = publisher.M5BurnerClient(session=publish_session, sleeper=lambda _: None)
        with self.assertRaises(publisher.TransportError):
            publish.publish_file(FIRMWARE_ID, "file-id")
        self.assertEqual(len(publish_session.calls), 1)

    def test_request_log_omits_query_credentials_and_bodies(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "requests.log"
            logger = publisher.RequestLogger(str(path))
            try:
                response = FakeResponse(status_code=200)
                response.request = SimpleNamespace(
                    method="GET",
                    url=f"{publisher.API_BASE_URL}/api/admin/firmware?token=DO-NOT-LOG",
                )
                logger._on_response(response)
            finally:
                logger.close()
            content = path.read_text(encoding="utf-8")
        self.assertNotIn("DO-NOT-LOG", content)
        self.assertNotIn("?", content)


class PublisherStateTests(unittest.TestCase):
    def test_dry_run_never_posts_or_puts(self):
        session = FakeSession([FakeResponse([firmware_info(versions=[])])])
        client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
        result = client.ensure_firmware_published(
            FIRMWARE_ID, RELEASE_TAG, Path("unused.bin"), dry_run=True
        )
        self.assertEqual(result, "would upload and publish")
        self.assertEqual([call[0] for call in session.calls], ["GET"])

    def test_existing_upload_is_published_and_affirmatively_refetched(self):
        unpublished = firmware_info(
            versions=[{"version": RELEASE_TAG, "file": "file-id", "published": 0}]
        )
        published = firmware_info(
            versions=[{"version": RELEASE_TAG, "file": "file-id", "published": 1}]
        )
        session = FakeSession(
            [FakeResponse([unpublished]), FakeResponse({"status": 1}), FakeResponse([published])]
        )
        client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
        result = client.ensure_firmware_published(
            FIRMWARE_ID, RELEASE_TAG, Path("unused.bin")
        )
        self.assertEqual(result, "published")
        self.assertEqual([call[0] for call in session.calls], ["GET", "PUT", "GET"])
        self.assertTrue(
            all(urlparse(call[1]).netloc == publisher.API_HOST for call in session.calls)
        )

    def test_missing_publication_state_is_failure_before_mutation(self):
        missing = firmware_info(versions=[{"version": RELEASE_TAG, "file": "file-id"}])
        session = FakeSession([FakeResponse([missing])])
        client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
        with self.assertRaisesRegex(publisher.PublishError, "omitted"):
            client.ensure_firmware_published(FIRMWARE_ID, RELEASE_TAG, Path("unused.bin"))
        self.assertEqual([call[0] for call in session.calls], ["GET"])

    def test_missing_publication_state_after_put_is_failure(self):
        unpublished = firmware_info(
            versions=[{"version": RELEASE_TAG, "file": "file-id", "published": 0}]
        )
        missing = firmware_info(versions=[{"version": RELEASE_TAG, "file": "file-id"}])
        session = FakeSession(
            [FakeResponse([unpublished]), FakeResponse({"status": 1}), FakeResponse([missing])]
        )
        client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
        with self.assertRaisesRegex(publisher.PublishError, "omitted"):
            client.ensure_firmware_published(FIRMWARE_ID, RELEASE_TAG, Path("unused.bin"))

    def test_conflicting_publication_fields_are_failure(self):
        conflicting = firmware_info(
            versions=[
                {
                    "version": RELEASE_TAG,
                    "file": "file-id",
                    "published": 1,
                    "is_publish": 0,
                }
            ]
        )
        session = FakeSession([FakeResponse([conflicting])])
        client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
        with self.assertRaisesRegex(publisher.PublishError, "conflicting"):
            client.ensure_firmware_published(FIRMWARE_ID, RELEASE_TAG, Path("unused.bin"))

    def test_non_integral_publication_state_is_failure(self):
        malformed = firmware_info(
            versions=[{"version": RELEASE_TAG, "file": "file-id", "published": 1.0}]
        )
        session = FakeSession([FakeResponse([malformed])])
        client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
        with self.assertRaisesRegex(publisher.PublishError, "invalid"):
            client.ensure_firmware_published(FIRMWARE_ID, RELEASE_TAG, Path("unused.bin"))

    def test_untrusted_file_identifier_cannot_change_request_path(self):
        malicious = firmware_info(
            versions=[
                {
                    "version": RELEASE_TAG,
                    "file": "file-id/../../other\nforged-log",
                    "published": 0,
                }
            ]
        )
        session = FakeSession([FakeResponse([malicious])])
        client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
        with self.assertRaisesRegex(publisher.PublishError, "invalid file identifier"):
            client.ensure_firmware_published(FIRMWARE_ID, RELEASE_TAG, Path("unused.bin"))
        self.assertEqual([call[0] for call in session.calls], ["GET"])

    def test_upload_then_publish_uses_api_origin_for_every_request(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / FIRMWARE_NAME
            binary.write_bytes(b"firmware")
            unpublished = firmware_info(
                versions=[{"version": RELEASE_TAG, "file": "new-file", "published": 0}]
            )
            published = firmware_info(
                versions=[{"version": RELEASE_TAG, "file": "new-file", "published": 1}]
            )
            session = FakeSession(
                [
                    FakeResponse([firmware_info(versions=[])]),
                    FakeResponse([firmware_info(versions=[])]),
                    FakeResponse({"status": 1}),
                    FakeResponse([unpublished]),
                    FakeResponse({"status": 1}),
                    FakeResponse([published]),
                ]
            )
            client = publisher.M5BurnerClient(session=session, sleeper=lambda _: None)
            result = client.ensure_firmware_published(FIRMWARE_ID, RELEASE_TAG, binary)
        self.assertEqual(result, "published")
        self.assertEqual(
            [call[0] for call in session.calls],
            ["GET", "GET", "POST", "GET", "PUT", "GET"],
        )
        self.assertTrue(
            all(
                urlparse(url).scheme == "https"
                and urlparse(url).netloc == publisher.API_HOST
                and "uiflow2" not in url
                and kwargs["allow_redirects"] is False
                for _, url, kwargs in session.calls
            )
        )


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
                ["--tag", RELEASE_TAG, "--artifacts-dir", str(directory)],
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
                ["--tag", RELEASE_TAG, "--artifacts-dir", str(directory)],
                {"M5BURNER_USER": "user", "M5BURNER_PWD": "password"},
                SuccessfulClient,
            )
        self.assertEqual(result, 0)

    def test_unexpected_failure_does_not_print_exception_secret(self):
        secret = "SENTINEL-PASSWORD"

        class ExplodingClient:
            def __init__(self, api_base_url, logger=None):
                del api_base_url, logger

            def login(self, username, password):
                del username, password

            def ensure_firmware_published(self, *args, **kwargs):
                del args, kwargs
                raise RuntimeError(secret)

        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            release_fixture(directory)
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                result = publisher.main(
                    ["--tag", RELEASE_TAG, "--artifacts-dir", str(directory)],
                    {"M5BURNER_USER": "user", "M5BURNER_PWD": secret},
                    ExplodingClient,
                )
        self.assertEqual(result, publisher.PublishError.exit_code)
        self.assertNotIn(secret, stderr.getvalue())

    def test_unexpected_login_failure_does_not_print_exception_secret(self):
        secret = "SENTINEL-LOGIN-PASSWORD"

        class ExplodingLoginClient:
            def __init__(self, api_base_url, logger=None):
                del api_base_url, logger

            def login(self, username, password):
                del username, password
                raise RuntimeError(secret)

        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            release_fixture(directory)
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                result = publisher.main(
                    ["--tag", RELEASE_TAG, "--artifacts-dir", str(directory)],
                    {"M5BURNER_USER": "user", "M5BURNER_PWD": secret},
                    ExplodingLoginClient,
                )
        self.assertEqual(result, publisher.PublishError.exit_code)
        self.assertNotIn(secret, stderr.getvalue())

    def test_missing_credentials_returns_validation_error(self):
        result = publisher.main(["--tag", RELEASE_TAG], {})
        self.assertEqual(result, publisher.ValidationError.exit_code)

    def test_tag_must_match_checked_in_version(self):
        result = publisher.main(
            ["--tag", "v0.5.0"],
            {"M5BURNER_USER": "user", "M5BURNER_PWD": "password"},
        )
        self.assertEqual(result, publisher.ValidationError.exit_code)


if __name__ == "__main__":
    unittest.main()
