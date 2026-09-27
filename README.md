# zmk-feature-runtime-tapdance

Configure double- and triple-tap actions from a browser without rebuilding your
ZMK keymap. The module listens to key-position changes and supports multiple
independent tap dances, with a global recognition interval and a per-position
choice of immediate or delayed normal keys.

Built from [cormoran's custom Studio RPC module template](https://github.com/cormoran/zmk-module-template-with-custom-studio-rpc). <!-- zmk-module-template:keep -->
The Web UI requires the unofficial custom Studio protocol in cormoran's ZMK fork.

## Install

Add these projects to your keyboard config's West manifest. Until the
implementation PR is merged, select `codex/runtime-tapdance` for this module;
after merging, use `main`.

```yaml
manifest:
  remotes:
    - name: cormoran
      url-base: https://github.com/cormoran
  projects:
    - name: zmk-feature-runtime-tapdance
      remote: cormoran
      revision: codex/runtime-tapdance
      import: true
    - name: zmk
      remote: cormoran
      revision: main+custom-studio-protocol
      import:
        file: app/west.yml
```

Enable the feature and Studio in your keyboard's `.conf`:

```conf
CONFIG_ZMK_RUNTIME_TAPDANCE=y
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_RUNTIME_TAPDANCE_STUDIO_RPC=y
CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y
CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=256
CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=256
CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048
```

Build with the `studio-rpc-usb-uart` snippet for USB Web Serial. Include an
`&studio_unlock` key binding and unlock Studio when prompted. The feature selects
the custom-settings persistence backend and behavior metadata/IDs it needs.
For a split keyboard, configure the module on the **central**: it also sees
positions originating from the peripheral. No extra peripheral event relay is
required. Keep the module disabled on the peripheral.

No special tap-dance binding or devicetree node is needed: your existing keymap
provides each position's normal single-tap behavior. Only behaviors included in
the firmware can be assigned at runtime.

## Configure from the browser

Run the UI locally with `cd web && npm ci && npm run dev`, then open the displayed
localhost URL in a Chromium browser. Connect USB or Bluetooth as supported by
your browser and keyboard. The firmware advertises the project's Pages UI URL;
GitHub Pages deployment is available after merging into `main` and enabling
Pages with GitHub Actions as its source.

1. Set the global interval (1–2000 ms; default 200 ms).
2. Add a tap dance and choose a zero-based key position. Positions are the
   physical-layout/keymap indexes, including the central's offset for split keys.
3. Assign a double-tap and/or triple-tap behavior and its parameters.
4. Choose **Delay normal keys** or **Immediate normal keys** for this entry.
5. Apply changes to RAM for testing, or save them to persist across reboot.

For example, leave position 0's normal binding as `&kp A`, then assign **Key
Press** to its double and triple actions. Use parameter 1 `458757` (`0x70005`,
keyboard B) for double and `458758` (`0x70006`, keyboard C) for triple; parameter
2 is `0`. In delayed mode this produces normal A for a single, B for a double,
and C for a triple. Parameters are numeric ZMK binding values; the firmware
validates them against the selected behavior's metadata.

An enabled position must be unique. Additional actions use a separate virtual
position so their hold-tap state is independent of the normal key. There are 16
slots by default; adjust
`CONFIG_ZMK_RUNTIME_TAPDANCE_MAX_TAPDANCES` at build time if needed.
Deleting a saved entry restores its normal keymap behavior. Changes made only
in RAM disappear on reboot. Module values are also registered in the common
custom-settings subsystem for settings import/export. Behavior IDs in a slot
refer to the connected keyboard; reuse exports with the same behavior mapping.

## Recognition rules

The interval runs from each press to the next press. Each counted tap must
include a release before the next press. A press exactly at the deadline starts
a new sequence. Two presses wait until the deadline to distinguish a double
from a triple; the third press triggers the triple immediately.

| Mode      | Single tap                                         | Double/triple tap                                     |
| --------- | -------------------------------------------------- | ----------------------------------------------------- |
| Delayed   | Replay the normal press/release after the interval | Suppress normal keys and invoke the assigned behavior |
| Immediate | Send every normal press/release immediately        | Also invoke the assigned behavior                     |

If the matching double/triple action is unassigned, delayed mode replays the
normal keys for that sequence. A long first hold becomes a normal held key when
the interval expires, then releases normally. A recognized action stays pressed
while its final physical tap is held and releases with that tap. Other key
positions continue normally. Active sequences keep their original settings, so
editing or deleting an entry during a held key does not leave it stuck.

Bindings that change layers follow ordinary ZMK behavior semantics. Delayed
normal keys reach the keymap when replayed, so intervening layer changes can
change their normal action. ZMK's limit of one undecided hold-tap still applies;
an action returning an error falls back to normal keys in delayed mode. This
module does not promise composition with other position-event consumers such as
compile-time combos or tap dances.

## Development and tests

See [DESIGN.md](DESIGN.md) for the firmware/API contract and
[web/README.md](web/README.md) for UI development.

For a standalone checkout, run `bash scripts/setup_workspace.sh` to install the
complete isolated test manifest. In `zmk-workspace`, use a compatible shared West
profile and place the Git worktree inside it; do not initialize/update West from
the worktree. Build results are local to the module worktree.

```sh
python3 -m unittest -v
west zmk-build tests/zmk-config -m . -d ./build -q
west zmk-test tests -m . -d ./build/native-tests
cd web
npm ci
npm run generate
npm run lint
npm test -- --runInBand
VITE_BASE=/ npm run build
VITE_BASE=/zmk-feature-runtime-tapdance/ npm run build
```

Firmware tests assert emitted normal-key and dance-action events. RPC persistence
and validation are exercised through real firmware in Renode; browser E2E tests
connect the real UI to that firmware. The BLE fixture exercises settings RPC
while a split peripheral is connected.

```sh
west zmk-renode-test tests/renode --mode wired-split \
  --elf build/usb_wired_central/zephyr/zmk.elf \
  --peripheral-elf build/usb_wired_peripheral/zephyr/zmk.elf
west zmk-web-e2e --elf build/web_e2e/zephyr/zmk.elf -- npm --prefix web run e2e
```
