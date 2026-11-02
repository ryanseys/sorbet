import { Disposable, Event, EventEmitter } from "vscode";
import { SorbetLanguageClient } from "./sorbetLanguageClient";
import { SorbetExtensionContext } from "./sorbetExtensionContext";
import { RestartReason, ServerStatus, ShowOperationParams } from "./types";

const MIN_TIME_BETWEEN_RETRIES_MS = 7000;
const MAX_TIME_BETWEEN_RETRIES_MS = 300000;
const QUICK_CRASH_MS = 30000;

// Not WRAPPER_REFUSED_SPAWN: a wrapper script uses it to ask for a retry.
const CRASH_REASONS: ReadonlySet<RestartReason> = new Set([
  RestartReason.CRASH_EXT_ERROR,
  RestartReason.CRASH_LC_CLOSED,
  RestartReason.CRASH_LC_ERROR,
  RestartReason.FORCIBLY_TERMINATED,
]);

/**
 * Get the minimum time between Sorbet starts after a crash. Each process that
 * crashes less than {@link QUICK_CRASH_MS} after it starts doubles the time, up
 * to {@link MAX_TIME_BETWEEN_RETRIES_MS}. A process that runs longer resets it.
 * @param retryDelayMs Minimum time between starts before the crash.
 * @param uptimeMs Time from the start of the process to the crash.
 */
export function getNextRetryDelayMs(
  retryDelayMs: number,
  uptimeMs: number,
): number {
  return uptimeMs < QUICK_CRASH_MS
    ? Math.min(retryDelayMs * 2, MAX_TIME_BETWEEN_RETRIES_MS)
    : MIN_TIME_BETWEEN_RETRIES_MS;
}

export type StatusChangedEvent = {
  status: ServerStatus;
  stopped?: true;
  error?: string;
};

export class SorbetStatusProvider implements Disposable {
  private wrappedActiveLanguageClient?: SorbetLanguageClient;
  private readonly context: SorbetExtensionContext;
  private readonly disposables: Disposable[];
  /** Ends the wait in startSorbet. Set only while startSorbet waits. */
  private endRetryWait?: () => void;
  /** Mutex for startSorbet. Prevents us from starting multiple processes at once. */
  private isStarting: boolean;
  private lastSorbetRetryTime: number;
  private operationStack: ShowOperationParams[];
  private readonly onShowOperationEmitter: EventEmitter<ShowOperationParams>;
  private readonly onStatusChangedEmitter: EventEmitter<StatusChangedEvent>;
  private retryDelayMs: number;

  constructor(context: SorbetExtensionContext) {
    this.context = context;
    this.isStarting = false;
    this.lastSorbetRetryTime = 0;
    this.operationStack = [];
    this.onShowOperationEmitter = new EventEmitter<ShowOperationParams>();
    this.onStatusChangedEmitter = new EventEmitter<StatusChangedEvent>();
    this.retryDelayMs = MIN_TIME_BETWEEN_RETRIES_MS;

    this.disposables = [
      this.onShowOperationEmitter,
      this.onStatusChangedEmitter,
    ];
  }

  /**
   * Dispose and free associated resources.
   */
  public dispose() {
    Disposable.from(...this.disposables).dispose();
  }

  /**
   * Current Sorbet client, if any.
   */
  public get activeLanguageClient(): SorbetLanguageClient | undefined {
    return this.wrappedActiveLanguageClient;
  }

  private set activeLanguageClient(value: SorbetLanguageClient | undefined) {
    if (this.wrappedActiveLanguageClient === value) {
      return;
    }

    // Clean-up existing client, if any.
    if (this.wrappedActiveLanguageClient) {
      this.wrappedActiveLanguageClient.dispose();
      const i = this.disposables.indexOf(this.wrappedActiveLanguageClient);
      if (i !== -1) {
        this.disposables.splice(i, 1);
      }
    }

    // Register new client for clean-up, if any.
    if (value) {
      const i = this.disposables.indexOf(value);
      if (i === -1) {
        this.disposables.push(value);
      }
    }

    this.wrappedActiveLanguageClient = value;

    // State might have changed based on new client.
    if (this.wrappedActiveLanguageClient) {
      this.fireOnStatusChanged({
        status: this.wrappedActiveLanguageClient.status,
        error: this.wrappedActiveLanguageClient.lastError,
      });
    }
  }

  /**
   * Raise {@link onShowOperation} event. Prefer this over calling
   * {@link EventEmitter.fire} directly so known state is updated before
   * event listeners are notified. Spurious events are filtered out.
   */
  private fireOnShowOperation(data: ShowOperationParams): void {
    let changed: boolean = false;
    if (data.status === "end") {
      const filteredOps = this.operationStack.filter(
        (otherP) => otherP.operationName !== data.operationName,
      );
      if (filteredOps.length !== this.operationStack.length) {
        this.operationStack = filteredOps;
        changed = true;
      }
    } else {
      this.operationStack.push(data);
      changed = true;
    }

    if (changed) {
      this.onShowOperationEmitter.fire(data);
    }
  }

  /**
   * Raise {@link onServerStatusChanged} event. Prefer this over calling
   * {@link EventEmitter.fire} directly so known state is updated before
   * event listeners are notified.
   */
  private fireOnStatusChanged(data: StatusChangedEvent): void {
    if (data.stopped) {
      this.operationStack = [];
    }
    this.onStatusChangedEmitter.fire(data);
  }

  /**
   * Sorbet client current operation stack.
   */
  public get operations(): ReadonlyArray<Readonly<ShowOperationParams>> {
    return this.operationStack;
  }

  /**
   * Event raised on a {@link ShowOperationParams show-operation} event.
   */
  public get onShowOperation(): Event<ShowOperationParams> {
    return this.onShowOperationEmitter.event;
  }

  /**
   * Event raised on {@link ServerStatus status} changes.
   */
  public get onStatusChanged(): Event<StatusChangedEvent> {
    return this.onStatusChangedEmitter.event;
  }

  /**
   * Restart Sorbet.
   * @param reason Telemetry reason. Also decides if this start follows a crash.
   */
  public async restartSorbet(reason: RestartReason): Promise<void> {
    await this.stopSorbet(ServerStatus.RESTARTING);
    // `reason` is an enum type with a small and finite number of values.
    this.context.metrics.emitCountMetric(`restart.${reason}`, 1);
    await this.startSorbet(CRASH_REASONS.has(reason));
  }

  /**
   * Error information, if {@link serverStatus} is {@link ServerStatus.ERROR}
   */
  public get serverError(): string | undefined {
    return this.activeLanguageClient?.lastError;
  }

  /**
   * Return current {@link ServerStatus server status}.
   */
  public get serverStatus(): ServerStatus {
    return (
      this.activeLanguageClient?.status ||
      (this.isStarting ? ServerStatus.RESTARTING : ServerStatus.DISABLED)
    );
  }

  /**
   * Start Sorbet.
   * @param afterCrash Whether this start follows a crash. A quick crash makes
   * the wait before the start longer.
   */
  public async startSorbet(afterCrash = false): Promise<void> {
    if (!afterCrash && this.retryDelayMs > MIN_TIME_BETWEEN_RETRIES_MS) {
      // Do not make the user wait for the crash backoff. Do not end a wait of
      // MIN_TIME_BETWEEN_RETRIES_MS: it merges a burst of config changes.
      this.retryDelayMs = MIN_TIME_BETWEEN_RETRIES_MS;
      this.endRetryWait?.();
    }

    if (this.isStarting) {
      this.context.log.trace("Ignored start request, already starting.");
      return;
    }

    if (!this.context.configuration.activeLspConfig) {
      this.context.log.info(
        "Ignored start request, no active configuration. See https://sorbet.org/docs/vscode",
      );
      return;
    }

    const msSinceLastStart = Date.now() - this.lastSorbetRetryTime;
    if (afterCrash) {
      this.retryDelayMs = getNextRetryDelayMs(
        this.retryDelayMs,
        msSinceLastStart,
      );
    }

    // Debounce by retryDelayMs. Returns 0 if the calculated time to sleep is negative.
    const sleepMS = this.retryDelayMs - msSinceLastStart;
    if (sleepMS > 0) {
      // Wait timeToSleep ms. Use mutex, as this yields the event loop for future events.
      this.context.log.info(
        `Waiting ${sleepMS.toFixed(0)}ms before restarting Sorbet…`,
      );
      this.isStarting = true;
      await new Promise<void>((res) => {
        const timer = setTimeout(res, sleepMS);
        this.endRetryWait = () => {
          clearTimeout(timer);
          res();
        };
      });
      this.endRetryWait = undefined;
      this.isStarting = false;
      if (!this.context.configuration.activeLspConfig) {
        this.context.log.info(
          "Ignored start request, no active configuration.",
        );
        return;
      }
    }
    this.lastSorbetRetryTime = Date.now();

    // Create client
    const newClient = new SorbetLanguageClient(
      this.context,
      (reason: RestartReason) => this.restartSorbet(reason),
    );
    this.disposables.push(
      newClient.onStatusChange((status: ServerStatus) => {
        // Ignore event if this is not the current client (e.g. old client being shut down).
        if (this.activeLanguageClient === newClient) {
          this.fireOnStatusChanged({
            status,
            error: newClient.lastError,
          });
        }
      }),
    );

    this.activeLanguageClient = newClient;
    await newClient.start();

    this.disposables.push(
      newClient.onNotification(
        "sorbet/showOperation",
        (params: ShowOperationParams) => {
          // Ignore event if this is not the current client (e.g. old client being shut down).
          if (this.activeLanguageClient === newClient) {
            this.fireOnShowOperation(params);
          }
        },
      ),
    );
  }

  /**
   * Stop Sorbet.
   * @param newStatus Status to report.
   */
  public async stopSorbet(newStatus: ServerStatus): Promise<void> {
    if (newStatus === ServerStatus.DISABLED) {
      this.endRetryWait?.();
    }
    // Use property-setter to ensure proper clean-up.
    this.activeLanguageClient = undefined;
    this.fireOnStatusChanged({ status: newStatus, stopped: true });
  }
}
