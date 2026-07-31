#!/usr/bin/env bash
# Assert every "basket json {...}" line in a smoke-test log is a successful
# response: .error must be an empty object and .response.payload non-empty.
# Usage: assert_basket.sh <logfile>
set -euo pipefail

log="$1"
count=0

while IFS= read -r line; do
    # Strip the log prefix up to and including "basket json " to get raw JSON.
    json="${line#*basket json }"

    if ! printf '%s' "$json" | jq -e . > /dev/null 2>&1; then
        echo "::error::basket output is not valid JSON in ${log}"
        exit 1
    fi

    err=$(printf '%s' "$json" | jq -c '.error')
    if [ "$err" != "{}" ]; then
        echo "::error::basket error is not empty: ${err}"
        exit 1
    fi

    payload_len=$(printf '%s' "$json" | jq '.response.payload | length')
    if [ "$payload_len" -eq 0 ]; then
        echo "::error::response.payload is empty"
        exit 1
    fi

    count=$((count + 1))
done < <(grep "basket json {" "$log")

if [ "$count" -eq 0 ]; then
    echo "::error::no basket json found in ${log}"
    exit 1
fi

echo "OK: ${count} basket(s) checked — error empty, response.payload non-empty"
