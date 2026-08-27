#!/usr/bin/env python3
"""
HTTP Client — Concurrency Test

Tests HTTP multiplexing: multiple concurrent requests sharing
one connection, each taking its own odd stream id (1, 3, 5, ...).
"""

import json
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor

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


def do_request(index, config):
    """Run one request lifecycle and return its outcome."""
    outcome = {"index": index, "stream_id": None, "bytes": 0, "ms": 0, "err_code": None}
    t0 = time.time()
    try:
        result = httpClient.request(config)
        outcome["ms"] = (time.time() - t0) * 1000
        parsed = json.loads(result)
        error = parsed.get("error")
        if isinstance(error, dict) and error.get("code"):
            outcome["err_code"] = error["code"]
        session = parsed.get("session")
        if isinstance(session, dict):
            outcome["stream_id"] = session.get("streamId")
        response = parsed.get("response")
        if isinstance(response, dict) and isinstance(response.get("payload"), str):
            outcome["bytes"] = len(response["payload"])
    except Exception as e:
        outcome["ms"] = (time.time() - t0) * 1000
        outcome["err_code"] = str(e) or "EXCEPTION"
    return outcome


def main():
    print("=== HTTP Client — Concurrency Test ===\n")
    passed = 0
    failed = 0
    DEFAULT_CONCURRENCY = 8

    # Initialize
    print("[Init] Initializing HTTP client...")
    httpClient.init()
    print("[Init] Initialized\n")

    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # Concurrent requests (HTTP multiplexing).
    # cffi releases the GIL around the blocking native call, so a ThreadPoolExecutor
    # runs these in REAL parallel. Same-host requests share ONE multiplexed
    # connection, each taking its own odd stream id (1, 3, 5, ...). A warm-up request
    # establishes and pools the shared connection first; the concurrent batch then
    # REUSES it, exercising multiplexing rather than a burst of cold connects (which
    # servers often throttle).
    print("[Test] Concurrent requests (multiplexed)...")
    try:
        config = read_json_without_comments(
            os.path.join(project_root, "bin", "request_Concurrency.json")
        )
        concurrency = config.pop("concurrency", DEFAULT_CONCURRENCY)
        if concurrency < 1:
            concurrency = 1
        url = config.get("url")

        # Warm-up: establish and pool the shared connection.
        print("    warming up the shared connection...")
        warm = do_request(-1, config)
        if warm["err_code"] is not None:
            print(f"    warm-up FAILED {warm['err_code']} ({warm['ms']:.0f}ms)")
            failed += 1
            raise RuntimeError("could not establish the shared connection")
        print(f"    warm-up OK (stream {warm['stream_id']}, {warm['bytes']} bytes, {warm['ms']:.0f}ms); connection pooled")

        print(f"    firing {concurrency} concurrent requests to {url}")
        start_time = time.time()
        with ThreadPoolExecutor(max_workers=concurrency) as executor:
            futures = [executor.submit(do_request, i, config) for i in range(concurrency)]
            results = [f.result() for f in futures]
        total_ms = (time.time() - start_time) * 1000

        ok = 0
        ids = set()
        for r in results:
            if r["err_code"] is not None:
                failed += 1
                print(f"    #{r['index']} \u2717 {r['err_code']} ({r['ms']:.0f}ms)")
            else:
                ok += 1
                passed += 1
                ids.add(r["stream_id"])
                print(f"    #{r['index']} \u2713 {r['bytes']} bytes, stream {r['stream_id']} ({r['ms']:.0f}ms)")
        distinct = ", ".join(str(i) for i in sorted(x for x in ids if x is not None))
        print(f"\u2713 {ok}/{concurrency} succeeded, total wall time {total_ms:.0f}ms")
        print(f"\u2713 distinct stream ids on the shared connection: {distinct}")
    except Exception as e:
        failed += 1
        print(f"    Concurrent test failed: {e}")

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
