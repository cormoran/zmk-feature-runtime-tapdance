# Runtime tap dance design

## Invocation and recognition

The keymap invokes `&rtd <id>` through a one-parameter behavior. Its callback
starts or advances a sequence identified by physical position and split source.
The same ID can be bound to several positions and layers. The position-state
listener only resolves undecided dances on another key press and observes key
release if the keymap binding changes while the key is held. It never captures
or replays the ordinary keymap event. All state changes use one mutex and timer.

Each sequence snapshots the entry configuration, layer, source, and global interval
at its first press. An entry is active when its single binding is set. Double is optional;
triple requires double. The last configured count resolves on its press. Lesser
counts resolve at the interval deadline or when another key is pressed. The
chosen action receives the original position/layer/source and stays pressed
until the final physical release. A press at the exact deadline starts a new
sequence. An unallocated ID or empty single binding is a no-op.

## Settings and RPC

`cormoran__runtime_tapdance` stores `timeout_ms` and a persistent `next_id`
INT32 plus a keyspace of pooled BYTES values. Each key is `tapdance/<id>`;
its payload has a version byte, a name length, a name up to 24 UTF-8 bytes,
and three `(behavior_id,param1,param2)` tuples. The keyspace has at most 16
live entries and a 1024-byte shared pool by default. Create allocates a new
monotonic ID, even when other entries are deleted. The ID high-water mark is
saved before the entry is made visible; failed allocations may leave ID gaps.
Rename writes the same entry, preserving its ID. Delete erases the persisted
entry and frees its pool region. An empty binding set is a valid named entry.

Validate all bindings with ZMK metadata, require single before double, and
reject a triple action without a double. Edits affect only future sequences.
The runtime tap dance behavior itself is rejected as an action to avoid
recursive invocation. Get and Set address an ID; Set sends one binding. List
returns up to four entries per page in ID order. GetGlobal reports capacity,
pool use, name limit, interval, and dirty state without needing an entry.
Create/Rename/Set/SetInterval can edit RAM; Save persists pending edits and
Discard reverts saved edits and frees RAM-only new entries. Delete is immediate
and permanent, matching runtime macro. The high-water ID counter is excluded
from Save/Discard. RX/TX buffers are at least 256 bytes; response storage is
static. API version 2 in GetGlobal distinguishes this contract from the
earlier Get/Set-only firmware.

For existing saved slot settings, the legacy array stays registered only for
boot migration. A configured slot `N` is copied into a named entry ID `N`
before the legacy array is erased; the original `&rtd N` remains valid. The
legacy data remains intact on a migration failure so the next boot retries.

## UI and verification

The Web UI lists allocated names and IDs, three action editors, pool use, and
the global interval. It uses Studio behavior metadata and supports RAM apply,
Save, and Discard. Native-sim tests drive public keymap events and
check HID output. Renode tests round-trip settings over real Studio RPC, while
browser E2E uses the same firmware. ARM builds cover Studio, no RPC, and split.
