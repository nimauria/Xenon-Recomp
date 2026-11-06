import { createRequire } from "node:module";
import { mkdir, copyFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
const require = createRequire(import.meta.url),
  root = fileURLToPath(new URL("../", import.meta.url));
const output = join(root, "public/licenses");
await mkdir(output, { recursive: true });
for (const name of ["react", "react-dom", "scheduler"])
  await copyFile(
    join(dirname(require.resolve(name + "/package.json")), "LICENSE"),
    join(output, name + "-LICENSE.txt"),
  );
