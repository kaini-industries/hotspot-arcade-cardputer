#!/usr/bin/env python3

"""Publish a validated Hotspot Arcade release to M5Burner.

Credentials are read from M5BURNER_USER and M5BURNER_PWD. The publisher is
deliberately strict: it accepts only the HTTPS M5Burner endpoint, verifies the
catalog repository identity and artifact checksum, resumes incomplete releases,
and exits non-zero whenever the requested published state was not reached.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
from pathlib import Path
from urllib.parse import urlparse


API_BASE_URL = "https://m5burner-api.m5stack.com"
API_HOST = "m5burner-api.m5stack.com"
LOGIN_URL = "https://uiflow2.m5stack.com/api/v1/account/login"
EXPECTED_REPOSITORY = "https://github.com/kaini-industries/hotspot-arcade-cardputer"
REQUEST_TIMEOUT = (10, 60)
TAG_RE = re.compile(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?$")
VERSION_FILE = Path(__file__).resolve().parents[1] / "VERSION"


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
    parsed = urlparse(value)
    if (
        parsed.scheme != "https"
        or parsed.hostname != API_HOST
        or parsed.username
        or parsed.password
        or parsed.port
        or parsed.query
        or parsed.fragment
        or parsed.path not in ("", "/")
    ):
        raise ValidationError(
            f"API base URL must be exactly https://{API_HOST}; got {value!r}"
        )
    return f"https://{API_HOST}"


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


def validate_artifacts(artifacts_dir: Path, checksums_path: Path) -> dict[str, Path]:
    if not artifacts_dir.is_dir():
        raise ValidationError(f"artifacts directory does not exist: {artifacts_dir}")

    checksums = _read_checksums(checksums_path)
    artifacts: dict[str, Path] = {}
    for firmware_id, filename in FIRMWARE_MAP.items():
        path = artifacts_dir / filename
        if not path.is_file():
            raise ValidationError(f"required firmware artifact is missing: {path}")
        if path.stat().st_size == 0:
            raise ValidationError(f"required firmware artifact is empty: {path}")
        expected = checksums.get(filename)
        if not expected:
            raise ValidationError(f"{filename} is missing from {checksums_path}")
        actual = _sha256(path)
        if actual != expected:
            raise ValidationError(
                f"checksum mismatch for {filename}: expected {expected}, got {actual}"
            )
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
        safe_url = f"{parsed.scheme}://{parsed.netloc}{parsed.path}"
        self._file.write(
            f"[{self._counter}] {request.method} {safe_url} -> {response.status_code}\n"
        )
        self._file.flush()

    def close(self) -> None:
        self._file.close()


class M5BurnerClient:
    def __init__(self, api_base_url: str = API_BASE_URL, logger=None, session=None):
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
        if logger:
            logger.attach(self.session)

    def _request(
        self, method: str, url: str, *, expected=(200,), status_error=TransportError, **kwargs
    ):
        kwargs.setdefault("timeout", REQUEST_TIMEOUT)
        kwargs.setdefault("allow_redirects", False)
        try:
            response = self.session.request(method, url, **kwargs)
        except Exception as exc:
            raise TransportError(f"{method} {urlparse(url).path} failed: {exc}") from exc
        if response.status_code not in expected:
            raise status_error(
                f"{method} {urlparse(url).path} returned HTTP {response.status_code}"
            )
        return response

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
            raise ValidationError(
                "M5Burner catalog repository mismatch: "
                f"expected {EXPECTED_REPOSITORY}, got {repository or '(empty)'}"
            )

    @staticmethod
    def _matching_versions(firmware_info: dict, version: str) -> list[dict]:
        versions = firmware_info.get("versions") or firmware_info.get("version_list") or []
        if not isinstance(versions, list):
            raise TransportError("firmware version list is malformed")
        return [entry for entry in versions if isinstance(entry, dict) and entry.get("version") == version]

    @staticmethod
    def _is_published(version: dict) -> bool:
        for key in ("published", "publish", "is_publish"):
            if key in version:
                return version[key] in (True, 1, "1", "published")
        return False

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
                raise UploadError(f"upload failed for {firmware_id}: {exc}") from exc
            firmware_info = self.get_firmware_info(firmware_id)
            self._validate_repository(firmware_info)
            matches = self._matching_versions(firmware_info, version)
            if len(matches) != 1:
                raise UploadError(
                    f"upload completed but exactly one {version} entry was not found for {firmware_id}"
                )

        entry = matches[0]
        if self._is_published(entry):
            return "already published"
        file_id = entry.get("file")
        if not file_id:
            raise PublishError(f"version {version} for {firmware_id} has no file identifier")
        if dry_run:
            return "would publish existing upload"
        try:
            self.publish_file(firmware_id, str(file_id))
        except PublisherError:
            raise
        except Exception as exc:
            raise PublishError(f"publish failed for {firmware_id}: {exc}") from exc
        verified_info = self.get_firmware_info(firmware_id)
        self._validate_repository(verified_info)
        verified = self._matching_versions(verified_info, version)
        if len(verified) != 1 or str(verified[0].get("file", "")) != str(file_id):
            raise PublishError(
                f"M5Burner did not retain the expected {version} file after publication"
            )
        publication_fields = {"published", "publish", "is_publish"}.intersection(verified[0])
        if publication_fields and not self._is_published(verified[0]):
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
        artifacts = validate_artifacts(artifacts_dir, checksums_path)

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
                raise PublishError(f"unexpected publisher failure for {firmware_id}: {exc}") from exc

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
    finally:
        if logger:
            logger.close()


if __name__ == "__main__":
    sys.exit(main())
