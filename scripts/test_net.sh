#!/bin/bash

# Array of diverse URLs to generate overlapping network traffic
URLS=(
    "https://www.google.com"
    "https://www.github.com"
    "https://www.wikipedia.org"
    "https://www.kernel.org"
    "https://www.reddit.com"
    "https://www.amazon.com"
)

echo "=== FIRING PARALLEL CURL REQUESTS ==="

# Launch curls in the background to force concurrent execution paths
for url in "${URLS[@]}"; do
    # Discard curl output, only interested in generating network stack metrics
    curl -s -o /dev/null "$url" &
    echo "Launched background fetch for: $url (PID: $!)"
done

# Wait for all background curl processes to complete
echo "Waiting for transfers to finalize..."
wait

echo -e "\n=== FETCHING LIVE TELEMETRY MATRIX SNAPSHOT ==="
# Dump the 24 slots immediately to inspect the tickets and slot distribution
sudo cat /dev/telemetry_net
