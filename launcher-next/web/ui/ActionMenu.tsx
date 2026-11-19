import { createPortal } from "react-dom";
import { useEffect, useId, useRef, useState } from "react";
import { t } from "../localization";
export interface MenuAction {
  id: string;
  label: string;
  disabled?: boolean;
  run: () => void;
}
export interface MenuRequest {
  source: HTMLElement;
  x: number;
  y: number;
  token: number;
}
export function ActionMenu({
  label,
  items,
  request,
  quick = false,
}: {
  label: string;
  items: MenuAction[];
  request?: MenuRequest;
  quick?: boolean;
}) {
  const id = useId(),
    trigger = useRef<HTMLButtonElement>(null),
    menu = useRef<HTMLDivElement>(null);
  const anchor = useRef<HTMLElement | null>(null),
    [open, setOpen] = useState(false);
  function close() {
    menu.current?.hidePopover();
    if (anchor.current?.isConnected) anchor.current.focus();
  }
  function show(source: HTMLElement, x?: number, y?: number) {
    const node = menu.current;
    if (!node) return;
    anchor.current = source;
    node.showPopover();
    const rect = source.getBoundingClientRect(),
      width = node.offsetWidth,
      height = node.offsetHeight;
    node.style.left =
      Math.max(8, Math.min(x ?? rect.left, innerWidth - width - 8)) + "px";
    node.style.top =
      Math.max(8, Math.min(y ?? rect.bottom, innerHeight - height - 8)) + "px";
    node.querySelector<HTMLButtonElement>("button:not(:disabled)")?.focus();
  }
  useEffect(() => {
    if (request) show(request.source, request.x, request.y);
  }, [request]);
  useEffect(
    () => () => {
      menu.current?.hidePopover();
    },
    [],
  );
  return (
    <>
      <button
        data-quick-actions={quick ? "true" : undefined}
        ref={trigger}
        aria-label={label}
        aria-haspopup="menu"
        aria-expanded={open}
        aria-controls={id}
        onClick={() => {
          if (open) close();
          else if (trigger.current) show(trigger.current);
        }}
        onKeyDown={(event) => {
          if (event.key === "ArrowDown") {
            event.preventDefault();
            show(event.currentTarget);
          }
        }}
      >
        {t("menu.actions")}
      </button>
      {createPortal(
        <div
          id={id}
          ref={menu}
          popover="auto"
          role="menu"
          aria-label={label}
          className="action-menu"
          onToggle={(event) => setOpen(event.newState === "open")}
          onKeyDown={(event) => {
            const controls = Array.from(
              menu.current?.querySelectorAll<HTMLButtonElement>(
                "button:not(:disabled)",
              ) ?? [],
            );
            if (event.key === "Escape" || event.key === "Tab") {
              if (event.key === "Escape") event.preventDefault();
              event.stopPropagation();
              close();
              return;
            }
            if (!["ArrowUp", "ArrowDown", "Home", "End"].includes(event.key))
              return;
            event.preventDefault();
            event.stopPropagation();
            const index = controls.indexOf(
              document.activeElement as HTMLButtonElement,
            );
            const next =
              event.key === "Home"
                ? 0
                : event.key === "End"
                  ? controls.length - 1
                  : (index +
                      (event.key === "ArrowUp" ? -1 : 1) +
                      controls.length) %
                    controls.length;
            controls[next]?.focus();
          }}
        >
          {items.map((item) => (
            <button
              role="menuitem"
              key={item.id}
              disabled={item.disabled}
              onClick={() => {
                close();
                item.run();
              }}
            >
              {item.label}
            </button>
          ))}
        </div>,
        document.body,
      )}
    </>
  );
}
