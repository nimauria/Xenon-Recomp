import type { Page } from "../contracts/launcher";
export const navigationGroups: {
  label: string;
  pages: Page[];
  bottom?: boolean;
  developer?: boolean;
}[] = [
  { label: "Browse", pages: ["Home", "Library", "Captures"] },
  { label: "Manage", pages: ["Modules", "Profiles"] },
  { label: "Activity", pages: ["Downloads"] },
  {
    label: "Developer tools",
    pages: ["Network", "Filesystem", "Developer"],
    developer: true,
  },
  { label: "Application", pages: ["Settings", "Support"], bottom: true },
];
export const pages = navigationGroups
  .filter((group) => !group.developer)
  .flatMap((group) => group.pages);
