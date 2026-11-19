import { t } from "../localization";
import { Component, type ErrorInfo, type ReactNode } from "react";
export class ErrorBoundary extends Component<
  { children: ReactNode },
  { error: string }
> {
  state = { error: "" };
  static getDerivedStateFromError(error: Error) {
    return { error: error.message };
  }
  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error("Launcher render failed", error.message, info.componentStack);
  }
  render() {
    return this.state.error ? (
      <main className="startup" role="alert">
        <h1>{t("ui.93f5e76befd5")}</h1>
        <p>{this.state.error}</p>
        <p>{t("ui.daf3302ae966")}</p>
        <button onClick={() => location.reload()}>
          {t("ui.a0912e803e23")}
        </button>
      </main>
    ) : (
      this.props.children
    );
  }
}
