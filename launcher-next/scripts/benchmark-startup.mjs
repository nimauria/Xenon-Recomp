import {spawn,execFileSync} from 'node:child_process';
import {mkdir,readFile,writeFile,copyFile,stat} from 'node:fs/promises';
import {createWriteStream} from 'node:fs';
import {createServer} from 'node:net';
import {resolve,join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {cpus,totalmem,release} from 'node:os';
import {performance} from 'node:perf_hooks';
import {chromium} from 'playwright-core';
process.chdir(fileURLToPath(new URL('../',import.meta.url)));
const desktop=resolve(process.env.XENON_TEST_DESKTOP||'build/native/desktop');
const executable=join(desktop,process.platform==='darwin'?'XenonLauncher.app/Contents/MacOS/XenonLauncher':'xenon_launcher_next'+(process.platform==='win32'?'.exe':''));
const seeds=execFileSync(process.execPath,['scripts/benchmark-library.mjs','prepare'],{encoding:'utf8'}).trim();
const output=resolve('build/startup-benchmark-'+Date.now());await mkdir(output,{recursive:true});
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function freePort(){const server=createServer();await new Promise((resolve,reject)=>server.once('error',reject).listen(0,'127.0.0.1',resolve));const port=server.address().port;await new Promise(resolve=>server.close(resolve));return port;}
async function sample(data,count){
 const port=await freePort(),marker=join(data,'startup.log'),before=(await stat(marker).catch(()=>({size:0}))).size;
 const log=createWriteStream(join(data,'benchmark-process.log'),{flags:'a'});
 const started=performance.now();
 const app=spawn(executable,['--data-dir='+data,'--remote-debugging-port='+port,'--offline'],{windowsHide:true,stdio:['ignore','pipe','pipe']});
 app.stdout.pipe(log,{end:false});app.stderr.pipe(log,{end:false});
 let exit,error;app.once('exit',(code,signal)=>{exit={code,signal};});app.once('error',e=>{error=e;});
 let browser;
 try{
  let ready=false;
  for(let i=0;i<6000;++i){
   if(error)throw error;if(exit)throw Error('Launcher exited: '+JSON.stringify(exit));
   try{const text=await readFile(marker,'utf8');if(Buffer.byteLength(text)>before&&text.includes('Launcher interface ready; bridge protocol 1')){ready=true;break;}}catch{}
   await delay(5);
  }
  if(!ready)throw Error('No interface-ready marker after 30 seconds');
  const milliseconds=performance.now()-started;
  browser=await chromium.connectOverCDP('http://127.0.0.1:'+port);
  const page=browser.contexts().flatMap(c=>c.pages()).find(p=>p.url()==='xenon://app/');
  if(!page)throw Error('No library page');
  await page.getByRole('navigation',{name:'Main navigation'}).waitFor();
  const actual=await page.evaluate(()=>new Promise((resolve,reject)=>window.cefQuery({request:JSON.stringify({version:1,id:'startup-proof',command:'application.snapshot',payload:{}}),onSuccess:raw=>resolve(JSON.parse(raw)),onFailure:(_c,m)=>reject(Error(m))})));
  if(!actual.ok||actual.data.games.length!==count)throw Error('Startup did not expose the expected native library');
  await page.close();await browser.close();browser=undefined;
  return Math.round(milliseconds*10)/10;
 }finally{
  if(browser){for(const page of browser.contexts().flatMap(c=>c.pages()))await page.close().catch(()=>{});await browser.close().catch(()=>{});}
  for(let i=0;i<100&&!exit&&!error;++i)await delay(20);
  if(!exit&&!error)app.kill();log.end();
 }
}
const groups=[];
for(const count of [0,1000,10000]){
 for(const cache of ['fresh-data','repeat-data']){
  const samples=[];
  for(let i=0;i<5;++i){
   const data=join(output,count+'-'+cache+(cache==='fresh-data'?'-'+i:''));
   if(cache==='fresh-data'||i===0){await mkdir(data,{recursive:true});await copyFile(join(seeds,String(count),'state.json'),join(data,'state.json'));}
   if(cache==='repeat-data'&&i===0)await sample(data,count); // Untimed cache priming.
   const milliseconds=await sample(data,count);samples.push(milliseconds);
   console.log(`${count} games / ${cache} / ${i+1}: ${milliseconds} ms`);
  }
  const sorted=[...samples].sort((a,b)=>a-b);groups.push({games:count,cache,samples,medianMs:sorted[2],maximumMs:sorted.at(-1)});
  await writeFile(join(output,'results.json'),JSON.stringify({date:new Date().toISOString(),desktop,cpu:cpus()[0]?.model,logicalProcessors:cpus().length,memoryGiB:Math.round(totalmem()/1024**3),os:release(),method:'Child spawn to observed native interface-ready marker, 5ms polling; bridge/library verified afterwards. Fresh means separate launcher state/CEF cache, not an OS disk-cache flush. Repeat means reused state/cache after an untimed priming run. Five samples per group; no Steam timing was collected.',groups},null,2)+'\n');
 }
}
console.log('Startup evidence: '+output);console.table(groups.map(({games,cache,medianMs,maximumMs})=>({games,cache,medianMs,maximumMs})));
