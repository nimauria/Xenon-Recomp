import { t } from "../localization";
import {
  createContext,
  useContext,
  useEffect,
  useId,
  useRef,
  type ReactNode,
  type Dispatch,
  type SetStateAction,
} from "react";
import type { Page, Snapshot } from "../../contracts/launcher";
export interface LibraryBrowse {
  selected: string;
  filter: string;
  sort: string;
  scroll: number;
}
export interface LauncherContext {
  state: Snapshot;
  busy: boolean;
  query: string;
  navigate: (page: Page) => void;
  openGame: (id: string) => void;
  libraryBrowse: LibraryBrowse;
  setLibraryBrowse: Dispatch<SetStateAction<LibraryBrowse>>;
  action: (action: string, data?: Record<string, unknown>) => Promise<boolean>;
  pick: (mode: "open" | "multiple" | "folder" | "save") => Promise<string[]>;
  refresh: () => Promise<void>;
  navigationIntent?: { page: Page; targetId?: string; section?: string };
  clearNavigationIntent: () => void;
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
  const titleId = useId();
  function dismiss() {
    if (!dirty || window.confirm(t("ui.470e7ee2a579"))) close();
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
      aria-labelledby={titleId}
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
          <h2 id={titleId}>{title}</h2>
          <button aria-label={t("ui.c8df66c5fe3f")} onClick={dismiss}>
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
