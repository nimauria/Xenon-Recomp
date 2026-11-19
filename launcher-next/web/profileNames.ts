import data from "../contracts/profile-unicode.json";
const lower = new Map<number, string>(
  data.lower.map(([point, value]) => [Number(point), String(value)]),
);
const whitespace = new Set(data.space);
function scalars(name: string) {
  const result = Array.from(name);
  if (
    result.some((s) => {
      const p = s.codePointAt(0)!;
      return p === 0 || (p >= 0xd800 && p <= 0xdfff);
    })
  )
    throw new Error(
      "Profile names must contain valid Unicode without NUL characters",
    );
  return result;
}
export function trimProfileName(name: string) {
  const points = scalars(name);
  let first = 0,
    last = points.length;
  while (first < last && whitespace.has(points[first].codePointAt(0)!)) ++first;
  while (last > first && whitespace.has(points[last - 1].codePointAt(0)!))
    --last;
  return points.slice(first, last).join("");
}
export function profileNameKey(name: string) {
  return Array.from(
    trimProfileName(name),
    (point) => lower.get(point.codePointAt(0)!) ?? point,
  ).join("");
}
