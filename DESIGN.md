# Runtime tap dance design

## Invocation and recognition

The keymap invokes `&rtd <slot>` through a one-parameter behavior. Its callback
starts or advances a sequence identified by physical position and split source.
The same slot can be bound to several positions and layers. The position-state
listener only resolves undecided dances on another key press and observes key
release if the keymap binding changes while the key is held. It never captures
or replays the ordinary keymap event. All state changes use one mutex and timer.

Each sequence snapshots the slot configuration, layer, source, and global interval
at its first press. A slot is active when its single binding is set. Double is optional;
triple requires double. The last configured count resolves on its press. Lesser
counts resolve at the interval deadline or when another key is pressed. The
chosen action receives the original position/layer/source and stays pressed
until the final physical release. A press at the exact deadline starts a new
sequence. An empty slot is a no-op.

## Settings and RPC

`cormoran__runtime_tapdance` stores a global `timeout_ms` INT32 and an array
of slot BYTES values in custom-settings. A version-3 packed slot is 37 bytes:
version, then three `(behavior_id,param1,param2)` tuples. Validate all
bindings with ZMK metadata, require single before double, and reject a triple
action without a double. Slot updates affect only future
sequences. The runtime tap dance behavior itself is rejected as an action to
avoid recursive invocation. The module's custom RPC exposes get/remove per slot,
set for one tap-count binding, and a global interval setter. Its responses include all three bindings and
the slot limit.
RX/TX buffers are at least 256 bytes; response storage is static.

## UI and verification

The Web UI lists slot numbers, three action editors, and the global interval.
It uses Studio behavior metadata, supports RAM apply and flash save, and keeps
drafts after failed writes. Native-sim tests drive public keymap events and
check HID output. Renode tests round-trip settings over real Studio RPC, while
browser E2E uses the same firmware. ARM builds cover Studio, no RPC, and split.
