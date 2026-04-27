# Design Decisions

Non-obvious choices in the codebase and why they were made.

## Single-threaded scheduling

All scheduling decisions run on one event loop thread (`process_work_event_loop`). This
avoids complex multi-threaded coordination for the scheduling logic. The tradeoff is that
scheduling throughput is limited to one core, but in practice the scheduler only moves
pointers and makes quick decisions — the expensive work (body reads, signing, network I/O)
happens in parallel on other threads. The lock on `synced_data` is held only long enough
to swap linked list heads.

## State-driven work loop, not event-driven

`process_work_default()` does not receive an event describing what changed. It does a full
sweep every time: drain all inboxes, iterate all meta requests, assign all queued requests.
This is simpler than event dispatch — no event types to define, no ordering concerns, no
risk of missed events. The cost is a small amount of redundant iteration (checking a meta
request that has nothing new), which is cheap compared to the complexity of event routing.

## Two-pass scheduling

The conservative first pass lets each meta request type self-limit (GET: 8 in-flight,
PUT: 1 pending read, COPY: 1 in-flight). This ensures that when multiple meta requests
compete, each gets a fair share of the prepare pipeline before any one floods it. The
second pass then fills remaining capacity greedily. Without two passes, a single PUT with
a fast input stream could fill the entire prepare pipeline before any GET gets a turn.

## `s_max_parts_pending_read = 5` (not 1, not 25)

For PUT, body reads from a sequential input stream are serial — only one can read at a
time. A limit of 1 would leave gaps in the pipeline: after a read completes, the request
must be signed, queued, and the work loop must run again before the next read starts. A
limit of 5 keeps the pipeline full by ensuring there is always a request ready to start
reading the moment the previous read completes. Higher values waste buffer memory on
requests that just sit waiting for their turn to read.

For parallel file I/O streams, the 5 also caps concurrent disk reads to avoid thrashing.

## `max_requests_in_flight = ideal_connection_count * 4`

Requests spend time at multiple stages: being prepared, queued, on the network, and
streaming the response body. The multiplier of 4 ensures the pipeline stays full across
all stages. If the limit equaled the connection count, the network would starve while
requests were being prepared.

## `max_requests_prepare = ideal_connection_count`

No point preparing more requests than can be sent simultaneously. Preparing beyond this
wastes buffer memory and signing effort on requests that will just sit in the queue.

## Per-meta-request prepare cap = per-meta-request connection cap

Each meta request cannot have more requests being prepared than its connection cap. This
prevents one meta request from monopolizing the prepare pipeline and starving others. The
check uses an atomic (`num_request_being_prepared`) which is not perfectly synchronized
with the threaded_data counter, but over-preparing by a small amount is harmless.

## Connection overrides can only reduce, never increase

`aws_s3_client_get_max_active_connections()` starts with `ideal_connection_count` and
applies overrides as `min()`. A meta request override cannot exceed the client cap, and
the client cap cannot exceed the throughput-derived ideal. This prevents a single meta
request from exceeding what the client was configured for.

## DNS slow-start

If DNS has not resolved any addresses for an endpoint, the scheduler limits queued
requests to `g_min_num_connections` (10). There is no point preparing hundreds of requests
when we do not yet know where to send them. Once DNS resolves, the full pipeline opens up.

## CreateSession bypasses flow control

S3 Express requests need a session before they can proceed. If CreateSession were subject
to the same flow-control limits as regular requests, it could be blocked behind a full
pipeline, deadlocking all S3 Express operations. So `s_s3_client_should_update_meta_request()`
lets CreateSession through unconditionally.

## Buffer pool: async wait, not fail

When the buffer pool is full, `aws_s3_buffer_pool_reserve()` does not fail. It returns a
future that resolves when memory becomes available. This avoids error handling complexity
at every call site and naturally creates backpressure — the prepare pipeline slows down
as memory fills up, without any explicit coordination between the buffer pool and the
scheduler.

## Buffer pool: forced buffers

Forced buffers are allowed to exceed the memory limit. They exist only for async-writes,
where waiting for a normal reservation could deadlock (the user is blocked trying to write
data, but the pool is waiting for a request to finish which needs that data). Forced
buffers are capped at a percentage of the memory limit to prevent unbounded growth.

## `process_work_task_scheduled` flag

The flag is set to `false` at the start of `process_work_default()`, not at the end. This
means another thread can re-schedule the task while the current run is still in progress.
The new task queues behind the current one on the event loop. This ensures no work is
missed: if a prepare callback fires during step 3, the doorbell rings and a new run is
queued, which will drain whatever accumulated during the current run.
