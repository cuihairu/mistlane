// VS Code shell for mistlane-devcore: status bar, attach/detach commands, and
// the dashboard webview. Every data surface comes from the core (metrics HTTP
// poll, log file tail, /proc sampling) — the engine needs no changes.

import * as vscode from "vscode";
import * as fs from "node:fs";
import { AttachSession, AttachStatus } from "@mistlane/devcore/attach";
import { contractRows, parseContractDef } from "@mistlane/devcore/contract";
import type { ContractMethodRow } from "@mistlane/devcore/contract";
import { readPidFile } from "@mistlane/devcore/proc";
import { PanelModel, snapshot } from "./panelModel";
import { buildPanelHtml, randomNonce } from "./panelHtml";

class Controller {
  private readonly context: vscode.ExtensionContext;
  private readonly statusItem: vscode.StatusBarItem;
  private session: AttachSession | null = null;
  private panel: vscode.WebviewPanel | null = null;
  private readonly model = new PanelModel();
  private contractRowsCache: ContractMethodRow[] = [];
  private contractPath = "";

  constructor(context: vscode.ExtensionContext) {
    this.context = context;
    this.statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
    this.statusItem.text = "$(plug) mistlane: detached";
    this.statusItem.command = "mistlane.showPanel";
    this.statusItem.show();
  }

  private config<T>(key: string): T | undefined {
    return vscode.workspace.getConfiguration("mistlane").get<T>(key);
  }

  private setStatus(status: AttachStatus): void {
    const parts = (["metrics", "log", "proc"] as const).map((name) => `${name}:${status[name]}`);
    this.statusItem.text = `$(plug) mistlane ${parts.join(" ")}`;
  }

  /** Reads the contract .def once per attach; a missing file is not fatal. */
  private loadContract(): void {
    this.contractRowsCache = [];
    this.contractPath = String(this.config<string>("contractPath") ?? "");
    if (this.contractPath !== "") {
      try {
        this.contractRowsCache = contractRows(parseContractDef(fs.readFileSync(this.contractPath, "utf8")));
      } catch (error) {
        vscode.window.showWarningMessage(
          `Mistlane: cannot read contract ${this.contractPath}: ${error instanceof Error ? error.message : error}`,
        );
        this.contractPath = "";
      }
    }
    this.model.setContract(this.contractRowsCache, this.contractPath);
  }

  private async resolvePid(): Promise<number | null> {
    const pid = this.config<number>("pid");
    if (typeof pid === "number" && Number.isInteger(pid) && pid > 0) return pid;
    const pidFile = this.config<string>("pidFile");
    if (pidFile) {
      try {
        return await readPidFile(pidFile);
      } catch (error) {
        vscode.window.showWarningMessage(
          `Mistlane: cannot read pidfile ${pidFile}: ${error instanceof Error ? error.message : error}`,
        );
      }
    }
    return null;
  }

  async attach(): Promise<void> {
    if (this.session) {
      this.revealPanel();
      return;
    }
    this.loadContract();
    const metricsUrl = this.config<string>("dashboardUrl");
    const logFile = this.config<string>("logFile");
    const pid = await this.resolvePid();
    if (!metricsUrl && !logFile && pid === null) {
      vscode.window.showWarningMessage(
        "Mistlane: nothing to attach to — set mistlane.dashboardUrl, mistlane.logFile, or mistlane.pid/pidFile.",
      );
      return;
    }
    const session = new AttachSession({
      ...(metricsUrl ? { metrics: { baseUrl: metricsUrl, intervalMs: this.config<number>("metricsPollMs") ?? 1000 } } : {}),
      ...(logFile ? { log: { path: logFile, intervalMs: this.config<number>("logPollMs") ?? 500 } } : {}),
      ...(pid !== null ? { proc: { pid } } : {}),
    });
    session.on("metrics", (event) => {
      this.model.addMetrics(event);
      this.postSnapshot();
    });
    session.on("log", (event) => {
      this.model.addLog(event);
      this.postSnapshot();
    });
    session.on("proc", (sample) => {
      this.model.addProc(sample);
      this.postSnapshot();
    });
    const onState = (): void => {
      this.setStatus(session.status);
      this.postSnapshot();
    };
    session.on("metrics-state", onState);
    session.on("log-state", onState);
    session.on("proc-state", onState);
    this.session = session;
    session.attach();
    this.setStatus(session.status);
    this.revealPanel();
    this.postSnapshot();
  }

  detach(): void {
    this.session?.detach();
    this.session = null;
    this.model.reset();
    this.statusItem.text = "$(plug) mistlane: detached";
    this.postSnapshot();
  }

  revealPanel(): void {
    if (this.panel) {
      this.panel.reveal(vscode.ViewColumn.Beside);
      return;
    }
    const panel = vscode.window.createWebviewPanel(
      "mistlaneDashboard",
      "Mistlane Dashboard",
      vscode.ViewColumn.Beside,
      { enableScripts: true, retainContextWhenHidden: true },
    );
    panel.webview.html = buildPanelHtml(snapshot(this.model), randomNonce());
    panel.webview.onDidReceiveMessage((message) => this.onPanelMessage(message));
    panel.onDidDispose(() => {
      this.panel = null;
    });
    this.panel = panel;
  }

  private onPanelMessage(message: unknown): void {
    if (typeof message !== "object" || message === null) return;
    const { type, value } = message as { type?: string; value?: unknown };
    if (type === "setFilter" && typeof value === "string") {
      this.model.setFilter(value);
      this.postSnapshot();
    } else if (type === "openContract") {
      this.openContractAt(Number(value));
    }
  }

  private async openContractAt(line: number): Promise<void> {
    if (this.contractPath === "") return;
    try {
      const document = await vscode.workspace.openTextDocument(this.contractPath);
      const target = Math.min(Math.max(line - 1, 0), document.lineCount - 1);
      await vscode.window.showTextDocument(document, {
        selection: new vscode.Range(target, 0, target, 0),
        preview: true,
      });
    } catch (error) {
      vscode.window.showErrorMessage(
        `Mistlane: cannot open ${this.contractPath}: ${error instanceof Error ? error.message : error}`,
      );
    }
  }

  private postSnapshot(): void {
    if (!this.panel) return;
    void this.panel.webview.postMessage(snapshot(this.model));
  }

  dispose(): void {
    this.detach();
    this.statusItem.dispose();
  }
}

let controller: Controller | undefined;

export function activate(context: vscode.ExtensionContext): void {
  controller = new Controller(context);
  context.subscriptions.push(
    vscode.commands.registerCommand("mistlane.attach", () => void controller?.attach()),
    vscode.commands.registerCommand("mistlane.detach", () => controller?.detach()),
    vscode.commands.registerCommand("mistlane.showPanel", () => controller?.revealPanel()),
    { dispose: () => controller?.dispose() },
  );
}

export function deactivate(): void {
  controller?.dispose();
  controller = undefined;
}
