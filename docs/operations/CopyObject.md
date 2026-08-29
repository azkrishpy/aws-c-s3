# CopyObject

## Overview

The copy object meta request copies objects within S3. For small objects (below
`s_multipart_copy_minimum_object_size`), it sends a single CopyObject request.
For large objects, it uses multipart copy: CreateMultipartUpload → UploadPartCopy
× N → CompleteMultipartUpload. Implemented in `s3_copy_object.c`.

## State Machine

The `update()` function walks through these states:

1. **Discover source object size.** Send a HeadObject against the source object
   to get its Content-Length. Wait for the response.

2. **Small object bypass.** If the object is smaller than the multipart
   threshold, send a single CopyObject request (tagged `BYPASS`) and skip
   directly to done when it completes.

3. **Create multipart upload.** For large objects, send CreateMultipartUpload.
   Wait for the `upload_id`.

4. **Upload part copies.** Produce UploadPartCopy requests for each part. These
   are server-side copies — S3 reads the source object internally, so there is
   no body to read from the client. No buffer pool allocation is needed.

   Conservative pass: caps at 1 in-flight part. Non-conservative pass: no
   internal limit (only client-level caps apply).

5. **Complete multipart upload.** Once all parts complete, send
   CompleteMultipartUpload.

6. **Done.** When CompleteMultipartUpload finishes, `update()` returns
   `work_remaining=false`.

## Error / Cancellation Path

Same pattern as PUT: wait for in-flight parts, then send AbortMultipartUpload.

## Prepare

Copy prepare is lightweight. UploadPartCopy requests have no body — the HTTP
message just specifies the source object, byte range, and upload ID. No buffer
pool reservation is needed for the request (unlike PUT and GET).

## Key Difference from PUT

CopyObject does not read from a user-provided input stream. All data movement
happens server-side within S3. This means:
- No `num_parts_pending_read` counter (no body reads to throttle).
- No buffer pool pressure from request bodies.
- The conservative pass limit (1 in-flight) is the only internal throttle.

## Key Files

- `s3_copy_object.c` — state machine, update, prepare, finished_request.
- `s3_request_messages.c` — builds UploadPartCopy, CreateMultipartUpload,
  CompleteMultipartUpload HTTP messages.
