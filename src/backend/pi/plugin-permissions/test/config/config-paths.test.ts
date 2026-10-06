import { join } from "node:path";
import { describe, expect, it } from "vitest";

import {
  DEBUG_LOG_FILENAME,
  getGlobalConfigDir,
  getGlobalConfigPath,
  getGlobalLogsDir,
  getProjectAgentsDir,
  getProjectConfigPath,
  REVIEW_LOG_FILENAME,
} from "#src/config/config-paths";

describe("config-paths", () => {
  const agentDir = "/home/user/.pi/agent";
  const cwd = "/projects/my-app";
  const extensionRoot = "/opt/extensions/plugin-permissions";

  describe("new layout paths", () => {
    it("getGlobalConfigDir returns extensions/plugin-permissions under agentDir", () => {
      expect(getGlobalConfigDir(agentDir)).toBe(
        join(agentDir, "extensions", "plugin-permissions"),
      );
    });

    it("getGlobalConfigPath returns config.json under the global config dir", () => {
      expect(getGlobalConfigPath(agentDir)).toBe(
        join(agentDir, "extensions", "plugin-permissions", "config.json"),
      );
    });

    it("getGlobalLogsDir returns logs under the global config dir", () => {
      expect(getGlobalLogsDir(agentDir)).toBe(
        join(agentDir, "extensions", "plugin-permissions", "logs"),
      );
    });

    it("getProjectConfigPath returns .pi/extensions/plugin-permissions/config.json under cwd", () => {
      expect(getProjectConfigPath(cwd)).toBe(
        join(cwd, ".pi", "extensions", "plugin-permissions", "config.json"),
      );
    });

    it("getProjectAgentsDir returns .pi/agents under cwd", () => {
      expect(getProjectAgentsDir(cwd)).toBe(join(cwd, ".pi", "agents"));
    });
  });


  describe("log filenames", () => {
    it("DEBUG_LOG_FILENAME is a .jsonl file", () => {
      expect(DEBUG_LOG_FILENAME).toBe("plugin-permissions-debug.jsonl");
    });

    it("REVIEW_LOG_FILENAME is a .jsonl file", () => {
      expect(REVIEW_LOG_FILENAME).toBe(
        "plugin-permissions-permission-review.jsonl",
      );
    });
  });
});
