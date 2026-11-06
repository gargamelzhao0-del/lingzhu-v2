"use strict";
// Read durable gateway archives; never alter .part/.complete files.
const fs=require("node:fs"), path=require("node:path");
const {archiveCrc}=require("./offline-audio-replay-gateway");
const [directory,sessionId,expectedText,output,gatewayLog,targetText]=process.argv.slice(2);
if(!directory || !sessionId) throw new Error("Usage: node audit-offline-session.js ARCHIVES SESSION [EXPECTED_FRAMES] [OUTPUT_JSON] [GATEWAY_LOG] [TARGET_KIBPS=500]");
const expected=expectedText===undefined?null:Number(expectedText);
if(expected!==null && (!Number.isSafeInteger(expected) || expected<0)) throw new Error("Invalid expected frame count");
const sequences=new Set(), segments=[], errors=[]; let frames=0, duplicates=0;
for(const name of fs.readdirSync(directory).filter(n=>n.endsWith(".complete"))) {
  const file=path.join(directory,name);
  let fd;
  try {
    fd=fs.openSync(file,"r"); const h=Buffer.alloc(96);
    if(fs.readSync(fd,h,0,96,0)!==96 || h.toString("ascii",36,84).replace(/\0.*$/,"")!==sessionId) continue;
    const data=fs.readFileSync(file), checked=archiveCrc(data);
    if(!checked.sequenceVerified) throw new Error("Sequence metadata is incomplete");
    for(let pos=96;pos<data.length-32;) {
      const len=data.readUInt32LE(pos), seq=data.readUInt32LE(pos+4+10);
      if(sequences.has(seq)) duplicates++; else sequences.add(seq);
      pos+=4+len;
    }
    frames+=checked.frameCount; segments.push({file:name,bytes:data.length,...checked});
  } catch(error) { errors.push({file:name,error:error.message}); }
  finally { if(fd!==undefined) fs.closeSync(fd); }
}
const ordered=[...sequences].sort((a,b)=>a-b), gaps=[];
let next=0;
for(const seq of ordered) { if(seq>next) gaps.push({first:next,last:seq-1,frames:seq-next}); next=seq+1; }
if(expected!==null && next<expected) gaps.push({first:next,last:expected-1,frames:expected-next});
const verifiedComplete=expected!==null && errors.length===0 && duplicates===0 &&
  sequences.size===expected && gaps.length===0 && next===expected;
const report={checkedAt:new Date().toISOString(),sessionId,expectedFrames:expected,
  durableSegments:segments.length,frames,uniqueFrames:sequences.size,duplicates,
  firstSequence:ordered[0]??null,lastSequence:ordered.at(-1)??null,gaps,errors,
  verifiedComplete,segments};
if(gatewayLog) report.throughput=require("./offline-replay-throughput").replayThroughput(
  report,fs.readFileSync(gatewayLog,"utf8"),targetText===undefined?500:Number(targetText));
if(output) fs.writeFileSync(output,JSON.stringify(report,null,2));
console.log(JSON.stringify({...report,segments:undefined},null,2));
// No expected total means coverage-only reporting, never an end-to-end claim.
if(errors.length || (expected!==null && !verifiedComplete)) process.exitCode=2;
else if(report.throughput && !report.throughput.throughputTargetMet) process.exitCode=3;
