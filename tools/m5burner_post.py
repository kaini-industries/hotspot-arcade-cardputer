#!/usr/bin/env python3

"""Publish a validated Hotspot Arcade release to M5Burner.

Credentials are read from M5BURNER_USER and M5BURNER_PWD. The publisher is
deliberately strict: it accepts only the HTTPS M5Burner endpoint, verifies the
catalog repository identity, binds artifacts to checksums and the build manifest,
resumes incomplete releases, and exits non-zero unless publication is affirmed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import time
from pathlib import Path
from urllib.parse import urlparse


API_BASE_URL = "https://m5burner-api.m5stack.com"
API_HOST = "m5burner-api.m5stack.com"
LOGIN_URL = f"{API_BASE_URL}/api/v1/account/login"
EXPECTED_REPOSITORY = "https://github.com/kaini-industries/hotspot-arcade-cardputer"
REQUEST_TIMEOUT = (10, 60)
GET_MAX_ATTEMPTS = 3
GET_RETRY_STATUS = frozenset((408, 429, 500, 502, 503, 504))
GET_RETRY_BACKOFF_SECONDS = (0.25, 0.5)
TAG_RE = re.compile(
    r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?$"
)
VERSION_FILE = Path(__file__).resolve().parents[1] / "VERSION"
BUILD_MANIFEST_NAME = "build-manifest.json"
FILE_ID_RE = re.compile(r"^[0-9A-Za-z._-]{1,128}$")


# M5Burner catalog firmware ID -> release artifact filename.
FIRMWARE_MAP = {
    "4fe5fa2bc2acca1f172ae982c5ceb61d": "hotspot-arcade-cardputer.full.bin",
}


class PublisherError(Exception):
    exit_code = 2


class ValidationError(PublisherError):
    exit_code = 2


class AuthenticationError(PublisherError):
    exit_code = 3


class TransportError(PublisherError):
    exit_code = 4


class UploadError(PublisherError):
    exit_code = 5


class PublishError(PublisherError):
    exit_code = 6


def _validate_api_base_url(value: str) -> str:
    try:
        parsed = urlparse(value)
        port = parsed.port
    except (TypeError, ValueError) as exc:
        raise ValidationError(f"API base URL must be exactly {API_BASE_URL}") from exc
    if (
        parsed.scheme != "https"
        or parsed.hostname != API_HOST
        or parsed.username
        or parsed.password
        or port
        or parsed.query
        or parsed.fragment
        or parsed.path not in ("", "/")
        or parsed.netloc.lower() != API_HOST
    ):
        # Never echo an invalid URL: it may contain userinfo or another secret.
        raise ValidationError(f"API base URL must be exactly {API_BASE_URL}")
    return API_BASE_URL


def _validate_request_url(value: str) -> None:
    """Require the exact credential-bearing API origin before opening a socket."""

    try:
        parsed = urlparse(value)
        port = parsed.port
    except (TypeError, ValueError) as exc:
        raise ValidationError("request URL is outside the approved M5Burner API origin") from exc
    if (
        parsed.scheme != "https"
        or parsed.hostname != API_HOST
        or parsed.username
        or parsed.password
        or port
        or parsed.fragment
        or parsed.netloc.lower() != API_HOST
        or not parsed.path.startswith("/")
        or not value.startswith(f"{API_BASE_URL}/")
    ):
        raise ValidationError("request URL is outside the approved M5Burner API origin")


def _normalize_repository(value: str) -> str:
    value = (value or "").strip()
    if value.startswith("git@github.com:"):
        value = "https://github.com/" + value.removeprefix("git@github.com:")
    parsed = urlparse(value)
    if parsed.scheme not in ("http", "https") or parsed.hostname != "github.com":
        return value.rstrip("/").removesuffix(".git").lower()
    path = parsed.path.rstrip("/").removesuffix(".git")
    return f"https://github.com{path}".lower()


def _response_json(response, context: str):
    try:
        return response.json()
    except Exception as exc:
        raise TransportError(f"{context} returned invalid JSON") from exc


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_checksums(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise ValidationError(f"cannot read checksum file {path}: {exc}") from exc

    checksums: dict[str, str] = {}
    for line_number, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line:
            continue
        parts = line.split(maxsplit=1)
        if len(parts) != 2 or not re.fullmatch(r"[0-9a-fA-F]{64}", parts[0]):
            raise ValidationError(f"invalid SHA256SUMS entry at {path}:{line_number}")
        filename = parts[1].lstrip("*").strip()
        if not filename or Path(filename).name != filename:
            raise ValidationError(f"unsafe SHA256SUMS filename at {path}:{line_number}")
        if filename in checksums:
            raise ValidationError(f"duplicate SHA256SUMS entry for {filename}")
        checksums[filename] = parts[0].lower()
    return checksums


def _read_build_manifest(path: Path) -> dict:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValidationError(f"cannot read valid build manifest {path}") from exc
    if not isinstance(manifest, dict):
        raise ValidationError("build manifest root must be an object")
    return manifest


def _manifest_artifacts(manifest: dict) -> dict[str, dict]:
    raw_artifacts = manifest.get("artifacts")
    if not isinstance(raw_artifacts, list):
        raise ValidationError("build manifest artifacts must be a list")
    artifacts: dict[str, dict] = {}
    for entry in raw_artifacts:
        if not isinstance(entry, dict):
            raise ValidationError("build manifest contains a malformed artifact entry")
        filename = entry.get("filename")
        if not isinstance(filename, str) or not filename or Path(filename).name != filename:
            raise ValidationError("build manifest contains an unsafe artifact filename")
        if filename in artifacts:
            raise ValidationError(f"build manifest contains duplicate artifact {filename}")
        artifacts[filename] = entry
    return artifacts


def validate_artifacts(
    artifacts_dir: Path,
    checksums_path: Path,
    manifest_path: Path | None = None,
    *,
    expected_version: str | None = None,
) -> dict[str, Path]:
    if not artifacts_dir.is_dir():
        raise ValidationError(f"artifacts directory does not exist: {artifacts_dir}")

    manifest_path = manifest_path or artifacts_dir / BUILD_MANIFEST_NAME
    checksums = _read_checksums(checksums_path)
    manifest_checksum = checksums.get(manifest_path.name)
    if not manifest_checksum:
        raise ValidationError(f"{manifest_path.name} is missing from {checksums_path}")
    if not manifest_path.is_file() or _sha256(manifest_path) != manifest_checksum:
        raise ValidationError("build manifest checksum does not match SHA256SUMS")

    manifest = _read_build_manifest(manifest_path)
    if type(manifest.get("schemaVersion")) is not int or manifest["schemaVersion"] != 1:
        raise ValidationError("build manifest schemaVersion must be 1")
    if expected_version is None:
        try:
            expected_version = VERSION_FILE.read_text(encoding="utf-8").strip()
        except OSError as exc:
            raise ValidationError("cannot read release VERSION file") from exc
    if manifest.get("version") != expected_version or manifest.get("tag") != f"v{expected_version}":
        raise ValidationError("build manifest version/tag does not match the requested release")
    if manifest.get("repository") != EXPECTED_REPOSITORY:
        raise ValidationError("build manifest repository is not the canonical release repository")
    if not isinstance(manifest.get("commit"), str) or not re.fullmatch(
        r"[0-9a-f]{40}", manifest["commit"]
    ):
        raise ValidationError("build manifest source commit is not a full Git object ID")
    if manifest.get("sourceTreeClean") is not True:
        raise ValidationError("build manifest was not produced from a clean source checkout")
    if manifest.get("candidate") is not False:
        raise ValidationError("release publication refuses candidate build manifests")
    manifest_artifacts = _manifest_artifacts(manifest)

    artifacts: dict[str, Path] = {}
    for firmware_id, filename in FIRMWARE_MAP.items():
        path = artifacts_dir / filename
        if not path.is_file():
            raise ValidationError(f"required firmware artifact is missing: {path}")
        actual_size = path.stat().st_size
        if actual_size == 0:
            raise ValidationError(f"required firmware artifact is empty: {path}")
        expected = checksums.get(filename)
        if not expected:
            raise ValidationError(f"{filename} is missing from {checksums_path}")
        actual = _sha256(path)
        if actual != expected:
            raise ValidationError(
                f"checksum mismatch for {filename}: expected {expected}, got {actual}"
            )
        manifest_entry = manifest_artifacts.get(filename)
        if not manifest_entry:
            raise ValidationError(f"{filename} is missing from the build manifest")
        manifest_size = manifest_entry.get("bytes")
        manifest_sha = manifest_entry.get("sha256")
        if type(manifest_size) is not int or manifest_size <= 0:
            raise ValidationError(f"build manifest size is invalid for {filename}")
        if not isinstance(manifest_sha, str) or not re.fullmatch(r"[0-9a-f]{64}", manifest_sha):
            raise ValidationError(f"build manifest hash is invalid for {filename}")
        if manifest_size != actual_size:
            raise ValidationError(f"build manifest size does not match {filename}")
        if manifest_sha != actual or manifest_sha != expected:
            raise ValidationError(f"build manifest hash does not match {filename}")
        artifacts[firmware_id] = path
    return artifacts


class RequestLogger:
    """Log request metadata without headers, credentials, bodies, or cookies."""

    def __init__(self, output_path: str):
        self._file = open(output_path, "w", encoding="utf-8")
        self._counter = 0

    def attach(self, session) -> None:
        session.hooks.setdefault("response", []).append(self._on_response)

    def _on_response(self, response, *args, **kwargs) -> None:
        del args, kwargs
        self._counter += 1
        request = response.request
        parsed = urlparse(request.url)
        # The client validates the origin before sending. Reconstruct it from the
        # constant anyway so userinfo, query strings, and fragments can never be logged.
        safe_url = f"{API_BASE_URL}{parsed.path}"
        self._file.write(
            f"[{self._counter}] {request.method} {safe_url} -> {response.status_code}\n"
        )
        self._file.flush()

    def close(self) -> None:
        self._file.close()


class M5BurnerClient:
    def __init__(
        self, api_base_url: str = API_BASE_URL, logger=None, session=None, sleeper=None
    ):
        self.api_base_url = _validate_api_base_url(api_base_url)
        if session is None:
            try:
                import requests
            except ImportError as exc:
                raise ValidationError(
                    "the release publisher requires the locked requests dependency"
                ) from exc
            session = requests.Session()
        self.session = session
        self._sleep = sleeper or time.sleep
        if logger:
            logger.attach(self.session)

    def _request(
        self, method: str, url: str, *, expected=(200,), status_error=TransportError, **kwargs
    ):
        method = method.upper()
        _validate_request_url(url)
        if kwargs.pop("allow_redirects", False):
            raise ValidationError("redirect following is disabled for M5Burner requests")
        kwargs["timeout"] = REQUEST_TIMEOUT
        kwargs["allow_redirects"] = False
        attempts = GET_MAX_ATTEMPTS if method == "GET" else 1
        path = urlparse(url).path

        for attempt in range(attempts):
            try:
                response = self.session.request(method, url, **kwargs)
            except Exception as exc:
                if method == "GET" and attempt + 1 < attempts:
                    self._sleep(GET_RETRY_BACKOFF_SECONDS[attempt])
                    continue
                # Exception strings may contain serialized headers or request bodies.
                raise TransportError(
                    f"{method} {path} failed after {attempt + 1} attempt(s)"
                ) from exc

            if response.status_code in expected:
                return response
            if (
                method == "GET"
                and response.status_code in GET_RETRY_STATUS
                and attempt + 1 < attempts
            ):
                close = getattr(response, "close", None)
                if close:
                    close()
                self._sleep(GET_RETRY_BACKOFF_SECONDS[attempt])
                continue
            raise status_error(f"{method} {path} returned HTTP {response.status_code}")

        raise TransportError(f"{method} {path} exhausted its retry budget")

    def login(self, username: str, password: str) -> None:
        response = self._request(
            "POST",
            LOGIN_URL,
            json={"email": username, "password": password},
            status_error=AuthenticationError,
        )
        del response
        token = self.session.cookies.get("m5_auth_token")
        if not token:
            raise AuthenticationError("login succeeded but no authentication token was received")
        self.session.headers.update({"m5_auth_token": token})

    def get_firmware_info(self, firmware_id: str) -> dict:
        response = self._request("GET", f"{self.api_base_url}/api/admin/firmware")
        payload = _response_json(response, "firmware catalog request")
        if isinstance(payload, dict):
            payload = payload.get("data", payload.get("firmwares", []))
        if not isinstance(payload, list):
            raise TransportError("firmware catalog response is not a list")
        for firmware in payload:
            if isinstance(firmware, dict) and firmware.get("fid") == firmware_id:
                return firmware
        raise ValidationError(f"M5Burner firmware ID {firmware_id} was not found")

    @staticmethod
    def _validate_repository(firmware_info: dict) -> None:
        repository = firmware_info.get("github") or firmware_info.get("repository") or ""
        if _normalize_repository(repository) != _normalize_repository(EXPECTED_REPOSITORY):
            raise ValidationError("M5Burner catalog repository is not the canonical repository")

    @staticmethod
    def _matching_versions(firmware_info: dict, version: str) -> list[dict]:
        versions = firmware_info.get("versions") or firmware_info.get("version_list") or []
        if not isinstance(versions, list):
            raise TransportError("firmware version list is malformed")
        return [entry for entry in versions if isinstance(entry, dict) and entry.get("version") == version]

    @staticmethod
    def _published_state(version: dict) -> bool:
        states = []
        for key in ("published", "publish", "is_publish"):
            if key not in version:
                continue
            value = version[key]
            if (
                value is True
                or (type(value) is int and value == 1)
                or value in ("1", "published")
            ):
                states.append(True)
            elif (
                value is False
                or (type(value) is int and value == 0)
                or value in ("0", "unpublished")
            ):
                states.append(False)
            else:
                raise PublishError("M5Burner returned an invalid publication state")
        if not states:
            raise PublishError("M5Burner omitted the publication state")
        if any(state != states[0] for state in states[1:]):
            raise PublishError("M5Burner returned conflicting publication states")
        return states[0]

    def upload_firmware_version(self, firmware_id: str, version: str, binary_path: Path) -> None:
        firmware_info = self.get_firmware_info(firmware_id)
        self._validate_repository(firmware_info)
        data = {
            "name": firmware_info.get("name", ""),
            "description": firmware_info.get("description", ""),
            "category": firmware_info.get("category", ""),
            "author": "kaini-industries",
            "version": version,
            "github": EXPECTED_REPOSITORY,
            "cover": "null",
        }
        try:
            with binary_path.open("rb") as source:
                response = self._request(
                    "POST",
                    f"{self.api_base_url}/api/admin/firmware",
                    data=data,
                    files={"firmware": source},
                )
        except OSError as exc:
            raise UploadError(f"cannot open firmware artifact {binary_path}: {exc}") from exc
        try:
            _response_json(response, "firmware upload")
        except TransportError as exc:
            raise UploadError(str(exc)) from exc

    def publish_file(self, firmware_id: str, file_id: str) -> None:
        response = self._request(
            "PUT",
            f"{self.api_base_url}/api/admin/firmware/{firmware_id}/publish/{file_id}/1",
        )
        result = _response_json(response, "firmware publish")
        if not isinstance(result, dict) or result.get("status") != 1:
            raise PublishError(f"M5Burner did not confirm publication for {firmware_id}")

    def ensure_firmware_published(
        self, firmware_id: str, version: str, binary_path: Path, *, dry_run: bool = False
    ) -> str:
        firmware_info = self.get_firmware_info(firmware_id)
        self._validate_repository(firmware_info)
        matches = self._matching_versions(firmware_info, version)
        if len(matches) > 1:
            raise PublishError(f"multiple M5Burner entries exist for {firmware_id} version {version}")

        if not matches:
            if dry_run:
                return "would upload and publish"
            try:
                self.upload_firmware_version(firmware_id, version, binary_path)
            except PublisherError:
                raise
            except Exception as exc:
                raise UploadError(f"upload failed for {firmware_id}") from exc
            firmware_info = self.get_firmware_info(firmware_id)
            self._validate_repository(firmware_info)
            matches = self._matching_versions(firmware_info, version)
            if len(matches) != 1:
                raise UploadError(
                    f"upload completed but exactly one {version} entry was not found for {firmware_id}"
                )

        entry = matches[0]
        if self._published_state(entry):
            return "already published"
        file_id = entry.get("file")
        if not file_id:
            raise PublishError(f"version {version} for {firmware_id} has no file identifier")
        file_id = str(file_id)
        if not FILE_ID_RE.fullmatch(file_id):
            raise PublishError(f"version {version} for {firmware_id} has an invalid file identifier")
        if dry_run:
            return "would publish existing upload"
        try:
            self.publish_file(firmware_id, file_id)
        except PublisherError:
            raise
        except Exception as exc:
            raise PublishError(f"publish failed for {firmware_id}") from exc
        verified_info = self.get_firmware_info(firmware_id)
        self._validate_repository(verified_info)
        verified = self._matching_versions(verified_info, version)
        if len(verified) != 1 or str(verified[0].get("file", "")) != str(file_id):
            raise PublishError(
                f"M5Burner did not retain the expected {version} file after publication"
            )
        if not self._published_state(verified[0]):
            raise PublishError(f"M5Burner still reports {version} as unpublished")
        return "published"


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Upload and publish firmware to M5Burner")
    parser.add_argument("--tag", required=True, help="release tag, for example v0.6.0")
    parser.add_argument(
        "--api-base-url",
        default=API_BASE_URL,
        help=f"M5Burner API URL; only {API_BASE_URL} is accepted",
    )
    parser.add_argument(
        "--artifacts-dir",
        default="build",
        help="directory containing release binaries (default: build)",
    )
    parser.add_argument(
        "--checksums",
        help="SHA256SUMS path (default: <artifacts-dir>/SHA256SUMS)",
    )
    parser.add_argument(
        "--manifest",
        help=f"build manifest path (default: <artifacts-dir>/{BUILD_MANIFEST_NAME})",
    )
    parser.add_argument("--output", help="write sanitized request metadata to this file")
    parser.add_argument("--dry-run", action="store_true", help="validate and inspect without mutation")
    return parser


def main(argv=None, environ=None, client_factory=M5BurnerClient) -> int:
    args = _parser().parse_args(argv)
    environ = os.environ if environ is None else environ
    logger = None
    try:
        if not TAG_RE.fullmatch(args.tag):
            raise ValidationError(f"release tag is not valid semantic version syntax: {args.tag}")
        try:
            expected_tag = f"v{VERSION_FILE.read_text(encoding='utf-8').strip()}"
        except OSError as exc:
            raise ValidationError(f"cannot read release VERSION file: {exc}") from exc
        if args.tag != expected_tag:
            raise ValidationError(f"release tag {args.tag} does not match VERSION {expected_tag}")
        username = environ.get("M5BURNER_USER", "")
        password = environ.get("M5BURNER_PWD", "")
        if not username or not password:
            raise ValidationError("M5BURNER_USER and M5BURNER_PWD must both be set")

        artifacts_dir = Path(args.artifacts_dir)
        checksums_path = Path(args.checksums) if args.checksums else artifacts_dir / "SHA256SUMS"
        manifest_path = (
            Path(args.manifest) if args.manifest else artifacts_dir / BUILD_MANIFEST_NAME
        )
        artifacts = validate_artifacts(
            artifacts_dir,
            checksums_path,
            manifest_path,
            expected_version=args.tag.removeprefix("v"),
        )

        if args.output:
            logger = RequestLogger(args.output)
        client = client_factory(args.api_base_url, logger=logger)
        client.login(username, password)

        results = []
        for firmware_id, binary_path in artifacts.items():
            try:
                result = client.ensure_firmware_published(
                    firmware_id, args.tag, binary_path, dry_run=args.dry_run
                )
                results.append(f"{firmware_id}: {result}")
            except PublisherError:
                raise
            except Exception as exc:
                raise PublishError(f"unexpected publisher failure for {firmware_id}") from exc

        for result in results:
            print(result)
        if args.dry_run:
            print("M5Burner dry run completed; no mutations were requested")
        else:
            print("All requested M5Burner firmware versions are published")
        return 0
    except PublisherError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return exc.exit_code
    except Exception:
        # A library exception can embed request headers or bodies. Keep the CLI
        # failure useful and non-zero without reflecting untrusted exception text.
        print("Error: unexpected publisher failure", file=sys.stderr)
        return PublishError.exit_code
    finally:
        if logger:
            logger.close()


if __name__ == "__main__":
    sys.exit(main())
