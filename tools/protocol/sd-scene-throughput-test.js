"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const { once } = require("node:events");
const { performance } = require("node:perf_hooks");
const { startServer, parseArgs, ReplayReceiver, archiveCrc } = require("./offline-audio-replay-gateway");
const { fixture } = require("./sd-audio-sessions-test");
const { packet } = require("./sd-scene-protocol-test");
const { ARCHIVE_LIMIT } = require("./sd-gateway-report");

async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-scene-throughput-"));
  const args=parseArgs(["node","gateway","--host","127.0.0.1","--storage-dir",root,"--report",path.join(root,"report.json")]);
  args.port=0;
  const header=ReplayReceiver.prototype.registerHeader;
  let seeded=false, stats;
  ReplayReceiver.prototype.registerHeader=function(tx,...rest) {
    if(!seeded) {
      seeded=true;stats=this.stats;
      const summary=archiveCrc(fixture("synthetic_history",0,true));
      this.stats.archives=Array.from({length:30000},(_,i)=>({deviceId:"PERF",segmentId:`old_${i}`,
        mode:1,sourceFlags:2,sampleRate:16000,frameDurationMs:20,sessionId:"monitor_history",...summary,committed:true,totalBytes:32096}));
    }
    return header.call(this,tx,...rest);
  };
  const server=startServer(args);let ws,timer;const results=[];
  try {
    await once(server,"listening");
    ws=new WebSocket(`ws://127.0.0.1:${server.address().port}/api/device/ws?device_id=PERF`);
    await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
    const exchange=message=>new Promise((resolve,reject)=>{
      const timeout=setTimeout(()=>{ws.removeEventListener("message",receive);reject(new Error(`timeout ${message.type}`));},10000);
      function receive(event) {
        const reply=JSON.parse(event.data);if(reply.request_id!==message.request_id)return;
        clearTimeout(timeout);ws.removeEventListener("message",receive);
        if(!reply.ok)reject(new Error(JSON.stringify(reply)));else resolve(reply);
      }
      ws.addEventListener("message",receive);ws.send(JSON.stringify(message));
    });
    for(const [name,mode,counts] of [["history_small","monitoring",Array(64).fill(48)],["meeting_4m","meeting",[6297,6297,6297,6297,3148]]]) {
      const data=[];let frames=0;
      for(let i=0;i<counts.length;i++){data.push(fixture(name,i,mode==="monitoring",frames*20,counts[i],frames));frames+=counts[i];}
      let previous=performance.now(),maxLoopGapMs=0;
      timer=setInterval(()=>{const now=performance.now();maxLoopGapMs=Math.max(maxLoopGapMs,now-previous);previous=now;},10);
      const commits=[],started=performance.now();
      await exchange({type:`${mode}_audio_begin`,request_id:`${name}_begin`,audio_session_id:name});
      for(let i=0;i<data.length;i++) {
        for(let at=0;at<data[i].length;at+=8064)ws.send(packet(data[i],name,i,at));
        const commitStart=performance.now();
        await exchange({type:mode==="meeting"?"meeting_audio_commit":"monitoring_audio_history_commit",request_id:`${name}_${i}`,
          audio_session_id:name,segment_index:i,total_bytes:data[i].length,crc32:archiveCrc(data[i]).crc});
        commits.push(performance.now()-commitStart);
      }
      await exchange({type:`${mode}_audio_session_end`,request_id:`${name}_end`,audio_session_id:name,
        segment_count:data.length,recording_complete:true});
      const elapsedMs=performance.now()-started;clearInterval(timer);timer=null;
      const bytes=data.reduce((sum,b)=>sum+b.length,0),speed=bytes/1024/(elapsedMs/1000);
      for(let i=0;i<data.length;i++)assert.deepEqual(fs.readFileSync(path.join(root,`PERF__sd_${name}_${i.toString(16).padStart(8,"0")}.complete`)),data[i]);
      const manifest=JSON.parse(fs.readFileSync(path.join(root,"sessions",`PERF__${name}`,"session.json")));
      assert.equal(manifest.audioDurationMs,frames*20);assert.deepEqual(manifest.missingSegments,[]);assert(manifest.recordingComplete);
      assert(stats.archives.length<=ARCHIVE_LIMIT);assert(await args.flushReport());
      assert(fs.statSync(args.reportFile).size<300000,"diagnostic report grew with full history");
      const result={scenario:name,files:data.length,bytes,audioDurationMs:frames*20,elapsedMs,KiBps:speed,maxLoopGapMs,
        commitAckMs:commits,reportBytes:fs.statSync(args.reportFile).size,syntheticHistoryRecords:30000,hardwareMeasured:false};
      results.push(result);console.log(JSON.stringify(result));
      assert(speed>=500,`PC protocol loopback below 500 KiB/s: ${speed}`);
      assert(maxLoopGapMs<1000,`network event loop stalled: ${maxLoopGapMs}`);
    }
    if(process.env.SD_SCENE_THROUGHPUT_RESULT)fs.writeFileSync(process.env.SD_SCENE_THROUGHPUT_RESULT,JSON.stringify(results,null,2));
    console.log("PASS: real WebSocket file ACK + session end, byte-exact PCM/archive and duration, 30000-entry history bounded, both PC loopback workloads exceed 500 KiB/s; not ESP32/Wi-Fi measurement");
  } finally {
    ReplayReceiver.prototype.registerHeader=header;if(timer)clearInterval(timer);
    if(ws && ws.readyState!==WebSocket.CLOSED)await new Promise(resolve=>{ws.addEventListener("close",resolve,{once:true});ws.close();});
    await new Promise(resolve=>server.close(resolve));await args.closeReport();
    assert.equal(path.dirname(root),path.resolve(os.tmpdir()));assert(path.basename(root).startsWith("nuwax-scene-throughput-"));
    fs.rmSync(root,{recursive:true,force:true});
  }
}
main().catch(e=>{console.error(e);process.exitCode=1;});
