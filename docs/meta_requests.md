# Meta Requests

A meta request represents a single user-initiated S3 operation (GetObject, PutObject,
CopyObject, or a pass-through default request). It is a state machine that produces
individual HTTP requests on demand, not a pre-planned list. The client's work loop
calls `vtable->update()` repeatedly, and the meta request decides what to produce next
based on its current state and the results of previous requests.

## State Machines

**PutObject:** Starts by producing a CreateMultipartUpload request. Once the response
arrives with an `upload_id`, subsequent `update()` calls produce UploadPart requests
until flow-control limits are hit or all parts are started. After all parts complete,
it produces a CompleteMultipartUpload. When that finishes, `update()` returns
`work_remaining=false` and the client removes the meta request.

**GetObject:** First sends a HeadObject or a ranged GET for part 1 to discover the
object size. Once the size is known, subsequent `update()` calls produce ranged GETs
(`Range: bytes=X-Y`) for each part until all parts are requested. See
[GetObject.md](GetObject.md) for the full flow diagram and details.

## Request Routing

Every `aws_s3_request` carries a backlink (`request->meta_request`) and a `request_tag`
so completions route back to the right meta request and it knows which sub-operation
finished. The client does not understand PUT/GET/COPY logic; it just calls `update()`
on each meta request and routes completions back via the backlink by calling
`aws_s3_meta_request_finished_request()`.

## Vtable

Each meta request type implements three vtable functions:

| Function | Purpose | Thread |
|----------|---------|--------|
| `update()` | Produce the next request based on current state. | `process_work_event_loop` |
| `prepare_request()` | Build the HTTP message (headers, body). | `body_streaming_elg` or meta request's `io_event_loop` |
| `finished_request()` | Handle completion, advance the state machine. | Any networking thread |

See `s3_auto_ranged_get.c`, `s3_auto_ranged_put.c`, `s3_copy_object.c`,
`s3_default_meta_request.c` for implementations.

## Threading

The meta request has its own `synced_data`/lock for state shared across threads,
following the same pattern as the client (see [overview.md](overview.md#client-data-categories)).

## Preparation Pipeline

The preparation pipeline in `s3_meta_request.c`:

1. `vtable->prepare_request()` builds the HTTP message and reads the body.
2. `s_s3_meta_request_sign_request()` signs it with SigV4.
3. The callback notifies the client that the request is ready for connection assignment.

See [scheduling.md](scheduling.md#request-pipeline) for how this fits into the full
request lifecycle.
