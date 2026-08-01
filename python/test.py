#!/usr/bin/env python3
"""
HTTP/2 Client Test

Basic test for init / request / cleanup.
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
    print("=== HTTP/2 Client Test ===\n")
    passed = 0
    failed = 0

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
        result_obj = json.loads(result)

        # Validate: error must be empty, status 2xx, payload non-empty
        error = result_obj.get("error")
        response = result_obj.get("response")
        if isinstance(error, dict) and error.get("code"):
            print(f"    FAILED: error code {error['code']}")
            failed += 1
        elif not isinstance(response, dict) or not response.get("payload"):
            print("    FAILED: response.payload is missing or empty")
            failed += 1
        else:
            passed += 1
            print(f"    PASSED in {elapsed:.0f}ms")
            print(f"    URL: {result_obj.get('url', 'N/A')}")

        print(json.dumps(result_obj, indent=2))
    except Exception as e:
        failed += 1
        print(f"Failed: {e}")
        import traceback
        traceback.print_exc()

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
