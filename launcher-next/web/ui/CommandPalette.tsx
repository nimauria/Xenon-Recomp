import { t } from "../localization";
import { useEffect, useState } from "react";
import type { LauncherIntent } from "../../contracts/launcher";
import { commandResults } from "../search";
import { Empty, Modal, useLauncher } from "./context";
export function CommandPalette({
  close,
  execute,
}: {
  close: () => void;
  execute: (intent: LauncherIntent) => Promise<void>;
}) {
  const { state } = useLauncher();
  const [query, setQuery] = useState("");
  const [selected, select] = useState(0);
  const results = commandResults(state, query);
  useEffect(() => select(0), [query]);
  return (
    <Modal title={t("ui.fa8a81ce45d6")} close={close}>
      <label>
        {t("ui.00ff7398c308")}
        <input
          autoFocus
          role="combobox"
          aria-autocomplete="list"
          aria-controls="command-results"
          aria-expanded="true"
          aria-activedescendant={
            results[selected] ? `command-option-${selected}` : undefined
          }
          value={query}
          onChange={(event) => setQuery(event.target.value)}
          onKeyDown={(event) => {
            if (event.key === "ArrowDown")
              select((value) => Math.min(results.length - 1, value + 1));
            else if (event.key === "ArrowUp")
              select((value) => Math.max(0, value - 1));
            else if (event.key === "Enter" && results[selected])
              void execute(results[selected].intent);
            else return;
            event.preventDefault();
          }}
        />
      </label>
      <div
        className="command-list"
        id="command-results"
        role="listbox"
        aria-label={t("ui.b269dc4e81a5")}
      >
        {results.map((item, index) => (
          <button
            role="option"
            aria-selected={index === selected}
            tabIndex={-1}
            id={`command-option-${index}`}
            key={item.id}
            onMouseEnter={() => select(index)}
            onClick={() => void execute(item.intent)}
          >
            <span>{item.title}</span>
            <small>
              {item.description} · {item.group}
            </small>
          </button>
        ))}
      </div>
      {!results.length && (
        <Empty title={t("ui.1b8aace29798")}>{t("ui.d581988e9355")}</Empty>
      )}
    </Modal>
  );
}
