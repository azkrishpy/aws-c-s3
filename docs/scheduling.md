# Scheduling

The scheduling system is a single-threaded work loop driven by event loop tasks.

## Work Loop

Any thread can call `schedule_process_work_synced()` (under lock) to post a task to
the `process_work_event_loop`. If a task is already pending, this is a no-op.
The task runs `s_s3_client_process_work_task()`, which calls the vtable's `process_work`,
defaulting to `s_s3_client_process_work_default()`. All scheduling decisions happen there.

`schedule_process_work_synced()` is called from:
- `aws_s3_client_make_meta_request()` — when the user creates a new operation.
- `s_s3_client_prepare_callback_queue_request()` — when a request finishes preparing.
- `s_s3_client_meta_request_finished_request()` — when a request completes on the network.
- `aws_s3_client_schedule_process_work()` — when a meta request's internal state changes.
- `s_s3_client_endpoint_ref_count_zero()` — when an endpoint is cleaned up.

## Snapshot Semantics

At the start of each iteration, the scheduler locks and moves all pending work into
thread-local storage, capturing a snapshot. New work arriving during scheduling is
not visible until the next iteration. This means:
- The scheduler always operates on a consistent set of work.
- External threads can continue depositing work without contention beyond the brief
  lock to swap list pointers.

## Processing Steps

`s_s3_client_process_work_default()` runs in five steps:

1. **Drain mailbox** — Lock, swap `pending_meta_request_work` and `prepared_requests`
   into thread-local storage, adjust `num_requests_being_prepared`, set
   `process_work_task_scheduled=false`, unlock.
2. **Accept new meta requests** — Add newly arrived meta requests to
   `threaded_data.meta_requests`.
3. **Update and assign** — Call `update_meta_requests_threaded()` to ask each meta
   request for its next request, then `update_connections_threaded()` to assign queued
   requests to HTTP connections.
4. **Log stats.**
5. **Shutdown check** — If the client is shutting down and all work is drained, call
   `finish_destroy()`.

## Two-Pass Scheduling

`update_meta_requests_threaded()` iterates the meta request list twice:

**First pass (conservative):** Each meta request type self-limits how many requests it
produces. This keeps any single meta request from flooding the prepare pipeline.
- GET: caps at 8 in-flight.
- PUT: caps at 1 pending read.
- COPY: caps at 1 in-flight.

**Second pass (greedy):** No per-type restriction. Meta requests fill remaining capacity
on a first-come-first-served basis. The only limits are:
- GET: no internal limit (only client-level caps apply).
- PUT: `s_max_parts_pending_read` (5) limits concurrent body reads.
- COPY: no internal limit (only client-level caps apply).
- Default: produces at most one request total (single-request operation).

## Request Pipeline

A request moves through the pipeline as follows:

1. `meta_request->update()` produces a request.
2. `s_acquire_mem_and_prepare_request()` reserves a buffer from the pool. If memory is
   full, the reservation async-waits until a buffer is released — it does not block.
   See [memory_aware_request_execution.md](memory_aware_request_execution.md) for the
   buffer pool and ticketing mechanism.
3. `vtable->prepare_request()` builds the HTTP message (for PUT, this reads the body
   from the input stream).
4. `s_s3_meta_request_sign_request()` signs it with SigV4.
5. The signed request is pushed to `synced_data.prepared_requests` and the work loop
   is re-triggered.
6. On the next iteration, `process_work_default()` drains it into
   `threaded_data.request_queue`.
7. `update_connections_threaded()` assigns it to an HTTP connection via the retry
   strategy and connection manager.

## Flow Control Counters

Several counters track requests through the pipeline and enforce flow control:

| Counter | Scope | Storage | Tracks |
|---------|-------|---------|--------|
| `num_requests_in_flight` | client | atomic | Requests from prepare to network completion. Checked against `max_requests_in_flight` (= `ideal_connection_count * 4`). |
| `num_requests_being_prepared` | client | threaded_data | Requests in the prepare+sign stage. Checked against `max_requests_prepare` (= `ideal_connection_count`). |
| `num_request_being_prepared` | per meta_request | atomic | Same window as above, scoped to a single meta request. Checked against the per-meta-request connection cap. |
| `num_requests_network` | per meta_request | atomic | Requests from connection assignment to network completion. |
| `num_requests_network_io` | client | atomic | Same window as above, client-wide. Checked against `max_active_connections`. |
| `num_parts_pending_read` | per meta_request (PUT only) | synced_data | UploadPart requests from production to body read completion. Limits read-ahead to 5 parts. Internal to PUT meta request. |
| `request_queue_size` | client | threaded_data | Prepared requests waiting for connection assignment. Included in the prepare pipeline check. |

## See Also

- [overview.md](overview.md) — threading model, data categories, and event loop groups.
- [meta_requests.md](meta_requests.md) — how meta requests produce requests via their state machines.
- [memory_aware_request_execution.md](memory_aware_request_execution.md) — buffer pooling, memory limits, and the ticketing mechanism.
