import { useEffect, useId, useRef, useState } from "react";
import type { Game, ModuleSetting } from "../../contracts/launcher";
import definitions from "../../contracts/profile-runtime.json";
import { gameSettingsDraft, inheritedGameSettings } from "../gameSettings";
import { gamePolicy } from "../policies";
import { t, tText } from "../localization";
import { Modal, useLauncher } from "./context";
const tabs = ["graphics", "audio", "input", "module"] as const;
type Tab = (typeof tabs)[number];
const labels: Record<Tab, string> = {
  graphics: "gameSettings.graphics",
  audio: "gameSettings.audio",
  input: "gameSettings.controls",
  module: "gameSettings.options",
};
function ValueControl({
  definition,
  value,
  disabled,
  change,
}: {
  definition: ModuleSetting;
  value: unknown;
  disabled: boolean;
  change: (value: unknown) => void;
}) {
  const options = definition.options?.map((o) =>
    typeof o === "object" ? o : { label: String(o), value: o },
  );
  if (definition.type === "bool")
    return (
      <input
        aria-label={definition.label}
        type="checkbox"
        disabled={disabled}
        checked={Boolean(value)}
        onChange={(e) => change(e.target.checked)}
      />
    );
  if (options?.length && definition.type !== "number")
    return (
      <select
        aria-label={definition.label}
        disabled={disabled}
        value={options.findIndex((o) => o.value === value)}
        onChange={(e) => change(options[Number(e.target.value)].value)}
      >
        {options.map((option, i) => (
          <option key={i} value={i}>
            {option.label}
          </option>
        ))}
      </select>
    );
  if (definition.type === "number")
    return (
      <input
        aria-label={definition.label}
        type="number"
        disabled={disabled}
        value={Number(value)}
        min={definition.minimum}
        max={definition.maximum}
        step="any"
        onChange={(e) => change(e.target.valueAsNumber)}
      />
    );
  return (
    <input
      aria-label={definition.label}
      disabled={disabled}
      value={String(value ?? "")}
      maxLength={4096}
      onChange={(e) => change(e.target.value)}
    />
  );
}
export function GameSettings({
  game,
  close,
}: {
  game: Game;
  close: () => void;
}) {
  const { state, busy, action } = useLauncher();
  const [tab, setTab] = useState<Tab>("graphics");
  const [draft, setDraft] = useState(() => gameSettingsDraft(game));
  const original = useRef(JSON.stringify(gameSettingsDraft(game)));
  const originalModule = useRef(game.moduleId ?? "");
  const dirty = JSON.stringify(draft) !== original.current;
  const changedModule = originalModule.current !== (game.moduleId ?? "");
  const locked = !gamePolicy(state, game, busy).edit || changedModule;
  const module = state.modules?.find((m) => m.id === game.moduleId);
  const inherited = inheritedGameSettings(state);
  const prefix = useId();
  const tabList = useRef<HTMLDivElement>(null);
  const tabRef = useRef(tab);
  tabRef.current = tab;
  useEffect(() => {
    const cycle = (event: Event) => {
      const step = (event as CustomEvent).detail === -1 ? -1 : 1;
      const index =
        (tabs.indexOf(tabRef.current) + step + tabs.length) % tabs.length;
      setTab(tabs[index]);
      tabList.current
        ?.querySelectorAll<HTMLButtonElement>("button")
        [index]?.focus();
    };
    window.addEventListener("launcher-cycle-page", cycle);
    return () => window.removeEventListener("launcher-cycle-page", cycle);
  }, []);
  const moduleValues = module?.settings ?? {};
  const stale = Object.keys(draft.module).some(
    (key) => !module?.schema.some((d) => d.id === key),
  );
  return (
    <Modal
      title={t("gameSettings.title", { title: game.title })}
      close={close}
      dirty={dirty}
    >
      <p>{t("gameSettings.explanation")}</p>
      <p>
        {t("gameSettings.profile", {
          name:
            state.profiles.find((p) => p.id === state.activeProfileId)?.name ??
            "",
        })}
      </p>
      {!state.capabilities.runtime && (
        <p role="status">{t("gameSettings.runtimeLater")}</p>
      )}
      {changedModule && <p role="alert">{t("gameSettings.moduleChanged")}</p>}
      <div
        ref={tabList}
        className="game-settings-tabs"
        role="tablist"
        aria-label={t("gameSettings.categories")}
        onKeyDown={(event) => {
          let index = tabs.indexOf(tab);
          if (event.key === "ArrowRight") index = (index + 1) % tabs.length;
          else if (event.key === "ArrowLeft")
            index = (index + tabs.length - 1) % tabs.length;
          else if (event.key === "Home") index = 0;
          else if (event.key === "End") index = tabs.length - 1;
          else return;
          event.preventDefault();
          setTab(tabs[index]);
          tabList.current
            ?.querySelectorAll<HTMLButtonElement>("button")
            [index]?.focus();
        }}
      >
        {tabs.map((id) => (
          <button
            key={id}
            id={`${prefix}-${id}`}
            type="button"
            role="tab"
            aria-selected={tab === id}
            aria-controls={`${prefix}-panel`}
            tabIndex={tab === id ? 0 : -1}
            onClick={() => setTab(id)}
          >
            {t(labels[id])}
          </button>
        ))}
      </div>
      <form
        onSubmit={async (event) => {
          event.preventDefault();
          if (locked) return;
          if (
            await action("library.settings.save", {
              id: game.id,
              moduleId: game.moduleId ?? "",
              runtimeOverrides: draft.runtime,
              moduleOverrides: draft.module,
            })
          )
            close();
        }}
      >
        <section
          id={`${prefix}-panel`}
          role="tabpanel"
          aria-labelledby={`${prefix}-${tab}`}
          className="game-settings-panel"
        >
          {tab !== "module" &&
            definitions
              .filter((d) =>
                tab === "graphics"
                  ? d.category === "graphics" || d.category === "runtime"
                  : d.category === tab,
              )
              .map((d) => {
                const custom = Object.hasOwn(draft.runtime, d.key),
                  value = custom ? draft.runtime[d.key] : inherited[d.key];
                const label = tText(d.label),
                  hint = `${prefix}-${d.key.replaceAll("/", "-")}`;
                const hintKey =
                  "gameSettings.hint." + d.key.replaceAll("/", ".");
                const change = (value: unknown) =>
                  setDraft((previous) => ({
                    ...previous,
                    runtime: { ...previous.runtime, [d.key]: value },
                  }));
                const definition: ModuleSetting = {
                  id: d.key,
                  type: d.type,
                  label,
                  default: d.default,
                  options: d.options.map((o) => ({
                    ...o,
                    label: tText(o.label),
                  })),
                  ...(d as { minimum?: number; maximum?: number }),
                };
                definition.label = label;
                return (
                  <fieldset
                    className="game-setting"
                    key={d.key}
                    disabled={locked}
                    aria-describedby={hint}
                  >
                    <legend>{label}</legend>
                    <p id={hint}>{t(hintKey)}</p>
                    <label className="row">
                      <input
                        type="checkbox"
                        aria-label={t("gameSettings.override", { label })}
                        checked={custom}
                        onChange={(event) => {
                          if (event.target.checked) change(inherited[d.key]);
                          else
                            setDraft((previous) => {
                              const runtime = { ...previous.runtime };
                              delete runtime[d.key];
                              return { ...previous, runtime };
                            });
                        }}
                      />
                      {t("gameSettings.override", { label })}
                    </label>
                    <ValueControl
                      definition={definition}
                      value={value}
                      disabled={locked || !custom}
                      change={change}
                    />
                    {!custom && <small>{t("gameSettings.inherited")}</small>}
                  </fieldset>
                );
              })}
          {tab === "module" && (
            <>
              {(!module || !module.schema.length) && (
                <p>{t("gameSettings.noOptions")}</p>
              )}
              {stale && <p role="alert">{t("gameSettings.staleOptions")}</p>}
              {module?.schema.map((d) => {
                const custom = Object.hasOwn(draft.module, d.id),
                  value = custom
                    ? draft.module[d.id]
                    : (moduleValues[d.id] ?? d.default);
                const label = d.label ?? d.id;
                const change = (value: unknown) =>
                  setDraft((previous) => ({
                    ...previous,
                    module: { ...previous.module, [d.id]: value },
                  }));
                return (
                  <fieldset
                    className="game-setting"
                    key={d.id}
                    disabled={locked}
                  >
                    <legend>{label}</legend>
                    <p>{d.description}</p>
                    <label className="row">
                      <input
                        type="checkbox"
                        aria-label={t("gameSettings.override", { label })}
                        checked={custom}
                        onChange={(event) => {
                          if (event.target.checked)
                            change(moduleValues[d.id] ?? d.default);
                          else
                            setDraft((previous) => {
                              const module = { ...previous.module };
                              delete module[d.id];
                              return { ...previous, module };
                            });
                        }}
                      />
                      {t("gameSettings.override", { label })}
                    </label>
                    <ValueControl
                      definition={d}
                      value={value}
                      disabled={locked || !custom}
                      change={change}
                    />
                    {!custom && (
                      <small>{t("gameSettings.moduleDefault")}</small>
                    )}
                  </fieldset>
                );
              })}
            </>
          )}
        </section>
        <div className="actions">
          <button
            className="primary"
            type="submit"
            disabled={locked || !dirty || stale}
          >
            {t("gameSettings.save")}
          </button>
          <button
            type="button"
            disabled={locked}
            onClick={() =>
              setDraft((previous) =>
                tab === "module"
                  ? { ...previous, module: {} }
                  : {
                      ...previous,
                      runtime: Object.fromEntries(
                        Object.entries(previous.runtime).filter(
                          ([key]) =>
                            !definitions.some(
                              (d) =>
                                d.key === key &&
                                (tab === "graphics"
                                  ? d.category === "graphics" ||
                                    d.category === "runtime"
                                  : d.category === tab),
                            ),
                        ),
                      ),
                    },
              )
            }
          >
            {t("gameSettings.resetTab")}
          </button>
          <button
            type="button"
            disabled={locked}
            onClick={() => setDraft({ runtime: {}, module: {} })}
          >
            {t("gameSettings.resetAll")}
          </button>
        </div>
        <p>{t("gameSettings.nextLaunch")}</p>
      </form>
    </Modal>
  );
}
