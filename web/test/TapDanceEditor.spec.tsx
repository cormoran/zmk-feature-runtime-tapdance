import { StrictMode } from "react";
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { TapDanceEditor } from "../src/TapDanceEditor";
import {
  Binding,
  Config,
  Request,
  Response,
} from "../src/proto/cormoran/runtime-tapdance/runtime_tapdance";
import { LockState } from "@zmkfirmware/zmk-studio-ts-client/core";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  create_rpc_connection: jest.fn(),
  call_rpc: jest.fn(),
}));

const client = jest.mocked(call_rpc);
const blank = () =>
  Config.create({
    singleBinding: Binding.create(),
    doubleBinding: Binding.create(),
    tripleBinding: Binding.create(),
  });
type Entry = { name: string; config: Config; dirty: boolean };
let entries: Record<number, Entry>;
let nextId: number;
let interval: number;
let requests: Request[];

function mount() {
  const app = createConnectedMockZMKApp({
    subsystems: ["cormoran__runtime_tapdance"],
  });
  return render(
    <StrictMode>
      <ZMKAppProvider value={app}>
        <TapDanceEditor />
      </ZMKAppProvider>
    </StrictMode>
  );
}

beforeEach(() => {
  jest.clearAllMocks();
  entries = {};
  nextId = 0;
  interval = 200;
  requests = [];
  client.mockImplementation(async (_connection, request) => {
    if (request.core?.getLockState)
      return {
        core: { getLockState: LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED },
      };
    if (request.behaviors?.listAllBehaviors)
      return { behaviors: { listAllBehaviors: { behaviors: [1] } } };
    if (request.behaviors?.getBehaviorDetails)
      return {
        behaviors: {
          getBehaviorDetails: {
            id: 1,
            displayName: "Key Press",
            metadata: [],
          },
        },
      };
    if (!request.custom?.call) throw new Error("Unexpected request");
    const inner = Request.decode(request.custom.call.payload);
    requests.push(inner);
    const response: Response = {};
    const ids = Object.keys(entries)
      .map(Number)
      .sort((a, b) => a - b);
    if (inner.getGlobal || inner.setInterval) {
      if (inner.setInterval) interval = inner.setInterval.intervalMs;
      response.global = {
        intervalMs: interval,
        maxEntries: 3,
        usedEntries: ids.length,
        maxNameLength: 24,
        poolBytesTotal: 256,
        poolBytesUsed: ids.length * 48,
        hasUnsavedChanges: ids.some((id) => entries[id].dirty),
        apiVersion: 3,
      };
    } else if (inner.list) {
      const selected = ids.filter(
        (id) => !inner.list?.hasAfterId || id > inner.list.afterId
      );
      response.list = {
        entries: selected.slice(0, 4).map((id) => ({
          index: id,
          name: entries[id].name,
          hasUnsavedChanges: entries[id].dirty,
        })),
        hasMore: selected.length > 4,
        nextAfterId: selected[Math.min(selected.length, 4) - 1] ?? 0,
      };
    } else if (inner.create) {
      const id = nextId++;
      entries[id] = { name: inner.create.name, config: blank(), dirty: true };
      response.state = {
        index: id,
        name: entries[id].name,
        config: entries[id].config,
        intervalMs: interval,
        maxCount: 3,
        hasUnsavedChanges: true,
      };
    } else if (inner.rename || inner.set || inner.setTiming || inner.get) {
      const id =
        inner.rename?.index ??
        inner.set?.index ??
        inner.setTiming?.index ??
        inner.get?.index ??
        0;
      const entry = entries[id];
      if (!entry)
        response.error = { code: -2, message: "Tap dance ID does not exist" };
      else {
        if (inner.rename) entry.name = inner.rename.name;
        if (inner.set) {
          const key = (
            inner.set.hold
              ? [
                  "",
                  "singleHoldBinding",
                  "doubleHoldBinding",
                  "tripleHoldBinding",
                ]
              : ["", "singleBinding", "doubleBinding", "tripleBinding"]
          )[inner.set.tapCount] as keyof Config;
          entry.config = { ...entry.config, [key]: inner.set.binding };
        }
        if (inner.setTiming) {
          const key = [
            "danceIntervalMs",
            "singleIntervalMs",
            "doubleIntervalMs",
            "tripleIntervalMs",
          ][inner.setTiming.tapCount] as keyof Config;
          entry.config = { ...entry.config, [key]: inner.setTiming.intervalMs };
        }
        if (inner.set || inner.setTiming || inner.rename) entry.dirty = true;
        response.state = {
          index: id,
          name: entry.name,
          config: entry.config,
          intervalMs: interval,
          maxCount: 3,
          hasUnsavedChanges: entry.dirty,
        };
      }
    } else if (inner.delete || inner.save || inner.discard) {
      if (inner.delete) delete entries[inner.delete.index];
      if (inner.save)
        Object.values(entries).forEach((entry) => (entry.dirty = false));
      if (inner.discard)
        Object.keys(entries).forEach((id) => {
          if (entries[Number(id)].dirty) delete entries[Number(id)];
        });
      response.status = { affectedCount: 1 };
    }
    return {
      custom: {
        call: {
          subsystemIndex: 0,
          payload: Response.encode(response).finish(),
        },
      },
    };
  });
});

it("creates named entries, edits one binding at a time, saves and deletes", async () => {
  mount();
  await waitFor(() =>
    expect(screen.getByText(/0 \/ 3 tap dances/)).toBeInTheDocument()
  );
  expect(requests[0].getGlobal).toBeDefined();
  await userEvent.type(screen.getByLabelText("New tap dance name"), "Alpha");
  await userEvent.click(screen.getByRole("button", { name: "Add tap dance" }));
  await waitFor(() =>
    expect(screen.getByText("Alpha (ID 0)")).toBeInTheDocument()
  );
  expect(screen.getByText("&rtd 0")).toBeInTheDocument();
  await userEvent.selectOptions(
    screen.getByLabelText("Single 0 behavior"),
    "1"
  );
  await userEvent.click(
    screen.getByRole("button", { name: "Apply tap dance 0" })
  );
  await waitFor(() =>
    expect(requests.some((item) => item.set?.tapCount === 1)).toBe(true)
  );
  expect(
    requests.filter((item) => item.set).every((item) => item.set?.binding)
  ).toBe(true);
  await userEvent.click(screen.getByRole("button", { name: "Save all" }));
  await waitFor(() => expect(requests.some((item) => item.save)).toBe(true));
  await userEvent.clear(screen.getByLabelText("Tap dance 0 name"));
  await userEvent.type(screen.getByLabelText("Tap dance 0 name"), "Renamed");
  await userEvent.click(
    screen.getByRole("button", { name: "Rename tap dance 0" })
  );
  await waitFor(() =>
    expect(screen.getByText("Renamed (ID 0)")).toBeInTheDocument()
  );
  await userEvent.click(
    screen.getByRole("button", { name: "Delete tap dance 0" })
  );
  await waitFor(() =>
    expect(screen.getByText(/0 \/ 3 tap dances/)).toBeInTheDocument()
  );
});

it("uses a stable firmware-issued ID after deletion", async () => {
  entries[0] = { name: "Old", config: blank(), dirty: false };
  nextId = 1;
  mount();
  await waitFor(() =>
    expect(screen.getByText("Old (ID 0)")).toBeInTheDocument()
  );
  await userEvent.click(
    screen.getByRole("button", { name: "Delete tap dance 0" })
  );
  await userEvent.type(screen.getByLabelText("New tap dance name"), "New");
  await userEvent.click(screen.getByRole("button", { name: "Add tap dance" }));
  await waitFor(() =>
    expect(screen.getByText("New (ID 1)")).toBeInTheDocument()
  );
});

it("applies a hold action and interval overrides", async () => {
  entries[0] = { name: "Alpha", config: blank(), dirty: false };
  mount();
  await waitFor(() =>
    expect(screen.getByText("Alpha (ID 0)")).toBeInTheDocument()
  );
  await userEvent.selectOptions(
    screen.getByLabelText("Single 0 behavior"),
    "1"
  );
  await userEvent.selectOptions(
    screen.getByLabelText("Single 0 hold behavior"),
    "1"
  );
  await userEvent.type(
    screen.getByLabelText("Tap dance 0 default interval (ms)"),
    "300"
  );
  await userEvent.type(screen.getByLabelText("Single 0 interval (ms)"), "125");
  await userEvent.click(
    screen.getByRole("button", { name: "Apply tap dance 0" })
  );
  await waitFor(() =>
    expect(
      requests.some(
        (item) => item.set?.hold && item.set.binding?.behaviorId === 1
      )
    ).toBe(true)
  );
  expect(
    requests.some(
      (item) =>
        item.setTiming?.tapCount === 0 && item.setTiming.intervalMs === 300
    )
  ).toBe(true);
  expect(
    requests.some(
      (item) =>
        item.setTiming?.tapCount === 1 && item.setTiming.intervalMs === 125
    )
  ).toBe(true);
});
