# zmk-feature-runtime-tapdance

Configure single-, double-, and triple-tap behaviors at runtime through a Web UI.
Place `&rtd <id>` in the ZMK keymap, then edit that named tap dance's actions without
rebuilding firmware. An ID can be shared by several keys and used on different
layers. The module uses the unofficial custom Studio protocol in cormoran's ZMK
fork and persists named entries through `zmk-feature-custom-settings`' shared pool.

Built from [cormoran's custom Studio RPC module template](https://github.com/cormoran/zmk-module-template-with-custom-studio-rpc). <!-- zmk-module-template:keep -->

## Install

Add the module and custom Studio ZMK fork to your keyboard's West manifest.

```yaml
manifest:
  remotes:
    - name: cormoran
      url-base: https://github.com/cormoran
  projects:
    - name: zmk-feature-runtime-tapdance
      remote: cormoran
      revision: main
      import: true
    - name: zmk
      remote: cormoran
      revision: main+custom-studio-protocol
      import:
        file: app/west.yml
```

Enable the module and Studio in your keyboard's `.conf`:

```conf
CONFIG_ZMK_RUNTIME_TAPDANCE=y
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_RUNTIME_TAPDANCE_STUDIO_RPC=y
CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y
CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=256
CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=256
CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048
```

Build with `studio-rpc-usb-uart` for USB Web Serial. Include an
`&studio_unlock` key binding if Studio locking is enabled. On a split keyboard,
enable this module on the central; the central receives key events from both
halves. The peripheral does not need it.

Include the behavior definition and assign ID 0 to a key:

```dts
#include <behaviors.dtsi>
#include <behaviors/runtime_tapdance.dtsi>

/ {
    keymap {
        compatible = "zmk,keymap";
        default_layer {
            bindings = <&rtd 0 &kp B>;
        };
    };
};
```

An unallocated ID produces no key action. Create a named tap dance and use the
ID returned by Studio; the first ID on a fresh device is 0. Only behaviors
present in the firmware can be selected; `&rtd` itself is rejected as an action
to prevent recursive dances. `CONFIG_ZMK_RUNTIME_TAPDANCE_MAX_TAPDANCES`
sets the maximum number of simultaneous named entries (default 16). Entries
allocate and release bytes from `CONFIG_ZMK_RUNTIME_TAPDANCE_POOL_BYTES`
(default 1024), and names are limited to 24 UTF-8 bytes by default.
`CONFIG_ZMK_RUNTIME_TAPDANCE_MAX_ACTIVE`
sets the maximum number of keys that can dance simultaneously (default 16).

## Configure from the browser

Run `cd web && npm ci && npm run dev`, then open the displayed URL in Chrome or
Edge and connect to the keyboard. The firmware also advertises the project's Web
UI URL. Add a named tap dance, note the ID returned by the firmware, and place
that ID in `&rtd <id>` in the keymap or through Studio keymap editing. IDs do not
change on rename or when another tap dance is deleted. Deleted IDs are inert and
are never reused.

Choose a **single-tap behavior** to activate an entry. Double and triple
behaviors are optional, but triple requires double. For example, select **Key
Press** for all three and set parameter 1 to `458756` (`&kp A`), `458757`
(`&kp B`), and `458758` (`&kp C`); parameter 2 is `0`. The firmware checks
behavior IDs and parameters against ZMK metadata. Apply writes each binding to
RAM; Save persists pending edits to flash. Discard restores saved values and
removes entries created only in RAM. Delete erases an entry immediately from RAM
and flash. The ID counter is saved on every Create, including RAM-only creates,
so Discard never reuses an ID. The global
interval defaults to 200 ms and accepts 1–2000 ms.

Module values also appear in the custom-settings subsystem for import/export.
Behavior IDs are local to the firmware's behavior map; reuse exports with the
same mapping.

On the first boot after upgrading from the slot-based version of this PR,
configured legacy slot `N` is copied to a named entry with the same ID `N` and
name `Tapdance N`. Existing `&rtd N` bindings continue to refer to it. Empty
legacy slots are skipped. Once all entries are migrated, the old array setting
is erased. If the shared pool cannot hold the migration, the old setting is
retained so migration can be retried after increasing the pool size.

## Recognition rules

The interval runs from each press to the next press. A release must occur
between taps. A press exactly at the deadline starts a new sequence. The last
configured tap count resolves immediately: with single and double configured,
the second press invokes double; with triple configured, double waits for a
possible third press. Another key press resolves any undecided dances first.

The selected action stays pressed while the final physical tap is held, and
releases with that tap. A single held key resolves when its interval expires.
Each active sequence keeps its starting configuration, even if the slot is edited
while it runs. Sequences are tracked by physical position and split input source,
so several keys can share one ID. Keymap layers choose where `&rtd` is active.

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

Firmware tests assert emitted normal-key and dance-action events. RPC writes
and validation are exercised through real firmware in Renode; browser E2E tests
connect the real UI to that firmware. The BLE fixture exercises settings RPC
while a split peripheral is connected.

```sh
west zmk-renode-test tests/renode --mode wired-split \
  --elf build/usb_wired_central/zephyr/zmk.elf \
  --peripheral-elf build/usb_wired_peripheral/zephyr/zmk.elf
west zmk-web-e2e --elf build/web_e2e/zephyr/zmk.elf -- npm --prefix web run e2e
```
