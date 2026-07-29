#!/usr/bin/env python3
"""
HTTP/2 Client Test

Mirrors nodejs/test.js - basic test for init / request / cleanup.
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
    """Run one request lifecycle and return its outcome.

    Mirrors doRequest() in tests/test_Concurrency.c and java/Test.java, parsing the
    same error.code / session.streamId / response.payload fields.
    """
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
    print("=== HTTP/2 Client Test ===\n")

    # Initialize first
    print("[Init] Initializing HTTP/2 client...")
    httpClient.init()
    print("[Init] Initialized\n")

    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # Test 1: Single request
    print("[Test 1] Single request...")
    try:
        start_time = time.time()

        request_json = read_json_without_comments(
            os.path.join(project_root, "bin", "request_GET.json")
        )
        result = httpClient.request(request_json)

        elapsed = (time.time() - start_time) * 1000

        print(f"Completed in {elapsed:.0f}ms")

        result_obj = json.loads(result)
        print(f"URL: {result_obj.get('url', 'N/A')}")

        print(json.dumps(result_obj, indent=2))
    except Exception as e:
        print(f"Failed: {e}")
        import traceback
        traceback.print_exc()

    # Test 2: concurrent requests (HTTP/2 multiplexing).
    # cffi releases the GIL around the blocking native call, so a ThreadPoolExecutor
    # runs these in REAL parallel - mirrors the pthread workers in test_Concurrency.c
    # and Promise.all in nodejs/test.js. Same-host requests share ONE multiplexed
    # connection, each taking its own odd stream id (1, 3, 5, ...). A warm-up request
    # establishes and pools the shared connection first; the concurrent batch then
    # REUSES it, exercising multiplexing rather than a burst of cold connects (which
    # servers often throttle).
    print("\n[Test 2] Concurrent requests (multiplexed)...")
    DEFAULT_CONCURRENCY = 8
    try:
        config = read_json_without_comments(
            os.path.join(project_root, "tests", "test_Concurrency.json")
        )
        # "concurrency" is a test-only field; pop it before the native call. The
        # remaining keys keep their insertion order (Python dict + json round-trip
        # preserve order), so the header fingerprint is unchanged.
        concurrency = config.pop("concurrency", DEFAULT_CONCURRENCY)
        if concurrency < 1:
            concurrency = 1
        url = config.get("url")

        # Warm-up: establish and pool the shared connection.
        print("    warming up the shared connection...")
        warm = do_request(-1, config)
        if warm["err_code"] is not None:
            print(f"    warm-up FAILED {warm['err_code']} ({warm['ms']:.0f}ms)")
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
                print(f"    #{r['index']} \u2717 {r['err_code']} ({r['ms']:.0f}ms)")
            else:
                ok += 1
                ids.add(r["stream_id"])
                print(f"    #{r['index']} \u2713 {r['bytes']} bytes, stream {r['stream_id']} ({r['ms']:.0f}ms)")
        distinct = ", ".join(str(i) for i in sorted(x for x in ids if x is not None))
        print(f"\u2713 {ok}/{concurrency} succeeded, total wall time {total_ms:.0f}ms")
        print(f"\u2713 distinct stream ids on the shared connection: {distinct}")
    except Exception as e:
        print(f"    Concurrent test failed: {e}")

    print("\nCleaning up...")
    httpClient.cleanup()
    print("All tests completed!")


if __name__ == "__main__":
    main()
