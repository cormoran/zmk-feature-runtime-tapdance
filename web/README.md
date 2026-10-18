# Runtime tap dance Web UI

Connect a keyboard running this module over USB (Web Serial) or Bluetooth (Web
Bluetooth) in Chrome or Edge on HTTPS or localhost. Unlock Studio if prompted.

The editor loads all slots once per connection. Set the global tap interval
(1–2000 ms), then add a tap dance. The displayed slot number is the parameter
to assign in the keymap, for example `&rtd 0`.

Choose a single-tap behavior to activate a dance. Double and triple are
optional; triple requires double. The last configured tap count invokes its
action immediately. Other counts wait for the interval or another key press.
Behavior display names come from Studio metadata when available. Numeric IDs
remain editable; ID 0 means no behavior. Parameters use the same unsigned
values as Studio keymap bindings. Firmware validation errors keep your draft.

**Apply** changes the three bindings or interval in RAM. **Save** also writes
them to flash. Deletion has separate RAM and flash buttons. **Reload from keyboard**
replaces unsaved drafts with current device settings.

## Development

```sh
NPM_CONFIG_ALLOW_GIT=all npm ci
npm run generate
npm run dev
npm run lint
npm test -- --runInBand
VITE_BASE=/ npm run build
VITE_BASE=/zmk-feature-runtime-tapdance/ npm run build
```

The source proto is `../proto/cormoran/runtime-tapdance/runtime_tapdance.proto`;
`buf.gen.yaml` generates the TypeScript codecs. The codec is defined once and
loader dependencies use the scalar subsystem index to avoid reload loops.
`test/TapDanceEditor.spec.tsx` checks settled RPC/metadata request counts,
CRUD, integer validation, write errors and unlocking.

`e2e/rpc.spec.ts` runs the actual UI and Web Serial transport against real
firmware in Renode:

```sh
west zmk-web-e2e --elf build/web_e2e/zephyr/zmk.elf -- npm --prefix web run e2e
```
