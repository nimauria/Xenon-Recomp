import {
  createContext,
  useContext,
  useEffect,
  useRef,
  type ReactNode,
} from "react";
import type { Page, Snapshot } from "../../contracts/launcher";
export interface LauncherContext {
  state: Snapshot;
  busy: boolean;
  query: string;
  navigate: (page: Page) => void;
  action: (action: string, data?: Record<string, unknown>) => Promise<boolean>;
  pick: (mode: "open" | "multiple" | "folder" | "save") => Promise<string[]>;
  refresh: () => Promise<void>;
}
export const Context = createContext<LauncherContext | null>(null);
export function useLauncher() {
  const context = useContext(Context);
  if (!context) throw new Error("Missing launcher provider");
  return context;
}
export function Empty({
  title,
  children,
}: {
  title: string;
  children?: ReactNode;
}) {
  return (
    <section className="empty">
      <h2>{title}</h2>
      <p>{children}</p>
    </section>
  );
}
export function Modal({
  title,
  children,
  close,
  dirty = false,
}: {
  title: string;
  children: ReactNode;
  close: () => void;
  dirty?: boolean;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  function dismiss() {
    if (!dirty || window.confirm("Discard unsaved changes?")) close();
  }
  useEffect(() => {
    const node = dialog.current!;
    const dismissForNavigation = () => close();
    node.addEventListener("launcher-dismiss", dismissForNavigation);
    node.showModal();
    return () => {
      node.removeEventListener("launcher-dismiss", dismissForNavigation);
      node.close();
    };
  }, []);
  useEffect(() => {
    if (!dirty) return;
    const guard = (event: BeforeUnloadEvent) => {
      event.preventDefault();
    };
    window.addEventListener("beforeunload", guard);
    return () => window.removeEventListener("beforeunload", guard);
  }, [dirty]);
  return (
    <dialog
      ref={dialog}
      data-dirty={dirty ? "true" : "false"}
      onCancel={(e) => {
        e.preventDefault();
        dismiss();
      }}
      onClick={(e) => {
        if (e.target === dialog.current) dismiss();
      }}
    >
      <section>
        <div className="heading">
          <h2>{title}</h2>
          <button aria-label="Close dialog" onClick={dismiss}>
            ×
          </button>
        </div>
        {children}
      </section>
    </dialog>
  );
}
export function downloadJson(name: string, value: unknown) {
  const url = URL.createObjectURL(
    new Blob([JSON.stringify(value, null, 2)], { type: "application/json" }),
  );
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = name;
  anchor.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
