#!/usr/bin/env python3
"""
HTTP Client — Streaming Response Test

Mirrors tests/test_Streaming.c: the body is handed to Python callbacks chunk by
chunk instead of being buffered, and every case asserts both what the callbacks
saw and what the collected response reports ("streamed": 1, empty payload).

  1. request_Streaming.json over HTTP/2, whole body delivered chunk by chunk
  2. same, stopped by on_data after 64 KB (RST_STREAM path)
  3. same over HTTP/1.1
  4. same non-blocking (callbacks fire on the connection reader thread)
  5. request_Streaming_gzip.json: gzip body decoded incrementally
  6. control: no callbacks at all keeps the buffered response intact
  7. half a contract is refused before the request goes out
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from python import httpClient

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ABORT_AFTER_BYTES = 64 * 1024
ABORTED_BY_CONSUMER_CODE = "3-0014"

# label, request file, transport, bytes before on_data stops, expected body prefix
CASES = [
    ("http/2, whole body", "request_Streaming.json", "http2", 0, None),
    ("http/2, stopped by the consumer", "request_Streaming.json", "http2", ABORT_AFTER_BYTES, None),
    ("http/1.1, whole body", "request_Streaming.json", "http11", 0, None),
    ("non-blocking, callbacks on the reader thread", "request_Streaming.json", "async", 0, None),
    ("gzip body decoded incrementally", "request_Streaming_gzip.json", "http2", 0, "{"),
]


def read_json_without_comments(file_path):
    """Read a JSON file and strip comment lines (// or # style)."""
    with open(file_path, "r") as f:
        lines = []
        for line in f:
            stripped = line.strip()
            if stripped and not stripped.startswith("//") and not stripped.startswith("#"):
                lines.append(line)
        return json.loads("".join(lines))


class StreamRecorder:
    """What the callbacks saw; on a non-blocking request they run on the reader
    thread, one chunk at a time, so no locking is needed."""

    def __init__(self, abort_after=0):
        self.headers = None
        self.error = "unset"      # replaced by onComplete: None when clean
        self.completes = 0
        self.bytes = 0
        self.chunks = 0
        self.head = b""
        self.abort_after = abort_after

    def on_headers(self, headers):
        self.headers = headers

    def on_data(self, chunk):
        self.bytes += len(chunk)
        self.chunks += 1
        if len(self.head) < 8:
            self.head += chunk[:8 - len(self.head)]
        # returning True asks the library to tear the stream down
        return self.abort_after > 0 and self.bytes >= self.abort_after

    def on_complete(self, error):
        self.completes += 1
        self.error = error


def prepare_config(file_name, transport):
    """Load a request file and rewrite its transport knobs."""
    config = read_json_without_comments(os.path.join(PROJECT_ROOT, "bin", file_name))
    if transport == "http11":
        config.setdefault("session", {})["protocol"] = "http/1.1"
    return config


def collect_async(request_id):
    """Poll a non-blocking request until the library reaps it."""
    while True:
        status, data = httpClient.poll_request(request_id)
        if status != 0:
            return data
        time.sleep(0.005)


def run_case(label, file_name, transport, abort_after, expected_prefix):
    """One streaming case; True when every assertion holds."""
    config = prepare_config(file_name, transport)
    recorder = StreamRecorder(abort_after)

    if transport == "async":
        request_id = httpClient.start_request(
            config, recorder.on_headers, recorder.on_data, recorder.on_complete)
        if not request_id:
            return report(label, ["the request never started"], 0, 0)
        result_json = collect_async(request_id)
    else:
        result_json = httpClient.request(
            config, recorder.on_headers, recorder.on_data, recorder.on_complete)

    problems = validate(recorder, result_json, abort_after, expected_prefix)
    return report(label, problems, recorder.bytes, recorder.chunks)


def validate(recorder, result_json, abort_after, expected_prefix):
    """Compare what the callbacks saw with what the response reports."""
    problems = []
    aborted = abort_after > 0

    result = json.loads(result_json) if result_json else {}
    response = result.get("response") or {}
    error_code = (result.get("error") or {}).get("code")
    headers = recorder.headers or {}

    if response.get("streamed") != 1:
        problems.append(f"response.streamed is {response.get('streamed')}, expected 1")
    if response.get("payload") != "":
        problems.append(f"response.payload must be empty, got {len(response.get('payload') or '')} bytes")
    if aborted and error_code != ABORTED_BY_CONSUMER_CODE:
        problems.append(f"error.code is {error_code}, expected {ABORTED_BY_CONSUMER_CODE}")
    if not aborted and error_code:
        problems.append(f"unexpected error.code {error_code}")

    if recorder.completes != 1:
        problems.append(f"on_complete ran {recorder.completes} times, expected 1")
    if recorder.bytes == 0:
        problems.append("no body byte delivered")
    if aborted and recorder.bytes < abort_after:
        problems.append(f"stopped after {recorder.bytes} bytes, expected at least {abort_after}")

    completed_code = recorder.error.get("code") if isinstance(recorder.error, dict) else None
    if not aborted and recorder.error is not None:
        problems.append(f"on_complete reported {recorder.error}")
    if aborted and completed_code not in (None, ABORTED_BY_CONSUMER_CODE):
        problems.append(f"on_complete reported {completed_code}, expected {ABORTED_BY_CONSUMER_CODE}")

    if not aborted and headers.get(":status") != "200":
        problems.append(f":status is {headers.get(':status')}, expected 200")

    # an uncompressed body must arrive complete
    content_length = headers.get("content-length", "")
    encoded = headers.get("content-encoding", "identity").lower() != "identity"
    if not aborted and not encoded and content_length.isdigit():
        if recorder.bytes != int(content_length):
            problems.append(f"delivered {recorder.bytes} of {content_length} advertised bytes")

    if expected_prefix is not None and not recorder.head.startswith(expected_prefix.encode()):
        problems.append(f"body starts with {recorder.head!r}, expected {expected_prefix!r}")
    return problems


def report(label, problems, byte_count, chunk_count):
    """Print one case verdict."""
    if problems:
        print(f"    FAILED [{label}]")
        for problem in problems:
            print(f"      - {problem}")
        return False
    print(f"    passed [{label}]: {byte_count} bytes in {chunk_count} chunks")
    return True


def run_buffered_case():
    """Control: without callbacks the response stays a complete buffered one."""
    label = "no callbacks, buffered control"
    config = prepare_config("request_Streaming.json", "http2")
    result = json.loads(httpClient.request(config))
    response = result.get("response") or {}
    payload = response.get("payload") or ""

    problems = []
    if response.get("streamed") != 0:
        problems.append(f"response.streamed is {response.get('streamed')}, expected 0")
    if len(payload) == 0:
        problems.append("response.payload is empty")
    return report(label, problems, len(payload), 1)


def run_partial_contract_case():
    """Half a contract is refused up front: a missing on_data would leave the
    body with neither a consumer nor a place in the buffered response."""
    label = "partial callbacks refused"
    config = prepare_config("request_Streaming.json", "http2")

    partials = ({"on_headers": lambda headers: None},
                {"on_data": lambda chunk: False},
                {"on_complete": lambda error: None})

    problems = []
    for kwargs in partials:
        try:
            httpClient.request(config, **kwargs)
            problems.append(f"{'/'.join(kwargs)} was accepted")
        except ValueError:
            pass    # expected: rejected without touching the network
        except Exception as exc:
            problems.append(f"{'/'.join(kwargs)} raised {type(exc).__name__}: {exc}")
    return report(label, problems, 0, 0)


def main():
    print("=== HTTP Client — Streaming Response Test ===\n")

    print("[Init] Initializing HTTP client...")
    httpClient.init()
    print("[Init] Initialized\n")

    passed = 0
    failed = 0
    for label, file_name, transport, abort_after, expected_prefix in CASES:
        try:
            ok = run_case(label, file_name, transport, abort_after, expected_prefix)
        except Exception as exc:
            print(f"    FAILED [{label}]: {exc}")
            ok = False
        passed, failed = (passed + 1, failed) if ok else (passed, failed + 1)

    try:
        ok = run_buffered_case()
    except Exception as exc:
        print(f"    FAILED [no callbacks, buffered control]: {exc}")
        ok = False
    passed, failed = (passed + 1, failed) if ok else (passed, failed + 1)

    try:
        ok = run_partial_contract_case()
    except Exception as exc:
        print(f"    FAILED [partial callbacks refused]: {exc}")
        ok = False
    passed, failed = (passed + 1, failed) if ok else (passed, failed + 1)

    print("\nCleaning up...")
    httpClient.cleanup()

    total = passed + failed
    print(f"\n{'=' * 40}")
    print(f"Results: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        print("SOME TESTS FAILED")
        sys.exit(1)
    print("All tests passed!")


if __name__ == "__main__":
    main()
