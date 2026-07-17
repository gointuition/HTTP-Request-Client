#!/usr/bin/env python3
"""
HTTP/2 Client Example

Mirrors nodejs/example.js - demonstrates GET, POST, and custom timeout requests.
"""

import json
import os
import sys
import time

# Add parent directory to path so we can import the python package
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
    print("HTTP/2 Client Example\n")

    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # Example 1: Simple GET request
    print("Example 1: GET request")
    try:
        request_json = read_json_without_comments(
            os.path.join(project_root, "bin", "request_GET.json")
        )
        result = httpClient.request(request_json)
        print(json.dumps(json.loads(result), indent=2))
    except Exception as e:
        print(f"Error: {e}")

    # Example 2: POST request with JSON
    print("\nExample 2: POST request")
    try:
        request_json = read_json_without_comments(
            os.path.join(project_root, "bin", "request_POST.json")
        )
        # separators=(',', ':') matches C library's json_dumps(JSON_COMPACT)
        payload_str = json.dumps(request_json.get("payload", {}), separators=(",", ":"))
        # content-length is byte length, not character count
        request_json["headers"]["content-length"] = str(len(payload_str.encode("utf-8")))
        result = httpClient.request(request_json)
        print(json.dumps(json.loads(result), indent=2))
    except Exception as e:
        print(f"Error: {e}")

    # Example 3: Custom timeouts
    print("\nExample 3: Custom timeouts")
    try:
        request_json = read_json_without_comments(
            os.path.join(project_root, "bin", "request_GET.json")
        )
        request_json["url"] = "https://httpbin.org/delay/10"
        request_json["connectTimeoutInMilliseconds"] = 5000
        request_json["responseReadingTimeoutInMilliseconds"] = 5000
        result = httpClient.request(request_json)
        print(json.dumps(json.loads(result), indent=2))
    except Exception as e:
        print(f"Error: {e}")

    # Cleanup
    httpClient.cleanup()
    print("\nDone!")


if __name__ == "__main__":
    main()
