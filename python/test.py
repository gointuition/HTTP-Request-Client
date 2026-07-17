#!/usr/bin/env python3
"""
HTTP/2 Client Test

Mirrors nodejs/test.js - basic test for init / request / cleanup.
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

    print("\nCleaning up...")
    httpClient.cleanup()
    print("All tests completed!")


if __name__ == "__main__":
    main()
