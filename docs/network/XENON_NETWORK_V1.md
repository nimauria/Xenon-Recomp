# Xenon Network Client / Uplink V1

## Scope and identity

Xenon Network is an independent service boundary for replacement online
functionality needed by Xbox 360 titles. It is not Xbox Live, does not connect
to Xbox Live, and does not impersonate Microsoft accounts or services.

The V1 work in this repository is the client foundation. It does not bundle a
hosted service, matchmaking algorithm, relay, NAT traversal system, production
identity provider, or title-specific multiplayer translation. Game modules may
declare title identity and capabilities, but they must not implement separate
network clients.

## Repository audit

Before V1, the repository contained:

- a dormant `XENON_ENABLE_NETWORK` build option with no `xenon_network` target;
- an honest launcher Network page which showed only host Internet reachability
  and said that Xenon online services were not implemented;
- Qt Network use isolated to launcher concerns such as module/update downloads;
- no Xbox/XAM/XNet online exports and no kernel socket export implementation;
- local profiles, content, saves, XAM user/content/notification/achievement
  services, and module metadata, but no remote identity/session model;
- shared `CapabilityReportBuilder`, `RunFingerprint`, and structured logger
  facilities suitable for network diagnostics;
- no general-purpose HTTPS/TLS dependency in the Qt-free runtime core.

The V1 design reuses the existing logger, JSON value/parser, host identity
helpers, capability-report shape, launcher settings service, and Qt Network
dependency. It does not duplicate profile storage or the runtime-readiness
scheduler.

## Layering

```text
guest Xbox call
  -> future XAM/XNet ABI marshalling
  -> XboxServicesNetworkAdapter
  -> XenonNetworkClient
  -> versioned route + protocol model
  -> NetworkTransport / NetworkRealtimeChannel
  -> Qt HTTPS adapter or another host adapter
  -> Xenon Network service
```

`XboxServicesNetworkAdapter` is the compatibility boundary. Guest-memory
structures are decoded above it; URLs, HTTP and JSON remain below it. Its
offline, disabled, unavailable, unauthenticated and unimplemented outcomes are
typed and deterministic. No Xbox networking ordinals are claimed as implemented
in this pass.

## Offline-first configuration

The client is compiled by default, but launcher configuration defaults to:

```text
network/enabled = false
network/environment = Offline
network/baseUrl = ""
network/connectTimeoutMs = 5000
network/requestTimeoutMs = 10000
```

The supported environments are `Offline`, `Development`, and `Production`.
There is deliberately no default production URL. A Production endpoint must use
HTTPS. Development also uses HTTPS except that explicit `localhost`,
`127.0.0.1`, or `[::1]` endpoints may use HTTP for local deterministic tests.
There is no certificate-bypass option.

Failure to configure or reach the network never prevents launcher startup,
local profiles/saves/content, title preparation, recompilation, or offline game
boot.

## Control plane and discovery

The initial control plane uses request/response operations through
`NetworkTransport`. The first operation is `GET /v1/bootstrap`. Required V1
fields are:

- `protocolVersion`;
- `minimumClientProtocol`.

Optional fields include the service version/time, maintenance state,
authentication requirement, capabilities, required capabilities, and discovered
realtime/relay endpoints. Unknown optional fields and optional capability names
are ignored. Unknown required capabilities, missing required fields, malformed
JSON, oversized responses, and incompatible protocol versions fail explicitly.

`GET /v1/health` tests service reachability only. A successful health response
does not imply authentication, protocol negotiation, or online-service
readiness.

The central route catalogue defines:

- bootstrap, health, and client handshake;
- authentication session create/delete/refresh;
- current profile, presence and friends;
- matchmaking ticket create/get/delete;
- session create/get/patch/delete/join/leave;
- connectivity allocation create/delete;
- the conceptual `/v1/events` realtime endpoint.

Unimplemented server operations are not simulated. A service or compatibility
caller receives a typed offline, unavailable, authentication-required, or
not-implemented result.

## Protocol and handshake

Protocol V1 declares both the current and minimum client protocol version.
Bootstrap negotiation must complete before the client reports `Ready`.

The handshake model contains only the protocol version, Xenon build/version,
host OS and architecture, requested capabilities, a request ID, and optional
generic title context (title ID/version, TU identity, module identity/version,
and runtime session identity). It does not include filesystem paths, arbitrary
files, machine names, account passwords, or other private host data.

Every request receives a correlation ID. Retryable mutating operations require
an idempotency key; a state-changing request without one is never blindly
retried.

## State machines

Connection state is explicit:

```text
Offline | Disabled | Resolving | Connecting | ConnectedTransport
| Authenticating | Ready | Degraded | Reconnecting | Unavailable | Error
```

`ConnectedTransport` is intentionally distinct from authenticated and ready.
Authentication separately reports unauthenticated, authenticating,
authenticated, refreshing, expired, revoked, or error. Realtime separately
reports disconnected, connecting, connected, reconnecting, unavailable, or
error.

The realtime controller is independent of HTTP and uses bounded exponential
backoff with jitter through the transport scheduler. After a disconnect it does
not claim that presence or session membership survived; a future service layer
must confirm and restore that state.

## Authentication and credentials

V1 defines an authentication/session abstraction but does not choose an account
provider or credential schema. Access and refresh credentials may be held in a
`CredentialStore`. The only implementation shipped in V1 is memory-only and
clears token bytes during shutdown. Persistent login is unavailable until a
platform keychain/credential-vault adapter is implemented. Passwords, signing
keys, and production secrets are not stored or generated by Xenon.

## Security and resource policy

- Qt performs normal certificate-chain and hostname validation. The adapter
  never calls `ignoreSslErrors`.
- Redirects may not downgrade transport security.
- Plaintext is restricted to explicit loopback Development endpoints.
- Connect, request, and realtime heartbeat timeouts are bounded.
- Response size, request count, queued-event count, capabilities and identifiers
  have configured or protocol bounds.
- Authorization values, access/refresh tokens, join tokens, relay connection
  tokens, and passwords are redacted from diagnostics.
- Network completion is asynchronous and never waits on the launcher GUI thread.
- Shutdown cancels operations, aborts replies, stops reconnect scheduling,
  disconnects realtime, and releases transports.

Qt 6 Network was already a launcher dependency and is cross-platform. V1 adds no
third-party dependency and does not hand-write TLS or cryptography. The runtime
protocol/client target remains Qt-free so another platform host can supply a
different conforming transport.

## Control plane versus gameplay data plane

HTTPS carries bootstrap, identity, profile, friends, presence, matchmaking,
session management, relay allocation, and metadata. It is not a gameplay packet
transport.

The generic `ConnectivityPlan` can describe `Direct`, `Relay`, or `Unavailable`
with peer/relay endpoints, a short-lived connection token, expiry, and session
identity. No production peer traversal or relay is implemented, and higher
layers do not assume that all titles are peer-to-peer, dedicated-server, or
relay-only.

## Launcher and diagnostics

The launcher Network page displays host reachability separately from client
state, then reports configuration, service reachability, protocol compatibility,
authentication, realtime state, and full readiness. Developer Mode additionally
shows the configured endpoint, negotiated capabilities, last contact, typed last
error, and local request counters. Tokens and secrets are never displayed.

`network_status_json()` produces the corresponding capability-report section,
including compiled/transport/configured/reachable/compatible/authenticated/
realtime/ready flags and local counters. A runtime session can publish this as
the `network` section through the existing `CapabilityReportBuilder` when it
owns a network client; V1 does not alter the active runtime-readiness scheduler.

## Remaining work

A real development service must implement the V1 bootstrap/health contract and
whichever optional routes it advertises. Later Xenon work must add a secure
persistent credential store, choose an identity provider, implement a concrete
secure realtime channel, implement NAT/direct/relay infrastructure, and map
individual Xbox/XAM/XNet APIs onto the compatibility boundary with correct guest
scheduler semantics. Title-specific requirements belong in capability metadata,
not in parallel client stacks.
