import { t, useLocalization, translationCoverage } from "../localization";
import { useLauncher } from "./context";
export function LanguageSettings() {
  const { state, action, busy } = useLauncher();
  const locale = useLocalization(),
    coverage = translationCoverage(locale.language);
  return (
    <section aria-label={t("language.heading")}>
      <h3>{t("language.heading")}</h3>
      <label>
        {t("language.choose")}
        <select
          value={String(state.settings?.["general/language"] ?? "auto")}
          disabled={busy}
          onChange={(e) =>
            void action("settings.set", {
              key: "general/language",
              value: e.target.value,
            })
          }
        >
          <option value="auto">{t("language.system")}</option>
          {Object.entries(locale.languages).map(([id, value]) => (
            <option key={id} value={id}>
              {value.nativeName}
            </option>
          ))}
        </select>
      </label>
      <p>{t("language.fallback")}</p>
      {locale.language !== "en" && coverage.translated < coverage.total && (
        <p>{t("language.partial")}</p>
      )}
    </section>
  );
}
