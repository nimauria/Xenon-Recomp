export function playtime(milliseconds = 0) {
  const value =
    Number.isFinite(milliseconds) && milliseconds > 0 ? milliseconds : 0;
  const seconds = Math.floor(value / 1000);
  if (seconds < 60) return `${seconds} s`;
  const minutes = Math.floor(seconds / 60);
  return minutes < 60
    ? `${minutes} min`
    : `${Math.floor(minutes / 60)} h ${minutes % 60} min`;
}
export function activityTime(value?: string | null) {
  if (!value) return 0;
  const number = Number(value);
  return Number.isFinite(number) ? number : Date.parse(value) || 0;
}
