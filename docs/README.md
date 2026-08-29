# aws-c-s3 Documentation

## Start Here

- [Glossary](glossary.md) — terms used throughout the codebase.
- [Scheduling Analogy](concepts/scheduling_analogy.md) — how the client works, explained as a fulfillment center.
- [File Map](file_map.md) — what each source file does.

## Architecture

- [Overview](overview.md) — client architecture, threading model, data categories, event loop groups.
- [Meta Requests](meta_requests.md) — state machines, vtable, request routing, preparation pipeline.
- [Scheduling](scheduling.md) — work loop, two-pass scheduling, request pipeline, flow control counters.
- [Memory Management](memory_aware_request_execution.md) — buffer pool, memory limits, ticketing mechanism.

## Operations

- [GetObject](operations/GetObject.md) — auto-ranged GET, range splitting, conservative pass, read backpressure.
- [PutObject](operations/PutObject.md) — multipart upload, body reading, `num_parts_pending_read`, resume.
- [CopyObject](operations/CopyObject.md) — multipart copy, server-side data movement, small object bypass.

## Reference

- [Design Decisions](design_decisions.md) — non-obvious choices and their rationale.

## In-Code Documentation

For implementation details, see comments at the top of:
- `source/s3_client.c` — client architecture, scheduling logic, and counter lifecycles.
- `source/s3_meta_request.c` — meta request state machine design and preparation pipeline.
