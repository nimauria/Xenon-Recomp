import { t, tText } from "../localization";
import { useLauncher } from "./context";
export function SessionPanel() {
  const { state, busy, action } = useLauncher();
  const session = state.session;
  if (!session || session.state === "unavailable") return null;
  const active = !["idle", "stopped", "failed"].includes(session.state);
  return (
    <section
      className={`session-panel${session.state === "failed" ? " session-failed" : ""}`}
      aria-label={t("ui.a32789d2b53d")}
    >
      <div>
        <strong>
          {session.fixture ? t("ui.b85587ed6193") : t("ui.92fbc8484002")}
        </strong>
        <span role="status">
          {session.title ? `${session.title} · ` : ""}
          {tText(session.state)}
        </span>
        {session.fixture && <small>{t("ui.5203ba38197c")}</small>}
        {session.lastError && <p role="alert">{tText(session.lastError)}</p>}
      </div>
      {active ? (
        <button
          disabled={busy || session.state === "stopping"}
          onClick={() => void action("session.stop")}
        >
          {t("ui.7b4213c0988d")}
        </button>
      ) : (
        session.state !== "idle" && (
          <button
            disabled={busy}
            onClick={() => void action("session.dismiss")}
          >
            {t("ui.48845bff334a")}
          </button>
        )
      )}
    </section>
  );
}
