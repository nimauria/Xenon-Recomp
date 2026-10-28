import { useLauncher } from "./context";
export function SessionPanel() {
  const { state, busy, action } = useLauncher();
  const session = state.session;
  if (!session || session.state === "unavailable") return null;
  const active = !["idle", "stopped", "failed"].includes(session.state);
  return (
    <section
      className={`session-panel${session.state === "failed" ? " session-failed" : ""}`}
      aria-label="Current session"
    >
      <div>
        <strong>
          {session.fixture ? "Development fixture runtime" : "Runtime session"}
        </strong>
        <span role="status">
          {session.title ? `${session.title} · ` : ""}
          {session.state.replaceAll("-", " ")}
        </span>
        {session.fixture && (
          <small>
            This provider tests launcher lifecycle only. It does not execute a
            game.
          </small>
        )}
        {session.lastError && <p role="alert">{session.lastError}</p>}
      </div>
      {active ? (
        <button
          disabled={busy || session.state === "stopping"}
          onClick={() => void action("session.stop")}
        >
          Stop test session
        </button>
      ) : (
        session.state !== "idle" && (
          <button
            disabled={busy}
            onClick={() => void action("session.dismiss")}
          >
            Dismiss
          </button>
        )
      )}
    </section>
  );
}
