/**
 * The plugin's record of OpenGhost's access mode (OpenGhost fork).
 */
import { createEventBus } from "@earendil-works/pi-coding-agent";
import { afterEach, describe, expect, it, vi } from "vitest";
import {
  AccessModeState,
  getAccessModeState,
  MODE_CHANNEL,
} from "#src/openghost/access-mode";
import { ApprovalCards } from "#src/openghost/approval-card";
import { connectOpenGhost, PLUGIN_NAME } from "#src/openghost/connection";
import { ToolInputLedger } from "#src/openghost/tool-input-ledger";

afterEach(() => {
  const store = globalThis as Record<symbol, unknown>;
  for (const key of [
    "openghost:plugin-permissions:access-mode",
    "openghost:plugin-permissions",
  ])
    // eslint-disable-next-line @typescript-eslint/no-dynamic-delete -- Symbol-keyed global property
    delete store[Symbol.for(key)];
});

describe("AccessModeState", () => {
  it("starts in Ask, the most restrictive", () => {
    expect(new AccessModeState().current()).toEqual({ mode: "ask", seq: -1 });
  });

  it("takes each mode OpenGhost says", () => {
    const state = new AccessModeState();
    for (const mode of ["auto", "full", "ask"] as const) {
      expect(state.apply({ mode, seq: state.current().seq + 1 })).toBe(true);
      expect(state.mode()).toBe(mode);
    }
    expect(state.isFullAccess()).toBe(false);
  });

  it("ignores an update numbered below the last applied", () => {
    const state = new AccessModeState();
    state.apply({ mode: "full", seq: 5 });
    expect(state.apply({ mode: "ask", seq: 3 })).toBe(false);
    expect(state.current()).toEqual({ mode: "full", seq: 5 });
    expect(state.apply({ mode: "ask", seq: 6 })).toBe(true);
    expect(state.current()).toEqual({ mode: "ask", seq: 6 });
  });

  it("reads a malformed update as Ask", () => {
    const state = new AccessModeState();
    state.apply({ mode: "full", seq: 1 });
    for (const bad of [{ mode: "yolo", seq: 2 }, { seq: 3 }, null, "full", 7]) {
      state.apply(bad);
      expect(state.mode()).toBe("ask");
      state.apply({ mode: "full", seq: state.current().seq + 1 });
    }
  });

  it("tells listeners of a change, not of a repeat", () => {
    const state = new AccessModeState();
    const listener = vi.fn();
    state.onChange(listener);
    state.apply({ mode: "ask", seq: 1 });
    state.apply({ mode: "full", seq: 2 });
    expect(listener).toHaveBeenCalledTimes(1);
    expect(listener.mock.calls[0]?.[0]).toEqual({ mode: "full", seq: 2 });
  });

  it("is one state per process, shared by every module copy", () => {
    const first = getAccessModeState();
    first.apply({ mode: "auto", seq: 1 });
    expect(getAccessModeState()).toBe(first);
    expect(getAccessModeState().mode()).toBe("auto");
  });
});

describe("connectOpenGhost", () => {
  function connect() {
    const events = createEventBus();
    const mode = getAccessModeState();
    const cards = new ApprovalCards({ toolInputs: new ToolInputLedger(), cwd: () => undefined });
    const release = vi.spyOn(cards, "releaseForFullAccess");
    const disconnect = connectOpenGhost({ events }, mode, cards);
    const handle = (globalThis as Record<symbol, unknown>)[
      Symbol.for("openghost:plugin-permissions")
    ] as { name: string; mode(): unknown; choose(id: string, c: unknown): boolean };
    return { events, mode, cards, release, disconnect, handle };
  }

  it("applies openghost:mode at once, and the handle says what it holds", () => {
    const { events, handle } = connect();
    events.emit(MODE_CHANNEL, { mode: "auto", seq: 4 });
    expect(handle.name).toBe(PLUGIN_NAME);
    expect(handle.mode()).toEqual({ mode: "auto", seq: 4 });
  });

  it("releases open cards when Full arrives", () => {
    const { events, release } = connect();
    events.emit(MODE_CHANNEL, { mode: "full", seq: 1 });
    expect(release).toHaveBeenCalledTimes(1);
    events.emit(MODE_CHANNEL, { mode: "full", seq: 2 });
    expect(release).toHaveBeenCalledTimes(1);
  });

  it("counts as an enforcer only while an instance is connected", () => {
    const { disconnect } = connect();
    const handle = (globalThis as Record<symbol, unknown>)[
      Symbol.for("openghost:plugin-permissions")
    ] as { enforcing(): boolean };
    expect(handle.enforcing()).toBe(true);
    disconnect();
    expect(handle.enforcing()).toBe(false);
  });

  it("stops listening once disconnected", () => {
    const { events, mode, disconnect, handle } = connect();
    disconnect();
    events.emit(MODE_CHANNEL, { mode: "full", seq: 1 });
    expect(mode.mode()).toBe("ask");
    expect(handle.choose("pp-x", { action: "approve" })).toBe(false);
  });
});
