import type { Snapshot, UpdateState } from "../contracts/launcher";
export function updatePackages(state: Snapshot) {
  const modules = new Map((state.modules ?? []).map((m) => [m.id, m.name]));
  const catalogue = new Map(
    (state.catalog?.entries ?? []).map((m) => [m.id, m.name]),
  );
  return [
    {
      id: "launcher",
      name: "Xenon Launcher",
      launcher: true,
      value: state.launcherUpdate,
    },
    ...Object.entries(state.moduleUpdates ?? {}).map(([id, value]) => ({
      id,
      name: modules.get(id) ?? catalogue.get(id) ?? id,
      launcher: false,
      value,
    })),
  ];
}
export function installBlock(
  value: UpdateState | undefined,
  launcher: boolean,
  supported = true,
) {
  if (!supported) return "unsupported";
  if (value?.canInstall) return "";
  if (value?.busy) return "busy";
  if (value?.state === "up-to-date") return "installed";
  if (value?.state === "ready-to-install" && launcher) return "manual";
  if (
    ["no-release", "package-unavailable", "digest-unavailable"].includes(
      value?.state ?? "",
    )
  )
    return "unavailable";
  if (!value?.release) return "check";
  return "download";
}
