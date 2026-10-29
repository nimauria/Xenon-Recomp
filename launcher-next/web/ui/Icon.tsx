import type { Page } from "../../contracts/launcher";
const paths: Record<Page | "Menu", string> = {
  Home: "M3 11 12 3l9 8M5 10v11h5v-7h4v7h5V10",
  Library: "M4 4h6v16H4zM14 4h6v16h-6zM7 8h0M17 8h0",
  Captures: "M3 7h5l2-3h4l2 3h5v13H3zM16 13a4 4 0 1 1-8 0 4 4 0 0 1 8 0",
  Modules: "M12 2 3 7v10l9 5 9-5V7zM3 7l9 5 9-5M12 12v10",
  Profiles: "M16 7a4 4 0 1 1-8 0 4 4 0 0 1 8 0M4 22v-3a8 8 0 0 1 16 0v3",
  Downloads: "M12 3v12m-5-5 5 5 5-5M4 16v5h16v-5",
  Settings:
    "M12 2v3m0 14v3M2 12h3m14 0h3M5 5l2 2m10 10 2 2M5 19l2-2M17 7l2-2M17 12a5 5 0 1 1-10 0 5 5 0 0 1 10 0",
  Support: "M3 14v-3a9 9 0 0 1 18 0v3M3 12h4v7H3zM17 12h4v7h-4zM19 19v3h-7",
  Network:
    "M3 10a14 14 0 0 1 18 0M6 14a9 9 0 0 1 12 0M9 18a4 4 0 0 1 6 0M12 21h0",
  Filesystem: "M2 6h8l2 3h10v12H2zM2 6V3h8l2 3h8v3",
  Developer: "m8 5-6 7 6 7m8-14 6 7-6 7M14 3l-4 18",
  Menu: "M4 6h16M4 12h16M4 18h16",
};
export function Icon({ name }: { name: Page | "Menu" }) {
  return (
    <svg
      className="icon"
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth="1.6"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
    >
      <path d={paths[name]} />
    </svg>
  );
}
