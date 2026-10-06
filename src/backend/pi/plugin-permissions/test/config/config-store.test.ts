import { beforeEach, describe, expect, it, vi } from "vitest";

// ── Module mocks (hoisted) ─────────────────────────────────────────────────

const {
  mockLoadAndMergeConfigs,
  mockLoadUnifiedConfig,
  mockSyncPermissionSystemStatus,
  mockBuildResolvedConfigLogEntry,
  mockExistsSync,
  mockMkdirSync,
  mockWriteFileSync,
  mockRenameSync,
  mockUnlinkSync,
} = vi.hoisted(() => ({
  mockLoadAndMergeConfigs: vi.fn(),
  mockLoadUnifiedConfig: vi.fn(),
  mockSyncPermissionSystemStatus: vi.fn(),
  mockBuildResolvedConfigLogEntry: vi.fn(),
  mockExistsSync: vi.fn<(path: string) => boolean>(),
  mockMkdirSync: vi.fn(),
  mockWriteFileSync: vi.fn(),
  mockRenameSync: vi.fn(),
  mockUnlinkSync: vi.fn(),
}));

vi.mock("#src/config/config-loader", () => ({
  loadAndMergeConfigs: mockLoadAndMergeConfigs,
  loadUnifiedConfig: mockLoadUnifiedConfig,
}));

vi.mock("#src/config/config-reporter", () => ({
  buildResolvedConfigLogEntry: mockBuildResolvedConfigLogEntry,
}));

vi.mock("node:fs", () => ({
  existsSync: mockExistsSync,
  mkdirSync: mockMkdirSync,
  writeFileSync: mockWriteFileSync,
  renameSync: mockRenameSync,
  unlinkSync: mockUnlinkSync,
  default: {
    existsSync: mockExistsSync,
    mkdirSync: mockMkdirSync,
    writeFileSync: mockWriteFileSync,
    renameSync: mockRenameSync,
    unlinkSync: mockUnlinkSync,
  },
}));

// ── Imports ────────────────────────────────────────────────────────────────

import type { ExtensionCommandContext } from "@earendil-works/pi-coding-agent";
import {
  ConfigStore,
  type ConfigStoreDeps,
  type ResolvedPolicyPathProvider,
} from "#src/config/config-store";
import { DEFAULT_EXTENSION_CONFIG } from "#src/config/extension-config";
import type { ResolvedPolicyPaths } from "#src/config/policy-loader";

// ── Helpers ────────────────────────────────────────────────────────────────

function makePolicyPathProvider(
  paths?: Partial<ResolvedPolicyPaths>,
): ResolvedPolicyPathProvider {
  return {
    getResolvedPolicyPaths: vi.fn(
      (): ResolvedPolicyPaths => ({
        globalConfigPath: "/agent/config.json",
        globalConfigExists: false,
        projectConfigPath: null,
        projectConfigExists: false,
        agentsDir: "/agent/agents",
        agentsDirExists: false,
        projectAgentsDir: null,
        projectAgentsDirExists: false,
        ...paths,
      }),
    ),
  };
}

function makeLogger() {
  return {
    debug: vi.fn<(event: string, details?: Record<string, unknown>) => void>(),
    review: vi.fn<(event: string, details?: Record<string, unknown>) => void>(),
  };
}

function makeCommandCtx(
  overrides: Partial<ExtensionCommandContext> = {},
): ExtensionCommandContext {
  return {
    cwd: "/test/project",
    ui: { notify: vi.fn(), setStatus: vi.fn() },
    ...overrides,
  } as unknown as ExtensionCommandContext;
}

function makeStore(overrides: Partial<ConfigStoreDeps> = {}): {
  store: ConfigStore;
  logger: ReturnType<typeof makeLogger>;
} {
  const logger = makeLogger();
  const deps: ConfigStoreDeps = {
    agentDir: "/test/agent",
    policyPaths: makePolicyPathProvider(),
    logger,
    ...overrides,
  };
  return { store: new ConfigStore(deps), logger };
}

// ── Tests ──────────────────────────────────────────────────────────────────

describe("ConfigStore", () => {
  beforeEach(() => {
    mockLoadAndMergeConfigs.mockReset().mockReturnValue({
      merged: { ...DEFAULT_EXTENSION_CONFIG },
      issues: [],
    });
    mockLoadUnifiedConfig.mockReset().mockReturnValue({ config: {} });
    mockSyncPermissionSystemStatus.mockReset();
    mockBuildResolvedConfigLogEntry
      .mockReset()
      .mockReturnValue({ resolved: true });
    mockExistsSync.mockReset().mockReturnValue(false);
    mockMkdirSync.mockReset();
    mockWriteFileSync.mockReset();
    mockRenameSync.mockReset();
    mockUnlinkSync.mockReset();
  });

  // ── current() ─────────────────────────────────────────────────────────

  describe("current()", () => {
    it("returns DEFAULT_EXTENSION_CONFIG before any refresh", () => {
      const { store } = makeStore();
      expect(store.current()).toEqual(DEFAULT_EXTENSION_CONFIG);
    });
  });

  // ── getConfigIssues() ─────────────────────────────────────────────────

  describe("getConfigIssues()", () => {
    it("answers empty before any refresh", () => {
      const { store } = makeStore();
      expect(store.getConfigIssues()).toEqual([]);
    });

    it("answers the issues the last load produced", () => {
      const { store } = makeStore();
      mockLoadAndMergeConfigs.mockReturnValue({
        merged: { ...DEFAULT_EXTENSION_CONFIG },
        issues: ["first issue", "second issue"],
      });
      store.refresh("/test/project", true);
      expect(store.getConfigIssues()).toEqual(["first issue", "second issue"]);
    });

    it("answers empty again once a reload finds the config clean", () => {
      const { store } = makeStore();
      mockLoadAndMergeConfigs.mockReturnValue({
        merged: { ...DEFAULT_EXTENSION_CONFIG },
        issues: ["transient issue"],
      });
      store.refresh("/test/project", true);
      mockLoadAndMergeConfigs.mockReturnValue({
        merged: { ...DEFAULT_EXTENSION_CONFIG },
        issues: [],
      });
      store.refresh("/test/project", true);
      expect(store.getConfigIssues()).toEqual([]);
    });
  });

  // ── refresh() ─────────────────────────────────────────────────────────

  describe("refresh()", () => {
    it("uses the passed cwd for loadAndMergeConfigs and includes the project scope when trusted", () => {
      const { store } = makeStore();
      store.refresh("/my/project", true);
      expect(mockLoadAndMergeConfigs).toHaveBeenCalledWith(
        "/test/agent",
        "/my/project",
        { includeProjectScope: true },
      );
    });

    it("withholds the project scope when the project is untrusted", () => {
      const { store } = makeStore();
      store.refresh("/my/project", false);
      expect(mockLoadAndMergeConfigs).toHaveBeenCalledWith(
        "/test/agent",
        "/my/project",
        { includeProjectScope: false },
      );
    });

    it("uses empty string cwd when no cwd is provided", () => {
      const { store } = makeStore();
      store.refresh(undefined, true);
      expect(mockLoadAndMergeConfigs).toHaveBeenCalledWith(
        "/test/agent",
        "",
        { includeProjectScope: true },
      );
    });

    it("updates current() with normalized merged result", () => {
      const { store } = makeStore();
      mockLoadAndMergeConfigs.mockReturnValue({
        merged: { debugLog: true, permissionReviewLog: false },
        issues: [],
      });
      store.refresh(undefined, true);
      expect(store.current().debugLog).toBe(true);
      expect(store.current().permissionReviewLog).toBe(false);
    });

    it("writes config.loaded debug log", () => {
      const { store, logger } = makeStore();
      store.refresh(undefined, true);
      expect(logger.debug).toHaveBeenCalledWith(
        "config.loaded",
        expect.objectContaining({ debugLog: false }),
      );
    });

    // `config.loaded` is the durable record of what the load found, for
    // whoever is diagnosing after the fact. The operator-facing notification
    // is `ConfigIssueReporter`'s job; this field is not.
    it("records the issues on config.loaded as one joined string", () => {
      const { store, logger } = makeStore();
      mockLoadAndMergeConfigs.mockReturnValue({
        merged: { ...DEFAULT_EXTENSION_CONFIG },
        issues: ["first issue", "second issue"],
      });
      store.refresh(undefined, true);
      expect(logger.debug).toHaveBeenCalledWith(
        "config.loaded",
        expect.objectContaining({ warning: "first issue\nsecond issue" }),
      );
    });

    it("records a null warning on config.loaded when the config is clean", () => {
      const { store, logger } = makeStore();
      store.refresh(undefined, true);
      expect(logger.debug).toHaveBeenCalledWith(
        "config.loaded",
        expect.objectContaining({ warning: null }),
      );
    });


    it("carries piInfrastructureReadPaths from merged config into current()", () => {
      const { store } = makeStore();
      mockLoadAndMergeConfigs.mockReturnValue({
        merged: { piInfrastructureReadPaths: ["/extra/path"] },
        issues: [],
      });
      store.refresh(undefined, true);
      expect(store.current().piInfrastructureReadPaths).toEqual([
        "/extra/path",
      ]);
    });
  });



  // ── logResolvedPaths() ─────────────────────────────────────────────────

  describe("logResolvedPaths()", () => {
    it("writes config.resolved to both review and debug logs", () => {
      const { store, logger } = makeStore();
      store.logResolvedPaths();
      expect(logger.review).toHaveBeenCalledWith(
        "config.resolved",
        expect.any(Object),
      );
      expect(logger.debug).toHaveBeenCalledWith(
        "config.resolved",
        expect.any(Object),
      );
    });

    it("calls getResolvedPolicyPaths from the provider", () => {
      const mockProvider = makePolicyPathProvider();
      const { store } = makeStore({ policyPaths: mockProvider });
      store.logResolvedPaths();
      expect(mockProvider.getResolvedPolicyPaths).toHaveBeenCalled();
    });


  });
});
