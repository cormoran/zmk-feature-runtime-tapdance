import { useCallback, useContext, useEffect, useRef, useState } from "react";
import {
  ZMKAppContext,
  useCustomSubsystem,
  useStudioLockState,
  isUnlockRequiredError,
} from "@cormoran/zmk-studio-react-hook";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import {
  Binding,
  Config,
  Request,
  Response,
} from "./proto/cormoran/runtime-tapdance/runtime_tapdance";

const IDENTIFIER = "cormoran__runtime_tapdance";
// Stable across renders: an inline codec would restart the initial loader.
const codec = {
  encode: (request: Request) => Request.encode(request).finish(),
  decode: Response.decode,
};
type BehaviorOption = { id: number; name: string };
const emptyBinding = () => Binding.create();
const uint32 = (value: number) =>
  Number.isInteger(value) && value >= 0 && value <= 0xffffffff;
function checked(response: Response | null): Response {
  if (response?.error)
    throw new Error(`${response.error.message} (${response.error.code})`);
  if (!response) throw new Error("Firmware returned no tapdance response");
  return response;
}

export function TapDanceEditor() {
  const app = useContext(ZMKAppContext);
  const connection = app?.state.connection;
  const { ready, subsystem, call } = useCustomSubsystem(IDENTIFIER, codec);
  const subsystemIndex = subsystem?.index;
  const { locked } = useStudioLockState();
  const [slots, setSlots] = useState<Record<number, Config>>({});
  const [names, setNames] = useState<Record<number, string>>({});
  const [visible, setVisible] = useState<number[]>([]);
  const [maxCount, setMaxCount] = useState(0);
  const [maxNameLength, setMaxNameLength] = useState(24);
  const [pool, setPool] = useState({ used: 0, total: 0 });
  const [newName, setNewName] = useState("");
  const [interval, setInterval] = useState(200);
  const [behaviors, setBehaviors] = useState<BehaviorOption[]>([]);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState("");
  const [error, setError] = useState("");
  const [awaitingUnlock, setAwaitingUnlock] = useState(false);
  const pending = useRef<Request[] | null>(null);
  const previousLocked = useRef(locked);
  const loadGeneration = useRef({ value: 0 });

  const load = useCallback(async () => {
    const generation = ++loadGeneration.current.value;
    setBusy(true);
    setError("");
    try {
      const global = checked(await call({ getGlobal: {} })).global;
      if (!global || global.apiVersion !== 2)
        throw new Error("Firmware does not support named tap dances");
      if (generation !== loadGeneration.current.value) return;
      const loaded: Record<number, Config> = {};
      const loadedNames: Record<number, string> = {};
      const ids: number[] = [];
      let cursor = 0;
      let hasCursor = false;
      for (;;) {
        const page = checked(
          await call({ list: { afterId: cursor, hasAfterId: hasCursor } })
        ).list;
        if (!page) throw new Error("Firmware returned no tap dance list");
        for (const item of page.entries) {
          const detail = checked(
            await call({ get: { index: item.index } })
          ).state;
          if (!detail?.config)
            throw new Error("Firmware returned no tap dance state");
          ids.push(item.index);
          loaded[item.index] = detail.config;
          loadedNames[item.index] = item.name;
        }
        if (!page.hasMore) break;
        if (page.entries.length === 0)
          throw new Error("Firmware returned an empty list page");
        cursor = page.nextAfterId;
        hasCursor = true;
      }
      if (generation !== loadGeneration.current.value) return;
      setSlots(loaded);
      setNames(loadedNames);
      setVisible(ids);
      setMaxCount(global.maxEntries);
      setMaxNameLength(global.maxNameLength);
      setPool({ used: global.poolBytesUsed, total: global.poolBytesTotal });
      setInterval(global.intervalMs);
      setAwaitingUnlock(false);
    } catch (reason) {
      if (generation !== loadGeneration.current.value) return;
      if (isUnlockRequiredError(reason)) setAwaitingUnlock(true);
      else
        setError(
          reason instanceof Error ? reason.message : "Could not load tap dances"
        );
    } finally {
      if (generation === loadGeneration.current.value) setBusy(false);
    }
  }, [call]);

  useEffect(() => {
    if (!ready || subsystemIndex === undefined) return;
    let active = true;
    const generationRef = loadGeneration.current;
    // Defer work so mount cleanup can cancel the initial pass (React StrictMode).
    void Promise.resolve().then(() => {
      if (active) void load();
    });
    return () => {
      active = false;
      generationRef.value++;
    };
  }, [ready, subsystemIndex, load]);

  useEffect(() => {
    if (!connection) return;
    let active = true;
    void Promise.resolve()
      .then(async () => {
        if (!active) return;
        const list = await call_rpc(connection, {
          behaviors: { listAllBehaviors: true },
        });
        if (!active) return;
        const options: BehaviorOption[] = [];
        for (const id of list?.behaviors?.listAllBehaviors?.behaviors ?? []) {
          if (!active) return;
          const result = await call_rpc(connection, {
            behaviors: { getBehaviorDetails: { behaviorId: id } },
          });
          options.push({
            id,
            name:
              result?.behaviors?.getBehaviorDetails?.displayName ||
              `Behavior ${id}`,
          });
        }
        if (active) setBehaviors(options);
      })
      .catch(() => {
        /* Numeric IDs remain available if metadata is unavailable. */
      });
    return () => {
      active = false;
    };
  }, [connection]);

  const send = useCallback(
    async (requests: Request | Request[]) => {
      const batch = Array.isArray(requests) ? requests : [requests];
      pending.current = batch;
      setBusy(true);
      setError("");
      setMessage("");
      let next = 0;
      try {
        for (; next < batch.length; next++) {
          checked(await call(batch[next]));
        }
        await load();
        setMessage(
          batch.some(
            (item) =>
              item.save ||
              item.delete ||
              item.remove ||
              item.set?.persist ||
              item.setInterval?.persist
          )
            ? "Saved to flash."
            : "Applied in RAM. Save to flash to keep changes after reboot."
        );
        setAwaitingUnlock(false);
        pending.current = null;
      } catch (reason) {
        pending.current = batch.slice(next);
        if (isUnlockRequiredError(reason)) setAwaitingUnlock(true);
        else setError(reason instanceof Error ? reason.message : "RPC failed");
      } finally {
        setBusy(false);
      }
    },
    [call, load]
  );

  const retry = useCallback(() => {
    if (pending.current) void send(pending.current);
    else void load();
  }, [send, load]);
  useEffect(() => {
    const wasLocked = previousLocked.current;
    previousLocked.current = locked;
    if (wasLocked && !locked && awaitingUnlock) retry();
  }, [locked, awaitingUnlock, retry]);

  if (!app) return null;
  if (subsystemIndex === undefined)
    return (
      <section className="card">
        <p>
          Subsystem "{IDENTIFIER}" not found. Make sure your firmware includes
          the runtime tap dance module. See the{" "}
          <a
            href="https://github.com/cormoran/zmk-feature-runtime-tapdance#readme"
            target="_blank"
            rel="noreferrer"
          >
            module README
          </a>
          .
        </p>
      </section>
    );
  const disabled = busy || locked || awaitingUnlock;
  const edit = (index: number, update: Partial<Config>) =>
    setSlots((current) => ({
      ...current,
      [index]: { ...current[index], ...update },
    }));
  const validate = (config: Config) => {
    if (!config.singleBinding?.behaviorId)
      return "Select a single-tap behavior for this dance.";
    if (config.tripleBinding?.behaviorId && !config.doubleBinding?.behaviorId)
      return "Select a double-tap behavior before adding a triple tap.";
    for (const binding of [
      config.singleBinding,
      config.doubleBinding,
      config.tripleBinding,
    ]) {
      if (
        binding &&
        ![binding.behaviorId, binding.param1, binding.param2].every(uint32)
      )
        return "Behavior IDs and parameters must be unsigned 32-bit integers.";
    }
    return "";
  };
  const write = (index: number, persist: boolean) => {
    const config = slots[index];
    if (!config) return;
    const issue = validate(config);
    if (issue) {
      setError(issue);
      return;
    }
    const requests: Request[] = [];
    const add = (tapCount: number, binding: Binding) =>
      requests.push({ set: { index, tapCount, binding, persist } });
    if (!config.tripleBinding?.behaviorId) add(3, emptyBinding());
    if (!config.doubleBinding?.behaviorId) add(2, emptyBinding());
    add(1, config.singleBinding!);
    if (config.doubleBinding?.behaviorId) add(2, config.doubleBinding);
    if (config.tripleBinding?.behaviorId) add(3, config.tripleBinding);
    void send(requests);
  };
  const changeInterval = (persist: boolean) => {
    if (!Number.isInteger(interval) || interval < 1 || interval > 2000) {
      setError("Tap interval must be an integer from 1 to 2000 ms.");
      return;
    }
    void send({ setInterval: { intervalMs: interval, persist } });
  };
  return (
    <section className="card">
      <h2>Runtime tap dances</h2>
      <p>
        Create a named tap dance and assign its ID as{" "}
        <code>&amp;rtd &lt;id&gt;</code>
        to a key. Single, double and triple taps invoke the selected behaviors.
        The last configured tap count resolves immediately; other counts wait
        for the interval or another key press.
      </p>
      <p>Apply changes in RAM. Save all writes pending changes to flash.</p>
      {locked && <p className="locked-banner">ZMK Studio is locked.</p>}
      {awaitingUnlock && (
        <div className="unlock-prompt">
          <p>
            Press <code>&amp;studio_unlock</code> on your keyboard. The request
            retries when Studio unlocks.
          </p>
          <button onClick={retry} disabled={busy || locked}>
            Retry
          </button>
        </div>
      )}
      {error && <p role="alert">{error}</p>}
      {message && <p role="status">{message}</p>}
      {busy && <p role="status">Communicating with keyboard…</p>}
      <fieldset disabled={disabled}>
        <legend>Global tap interval</legend>
        <label htmlFor="interval">Tap interval (ms)</label>{" "}
        <input
          id="interval"
          type="number"
          min="1"
          max="2000"
          value={Number.isNaN(interval) ? "" : interval}
          onChange={(event) => setInterval(event.target.valueAsNumber)}
        />
        <div className="editor-actions">
          <button onClick={() => changeInterval(false)}>Apply interval</button>
          <button onClick={() => changeInterval(true)}>Save interval</button>
        </div>
      </fieldset>
      <p>
        {visible.length} / {maxCount} tap dances · pool {pool.used} /{" "}
        {pool.total} bytes
      </p>
      <div className="editor-actions">
        <input
          aria-label="New tap dance name"
          maxLength={maxNameLength}
          value={newName}
          onChange={(event) => setNewName(event.target.value)}
        />
        <button
          disabled={disabled || !newName.trim() || visible.length >= maxCount}
          onClick={() => {
            void send({ create: { name: newName, persist: false } });
            setNewName("");
          }}
        >
          Add tap dance
        </button>
        <button disabled={disabled} onClick={() => void send({ save: {} })}>
          Save all
        </button>
        <button disabled={disabled} onClick={() => void send({ discard: {} })}>
          Discard unsaved
        </button>
      </div>
      {visible.map((index) => (
        <fieldset key={index} disabled={disabled} className="dance-slot">
          <legend>
            {names[index]} (ID {index})
          </legend>
          <p>
            Keymap binding: <code>&amp;rtd {index}</code>
          </p>
          <label>
            Name{" "}
            <input
              aria-label={`Tap dance ${index} name`}
              maxLength={maxNameLength}
              value={names[index] ?? ""}
              onChange={(event) =>
                setNames((current) => ({
                  ...current,
                  [index]: event.target.value,
                }))
              }
            />
          </label>
          <button
            onClick={() =>
              void send({
                rename: { index, name: names[index], persist: false },
              })
            }
          >
            Rename tap dance {index}
          </button>
          {(["singleBinding", "doubleBinding", "tripleBinding"] as const).map(
            (key) => (
              <BindingEditor
                key={key}
                label={`${key === "singleBinding" ? "Single" : key === "doubleBinding" ? "Double" : "Triple"} ${index}`}
                binding={slots[index][key] ?? emptyBinding()}
                options={behaviors}
                onChange={(binding) => edit(index, { [key]: binding })}
              />
            )
          )}
          <div className="editor-actions">
            <button onClick={() => write(index, false)}>
              Apply tap dance {index}
            </button>
            <button onClick={() => void send({ delete: { index } })}>
              Delete tap dance {index}
            </button>
          </div>
        </fieldset>
      ))}
      <div className="editor-actions">
        <button disabled={disabled} onClick={() => void load()}>
          Reload from keyboard
        </button>
      </div>
    </section>
  );
}

function BindingEditor({
  label,
  binding,
  options,
  onChange,
}: {
  label: string;
  binding: Binding;
  options: BehaviorOption[];
  onChange: (binding: Binding) => void;
}) {
  return (
    <div className="binding-editor">
      <h3>{label.split(" ")[0]} tap behavior</h3>
      {options.length > 0 && (
        <label>
          Behavior{" "}
          <select
            aria-label={`${label} behavior`}
            value={binding.behaviorId}
            onChange={(event) =>
              onChange(
                Number(event.target.value) === 0
                  ? emptyBinding()
                  : { ...binding, behaviorId: Number(event.target.value) }
              )
            }
          >
            <option value="0">None</option>
            {options.map((option) => (
              <option key={option.id} value={option.id}>
                {option.name} ({option.id})
              </option>
            ))}
            {binding.behaviorId !== 0 &&
              !options.some((option) => option.id === binding.behaviorId) && (
                <option value={binding.behaviorId}>
                  Unknown behavior ({binding.behaviorId})
                </option>
              )}
          </select>
        </label>
      )}
      <label>
        Behavior ID{" "}
        <input
          aria-label={`${label} behavior ID`}
          type="number"
          min="0"
          max="4294967295"
          value={Number.isNaN(binding.behaviorId) ? "" : binding.behaviorId}
          onChange={(event) =>
            onChange(
              event.target.valueAsNumber === 0
                ? emptyBinding()
                : { ...binding, behaviorId: event.target.valueAsNumber }
            )
          }
        />
      </label>
      <label>
        Parameter 1{" "}
        <input
          aria-label={`${label} parameter 1`}
          type="number"
          min="0"
          max="4294967295"
          value={Number.isNaN(binding.param1) ? "" : binding.param1}
          onChange={(event) =>
            onChange({ ...binding, param1: event.target.valueAsNumber })
          }
        />
      </label>
      <label>
        Parameter 2{" "}
        <input
          aria-label={`${label} parameter 2`}
          type="number"
          min="0"
          max="4294967295"
          value={Number.isNaN(binding.param2) ? "" : binding.param2}
          onChange={(event) =>
            onChange({ ...binding, param2: event.target.valueAsNumber })
          }
        />
      </label>
    </div>
  );
}
