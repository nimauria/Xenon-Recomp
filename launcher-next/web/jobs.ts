import type { Job } from "../contracts/launcher";
export function isActiveJob(job: Job) {
  return ["queued", "running", "cancelling", "committing"].includes(job.state);
}
