# Runtime tapdance

## Scope and semantics

A central-side position-state listener recognizes repeated press/release cycles at a
configured physical key position, without replacing the keymap binding. Up to 16
independent slots are configurable at runtime; enabled positions must be unique.
The default global interval is 200 ms (valid 1–2000 ms), measured from each press
to the next press; a press at the exact deadline starts a new sequence. A release
must occur between presses. Triple is decided on the third press; double waits
for the second press's deadline so a triple takes precedence.

Each slot selects immediate or delayed normal keys. Immediate mode bubbles every
normal press/release, then also invokes the matched binding. Delayed mode captures
the position events: a single is replayed after the deadline, preserving balanced
press/release; multi-taps suppress normal keys. An unassigned double/triple falls
back to replaying captured normal keys. A long first hold becomes a normal held
key at its deadline and its later release bubbles. A matched held dance binding
releases when the physical key releases. Other positions continue normally; the
module does not alter layers, existing combos or compile-time tapdance policies.
Actions use dedicated virtual positions (0x40000000 + slot) so immediate normal
hold-tap behaviors cannot alias action hold-taps. Preserve the physical event
source in action calls. Active sequences snapshot their configuration; edits affect subsequent sequences.
Timers and listener access must be serialized. Captured replay resumes after this
listener to avoid recursive recognition. Preserve event source for split positions.

## Runtime API and persistence

`include/cormoran/runtime_tapdance/runtime_tapdance.h` exposes:
- config: enabled, position, delay_single, double_binding/triple_binding (custom
  settings behavior values containing behavior_id, param1, param2).
- read(index, config), write(index, config, persist), delete(index, persist),
  timeout_ms(), set_timeout_ms(value, persist), max_count().

Register timeout_ms (INT32) and slots (BYTES array) with custom-settings under
`cormoran__runtime_tapdance`. A versioned packed slot contains the two behavior
local IDs and parameters. Validate positions, duplicate enabled positions,
behavior IDs/parameters and bounds before writes. Empty binding ID 0 means absent.
Persistence uses the custom-settings registry, loaded lazily when the first
position event occurs (settings_load happens after SYS_INIT). No independent
settings backend. Generic settings changes must undergo validation before use.
Settings can be imported/exported through the existing settings subsystem.

## RPC

Subsystem `cormoran__runtime_tapdance` uses the template registration/codec.
Proto Request oneof: get(index), set(index, config, persist), remove(index,
persist), set_interval(interval_ms,persist). Response oneof: state or error.
State contains index, config, interval_ms, max_count. Config has enabled,
position, delay_single, double_binding and triple_binding. Binding contains
uint32 behavior_id,param1,param2. Error contains int32 code,string message.
Every config/binding submessage sets nanopb has_*; messages stay under 128 bytes;
configure RX/TX buffers to at least 256 bytes and compile-time assert fit.
Writes reject invalid configuration and return errors. Read/write return current
slot and globals; the UI loads slots one at a time. No bulk messages/notifications.

## Web

Connection controls from template. One stable protobuf codec and effects depend
on scalar subsystem index. Load global state and slots once per connection.
Provide interval input and a list of enabled/disabled entries with editable
position, delayed/immediate mode and double/triple behavior ID + both parameters.
Use Studio behavior metadata for user-friendly behavior selection when available.
Add/delete entries; Apply vs Save distinguish RAM from persistent settings.
Explain immediate additive output, delayed keys, triple precedence and fallback.
Use bounded integer validation and surface RPC errors without losing draft input.

## Validation and phases

A: initialize template, test it, commit.
B: implement settings/API and position listener, paired native_sim tests observing
normal keycode events and behavior output (single/hold/double/triple, absent
binding fallback, immediate, disabled, exact deadline, simultaneous positions,
configuration edit mid-sequence, invalid IDs/positions/duplicates).
C: proto/handler/Web and mock RPC UI tests including loader call count regression,
add/edit/delete, validation and failed RPC; update Renode RPC and Web E2E cases.
D: build USB and split targets, run native_sim, web lint/test/root+Pages builds,
pre-commit, Renode RPC/E2E where available; push PR and follow CI to completion.
Build directories belong to this worktree, never the profile-wide build directory.
