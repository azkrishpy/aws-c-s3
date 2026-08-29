# PutObject

## Overview

The auto-ranged PUT uploads objects to S3 using multipart upload. For objects
smaller than the multipart threshold, it falls back to a single PutObject
request. Implemented in `s3_auto_ranged_put.c`.

## State Machine

The `update()` function walks through these states:

1. **List parts (resume only).** If resuming a previously paused upload, send
   ListParts requests to discover which parts were already uploaded. May require
   multiple paginated requests. Skip this state entirely for new uploads.

2. **Create multipart upload.** Send a CreateMultipartUpload request. Wait for
   the response, which returns the `upload_id` needed for all subsequent part
   uploads.

3. **Upload parts.** Produce UploadPart requests for each part. Each request
   gets a `part_number` and carries `AWS_S3_REQUEST_FLAG_ALLOCATE_BUFFER_FROM_POOL`
   so a buffer is reserved for reading the body. Parts that were previously
   uploaded (resume case) are still prepared (to verify checksums) but marked
   `is_noop` so they are not actually sent.

   The number of parts produced per `update()` call is limited by
   `s_should_skip_scheduling_more_parts_based_on_flags()`:
   - Conservative pass: 1 pending read.
   - Non-conservative pass: `s_max_parts_pending_read` (5).
   - Async stream: 1 (reads are strictly serial).
   - Async writes: 1 (must wait for user to provide data).

   For known content length, parts are produced until `num_parts_started` equals
   `total_num_parts_from_content_length`. For unknown content length (streaming),
   parts are produced until the input stream reports end-of-stream.

4. **Complete multipart upload.** Once all parts have completed, send a
   CompleteMultipartUpload request containing the ETags of all parts.

5. **Done.** When CompleteMultipartUpload finishes, `update()` returns
   `work_remaining=false`.

## Error / Cancellation Path

If the meta request is cancelled or encounters an error after CreateMultipartUpload
was sent:
- Wait for all in-flight parts to complete.
- Send an AbortMultipartUpload to clean up the incomplete upload on S3.
- Then report done.

## Prepare

PUT prepare is the most expensive of all meta request types:

1. **Acquire buffer** from the pool (may wait if memory is full).
2. **Read the body** — `aws_s3_meta_request_read_body()` reads `part_size` bytes
   from the user's input stream into the buffer. This is async and is the
   bottleneck for sequential streams.
3. **Build the HTTP message** — set Content-Length, part number, upload ID,
   checksum headers.

For file I/O streaming mode, step 2 is skipped — instead a
`aws_part_streaming_input_stream` is created that reads directly from the file
during HTTP send, avoiding the buffer copy.

On retry, the body is already in the buffer, so only the HTTP message is rebuilt
and re-signed.

## `num_parts_pending_read`

This counter tracks parts from the moment `update()` produces them until the body
read completes in `s_s3_new_upload_part_info_after_body()`. It is the gate that
limits read-ahead. See [design_decisions.md](../design_decisions.md) for why the
limit is 5.

## Key Files

- `s3_auto_ranged_put.c` — state machine, update, prepare, finished_request.
- `s3_request_messages.c` — builds UploadPart, CreateMultipartUpload,
  CompleteMultipartUpload HTTP messages.
- `s3_part_streaming_input_stream.c` — file I/O streaming for parallel reads.
