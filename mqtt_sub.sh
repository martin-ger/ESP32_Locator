#!/bin/bash
# Subscribe to ESP32 Locator MQTT topic and save messages as JSON files
# compatible with locator.html "Import JSON" function.
#
# Usage: ./mqtt_sub.sh <mqtt-url> <username> <password> [output-dir]
#   mqtt-url: mqtt://broker:port/topic/path (same format as ESP32 config)
#   output-dir: directory for saved JSON files (default: current dir)

set -euo pipefail

if [ $# -lt 3 ]; then
    echo "Usage: $0 <mqtt-url> <username> <password> [output-dir]"
    echo
    echo "  mqtt-url   mqtt://broker:port/topic/path"
    echo "  output-dir directory for JSON files (default: .)"
    echo
    echo "Example:"
    echo "  $0 mqtt://mybroker:1883/locator/last user pass ./scans"
    exit 1
fi

MQTT_URL="$1"
USERNAME="$2"
PASSWORD="$3"
OUTPUT_DIR="${4:-.}"

# Parse mqtt://host:port/topic/path
REST="${MQTT_URL#*://}"
HOST_PORT="${REST%%/*}"
TOPIC="${REST#*/}"
HOST="${HOST_PORT%%:*}"
PORT="${HOST_PORT##*:}"

# Default port if not specified (no colon in host_port)
if [ "$HOST" = "$PORT" ]; then
    PORT=1883
fi

if [ -z "$TOPIC" ]; then
    echo "ERROR: No topic in URL. Expected mqtt://host:port/topic/path"
    exit 1
fi

echo "Broker: $HOST:$PORT"
echo "Topic:  $TOPIC"
echo "Output: $OUTPUT_DIR"
echo "Waiting for messages... (Ctrl+C to stop)"
echo "---"

mkdir -p "$OUTPUT_DIR"

mosquitto_sub -h "$HOST" -p "$PORT" -u "$USERNAME" -P "$PASSWORD" -t "$TOPIC" -q 0 -C 1 | \
while IFS= read -r line; do
    [ -z "$line" ] && continue

    TS=$(date +%Y%m%d_%H%M%S)
    OUTFILE="$OUTPUT_DIR/LocatorScan_${TS}.json"

    # Single scan object → wrap in array; array → use as-is
    FIRST=$(echo "$line" | head -c1)
    if [ "$FIRST" = "{" ]; then
        echo "[$line]" > "$OUTFILE"
    else
        echo "$line" > "$OUTFILE"
    fi

    # Pretty-print if python3 available
    if command -v python3 >/dev/null 2>&1; then
        python3 -m json.tool "$OUTFILE" > "${OUTFILE}.tmp" 2>/dev/null && mv "${OUTFILE}.tmp" "$OUTFILE" || rm -f "${OUTFILE}.tmp"
    fi

    BYTES=$(wc -c < "$OUTFILE")
    echo "[$(date '+%H:%M:%S')] Saved: $OUTFILE ($BYTES bytes)"
done
