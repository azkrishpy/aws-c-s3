# GetObject

## Overview

The auto-ranged GET downloads objects from S3 by splitting them into parallel
range requests. Implemented in `s3_auto_ranged_get.c`.

## Flow Diagram

![GetObject Flow Diagram](../images/GetObjectFlow.svg)

## State Machine

The `update()` function walks through these states:

1. **Discover object size.** Send a HeadObject or a ranged GET for part 1
   (depending on whether the request already has a Range header). Wait for the
   response to learn the object size and compute the total number of parts.

2. **Request parts.** Produce ranged GET requests (`Range: bytes=X-Y`) for each
   part. Each request carries `AWS_S3_REQUEST_FLAG_ALLOCATE_BUFFER_FROM_POOL`
   so a buffer is reserved for the response body. Parts are requested until
   flow-control limits are hit, then `update()` returns NULL with
   `work_remaining=true`.

3. **Done.** When all parts are requested and completed, `update()` returns
   `work_remaining=false`.

## Conservative Pass Behavior

During the conservative pass, GET caps at `s_conservative_max_requests_in_flight`
(8). This counts parts that have been requested but not yet completed, plus parts
waiting in the body streaming priority queue. The limit exists because GET
responses must be delivered to the user in order — if part 5 arrives before parts
1-4, it sits in the priority queue holding its buffer until the earlier parts
arrive. Without a cap, this can consume large amounts of memory and hit the
global request limit.

## Read Backpressure

If `enable_read_backpressure` is set on the client, GET will not produce more
parts than the user's read window allows. This prevents the client from
downloading faster than the user can consume, which would waste memory on
buffered response bodies.

## Prepare

GET prepare is cheap and synchronous: copy the original request headers, set the
`Range` header, optionally add `If-Match` (ETag) and checksum validation headers.
No body to read. The buffer acquired from the pool is for the response body, not
the request.

## Key Files

- `s3_auto_ranged_get.c` — state machine, update, prepare, finished_request.
- `s3_request_messages.c` — `aws_s3_ranged_get_object_message_new()` builds the
  ranged GET HTTP message.
