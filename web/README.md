# Runtime tap dance Web UI

Connect a keyboard running this module over USB (Web Serial) or Bluetooth (Web
Bluetooth), using Chrome or Edge on HTTPS or localhost. Studio must be enabled
in the firmware. If Studio is locked, press the keyboard's `&studio_unlock` key
and retry. The page attempts one reconnect to a previously paired serial port.

The editor loads all firmware slots once per connection. Set the global tap
interval (1–2000 ms), then **Add tap dance** and choose a zero-based physical key
position. Multiple enabled dances must use different positions.

For each dance, choose delayed single keys or immediate normal keys. Delayed
mode waits for the interval before sending a single normal key. Immediate mode
sends every normal key immediately and adds the double/triple behavior output.
Double waits for a possible triple; triple takes precedence. If the recognized
dance has no behavior, its normal keys are sent instead.

Choose double and triple behaviors by their Studio display name when metadata
is available. Numeric behavior IDs remain editable as a fallback; ID 0 means no
behavior. Parameters use the same unsigned values as Studio behavior bindings.
The firmware validates positions, duplicate positions, behavior IDs and
parameters. Errors preserve your draft.

**Apply** changes the selected slot or interval in RAM. **Save** also stores it
in flash for the next boot. Deletion has separate RAM and flash buttons.
**Reload from keyboard** replaces unsaved drafts with current device settings.

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
