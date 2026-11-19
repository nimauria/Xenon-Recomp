import { readFile, writeFile, readdir, mkdir } from "node:fs/promises";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import parser from "prettier/plugins/typescript";
process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const file = "contracts/localization.json";
let catalog;
try {
  catalog = JSON.parse(await readFile(file, "utf8"));
} catch {
  catalog = {
    schema: "xenon.localization",
    version: 1,
    defaultLanguage: "en",
    languages: {
      en: { name: "English", nativeName: "English", direction: "ltr" },
      fr: { name: "French", nativeName: "Français", direction: "ltr" },
      de: { name: "German", nativeName: "Deutsch", direction: "ltr" },
      es: { name: "Spanish", nativeName: "Español", direction: "ltr" },
    },
    translations: { en: {}, fr: {}, de: {}, es: {} },
    sources: {},
  };
}
const used = new Set();
function message(text, domain, source) {
  const key =
    domain + "." + createHash("sha256").update(text).digest("hex").slice(0, 12);
  catalog.translations.en[key] ??= text;
  catalog.sources[key] ??= { original: text, locations: [] };
  if (!catalog.sources[key].locations.includes(source))
    catalog.sources[key].locations.push(source);
  used.add(key);
  return key;
}
function jsxText(value) {
  const lines = value.split(/\r\n|\n|\r/);
  let last = 0;
  lines.forEach((line, i) => {
    if (/\S/.test(line)) last = i;
  });
  return lines
    .map((line, i) => {
      let value = line.replace(/\t/g, " ");
      if (i) value = value.replace(/^ +/, "");
      if (i < lines.length - 1) value = value.replace(/ +$/, "");
      return value + (value && i < last ? " " : "");
    })
    .join("");
}
async function files(folder, extension) {
  const out = [];
  for (const entry of await readdir(folder, { withFileTypes: true })) {
    const path = folder + "/" + entry.name;
    if (entry.isDirectory()) out.push(...(await files(path, extension)));
    else if (extension.test(path)) out.push(path);
  }
  return out;
}
for (const path of await files("web/ui", /\.tsx$/)) {
  let source = await readFile(path, "utf8");
  const ast = parser.parsers.typescript.parse(source, {}),
    edits = [];
  let changed = false;
  const register = (text, node) =>
    message(text, "ui", path + ":" + node.loc.start.line);
  const call = (text, node) => `t(${JSON.stringify(register(text, node))})`;
  function display(node) {
    if (!node) return;
    if (
      node.type === "Literal" &&
      typeof node.value === "string" &&
      /[A-Za-z]/.test(node.value)
    ) {
      edits.push({
        start: node.range[0],
        end: node.range[1],
        value: call(node.value, node),
      });
      return;
    }
    if (node.type === "ConditionalExpression") {
      display(node.consequent);
      display(node.alternate);
    }
    if (node.type === "LogicalExpression") display(node.right);
    if (
      node.type === "TemplateLiteral" &&
      node.quasis.some((q) => /[A-Za-z]{2}/.test(q.value.cooked ?? ""))
    ) {
      const params = node.expressions.map((expression, index) => ({
        name: "arg" + index,
        value: source.slice(...expression.range),
      }));
      const text = node.quasis
        .map(
          (q, index) =>
            (q.value.cooked ?? q.value.raw) +
            (params[index] ? `{${params[index].name}}` : ""),
        )
        .join("");
      edits.push({
        start: node.range[0],
        end: node.range[1],
        value: `t(${JSON.stringify(register(text, node))}, {${params.map((p) => p.name + ": " + p.value).join(", ")}})`,
      });
    }
  }
  function walk(node) {
    if (!node || typeof node !== "object") return;
    // Values, roles, CSS classes, IDs and protocol enums are never translated.
    if (
      node.type === "JSXAttribute" &&
      !["title", "aria-label", "placeholder", "label"].includes(node.name?.name)
    )
      return;
    if (
      node.type === "JSXText" &&
      /[A-Za-z]/.test(node.value) &&
      !["catalog-art", "downloads"].includes(node.value.trim())
    ) {
      const text = jsxText(node.value);
      if (text.trim())
        edits.push({
          start: node.range[0],
          end: node.range[1],
          value: `{${call(text, node)}}`,
        });
    }
    if (
      node.type === "JSXAttribute" &&
      ["title", "aria-label", "placeholder", "label"].includes(
        node.name?.name,
      ) &&
      node.value?.type === "Literal" &&
      typeof node.value.value === "string" &&
      /[A-Za-z]/.test(node.value.value)
    )
      edits.push({
        start: node.value.range[0],
        end: node.value.range[1],
        value: `{${call(node.value.value, node)}}`,
      });
    if (node.type === "JSXExpressionContainer") display(node.expression);
    if (
      node.type === "JSXElement" &&
      node.openingElement.name?.name === "option" &&
      !node.openingElement.attributes.some((a) => a.name?.name === "value") &&
      node.children.length === 1 &&
      node.children[0].type === "JSXText"
    ) {
      const text = jsxText(node.children[0].value);
      edits.push({
        start: node.openingElement.range[1] - 1,
        end: node.openingElement.range[1] - 1,
        value: ` value=${JSON.stringify(text)}`,
      });
    }
    for (const [key, value] of Object.entries(node))
      if (!["tokens", "comments", "loc", "range"].includes(key)) {
        if (Array.isArray(value)) value.forEach(walk);
        else if (value && typeof value === "object" && value.type) walk(value);
      }
  }
  walk(ast);
  // Prompts in event handlers are visible copy too. Attribute traversal above deliberately
  // stops at handlers so that command IDs and input values inside them remain untouched.
  function prompts(node) {
    if (!node || typeof node !== "object") return;
    if (
      node.type === "CallExpression" &&
      node.callee?.type === "MemberExpression" &&
      node.callee.object?.name === "window" &&
      ["confirm", "alert", "prompt"].includes(node.callee.property?.name)
    )
      display(node.arguments[0]);
    for (const [key, value] of Object.entries(node))
      if (!["tokens", "comments", "loc", "range"].includes(key)) {
        if (Array.isArray(value)) value.forEach(prompts);
        else if (value && typeof value === "object" && value.type)
          prompts(value);
      }
  }
  prompts(ast);
  if (process.argv.includes("--convert") && edits.length) {
    await mkdir("build/localization-before", { recursive: true });
    await writeFile(
      "build/localization-before/" + path.replaceAll("/", "__"),
      source,
      { flag: "wx" },
    ).catch((error) => {
      if (error.code !== "EEXIST") throw error;
    });
    for (const edit of edits.sort((a, b) => b.start - a.start))
      source =
        source.slice(0, edit.start) + edit.value + source.slice(edit.end);
    if (
      !/import\s*\{[^}]*\bt\b[^}]*\}\s*from\s*["']\.\.\/localization["']/.test(
        source,
      )
    )
      source = 'import { t } from "../localization";\n' + source;
    await writeFile(path, source);
    changed = true;
  }
  console.log(
    `${path}: ${edits.length} text references${changed ? " converted" : " inventoried"}`,
  );
}
// Native source inventory records literal messages separately; core error/state wiring is audited
// independently rather than translating protocol enums, paths or arbitrary user content.
for (const path of await files("native", /\.cpp$/)) {
  if (path.includes("third_party/")) continue;
  const source = await readFile(path, "utf8");
  for (const match of source.matchAll(/"((?:\\.|[^"\\\n])*)"/g)) {
    try {
      const text = JSON.parse('"' + match[1] + '"');
      if (
        /[A-Za-z]+\s+[A-Za-z]+/.test(text) &&
        text.length < 1500 &&
        !/[<>]/.test(text)
      )
        message(
          text,
          "native",
          path +
            ":" +
            ((source.slice(0, match.index).match(/\n/g)?.length ?? 0) + 1),
        );
    } catch {}
  }
}
await writeFile(file, JSON.stringify(catalog, null, 2) + "\n");
console.log(
  `Master contains ${Object.keys(catalog.translations.en).length} English messages; ${used.size} found in this pass.`,
);
