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
        <h1>Unable to display the launcher</h1>
        <p>{this.state.error}</p>
        <p>Your stored data has been preserved.</p>
        <button onClick={() => location.reload()}>Reload launcher</button>
      </main>
    ) : (
      this.props.children
    );
  }
}
