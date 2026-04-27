# S3 Client Overview

The S3 client splits user operations (meta requests) into concurrent HTTP requests
and schedules them across pooled connections.

## Key Concepts

A **meta request** is a user-initiated operation (GetObject, PutObject, etc). Each type
has a state machine that produces individual HTTP requests on demand (range-GETs,
UploadParts, etc). See [meta_requests.md](meta_requests.md) for details.

A **request** goes through: buffer acquire → prepare (build message + read body) → sign → queue → send.

An **endpoint** represents an S3 hostname and owns an HTTP connection manager that pools
connections to that host.

## Threading Model

All scheduling decisions run on a single event loop thread (`process_work_event_loop`),
chosen at client init from the bootstrap ELG. This means `threaded_data` needs no lock.

Other threads (user threads, prepare callbacks, connection callbacks) push work into
`synced_data` (protected by a mutex) and call `schedule_process_work_synced()` to wake
the work loop. The lock is only held long enough to move list pointers.

## Client Data Categories

Client data falls into three categories:

- **synced_data** — mutex-protected, any thread. Mailbox for `pending_meta_request_work`
  and `prepared_requests`. External threads deposit here, the work loop drains.

- **threaded_data** — no lock, only the `process_work_event_loop` thread touches it.
  Contains `request_queue`, active `meta_requests` list, and prep counters.
  Functions with the `_threaded` suffix operate on this data.

- **stats** — atomic counters (`num_requests_in_flight`, etc). Any thread, no lock.

## Event Loop Groups

**Bootstrap ELG** (`client_bootstrap->event_loop_group`) drives networking. Each HTTP
connection is assigned to a specific event loop at creation time; all I/O for that
connection runs on that loop for its lifetime. One loop from this group is designated
as the `process_work_event_loop` at init; it still handles normal networking work too.

**Body Streaming ELG** (`body_streaming_elg`) is a separate group for delivering response
bodies to user callbacks and for async request preparation. Keeps slow user callbacks
from blocking networking or scheduling.

## Housekeeping

The `process_work_event_loop` clock (`aws_event_loop_current_clock_time`) is used to
schedule delayed housekeeping via `schedule_task_future()`:
- Buffer pool trim after 5s idle.
- Endpoint cleanup every 5s (removes endpoints with zero references).

## Further Reading

- [scheduling.md](scheduling.md) — the scheduling loop, request pipeline, and flow control counters.
- [meta_requests.md](meta_requests.md) — meta request state machines and the preparation pipeline.
- [memory_aware_request_execution.md](memory_aware_request_execution.md) — buffer pooling and memory management.
- [GetObject.md](GetObject.md) — GetObject flow diagram and details.
