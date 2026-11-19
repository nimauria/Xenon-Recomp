export function sessionActive(state?: string) {
  return (
    !!state &&
    !["idle", "unavailable", "stopped", "failed", "exited", "crashed"].includes(
      state,
    )
  );
}
