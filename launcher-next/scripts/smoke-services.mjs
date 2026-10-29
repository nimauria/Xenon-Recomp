import { chromium } from "playwright-core";
import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
process.chdir(fileURLToPath(new URL("../",import.meta.url)));
const browser=await chromium.connectOverCDP(process.env.CEF_DEBUG_URL||"http://127.0.0.1:9223");const page=browser.contexts().flatMap(c=>c.pages()).find(p=>p.url()==="xenon://app/");assert.ok(page);page.setDefaultTimeout(15000);
async function rpc(command,payload={}){const reply=await page.evaluate(({command,payload})=>new Promise(resolve=>window.cefQuery({request:JSON.stringify({version:1,id:crypto.randomUUID(),command,payload}),onSuccess:value=>resolve(JSON.parse(value))})),{command,payload});assert.ok(reply.ok,reply.error?.message);return reply.data;}
const action=(action,data={})=>rpc("launcher.action",{action,data});
async function until(predicate){for(let attempt=0;attempt<200;attempt++){const state=await rpc("application.snapshot");if(predicate(state))return state;await new Promise(resolve=>setTimeout(resolve,50));}throw Error("Service fixture did not reach expected state");}
const nav=name=>page.getByRole("navigation",{name:"Main navigation"}).getByRole("button",{name,exact:true}).click();
assert.equal((await rpc("application.snapshot")).runtimeServices.fixture,true,"Start with an explicit --fixture-host");
await action("settings.set",{key:"developer/modeEnabled",value:true});await nav("Filesystem");
const folder=resolve(`build/services-content-${Date.now()}`);await mkdir(folder,{recursive:true});await writeFile(resolve(folder,"hello.txt"),"Host-folder fixture text");
await page.getByLabel("Guest mount point",{exact:true}).fill("game:");await page.getByLabel("Mount source",{exact:true}).fill(folder);await page.getByRole("button",{name:"Mount source",exact:true}).click();
await page.getByRole("button",{name:"Unmount game:",exact:true}).waitFor();
await page.getByLabel("Guest path",{exact:true}).fill("game:/hello.txt");await page.getByRole("button",{name:"Test guest path",exact:true}).click();await until(s=>s.serviceResult?.command==="filesystem.test"&&s.serviceResult.result.exists===true);
await page.getByRole("button",{name:"Register guest link",exact:true}).click();await until(s=>s.runtimeServices.filesystem.symbolicLinks.some(link=>link.alias==="dvd:"));
await page.getByLabel("Guest path",{exact:true}).fill("dvd:/hello.txt");await page.getByRole("button",{name:"Test guest path",exact:true}).click();await until(s=>s.serviceResult?.result.path==="dvd:/hello.txt"&&s.serviceResult.result.exists===true);
await nav("Settings");await page.getByRole("navigation",{name:"Settings categories"}).getByRole("button",{name:"Input",exact:true}).click();
const player=page.getByRole("group",{name:"Player 1",exact:true});await player.getByRole("combobox",{name:"Player 1 device",exact:true}).selectOption("fixture-pad");await player.getByRole("button",{name:"Assign device",exact:true}).click();await until(s=>s.runtimeServices.input.users[0].sources.includes("fixture-pad"));
await player.getByRole("combobox",{name:"Player 1 device",exact:true}).selectOption("fixture-keyboard");await player.getByRole("button",{name:"Add source",exact:true}).click();await until(s=>s.runtimeServices.input.users[0].sources.length===2);
await player.getByRole("combobox",{name:"Player 1 bindings profile",exact:true}).selectOption("fixture-standard");await until(s=>s.runtimeServices.input.users[0].profileId==="fixture-standard");
await player.getByRole("button",{name:"Record fixture vibration",exact:true}).click();await until(s=>s.serviceResult?.command==="input.vibrate"&&s.serviceResult.result.fixture===true);
await nav("Network");await page.getByRole("combobox",{name:"Network fixture scenario",exact:true}).selectOption("transport-only");let state=await until(s=>s.runtimeServices.network.serviceReachable===true);assert.equal(state.runtimeServices.network.onlineServicesReady,false);
await page.getByRole("combobox",{name:"Network fixture scenario",exact:true}).selectOption("ready");await until(s=>s.runtimeServices.network.onlineServicesReady===true);await page.screenshot({path:"build/services-verified.png"});
await page.reload();state=await rpc("application.snapshot");assert.equal(state.runtimeServices.input.users[0].sources.length,2);assert.equal(state.runtimeServices.input.users[0].profileId,"fixture-standard");assert.equal(state.runtimeServices.filesystem.mounts[0].mountPoint,"game:");
console.log("Packaged fixture VFS forms/real host-source tests/aliases, multi-source input routing/bindings/vibration records, separate network readiness and reload persistence passed.");
if(!process.env.CEF_KEEP_OPEN)await page.close();await browser.close();
