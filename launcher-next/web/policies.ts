import type { Game, Snapshot, Profile } from "../contracts/launcher";
import { isActiveJob } from "./jobs";
export function gamePolicy(state: Snapshot, game: Game, busy: boolean) {
  const sessionActive =
    !!state.session &&
    ["preparing", "starting", "validating", "running", "stopping"].includes(
      state.session.state,
    );
  const ownSession =
    sessionActive &&
    (state.session?.gameId === game.id || !state.session?.gameId);
  const jobs = state.jobs?.some(
    (job) => (isActiveJob(job) || job.cleaningUp) && job.gameId === game.id,
  );
  const module = state.modules?.find((module) => module.id === game.moduleId);
  return {
    launch:
      !busy &&
      !sessionActive &&
      !!state.capabilities.runtime &&
      module?.enabled !== false &&
      !module?.compatibility?.issues.length,
    favorite: !busy,
    edit: !busy && !ownSession && !jobs,
    remove: !busy && !ownSession && !jobs,
    moduleSettings: !!module && !busy,
    openFiles: state.mode === "native",
  };
}
export function profilePolicy(
  state: Snapshot,
  profile: Profile,
  busy: boolean,
) {
  return {
    activate: !busy && profile.id !== state.activeProfileId,
    edit: !busy,
    duplicate: !busy,
    export: !busy && state.mode === "native",
    remove: !busy && profile.id !== state.activeProfileId,
  };
}
