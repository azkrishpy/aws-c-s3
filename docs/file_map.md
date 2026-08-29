# File Map

One-liner per source file explaining its role.

## Core

| File | Purpose |
|------|---------|
| `s3_client.c` | Client lifecycle, scheduling work loop, connection assignment, flow control. The main orchestrator. |
| `s3_meta_request.c` | Base meta request logic: preparation pipeline, signing, body streaming, event delivery. Shared by all meta request types. |
| `s3_request.c` | `aws_s3_request` creation, setup, and lifecycle. A request is one HTTP request within a meta request. |
| `s3_endpoint.c` | `aws_s3_endpoint` management. Each endpoint owns an HTTP connection manager for one S3 hostname. |

## Meta Request Types

Each implements `update()`, `prepare_request()`, and `finished_request()` for its operation type.

| File | Purpose |
|------|---------|
| `s3_auto_ranged_get.c` | GetObject. Discovers object size, then splits into parallel range-GET requests. |
| `s3_auto_ranged_put.c` | PutObject. Multipart upload: CreateMultipartUpload → UploadPart × N → CompleteMultipartUpload. |
| `s3_copy_object.c` | CopyObject. Multipart copy for large objects, single UploadPartCopy for small ones. |
| `s3_default_meta_request.c` | Pass-through. Sends a single HTTP request as-is. Used for operations the client does not optimize (e.g., CreateSession). |

## Memory

| File | Purpose |
|------|---------|
| `s3_buffer_pool.c` | Buffer pool vtable dispatch. Thin wrapper that forwards to the default implementation. |
| `s3_default_buffer_pool.c` | Default buffer pool implementation. Primary/secondary allocation, memory limiting, ticket reservation, and async waiting when the pool is full. |

## Checksums

| File | Purpose |
|------|---------|
| `s3_checksums.c` | Checksum algorithm implementations (CRC32, CRC32C, CRC64NVME, SHA1, SHA256, xxHash). |
| `s3_checksum_context.c` | Checksum context management: create, update, finalize for a given algorithm. |
| `s3_checksum_stream.c` | Wraps an input stream to compute a checksum as data flows through. |
| `s3_chunk_stream.c` | Wraps an input stream to produce aws-chunked transfer encoding with trailing checksums. |

## Streaming

| File | Purpose |
|------|---------|
| `s3_parallel_input_stream.c` | Parallel input stream for reading file parts at different offsets concurrently. |
| `s3_part_streaming_input_stream.c` | Wraps a parallel input stream to read a single part's range, used for file I/O streaming uploads. |

## HTTP Message Construction

| File | Purpose |
|------|---------|
| `s3_request_messages.c` | Builds HTTP messages for S3 operations: ranged GETs, UploadPart, CreateMultipartUpload, CompleteMultipartUpload, etc. |

## Pagination and Listing

| File | Purpose |
|------|---------|
| `s3_paginator.c` | Generic paginator for S3 list operations. |
| `s3_list_objects.c` | ListObjectsV2 response parsing. |
| `s3_list_parts.c` | ListParts response parsing (used for upload resume). |

## Utilities

| File | Purpose |
|------|---------|
| `s3.c` | Library init/cleanup, error registration, log subjects. |
| `s3_util.c` | Shared helpers: header parsing, XML parsing, part range calculation, content type detection. |
| `s3_platform_info.c` | EC2 instance type detection for auto-tuning throughput targets. |
| `s3express_credentials_provider.c` | S3 Express One Zone session credential management (CreateSession). |

## Key Headers

| File | Purpose |
|------|---------|
| `s3_client.h` (public) | Public client API: create client, make meta request, configuration structs. |
| `s3_client_impl.h` (private) | Client internals: `synced_data`, `threaded_data`, `stats`, vtable, scheduling functions. |
| `s3_meta_request_impl.h` (private) | Meta request internals: vtable, state enum, synced_data, event types. |
| `s3_request.h` (private) | Request struct: `meta_request` backlink, `request_tag`, send data, metrics. |
| `s3_buffer_pool.h` (public) | Buffer pool vtable interface. |
| `s3_default_buffer_pool.h` (private) | Default buffer pool internals: primary/secondary areas, ticket structs. |
