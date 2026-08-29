#!/bin/bash
# Run a mock-server put test and show logs about max_parts_pending_read.
# Usage:
#   ./check_max_parts.sh            # default (5)
#   AWS_CRT_S3_MAX_PARTS_PENDING_READ=10 ./check_max_parts.sh
SCRIPT_DIR="$(dirname "$0")"

# Start mock server
python3 "$SCRIPT_DIR/tests/mock_s3_server/mock_s3_server.py" &
SERVER_PID=$!
sleep 2

"$SCRIPT_DIR/build/tests/aws-c-s3-tests" multipart_upload_mock_server 2>&1 \
    | grep -iE "pending_read|from environment|no value was set"

kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null
