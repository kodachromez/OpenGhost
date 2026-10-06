import { describe, expect, it } from "vitest";

import { PermissionManager } from "#src/policy/permission-manager";
import type { ScopeConfig } from "#src/types";
import { createInMemoryPolicyLoader } from "#test/helpers/manager-harness";

/**
 * Full access as recorded authority (#526): when the injected reader reports
 * true, `check()` rewrites every matched `ask` to `allow` tagged
 * `origin: "full_access"`. Display surfaces (`getComposedConfigRules`,
 * `getToolPermission`) stay full-access-free.
 */
function makeManager(
  global: Record<string, unknown>,
  isFullAccess: () => boolean,
  project?: ScopeConfig,
): PermissionManager {
  return new PermissionManager({
    policyLoader: createInMemoryPolicyLoader({
      global: { permission: global } as ScopeConfig,
      project,
    }),
    isFullAccess,
  });
}

describe("PermissionManager full access rewrite", () => {
  it("rewrites a would-be-ask tool check to allow with origin 'full_access'", () => {
    const manager = makeManager({ bash: "ask" }, () => true);
    const result = manager.check({
      kind: "tool",
      surface: "bash",
      input: { command: "rm -rf /tmp/x" },
    });
    expect(result.state).toBe("allow");
    expect(result.origin).toBe("full_access");
  });

  it("rewrites the synthesized universal-default ask to allow with origin 'full_access'", () => {
    // No rule for the surface; falls through to the universal "*" default (ask).
    const manager = makeManager({}, () => true);
    const result = manager.check({
      kind: "tool",
      surface: "someExtensionTool",
      input: {},
    });
    expect(result.state).toBe("allow");
    expect(result.origin).toBe("full_access");
  });

  it("preserves explicit deny under Full access (hard denies survive)", () => {
    const manager = makeManager({ bash: "deny" }, () => true);
    const result = manager.check({
      kind: "tool",
      surface: "bash",
      input: { command: "rm -rf /" },
    });
    expect(result.state).toBe("deny");
    expect(result.origin).not.toBe("full_access");
  });

  it("passes an explicit allow through unchanged (not tagged full access)", () => {
    const manager = makeManager({ read: "allow" }, () => true);
    const result = manager.check({
      kind: "tool",
      surface: "read",
      input: { path: "/tmp/x" },
    });
    expect(result.state).toBe("allow");
    expect(result.origin).not.toBe("full_access");
  });

  it("does not rewrite when full access is disabled (state stays ask)", () => {
    const manager = makeManager({ bash: "ask" }, () => false);
    const result = manager.check({
      kind: "tool",
      surface: "bash",
      input: { command: "rm -rf /tmp/x" },
    });
    expect(result.state).toBe("ask");
    expect(result.origin).not.toBe("full_access");
  });

  it("leaves display surfaces full-access-free even when full access is enabled", () => {
    const manager = makeManager({ bash: "ask" }, () => true);

    // getComposedConfigRules shows the configured action, not the rewrite.
    const bashRule = manager
      .getComposedConfigRules()
      .find((r) => r.surface === "bash");
    expect(bashRule?.action).toBe("ask");
    expect(bashRule?.origin).not.toBe("full_access");

    // getToolPermission reports the configured surface state.
    expect(manager.getToolPermission("bash")).toBe("ask");
  });

  it("does not re-expose a denied tool — the rewrite touches asks, never denies", () => {
    const manager = makeManager({ bash: "deny" }, () => true);
    expect(manager.isToolFullyDenied("bash")).toBe(true);
  });
});

describe("PermissionManager fail-closed clamp under Full access (#646)", () => {
  it("full access re-permits a fail-closed floored allow (allow→ask→allow)", () => {
    // Invalid project scope floors global's `allow` to `ask` at composition;
    // full access then rewrites the `ask` back to `allow`. full access is an explicit
    // full-permissive opt-in, so full access users are unaffected by the clamp.
    const manager = makeManager({ bash: "allow" }, () => true, {
      invalid: true,
    });
    const result = manager.check({
      kind: "tool",
      surface: "bash",
      input: { command: "echo hi" },
    });
    expect(result.state).toBe("allow");
    expect(result.origin).toBe("full_access");
  });

  it("preserves a hard deny under Full access even with an invalid scope", () => {
    const manager = makeManager({ bash: "deny" }, () => true, {
      invalid: true,
    });
    const result = manager.check({
      kind: "tool",
      surface: "bash",
      input: { command: "rm -rf /" },
    });
    expect(result.state).toBe("deny");
    expect(result.origin).not.toBe("full_access");
  });
});
