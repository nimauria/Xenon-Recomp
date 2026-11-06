import { t } from "../localization";
import { attentionItems } from "../attention";
import { useLauncher } from "./context";

export function HomeAttention() {
  const { state, navigate } = useLauncher();
  const items = attentionItems(state);
  if (!items.length) return null;
  return (
    <section className="card home-attention" aria-label={t("home.attention")}>
      <h2>{t("home.attention")}</h2>
      <div className="attention-items">
        {items.map((item) => (
          <button
            key={item.key}
            onClick={() =>
              navigate(
                item.page,
                undefined,
                ["failed", "ready"].includes(item.key) ? item.key : undefined,
              )
            }
          >
            <strong>{item.count}</strong>
            <span>{t(`home.attention.${item.key}`)}</span>
            <small>{t(`home.attention.${item.key}.action`)}</small>
          </button>
        ))}
      </div>
    </section>
  );
}
