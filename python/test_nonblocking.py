#!/usr/bin/env python3
"""
HTTP Client — Non-Blocking Concurrency Test

Unlike test_concurrency.py (which runs the blocking handleRequest on a
ThreadPoolExecutor), this uses the async surface (start_request + poll_request):
requests are fired without blocking, then polled from the main thread, so the
number of concurrent in-flight requests is NOT bounded by any thread-pool size.
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from python import httpClient


def read_json_without_comments(file_path):
    """Read a JSON file and strip comment lines (// or # style)."""
    with open(file_path, "r") as f:
        lines = []
        for line in f:
            stripped = line.strip()
            if stripped and not stripped.startswith("//") and not stripped.startswith("#"):
                lines.append(line)
        return json.loads("".join(lines))


def main():
    print("=== HTTP Client — Non-Blocking Concurrency Test ===\n")
    passed = 0
    failed = 0
    DEFAULT_CONCURRENCY = 8

    print("[Init] Initializing HTTP client...")
    httpClient.init()
    print("[Init] Initialized\n")

    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    print("[Test] non-blocking concurrent requests (async surface)...")
    try:
        config = read_json_without_comments(
            os.path.join(project_root, "bin", "request_Concurrency.json")
        )
        concurrency = config.pop("concurrency", DEFAULT_CONCURRENCY)
        if concurrency < 1:
            concurrency = 1
        url = config.get("url")

        # Force the non-blocking socket I/O mode for the async surface.
        config["non-blocking"] = 1

        print(f"    firing {concurrency} requests to {url} (all in flight at once)")
        start_time = time.time()

        # Fire every request without blocking: this loop returns almost
        # instantly even though none of the responses have arrived yet.
        ids = [httpClient.start_request(config) for _ in range(concurrency)]
        for i, rid in enumerate(ids):
            if not rid:
                failed += 1
                print(f"    #{i} ✗ failed to start (id=0)")

        # Reap results by polling; one thread drives all requests.
        outcomes = [None] * concurrency
        pending = {rid: i for i, rid in enumerate(ids) if rid}
        while pending:
            for rid in list(pending.keys()):
                status, data = httpClient.poll_request(rid)
                if status == 0:
                    continue  # still in flight
                i = pending.pop(rid)
                outcomes[i] = (status, data)

        total_ms = (time.time() - start_time) * 1000

        ok = 0
        stream_ids = set()
        for i, (status, data) in enumerate(outcomes):
            if status == 1 and data is not None:
                parsed = json.loads(data)
                error = parsed.get("error")
                if isinstance(error, dict) and error.get("code"):
                    failed += 1
                    print(f"    #{i} ✗ {error['code']}")
                else:
                    ok += 1
                    passed += 1
                    session = parsed.get("session")
                    if isinstance(session, dict):
                        stream_ids.add(session.get("streamId"))
                    response = parsed.get("response")
                    nbytes = 0
                    if isinstance(response, dict) and isinstance(response.get("payload"), str):
                        nbytes = len(response["payload"])
                    print(f"    #{i} ✓ {nbytes} bytes, stream {session.get('streamId') if isinstance(session, dict) else '?'}")
            else:
                failed += 1
                print(f"    #{i} ✗ status={status}")

        distinct = ", ".join(str(s) for s in sorted(x for x in stream_ids if x is not None))
        print(f"✓ {ok}/{concurrency} succeeded, total wall time {total_ms:.0f}ms")
        print(f"✓ distinct stream ids on the shared connection: {distinct}")
    except Exception as e:
        failed += 1
        print(f"    Non-blocking test failed: {e}")

    print("\nCleaning up...")
    httpClient.cleanup()

    total = passed + failed
    if total == 0:
        total = 1
    print(f"\n{'='*40}")
    print(f"Results: {passed}/{total} passed, {failed}/{total} failed")
    if failed > 0:
        print("SOME TESTS FAILED")
        sys.exit(1)
    print("All tests passed!")


if __name__ == "__main__":
    main()
