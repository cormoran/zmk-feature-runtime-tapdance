import { StrictMode } from "react";
import { act, render, screen, waitFor } from "@testing-library/react";
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
  MetaError: class MetaError extends Error {
    condition: number;
    constructor(condition: number) {
      super("unlock required");
      this.condition = condition;
    }
  },
}));
const client = jest.mocked(call_rpc);
const blank = () =>
  Config.create({
    singleBinding: Binding.create(),
    doubleBinding: Binding.create(),
    tripleBinding: Binding.create(),
  });
let requests: Request[];
let database: Config[];
let interval: number;
let rejectWrite: boolean;
let unlockRequired: boolean;
let metadataCalls: number;
let notifyCore:
  | ((notification: { lockStateChanged?: LockState }) => void)
  | undefined;
function mount() {
  const app = createConnectedMockZMKApp({
    subsystems: ["cormoran__runtime_tapdance"],
  });
  app.onNotification = jest.fn((subscription) => {
    if (subscription.type === "core") notifyCore = subscription.callback;
    return () => {};
  });
  return render(
    <StrictMode>
      <ZMKAppProvider value={app}>
        <TapDanceEditor />
      </ZMKAppProvider>
    </StrictMode>
  );
}
async function loaded() {
  await waitFor(() =>
    expect(screen.getByText("0 / 3 tap dances")).toBeInTheDocument()
  );
}
beforeEach(() => {
  jest.clearAllMocks();
  requests = [];
  database = [blank(), blank(), blank()];
  interval = 200;
  rejectWrite = false;
  unlockRequired = false;
  metadataCalls = 0;
  notifyCore = undefined;
  client.mockImplementation(async (_connection, request) => {
    if (request.core?.getLockState)
      return {
        core: { getLockState: LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED },
      };
    if (request.behaviors?.listAllBehaviors) {
      metadataCalls++;
      return { behaviors: { listAllBehaviors: { behaviors: [1, 2] } } };
    }
    if (request.behaviors?.getBehaviorDetails) {
      metadataCalls++;
      const id = request.behaviors.getBehaviorDetails.behaviorId;
      return {
        behaviors: {
          getBehaviorDetails: {
            id,
            displayName: id === 1 ? "Key press" : "Toggle layer",
            metadata: [],
          },
        },
      };
    }
    if (!request.custom?.call) throw new Error("Unexpected request");
    const inner = Request.decode(request.custom.call.payload);
    requests.push(inner);
    if (unlockRequired) {
      // eslint-disable-next-line @typescript-eslint/no-require-imports
      const { MetaError } = require("@zmkfirmware/zmk-studio-ts-client");
      throw new MetaError(1);
    }
    let response: Response;
    const index =
      inner.get?.index ?? inner.set?.index ?? inner.remove?.index ?? 0;
    if (rejectWrite && !inner.get)
      response = { error: { code: -22, message: "Invalid binding" } };
    else {
      if (inner.set) {
        const key = ["", "singleBinding", "doubleBinding", "tripleBinding"][
          inner.set.tapCount
        ] as keyof Config;
        if (!key) throw new Error("Invalid test tap count");
        const candidate = {
          ...database[index],
          [key]: inner.set.binding,
        };
        if (
          (!candidate.singleBinding?.behaviorId &&
            (candidate.doubleBinding?.behaviorId ||
              candidate.tripleBinding?.behaviorId)) ||
          (candidate.tripleBinding?.behaviorId &&
            !candidate.doubleBinding?.behaviorId)
        )
          response = { error: { code: -22, message: "Invalid binding" } };
        else database[index] = candidate;
      }
      if (inner.remove) database[index] = blank();
      if (inner.setInterval) interval = inner.setInterval.intervalMs;
      response ??= {
        state: {
          index,
          config: database[index],
          intervalMs: interval,
          maxCount: database.length,
        },
      };
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

it("loads every slot and behavior metadata only once after renders settle", async () => {
  mount();
  await loaded();
  await waitFor(() => expect(metadataCalls).toBe(3));
  expect(requests.map((request) => request.get?.index)).toEqual([0, 1, 2]);
  const count = requests.length;
  await userEvent.click(screen.getByRole("button", { name: "Add tap dance" }));
  await userEvent.selectOptions(
    screen.getByLabelText("Single 1 behavior"),
    "1"
  );
  await act(async () => {
    await new Promise((resolve) => setTimeout(resolve, 40));
  });
  expect(requests).toHaveLength(count);
  expect(metadataCalls).toBe(3);
});

it("adds and edits multiple dances, applies RAM, saves flash and deletes", async () => {
  const user = userEvent.setup();
  mount();
  await loaded();
  await user.click(screen.getByRole("button", { name: "Add tap dance" }));
  await user.selectOptions(screen.getByLabelText("Single 1 behavior"), "1");
  await user.clear(screen.getByLabelText("Single 1 parameter 1"));
  await user.type(screen.getByLabelText("Single 1 parameter 1"), "458755");
  await waitFor(() =>
    expect(screen.getByLabelText("Double 1 behavior")).toBeInTheDocument()
  );
  await user.selectOptions(screen.getByLabelText("Double 1 behavior"), "1");
  await user.clear(screen.getByLabelText("Double 1 parameter 1"));
  await user.type(screen.getByLabelText("Double 1 parameter 1"), "458756");
  await user.selectOptions(screen.getByLabelText("Triple 1 behavior"), "2");
  await user.click(screen.getByRole("button", { name: "Apply tap dance 1" }));
  await screen.findByText(/Applied in RAM/);
  expect(
    requests.filter((request) => request.set).map((request) => request.set)
  ).toMatchObject([
    {
      index: 0,
      tapCount: 1,
      persist: false,
      binding: { behaviorId: 1, param1: 458755 },
    },
    {
      index: 0,
      tapCount: 2,
      persist: false,
      binding: { behaviorId: 1, param1: 458756 },
    },
    { index: 0, tapCount: 3, persist: false, binding: { behaviorId: 2 } },
  ]);
  await user.click(screen.getByRole("button", { name: "Save tap dance 1" }));
  await screen.findByText("Saved to flash.");
  expect(
    requests
      .filter((request) => request.set)
      .slice(-3)
      .map((request) => request.set?.persist)
  ).toEqual([true, true, true]);
  await user.selectOptions(screen.getByLabelText("Triple 1 behavior"), "0");
  await user.selectOptions(screen.getByLabelText("Double 1 behavior"), "0");
  await user.click(screen.getByRole("button", { name: "Apply tap dance 1" }));
  await screen.findByText(/Applied in RAM/);
  expect(
    requests
      .filter((request) => request.set)
      .slice(-3)
      .map((request) => request.set?.tapCount)
  ).toEqual([3, 2, 1]);
  await user.click(screen.getByRole("button", { name: "Add tap dance" }));
  expect(screen.getByLabelText("Single 2 behavior ID")).toBeInTheDocument();
  await user.click(
    screen.getByRole("button", { name: "Delete tap dance 1 (flash)" })
  );
  await waitFor(() =>
    expect(
      screen.queryByLabelText("Single 1 behavior ID")
    ).not.toBeInTheDocument()
  );
  expect(requests.at(-1)?.remove).toEqual({ index: 0, persist: true });
  await user.click(
    screen.getByRole("button", { name: "Delete tap dance 2 (RAM)" })
  );
  await waitFor(() =>
    expect(
      screen.queryByLabelText("Single 2 behavior ID")
    ).not.toBeInTheDocument()
  );
  expect(requests.at(-1)?.remove).toEqual({ index: 1, persist: false });
});

it("validates integer bounds and edits global interval separately", async () => {
  const user = userEvent.setup();
  mount();
  await loaded();
  await user.clear(screen.getByLabelText("Tap interval (ms)"));
  await user.type(screen.getByLabelText("Tap interval (ms)"), "2001");
  await user.click(screen.getByRole("button", { name: "Apply interval" }));
  expect(await screen.findByRole("alert")).toHaveTextContent("1 to 2000");
  expect(requests).toHaveLength(3);
  await user.clear(screen.getByLabelText("Tap interval (ms)"));
  await user.type(screen.getByLabelText("Tap interval (ms)"), "250");
  await user.click(screen.getByRole("button", { name: "Apply interval" }));
  await screen.findByText(/Applied in RAM/);
  expect(requests.at(-1)?.setInterval).toEqual({
    intervalMs: 250,
    persist: false,
  });
  await user.click(screen.getByRole("button", { name: "Save interval" }));
  await screen.findByText("Saved to flash.");
  expect(requests.at(-1)?.setInterval?.persist).toBe(true);
  await user.click(screen.getByRole("button", { name: "Add tap dance" }));
  await user.selectOptions(screen.getByLabelText("Single 1 behavior"), "1");
  await user.clear(screen.getByLabelText("Double 1 parameter 2"));
  await user.type(screen.getByLabelText("Double 1 parameter 2"), "4294967296");
  const count = requests.length;
  await user.click(screen.getByRole("button", { name: "Apply tap dance 1" }));
  expect(await screen.findByRole("alert")).toHaveTextContent("unsigned 32-bit");
  expect(requests).toHaveLength(count);
});

it("requires a single action and a double before a triple", async () => {
  const user = userEvent.setup();
  mount();
  await loaded();
  await user.click(screen.getByRole("button", { name: "Add tap dance" }));
  expect(screen.getByText("&rtd 0")).toBeInTheDocument();
  const count = requests.length;
  await user.click(screen.getByRole("button", { name: "Apply tap dance 1" }));
  expect(await screen.findByRole("alert")).toHaveTextContent("single-tap");
  expect(requests).toHaveLength(count);
  await user.selectOptions(screen.getByLabelText("Single 1 behavior"), "1");
  await user.selectOptions(screen.getByLabelText("Triple 1 behavior"), "2");
  await user.click(screen.getByRole("button", { name: "Apply tap dance 1" }));
  expect(await screen.findByRole("alert")).toHaveTextContent("double-tap");
  expect(requests).toHaveLength(count);
  await user.selectOptions(screen.getByLabelText("Double 1 behavior"), "1");
  await user.click(screen.getByRole("button", { name: "Apply tap dance 1" }));
  await screen.findByText(/Applied in RAM/);
});

it("keeps the draft when firmware rejects a write", async () => {
  const user = userEvent.setup();
  mount();
  await loaded();
  await user.click(screen.getByRole("button", { name: "Add tap dance" }));
  await user.selectOptions(screen.getByLabelText("Single 1 behavior"), "1");
  await user.clear(screen.getByLabelText("Single 1 parameter 1"));
  await user.type(screen.getByLabelText("Single 1 parameter 1"), "999");
  rejectWrite = true;
  await user.click(screen.getByRole("button", { name: "Apply tap dance 1" }));
  expect(await screen.findByRole("alert")).toHaveTextContent(
    "Invalid binding (-22)"
  );
  expect(screen.getByLabelText("Single 1 parameter 1")).toHaveValue(999);
});

it("retries a rejected write once Studio unlocks", async () => {
  const user = userEvent.setup();
  mount();
  await loaded();
  await user.click(screen.getByRole("button", { name: "Add tap dance" }));
  await user.selectOptions(screen.getByLabelText("Single 1 behavior"), "1");
  unlockRequired = true;
  await user.click(screen.getByRole("button", { name: "Save tap dance 1" }));
  await screen.findByRole("button", { name: "Retry" });
  const count = requests.length;
  await act(async () => {
    await new Promise((resolve) => setTimeout(resolve, 30));
  });
  expect(requests).toHaveLength(count);
  await act(async () =>
    notifyCore?.({
      lockStateChanged: LockState.ZMK_STUDIO_CORE_LOCK_STATE_LOCKED,
    })
  );
  unlockRequired = false;
  await act(async () =>
    notifyCore?.({
      lockStateChanged: LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED,
    })
  );
  await screen.findByText("Saved to flash.");
  expect(requests).toHaveLength(count + 3);
});

it("shows missing subsystem guidance and handles missing context", () => {
  const noContext = render(<TapDanceEditor />);
  expect(noContext.container.firstChild).toBeNull();
  noContext.unmount();
  const app = createConnectedMockZMKApp({ subsystems: [] });
  render(
    <ZMKAppProvider value={app}>
      <TapDanceEditor />
    </ZMKAppProvider>
  );
  expect(screen.getByText(/Subsystem.*not found/)).toBeInTheDocument();
});

it("clears parameters when selecting None or entering behavior ID zero", async () => {
  const user = userEvent.setup();
  mount();
  await loaded();
  await user.click(screen.getByRole("button", { name: "Add tap dance" }));
  await user.selectOptions(screen.getByLabelText("Single 1 behavior"), "1");
  await waitFor(() =>
    expect(screen.getByLabelText("Double 1 behavior")).toBeInTheDocument()
  );
  await user.selectOptions(screen.getByLabelText("Double 1 behavior"), "1");
  await user.clear(screen.getByLabelText("Double 1 parameter 1"));
  await user.type(screen.getByLabelText("Double 1 parameter 1"), "458756");
  await user.clear(screen.getByLabelText("Double 1 parameter 2"));
  await user.type(screen.getByLabelText("Double 1 parameter 2"), "2");
  await user.selectOptions(screen.getByLabelText("Double 1 behavior"), "0");
  expect(screen.getByLabelText("Double 1 parameter 1")).toHaveValue(0);
  expect(screen.getByLabelText("Double 1 parameter 2")).toHaveValue(0);
  await user.click(screen.getByRole("button", { name: "Apply tap dance 1" }));
  await screen.findByText(/Applied in RAM/);
  expect(
    requests.filter((request) => request.set?.tapCount === 2).at(-1)?.set
      ?.binding
  ).toEqual({
    behaviorId: 0,
    param1: 0,
    param2: 0,
  });
  await user.selectOptions(screen.getByLabelText("Triple 1 behavior"), "2");
  await user.clear(screen.getByLabelText("Triple 1 parameter 1"));
  await user.type(screen.getByLabelText("Triple 1 parameter 1"), "1");
  await user.clear(screen.getByLabelText("Triple 1 behavior ID"));
  await user.type(screen.getByLabelText("Triple 1 behavior ID"), "0");
  expect(screen.getByLabelText("Triple 1 parameter 1")).toHaveValue(0);
});
