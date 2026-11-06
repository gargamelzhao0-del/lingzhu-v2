"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs"), path = require("node:path"), os = require("node:os");
const { GatewayReportWriter, ARCHIVE_LIMIT } = require("./sd-gateway-report");
const { ReplayReceiver } = require("./offline-audio-replay-gateway");
async function main() {
  const root=fs.mkdtempSync(path.join(os.tmpdir(),"nuwax-report-writer-"));
  let value=0,writes=0,fail=false,hold=null,entered=null,errors=0;
  const file=path.join(root,"report.json");
  const writer=new GatewayReportWriter(file,()=>({value}),()=>errors++,10000);
  const write=fs.promises.writeFile;
  fs.promises.writeFile=async function(target,...args) {
    if(target===file+".tmp") {
      writes++;
      if(fail) {fail=false;const e=new Error("injected report EIO");e.code="EIO";throw e;}
      if(hold) {entered();await hold;}
    }
    return write.call(this,target,...args);
  };
  const receiver=new ReplayReceiver(path.join(root,"audio"));
  try {
    for(value=0;value<100;value++)writer.request();
    assert.equal(writes,0,"requests must not write on the ACK path");
    assert(await writer.flush());assert.equal(writes,1);assert.equal(JSON.parse(fs.readFileSync(file)).value,100);
    let release,started;hold=new Promise(r=>release=r);const blocked=new Promise(r=>started=r);entered=started;
    value=101;writer.request();const flushing=writer.flush();await blocked;
    value=102;writer.request();hold=null;release();
    assert(await flushing);assert.equal(writes,3);assert.equal(JSON.parse(fs.readFileSync(file)).value,102);
    value=103;fail=true;writer.request();assert.equal(await writer.flush(),false);
    assert.equal(errors,1);assert.equal(JSON.parse(fs.readFileSync(file)).value,102);
    assert(await writer.flush());assert.equal(JSON.parse(fs.readFileSync(file)).value,103);
    for(let i=0;i<30000;i++)receiver.noteArchive({deviceId:"LOAD",segmentId:`file_${i}`},{committed:true,totalBytes:32096});
    assert.equal(receiver.stats.archives.length,ARCHIVE_LIMIT);assert.equal(receiver.stats.archivesOmitted,30000-ARCHIVE_LIMIT);
    receiver.noteArchive({deviceId:"LOAD",segmentId:"file_29999"},{totalBytes:123});
    assert.equal(receiver.stats.archives.at(-1).totalBytes,123);assert.equal(receiver.stats.archivesOmitted,30000-ARCHIVE_LIMIT);
    value=104;writer.request();assert(await writer.close());assert.equal(JSON.parse(fs.readFileSync(file)).value,104);
    const closedWrites=writes;writer.request();assert(await writer.close());assert.equal(writes,closedWrites);
    console.log("PASS: no synchronous report on request; coalescing; update during write retained; I/O failure keeps old report and retries; bounded 30000-record history; close drains and prevents new work");
  } finally {
    fs.promises.writeFile=write;await writer.close();await receiver.audioSessions.close();
    assert.equal(path.dirname(root),path.resolve(os.tmpdir()));assert(path.basename(root).startsWith("nuwax-report-writer-"));
    fs.rmSync(root,{recursive:true,force:true});
  }
}
main().catch(e=>{console.error(e);process.exitCode=1;});
