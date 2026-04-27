# Glossary

Terms used throughout the aws-c-s3 codebase.

**meta request** — A user-initiated S3 operation (GetObject, PutObject, CopyObject, or
default pass-through). Contains a state machine that produces individual HTTP requests
on demand. See [meta_requests.md](meta_requests.md).

**request** (`aws_s3_request`) — A single HTTP request, one part of a meta request. Carries
a backlink (`request->meta_request`) and a `request_tag` identifying its role
(e.g., `CREATE_MULTIPART_UPLOAD`, `PART`, `COMPLETE_MULTIPART_UPLOAD`).

**endpoint** (`aws_s3_endpoint`) — Represents an S3 hostname. Owns an HTTP connection
manager that pools connections to that host. Endpoints are shared across meta requests
targeting the same host and cleaned up when their reference count drops to zero.

**process_work_event_loop** — A single event loop chosen from the bootstrap ELG at client
init. All scheduling decisions run on this thread. See [scheduling.md](scheduling.md).

**synced_data** — Mutex-protected fields on the client (or meta request) that any thread
may read or write. Acts as a mailbox: external threads deposit work here, the work loop
drains it. Lock is held only long enough to swap list pointers.

**threaded_data** — Fields on the client accessed only from the `process_work_event_loop`
thread. No lock needed. Functions with the `_threaded` suffix operate on this data.

**stats** — Atomic counters on the client (`num_requests_in_flight`, `num_requests_network_io`,
etc.) readable and writable by any thread without a lock. Used for flow control and logging.

**bootstrap ELG** — The event loop group (`client_bootstrap->event_loop_group`) that drives
networking. HTTP connections are pinned to loops in this group. The `process_work_event_loop`
is one loop from this group.

**body streaming ELG** — A separate event loop group (`body_streaming_elg`) for delivering
response bodies to user callbacks and for async request preparation. Isolated so slow user
callbacks do not block networking or scheduling.

**prepare** — The async pipeline a request goes through before it can be sent: acquire a
buffer from the pool, build the HTTP message (for PUT this includes reading the body from
the input stream), then sign with SigV4. See [scheduling.md](scheduling.md#request-pipeline).

**update()** — The vtable function the client calls on each meta request to get the next
request to send. Returns `work_remaining` (bool) and optionally an `aws_s3_request`.
Called from the `process_work_event_loop` thread.

**conservative pass** — The first of two passes in `update_meta_requests_threaded()`. Each
meta request type self-limits how many requests it produces (GET: 8 in-flight, PUT: 1
pending read, COPY: 1 in-flight). Prevents any single meta request from flooding the
prepare pipeline.

**request_tag** — An enum on each `aws_s3_request` identifying what kind of sub-operation
it is (e.g., `AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_PART`). Used by the meta request's
`finished_request()` to know which sub-operation completed and advance its state machine.

**ideal_connection_count** — The target number of HTTP connections, computed from
`throughput_target_gbps / s_throughput_per_connection_gbps` at client init. Clamped to
[10, 10000]. Drives all other limits: `max_requests_in_flight` = connections × 4,
`max_requests_prepare` = connections.

**buffer pool** — A memory pool that manages buffers for request bodies (PUT) and response
bodies (GET). Enforces a global memory limit. Requests reserve memory via tickets; if the
pool is full, reservations wait asynchronously until memory is freed. See
[memory_aware_request_execution.md](memory_aware_request_execution.md).

**num_parts_pending_read** — A PUT-only counter tracking UploadPart requests that have been
produced by `update()` but have not yet finished reading their body from the input stream.
Capped at 5 (`s_max_parts_pending_read`) to avoid wasting buffer memory on sequential reads.
Internal to the PUT meta request; the client does not see it.
