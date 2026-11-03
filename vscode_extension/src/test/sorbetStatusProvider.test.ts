import * as assert from "assert";
import * as path from "path";
import * as sinon from "sinon";

import { createLogStub } from "./testUtils";
import * as languageClient from "../languageClient";
import { MetricsClient } from "../metricsClient";
import { SorbetExtensionContext } from "../sorbetExtensionContext";
import {
  getNextRetryDelayMs,
  SorbetStatusProvider,
} from "../sorbetStatusProvider";
import { RestartReason, ServerStatus } from "../types";

suite(`Test Suite: ${path.basename(__filename, ".test.js")}`, () => {
  let testRestorables: { restore: () => void }[];

  setup(() => {
    testRestorables = [];
  });

  teardown(() => {
    testRestorables.forEach((r) => r.restore());
  });

  test("getNextRetryDelayMs: quick crashes double the delay, up to 5 minutes", () => {
    const delays: number[] = [];
    let delayMs = 7000;
    for (let i = 0; i < 8; i++) {
      delayMs = getNextRetryDelayMs(delayMs, 29999);
      delays.push(delayMs);
    }
    assert.deepStrictEqual(delays, [
      14000,
      28000,
      56000,
      112000,
      224000,
      300000,
      300000,
      300000,
    ]);
  });

  test("getNextRetryDelayMs: a process that ran for 30 s resets the delay", () => {
    assert.strictEqual(getNextRetryDelayMs(300000, 30000), 7000);
    assert.strictEqual(getNextRetryDelayMs(7000, 3600000), 7000);
  });

  test("restartSorbet: a restart from the user ends the crash backoff", async () => {
    const createClientStub = sinon
      .stub(languageClient, "createClient")
      .callsFake(
        () =>
          <any>{
            needsStart: () => true,
            onNotification: () => ({ dispose: () => {} }),
            start: async () => {},
            stop: async () => {},
          },
      );
    testRestorables.push(createClientStub);

    const statusProvider = new SorbetStatusProvider(<SorbetExtensionContext>{
      configuration: { activeLspConfig: {} },
      log: createLogStub(),
      metrics: <any>sinon.createStubInstance(MetricsClient),
    });
    await statusProvider.startSorbet();
    sinon.assert.calledOnce(createClientStub);

    // Sorbet crashed right after it started, so this restart waits 14 s.
    const crashRestart = statusProvider.restartSorbet(
      RestartReason.CRASH_LC_CLOSED,
    );
    await new Promise((res) => setTimeout(res, 0));
    sinon.assert.calledOnce(createClientStub);
    assert.strictEqual(statusProvider.serverStatus, ServerStatus.RESTARTING);

    await statusProvider.restartSorbet(RestartReason.COMMAND);
    await crashRestart;
    sinon.assert.calledTwice(createClientStub);
    assert.strictEqual(statusProvider.serverStatus, ServerStatus.RUNNING);

    statusProvider.dispose();
  });

  test("stopSorbet: disabling Sorbet ends the crash backoff", async () => {
    const createClientStub = sinon
      .stub(languageClient, "createClient")
      .callsFake(
        () =>
          <any>{
            needsStart: () => true,
            onNotification: () => ({ dispose: () => {} }),
            start: async () => {},
            stop: async () => {},
          },
      );
    testRestorables.push(createClientStub);

    const configuration = { activeLspConfig: <any>{} };
    const statusProvider = new SorbetStatusProvider(<SorbetExtensionContext>{
      configuration,
      log: createLogStub(),
      metrics: <any>sinon.createStubInstance(MetricsClient),
    });
    await statusProvider.startSorbet();

    const crashRestart = statusProvider.restartSorbet(
      RestartReason.CRASH_LC_CLOSED,
    );
    await new Promise((res) => setTimeout(res, 0));

    configuration.activeLspConfig = undefined;
    await statusProvider.stopSorbet(ServerStatus.DISABLED);
    await crashRestart;
    sinon.assert.calledOnce(createClientStub);
    assert.strictEqual(statusProvider.serverStatus, ServerStatus.DISABLED);

    statusProvider.dispose();
  });
});
