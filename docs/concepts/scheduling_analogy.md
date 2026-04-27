# Scheduling — The Fulfillment Center Analogy

This document explains the aws-c-s3 client's scheduling architecture using an
Amazon Fulfillment Center as an analogy. Read this first, then see
[scheduling.md](../scheduling.md) for the technical details.

## The Building

The fulfillment center is the `aws_s3_client`. One building, one operation.

## The Shift Manager

There is exactly one Shift Manager. They sit at one desk and process work
sequentially — they never clone themselves. This is
`s_s3_client_process_work_default()`, which always runs on a single event loop
thread (`process_work_event_loop`). Because there is only one Shift Manager,
the desk (`threaded_data`) needs no lock.

The Shift Manager's desk is not in a separate room. At client init, one desk in
the networking department is designated: "you also handle scheduling now." That
employee still does normal dock work; they just also run the scheduling loop
when the buzzer rings.

## The Intake Window

There is a window at the front of the building with a slot. Anyone can walk up
and drop a slip of paper through the slot:

- "New customer order arrived" (a new meta request from a user thread)
- "Package is labeled and ready to ship" (a request finished preparing)
- "A truck just came back from delivery" (a request completed on the network)

The slot has a lock on it (the mutex) — only one person can drop a slip at a
time. After dropping the slip, they press a buzzer
(`schedule_process_work_synced`). If the buzzer is already ringing, pressing it
again does nothing.

The intake window is `synced_data`. The lock is `synced_data.lock`.

## The Shift Manager's Routine

When the buzzer rings, the Shift Manager walks to the window, grabs all the
slips at once, then goes back to their desk and works through them:

**Step 1 — Empty the intake window.** Lock the window only long enough to grab
the papers. This is the `synced_data` → `threaded_data` transfer. The Shift
Manager also resets the buzzer (`process_work_task_scheduled = false`) so it
works again while they continue working.

**Step 2 — Register new customer orders.** Each customer order is a meta
request — like "ship 500 books to Seattle." That is not one package; it is a
big order that will be broken into many individual packages (parts).

**Step 3 — Break orders into packages and load trucks.** For each active order,
ask: "What is the next package I can prepare?" The order's own logic decides.
Hand each package to a prep station (async prepare). Then assign prepped
packages to delivery trucks (HTTP connections).

Two passes: the first is conservative (each order type self-limits), the second
fills remaining capacity first-come-first-served.

**Step 4 — Update the whiteboard.** Write current stats.

**Step 5 — Check if we are closing down.** If all work is done and the owner
said "shut down," turn off the lights.

## Customer Orders (Meta Requests)

A customer order is not a pre-planned list of packages. It is a state machine
that produces packages one at a time when asked. The Shift Manager asks each
order: "Got anything for me?" Three possible answers:

- "Here is a package, and I have more." — produce one request, keep the order.
- "Nothing right now, but I am not done." — waiting for a response.
- "I am done." — remove the order from the tracking board.

Every package carries a label saying which order it belongs to
(`request->meta_request`). When a truck returns, the Shift Manager reads the
label and routes the result back to the right order.

## The Prep Station (Async Prepare)

When the Shift Manager hands a package to a prep station, a worker (on another
thread) does:

1. **Acquire a box** — reserve memory from the buffer pool. If the warehouse is
   full, wait until space frees up.
2. **Pack the box** — build the HTTP message. For PUT, read the body from the
   input stream. For GET, just write the Range header.
3. **Sign the shipping label** — SigV4 signing.

When done, the worker drops a "package ready" slip at the intake window and
presses the buzzer.

## The Loading Dock (Connections)

The Shift Manager assigns prepped packages to delivery trucks (HTTP
connections). Getting a truck involves: get a delivery permit (retry token),
then get the truck (HTTP connection from the endpoint's connection manager).
When the truck returns, the driver drops a slip at the intake window.

## Departments (Event Loop Groups)

**Networking Department (Bootstrap ELG)** — multiple dock bays, each staffed by
one worker handling a few trucks. The Shift Manager's desk is one desk in this
department.

**Customer Delivery Department (Body Streaming ELG)** — a separate wing for
delivering response contents to the customer (user callback). Isolated so slow
customers do not block the docks or the Shift Manager.

## The Stats Board (Atomics)

A digital counter board visible to everyone. Anyone can read or update it
without stopping to ask permission. The Shift Manager uses it for capacity
decisions.

## Capacity Limits

The Shift Manager checks before producing more work:

- **Total packages in the building** (`num_requests_in_flight` ≤ connections × 4).
- **Packages at the prep station** (`num_requests_being_prepared + request_queue_size` ≤ connections).
- **Per-order prep limit** (`num_request_being_prepared` per meta request ≤ that order's connection cap).
- **Trucks on the road** (`num_requests_network_io` ≤ `max_active_connections`).
- **Per-order trucks** (`num_requests_network` per meta request ≤ that order's connection cap).
- **PUT read-ahead** (`num_parts_pending_read` ≤ 5). Prevents wasting boxes on sequential reads.
- **DNS slow-start.** If the address is unknown, cap at 10 queued packages.
- **Memory limit** (buffer pool). The warehouse has a fixed size. The ultimate backstop.

## The Self-Sustaining Loop

The Shift Manager does not poll. The loop sustains itself through completions:

1. Shift Manager produces requests → hands to prep stations → returns to desk.
2. Prep finishes → buzzer rings → Shift Manager drains prepped requests, assigns
   to trucks, asks for more work.
3. Truck returns → buzzer rings → Shift Manager runs again, meta request state
   advances, produces next request.

This continues until all orders report "I am done."
