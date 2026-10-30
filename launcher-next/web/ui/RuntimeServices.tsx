import { t } from "../localization";
import { useEffect, useState } from "react";
import type { Page } from "../../contracts/launcher";
import type { ServiceOperation } from "../../contracts/services";
import { isActiveJob } from "../jobs";
import { useLauncher } from "./context";

export function InputSettings() {
  const { state, action, busy } = useLauncher();
  const provider = state.runtimeServices;
  const input = provider?.input,
    blocked =
      busy ||
      !provider?.capabilities.input ||
      state.jobs?.some(
        (job) => job.kind === "service.operation" && isActiveJob(job),
      );
  const [devices, setDevices] = useState<Record<number, string>>({});
  const send = (operation: ServiceOperation) =>
    action(operation.command, operation.data);
  return (
    <section aria-label={t("ui.b25979de357c")}>
      <h3>{t("ui.940ce5bed3ec")}</h3>
      <p>{input?.status ?? t("ui.5ed6aa32cd0a")}</p>
      {provider?.fixture && <p className="status">{t("ui.12ff42a703ef")}</p>}
      <div className="actions">
        <button
          disabled={blocked}
          onClick={() => void send({ command: "input.refresh", data: {} })}
        >
          {t("ui.18ea204d7d01")}
        </button>
        <button
          disabled={blocked}
          onClick={() => void send({ command: "input.reconfigure", data: {} })}
        >
          {t("ui.c22f8eea129f")}
        </button>
      </div>
      <ul>
        {input?.devices.map((device) => (
          <li key={device.id}>
            {device.name} · {device.kind} ·{" "}
            {device.connected ? t("ui.22965568d22a") : t("ui.04dfac3671b4")} ·{" "}
            {device.provider}
          </li>
        ))}
      </ul>
      {input?.users.map((user) => (
        <fieldset key={user.index} disabled={blocked} className="input-player">
          <legend>
            {t("ui.2b6565434216")}
            {user.index + 1}
          </legend>
          <p>
            {user.automatic
              ? t("ui.39fcfc71c506")
              : user.sources.length
                ? t("ui.5321ef16d7dd")
                : t("ui.52f59ee3e0ec")}
          </p>
          <label>
            {t("ui.6ba0bdeccb42")}
            <select
              aria-label={t("ui.5f06f340f0a1", { arg0: user.index + 1 })}
              value={devices[user.index] ?? input.devices[0]?.id ?? ""}
              onChange={(event) =>
                setDevices((previous) => ({
                  ...previous,
                  [user.index]: event.target.value,
                }))
              }
            >
              {input.devices.map((device) => (
                <option value={device.id} key={device.id}>
                  {device.name}
                </option>
              ))}
            </select>
          </label>
          <div className="actions">
            <button
              onClick={() =>
                void send({
                  command: "input.assign",
                  data: {
                    user: user.index,
                    deviceId: devices[user.index] ?? input.devices[0]?.id ?? "",
                  },
                })
              }
            >
              {t("ui.f38714fd2161")}
            </button>
            <button
              onClick={() =>
                void send({
                  command: "input.sourceAdd",
                  data: {
                    user: user.index,
                    deviceId: devices[user.index] ?? input.devices[0]?.id ?? "",
                  },
                })
              }
            >
              {t("ui.6ea6c9ec2a76")}
            </button>
            <button
              onClick={() =>
                void send({
                  command: "input.automatic",
                  data: { user: user.index },
                })
              }
            >
              {t("ui.539ca38b1026")}
            </button>
            <button
              onClick={() =>
                void send({
                  command: "input.clear",
                  data: { user: user.index },
                })
              }
            >
              {t("ui.1998926869df")}
            </button>
            <button
              onClick={() =>
                void send({
                  command: "input.vibrate",
                  data: { user: user.index },
                })
              }
            >
              {provider?.fixture ? t("ui.07f8c3177e63") : t("ui.c76b072d9b94")}
            </button>
          </div>
          <ul>
            {user.sources.map((id) => (
              <li key={id}>
                {input.devices.find((device) => device.id === id)?.name ?? id}{" "}
                <button
                  aria-label={t("ui.88e95264db54", {
                    arg0: id,
                    arg1: user.index + 1,
                  })}
                  onClick={() =>
                    void send({
                      command: "input.sourceRemove",
                      data: { user: user.index, deviceId: id },
                    })
                  }
                >
                  {t("ui.f55296180370")}
                </button>
              </li>
            ))}
          </ul>
          <label>
            {t("ui.74344e5f699d")}
            <select
              aria-label={t("ui.fce98dbd82ef", { arg0: user.index + 1 })}
              value={user.profileId}
              onChange={(event) =>
                void send(
                  event.target.value
                    ? {
                        command: "input.profileBind",
                        data: {
                          user: user.index,
                          profileId: event.target.value,
                        },
                      }
                    : {
                        command: "input.profileClear",
                        data: { user: user.index },
                      },
                )
              }
            >
              <option value="">{t("ui.83a1853b9f77")}</option>
              {input.profiles.map((profile) => (
                <option key={profile.id} value={profile.id}>
                  {profile.name}
                </option>
              ))}
            </select>
          </label>
        </fieldset>
      ))}
      {input?.profileStorePath && (
        <p className="path">
          {t("ui.da971d7cbf54")}
          {input.profileStorePath}
        </p>
      )}
      {input?.moduleApi && (
        <details>
          <summary>{t("ui.e13008943671")}</summary>
          <pre>{JSON.stringify(input.moduleApi, null, 2)}</pre>
        </details>
      )}
    </section>
  );
}
export function RuntimeServices({ page }: { page: Page }) {
  const { state, action, busy, pick } = useLauncher();
  const provider = state.runtimeServices;
  const blocked = Boolean(
    busy ||
    state.jobs?.some(
      (job) => job.kind === "service.operation" && isActiveJob(job),
    ),
  );
  const [mountPoint, mount] = useState("game:"),
    [kind, setKind] = useState<"host" | "gdfx" | "stfs">("host"),
    [source, setSource] = useState(""),
    [readOnly, setReadOnly] = useState(true);
  const [alias, setAlias] = useState("dvd:"),
    [target, setTarget] = useState("game:/"),
    [path, setPath] = useState("game:/");
  const [online, setOnline] = useState(navigator.onLine);
  useEffect(() => {
    const update = () => setOnline(navigator.onLine);
    window.addEventListener("online", update);
    window.addEventListener("offline", update);
    return () => {
      window.removeEventListener("online", update);
      window.removeEventListener("offline", update);
    };
  }, []);
  const send = (operation: ServiceOperation) =>
    action(operation.command, operation.data);
  if (page === "Network")
    return (
      <section className="card">
        <h2>{t("ui.05c8305db484")}</h2>
        <p>
          {t("ui.2bfcf23d49c2")}
          {online ? t("ui.22965568d22a") : t("ui.04dfac3671b4")}
          {t("ui.c1723e251862")}
        </p>
        {provider?.fixture && <p className="status">{t("ui.5570bfab4674")}</p>}
        <button
          disabled={blocked || !provider?.capabilities.network}
          onClick={() => void send({ command: "network.refresh", data: {} })}
        >
          {t("ui.ffc1364cefeb")}
        </button>
        <dl>
          {Object.entries(provider?.network ?? {}).map(([key, value]) => (
            <div key={key}>
              <dt>{key.replace(/([A-Z])/g, " $1")}</dt>
              <dd>
                {Array.isArray(value)
                  ? value.join(", ") || t("ui.dc937b598926")
                  : typeof value === "boolean"
                    ? value
                      ? t("ui.85a39ab345d6")
                      : t("ui.1ea442a134b2")
                    : String(value || "—")}
              </dd>
            </div>
          ))}
        </dl>
        {!provider?.capabilities.network && <p>{t("ui.596248effa9f")}</p>}
        {provider?.fixture && (
          <label>
            {t("ui.cc2f759e31ff")}
            <select
              aria-label={t("ui.cc2f759e31ff")}
              disabled={blocked}
              defaultValue="offline"
              onChange={(event) =>
                void send({
                  command: "network.fixtureScenario",
                  data: { scenario: event.target.value as "offline" },
                })
              }
            >
              {[
                "offline",
                "transport-only",
                "ready",
                "protocol-mismatch",
                "auth-failed",
              ].map((scenario) => (
                <option key={scenario}>{scenario}</option>
              ))}
            </select>
          </label>
        )}
      </section>
    );
  if (page === "Filesystem")
    return (
      <section className="card">
        <h2>{t("ui.d5d0484d6668")}</h2>
        <p>{provider?.fixture ? t("ui.eeae12acc5fd") : t("ui.2eff41d6b0c6")}</p>
        <p>
          {t("ui.8ecfebfa0349")}
          {provider?.filesystem.initialized
            ? t("ui.85a39ab345d6")
            : t("ui.1ea442a134b2")}
          {t("ui.377350b45c69")}{" "}
          {provider?.filesystem.workingDirectory || t("ui.4895f73177ab")}
        </p>
        <fieldset
          disabled={blocked || !provider?.capabilities.filesystem}
          className="form"
        >
          <legend>{t("ui.52c19d7619c6")}</legend>
          <label>
            {t("ui.e45cb1d3f925")}
            <input
              value={mountPoint}
              onChange={(event) => mount(event.target.value)}
            />
          </label>
          <label>
            {t("ui.73d67d413194")}
            <select
              value={kind}
              onChange={(event) => setKind(event.target.value as typeof kind)}
            >
              <option value="host">{t("ui.758876bc921d")}</option>
              <option value="gdfx">{t("ui.34c97de2630d")}</option>
              <option value="stfs">{t("ui.f431a92fc2ac")}</option>
            </select>
          </label>
          <label>
            {t("ui.87361df7f715")}
            <input
              value={source}
              onChange={(event) => setSource(event.target.value)}
            />
          </label>
          <button
            onClick={async () => {
              const [file] = await pick(kind === "host" ? "folder" : "open");
              if (file) setSource(file);
            }}
          >
            {t("ui.7c6009b4f005")}
          </button>
          <label className="row">
            {t("ui.8ac767353080")}
            <input
              type="checkbox"
              checked={readOnly}
              onChange={(event) => setReadOnly(event.target.checked)}
            />
          </label>
          <button
            onClick={() =>
              void send({
                command: "filesystem.mount",
                data: { mountPoint, kind, source, readOnly },
              })
            }
          >
            {t("ui.87361df7f715")}
          </button>
        </fieldset>
        <h3>{t("ui.87715ce37186")}</h3>
        {provider?.filesystem.mounts.map((entry) => (
          <article className="card" key={entry.mountPoint}>
            <strong>{entry.mountPoint}</strong>
            <p className="path">{entry.source}</p>
            <p>
              {entry.kind} ·{" "}
              {entry.readOnly ? t("ui.8ac767353080") : t("ui.18685c640b42")}
            </p>
            <button
              disabled={blocked}
              onClick={() =>
                void send({
                  command: "filesystem.unmount",
                  data: { mountPoint: entry.mountPoint },
                })
              }
            >
              {t("ui.c1d200b896f0")}
              {entry.mountPoint}
            </button>
          </article>
        ))}
        <fieldset
          disabled={blocked || !provider?.capabilities.filesystem}
          className="form"
        >
          <legend>{t("ui.3158348f1d1d")}</legend>
          <label>
            {t("ui.c9fd979aa275")}
            <input
              value={alias}
              onChange={(event) => setAlias(event.target.value)}
            />
          </label>
          <label>
            {t("ui.d3d5cecbb7dc")}
            <input
              value={target}
              onChange={(event) => setTarget(event.target.value)}
            />
          </label>
          <button
            onClick={() =>
              void send({ command: "filesystem.link", data: { alias, target } })
            }
          >
            {t("ui.35e9c5455526")}
          </button>
        </fieldset>
        {provider?.filesystem.symbolicLinks.map((link) => (
          <p key={link.alias}>
            {link.alias} → {link.target}{" "}
            <button
              disabled={blocked}
              onClick={() =>
                void send({
                  command: "filesystem.unlink",
                  data: { alias: link.alias },
                })
              }
            >
              {t("ui.5948954e3b49")}
              {link.alias}
            </button>
          </p>
        ))}
        <fieldset
          disabled={blocked || !provider?.capabilities.filesystem}
          className="form"
        >
          <legend>{t("ui.bf1f7a6dab58")}</legend>
          <label>
            {t("ui.bf1f7a6dab58")}
            <input
              value={path}
              onChange={(event) => setPath(event.target.value)}
            />
          </label>
          <div className="actions">
            <button
              onClick={() =>
                void send({ command: "filesystem.cwd", data: { path } })
              }
            >
              {t("ui.30fcdc918df9")}
            </button>
            <button
              onClick={() =>
                void send({ command: "filesystem.test", data: { path } })
              }
            >
              {t("ui.90db9634d9cd")}
            </button>
          </div>
        </fieldset>
        {state.serviceResult?.command.startsWith("filesystem.") && (
          <pre aria-label={t("ui.c1e8adf47b65")}>
            {JSON.stringify(state.serviceResult.result, null, 2)}
          </pre>
        )}
      </section>
    );
  return (
    <section className="card">
      <h2>{t("ui.aab0a0a7c386")}</h2>
      <pre>
        {JSON.stringify(
          {
            revision: state.revision,
            protocolVersion: 1,
            capabilities: state.capabilities,
            runtimeServices: provider,
            serviceResult: state.serviceResult,
          },
          null,
          2,
        )}
      </pre>
    </section>
  );
}
