import {spawn} from "node:child_process";
import {chromium} from "playwright-core";
import {mkdir,access} from "node:fs/promises";
import {resolve} from "node:path";
import {fileURLToPath} from "node:url";
import assert from "node:assert/strict";
process.chdir(fileURLToPath(new URL("../",import.meta.url)));
const executable=resolve(process.argv[2]??"build/native/desktop/xenon_launcher_next.exe");
const folder=resolve("build/unclean-recovery-"+Date.now());await mkdir(folder,{recursive:true});
let child,browser,page,exited;
async function launch(){
  child=spawn(executable,["--data-dir="+folder,"--remote-debugging-port=9224","--offline"],{windowsHide:true,stdio:"ignore"});
  exited=new Promise(resolve=>child.once("exit",resolve));
  for(let i=0;i<200;i++){
    try {browser=await chromium.connectOverCDP("http://127.0.0.1:9224",{timeout:500});page=browser.contexts().flatMap(c=>c.pages()).find(p=>p.url()==="xenon://app/");if(!page)throw Error("Interface not ready");await page.getByRole("navigation",{name:"Main navigation"}).waitFor({timeout:500});break;}
    catch(error){await browser?.close().catch(()=>{});browser=undefined;if(i===199)throw error;await new Promise(resolve=>setTimeout(resolve,100));}
  }
  for(let i=0;i<100;i++){const state=await rpc("application.snapshot");if(state.recovery.currentPhase==="interface-ready")return state;await new Promise(resolve=>setTimeout(resolve,50));}
  throw Error("No interface-ready marker");
}
async function rpc(command,payload={}){const reply=await page.evaluate(({command,payload})=>new Promise((resolve,reject)=>window.cefQuery({request:JSON.stringify({version:1,id:crypto.randomUUID(),command,payload}),onSuccess:raw=>resolve(JSON.parse(raw)),onFailure:(_code,message)=>reject(Error(message))})),{command,payload});assert.ok(reply.ok,reply.error?.message);return reply.data;}
const action=(action,data={})=>rpc("launcher.action",{action,data});
async function stop(crash){
  if(crash){await browser.close();browser=undefined;assert.ok(child.kill("SIGKILL"));}
  else{await page.close();await browser.close();browser=undefined;}
  await Promise.race([exited,new Promise((_,reject)=>setTimeout(()=>reject(Error("Owned test process did not exit")),10000))]);
  child=undefined;
}
try{
  let state=await launch();assert.equal(state.recovery.safeMode,false);
  await rpc("profiles.create",{name:"Recovery preserved profile"});
  await action("settings.set",{key:"ui/theme",value:"carbon"});
  await stop(true);
  state=await launch();assert.equal(state.recovery.safeMode,false);assert.equal(state.recovery.consecutiveUncleanStarts,1);
  await page.getByRole("dialog",{name:"Launcher recovery",exact:true}).waitFor();
  await action("recovery.acknowledge");await stop(true);
  state=await launch();assert.equal(state.recovery.safeMode,true);assert.equal(state.recovery.automaticSafeModeReason,"repeated-unclean");assert.equal(state.recovery.consecutiveUncleanStarts,2);
  assert.equal(state.settings["ui/theme"],"carbon");assert.ok(state.profiles.some(p=>p.name==="Recovery preserved profile"));
  await page.getByText("Safe mode was enabled automatically after 2 consecutive unclean exits.",{exact:true}).waitFor();
  assert.equal(await page.evaluate(()=>document.documentElement.dataset.themeId),"xenon-dark");
  await page.getByRole("button",{name:"Continue in safe mode",exact:true}).click();
  await stop(false);
  await assert.rejects(access(resolve(folder,"running.json")));
  state=await launch();assert.equal(state.recovery.safeMode,false);assert.equal(state.recovery.consecutiveUncleanStarts,0);assert.ok(state.profiles.some(p=>p.name==="Recovery preserved profile"));
  await stop(false);
  console.log("Real process crashes, repeated-unclean automatic safe mode, acknowledgement, clean reset, restart and preserved profile/theme passed. Data: "+folder);
}finally{await browser?.close().catch(()=>{});if(child&&child.exitCode===null)child.kill("SIGKILL");}
