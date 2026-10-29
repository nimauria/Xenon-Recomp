/** Runtime-facing services are separate from the desktop host's own files/network. */
export interface HostServices {
  version: 1;
  fixture: boolean;
  revision: number;
  capabilities: {
    filesystem: boolean;
    input: boolean;
    network: boolean;
    content: boolean;
  };
  filesystem: {
    initialized: boolean;
    workingDirectory: string;
    mounts: {
      mountPoint: string;
      kind: "host" | "gdfx" | "stfs";
      source: string;
      readOnly: boolean;
      fixture?: boolean;
    }[];
    symbolicLinks: { alias: string; target: string }[];
  };
  input: {
    available: boolean;
    status: string;
    profileStorePath?: string;
    devices: {
      id: string;
      name: string;
      kind: string;
      connected: boolean;
      provider: string;
    }[];
    users: {
      index: number;
      automatic: boolean;
      sources: string[];
      profileId: string;
    }[];
    profiles: { id: string; name: string; schemaVersion: number }[];
    moduleApi?: {
      id: string;
      version: number;
      header: string;
      cmakeTarget: string;
      manifestKey: string;
      guestXamBridge: boolean;
      features: string[];
    };
  };
  network: {
    clientCompiled: boolean;
    configured: boolean;
    transportAvailable?: boolean;
    connectionState: string;
    connectionLabel: string;
    serviceReachable: boolean;
    protocolCompatible: boolean;
    authenticated: boolean;
    authenticationState?: string;
    realtimeState?: string;
    realtimeConnected: boolean;
    onlineServicesReady: boolean;
    protocolVersion: number;
    baseEndpoint?: string;
    capabilities: string[];
    lastSuccessfulContact?: string;
    requestsAttempted?: number;
    requestsFailed?: number;
    lastError: string;
  };
}
export type ServiceOperation =
  | {
      command: "filesystem.mount";
      data: {
        mountPoint: string;
        kind: "host" | "gdfx" | "stfs";
        source: string;
        readOnly: boolean;
      };
    }
  | { command: "filesystem.unmount"; data: { mountPoint: string } }
  | { command: "filesystem.link"; data: { alias: string; target: string } }
  | { command: "filesystem.unlink"; data: { alias: string } }
  | { command: "filesystem.cwd" | "filesystem.test"; data: { path: string } }
  | {
      command: "input.refresh" | "input.reconfigure" | "network.refresh";
      data: Record<string, never>;
    }
  | {
      command:
        | "input.automatic"
        | "input.clear"
        | "input.profileClear"
        | "input.vibrate";
      data: { user: number };
    }
  | {
      command: "input.assign" | "input.sourceAdd" | "input.sourceRemove";
      data: { user: number; deviceId: string };
    }
  | { command: "input.profileBind"; data: { user: number; profileId: string } }
  | {
      command: "network.fixtureScenario";
      data: {
        scenario:
          | "offline"
          | "transport-only"
          | "ready"
          | "protocol-mismatch"
          | "auth-failed";
      };
    }
  | {
      command: "content.identify" | "content.verify";
      data: { gameId: string };
    };
