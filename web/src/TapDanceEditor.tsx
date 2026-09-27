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
  type State,
} from "./proto/cormoran/runtime-tapdance/runtime_tapdance";

const IDENTIFIER = "cormoran__runtime_tapdance";
// Stable across renders: an inline codec would restart the initial loader.
const codec = {
  encode: (request: Request) => Request.encode(request).finish(),
  decode: Response.decode,
};
type BehaviorOption = { id: number; name: string };
const emptyBinding = () => Binding.create();
const emptyConfig = () =>
  Config.create({
    doubleBinding: emptyBinding(),
    tripleBinding: emptyBinding(),
  });
const uint32 = (value: number) =>
  Number.isInteger(value) && value >= 0 && value <= 0xffffffff;
function stateOf(response: Response | null): State {
  if (response?.error)
    throw new Error(`${response.error.message} (${response.error.code})`);
  if (!response?.state?.config)
    throw new Error("Firmware returned no tapdance state");
  return response.state;
}
function used(config: Config) {
  return (
    config.enabled ||
    config.position !== 0 ||
    config.delaySingle ||
    (config.doubleBinding?.behaviorId ?? 0) !== 0 ||
    (config.tripleBinding?.behaviorId ?? 0) !== 0
  );
}

export function TapDanceEditor() {
  const app = useContext(ZMKAppContext);
  const connection = app?.state.connection;
  const { ready, subsystem, call } = useCustomSubsystem(IDENTIFIER, codec);
  const subsystemIndex = subsystem?.index;
  const { locked } = useStudioLockState();
  const [slots, setSlots] = useState<Config[]>([]);
  const [visible, setVisible] = useState<number[]>([]);
  const [interval, setInterval] = useState(200);
  const [behaviors, setBehaviors] = useState<BehaviorOption[]>([]);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState("");
  const [error, setError] = useState("");
  const [awaitingUnlock, setAwaitingUnlock] = useState(false);
  const pending = useRef<Request | null>(null);
  const previousLocked = useRef(locked);
  const loadGeneration = useRef({ value: 0 });

  const load = useCallback(async () => {
    const generation = ++loadGeneration.current.value;
    setBusy(true);
    setError("");
    try {
      const first = stateOf(await call({ get: { index: 0 } }));
      if (first.maxCount < 1 || first.maxCount > 64)
        throw new Error("Invalid firmware slot limit");
      if (generation !== loadGeneration.current.value) return;
      const loaded = [first.config!];
      for (let index = 1; index < first.maxCount; index++) {
        if (generation !== loadGeneration.current.value) return;
        loaded.push(stateOf(await call({ get: { index } })).config!);
      }
      if (generation !== loadGeneration.current.value) return;
      setSlots(loaded);
      setVisible(
        loaded.flatMap((config, index) => (used(config) ? [index] : []))
      );
      setInterval(first.intervalMs);
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
    async (request: Request) => {
      pending.current = request;
      setBusy(true);
      setError("");
      setMessage("");
      try {
        const state = stateOf(await call(request));
        if (request.set || request.remove) {
          setSlots((current) =>
            current.map((config, index) =>
              index === state.index ? state.config! : config
            )
          );
        }
        if (request.remove)
          setVisible((current) =>
            current.filter((index) => index !== state.index)
          );
        if (request.setInterval) setInterval(state.intervalMs);
        const persist =
          request.set?.persist ||
          request.remove?.persist ||
          request.setInterval?.persist;
        setMessage(
          persist
            ? "Saved to flash."
            : "Applied in RAM. Save to flash to keep changes after reboot."
        );
        setAwaitingUnlock(false);
        pending.current = null;
      } catch (reason) {
        if (isUnlockRequiredError(reason)) setAwaitingUnlock(true);
        else setError(reason instanceof Error ? reason.message : "RPC failed");
      } finally {
        setBusy(false);
      }
    },
    [call]
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
    setSlots((current) =>
      current.map((config, slot) =>
        slot === index ? { ...config, ...update } : config
      )
    );
  const validate = (config: Config) => {
    if (!uint32(config.position))
      return "Position must be a non-negative integer.";
    for (const binding of [config.doubleBinding, config.tripleBinding]) {
      if (
        binding &&
        ![binding.behaviorId, binding.param1, binding.param2].every(uint32)
      )
        return "Behavior IDs and parameters must be unsigned 32-bit integers.";
    }
    return "";
  };
  const write = (index: number, persist: boolean) => {
    const issue = validate(slots[index]);
    if (issue) {
      setError(issue);
      return;
    }
    void send({ set: { index, config: slots[index], persist } });
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
        Double taps wait for a possible third tap; triple taps take precedence.
        Unassigned dances fall back to normal keys. Positions are zero-based
        physical key positions.
      </p>
      <p>
        Delayed mode waits before sending a single normal key. Immediate mode
        sends every normal key immediately and adds the dance output.
      </p>
      <p>Apply changes RAM only. Save writes the selected setting to flash.</p>
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
        {visible.length} / {slots.length} tap dances
      </p>
      {visible.map((index) => (
        <fieldset key={index} disabled={disabled} className="dance-slot">
          <legend>Tap dance {index + 1}</legend>
          <label>
            <input
              type="checkbox"
              checked={slots[index].enabled}
              onChange={(event) =>
                edit(index, { enabled: event.target.checked })
              }
            />{" "}
            Enabled
          </label>
          <label>
            Position{" "}
            <input
              aria-label={`Position ${index + 1}`}
              type="number"
              min="0"
              value={
                Number.isNaN(slots[index].position) ? "" : slots[index].position
              }
              onChange={(event) =>
                edit(index, { position: event.target.valueAsNumber })
              }
            />
          </label>
          <label>
            Normal key timing{" "}
            <select
              aria-label={`Timing ${index + 1}`}
              value={slots[index].delaySingle ? "delayed" : "immediate"}
              onChange={(event) =>
                edit(index, { delaySingle: event.target.value === "delayed" })
              }
            >
              <option value="delayed">Delayed single key</option>
              <option value="immediate">Immediate normal keys + dance</option>
            </select>
          </label>
          {(["doubleBinding", "tripleBinding"] as const).map((key) => (
            <BindingEditor
              key={key}
              label={`${key === "doubleBinding" ? "Double" : "Triple"} ${index + 1}`}
              binding={slots[index][key] ?? emptyBinding()}
              options={behaviors}
              onChange={(binding) => edit(index, { [key]: binding })}
            />
          ))}
          <div className="editor-actions">
            <button onClick={() => write(index, false)}>
              Apply tap dance {index + 1}
            </button>
            <button onClick={() => write(index, true)}>
              Save tap dance {index + 1}
            </button>
            <button
              onClick={() => void send({ remove: { index, persist: false } })}
            >
              Delete tap dance {index + 1} (RAM)
            </button>
            <button
              onClick={() => void send({ remove: { index, persist: true } })}
            >
              Delete tap dance {index + 1} (flash)
            </button>
          </div>
        </fieldset>
      ))}
      <div className="editor-actions">
        <button
          disabled={disabled || visible.length >= slots.length}
          onClick={() => {
            const index = slots.findIndex((_, slot) => !visible.includes(slot));
            if (index < 0) return;
            edit(index, { ...emptyConfig(), enabled: true, delaySingle: true });
            setVisible((current) => [...current, index].sort((a, b) => a - b));
            setMessage("");
          }}
        >
          Add tap dance
        </button>
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
            <option value="0">None (normal keys)</option>
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
