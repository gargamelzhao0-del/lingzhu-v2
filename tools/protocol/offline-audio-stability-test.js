"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { once } = require("node:events");
const { startServer, parseArgs, archiveCrc, makeChunk, ReplayReceiver } = require("./offline-audio-replay-gateway");
function crcUpdate(crc, data) {
  let c = (~crc) >>> 0;
  for (const b of data) {
    c ^= b;
    for (let i=0;i<8;i++) c=(c>>>1)^((c&1)?0xedb88320:0);
  }
  return (~c)>>>0;
}
function archive(first, frames) {
  const data=Buffer.alloc(96+frames*666+32);
  data.write("NWXOFF1"); data.writeUInt16LE(1,8); data.writeUInt16LE(96,10);
  data[13]=1; data.writeUInt16LE(1,14); data.writeUInt32LE(16000,16);
  data.writeUInt16LE(20,20); data.writeUInt16LE(1,22); data.write("five_minute_integrity",36);
  let crc=0;
  for(let i=0;i<frames;i++) {
    const off=96+i*666; data.writeUInt32LE(662,off);
    const pcm=data.subarray(off+4,off+666); pcm.fill((first+i)%251);
    pcm.write("NA"); pcm[2]=1; pcm.writeUInt32LE(first+i,10);
    pcm.writeUInt32LE((first+i)*20,14); pcm.writeUInt16LE(640,18);
    crc=crcUpdate(crc,pcm);
  }
  const footer=data.subarray(data.length-32); footer.fill(0);
  footer.write("NWXEND1"); footer.writeUInt16LE(1,8); footer.writeUInt16LE(32,10);
  footer.writeUInt32LE(frames,12); footer.writeBigUInt64LE(BigInt(frames*662),16);
  footer.writeUInt32LE(crc,24); footer[28]=1;
  return data;
}
async function main() {
  const root=fs.mkdtempSync(path.join(os.tmpdir(),"nuwax-stability-"));
  const args=parseArgs(["node","gateway","--host","127.0.0.1","--storage-dir",root,"--report",path.join(root,"report.json")]);
  args.port=0;
  const server=startServer(args); let ws;
  async function connect() {
    ws=new WebSocket(`ws://127.0.0.1:${server.address().port}/api/device/ws`);
    await new Promise((resolve,reject)=>{ ws.onopen=resolve; ws.onerror=reject; });
    await exchange({type:"hello",device_id:"stability-test"},"hello");
  }
  function exchange(payload,type) {
    return new Promise((resolve,reject)=>{
      const timer=setTimeout(()=>{ws.removeEventListener("message",receive);reject(new Error(`Timeout ${type}`));},4000);
      function receive(event) {
        const msg=JSON.parse(event.data);
        if(msg.type!==type) return;
        clearTimeout(timer); ws.removeEventListener("message",receive); resolve(msg);
      }
      ws.addEventListener("message",receive);
      ws.send(Buffer.isBuffer(payload)?payload:JSON.stringify(payload));
    });
  }
  async function disconnect() {
    const closed=new Promise(resolve=>ws.addEventListener("close",resolve,{once:true}));
    ws.close(); await closed;
  }
  try {
    await once(server,"listening"); await connect();
    let checkedFrames=0;
    for(let segment=0;segment<10;segment++) {
      const data=archive(segment*1500,1500);
      const meta={mode:"archive_only",segment_id:`segment_${segment}`,total_bytes:data.length,crc32:archiveCrc(data).crc};
      const begin=()=>exchange({type:"offline_audio_begin",request_id:`begin_${segment}`,...meta},"offline_audio_begin_ack");
      let offset=(await begin()).next_offset, chunks=0;
      while(offset<data.length) {
        const packet=makeChunk(data,offset,`chunk_${segment}_${offset}`);
        let ack=await exchange(packet,"offline_audio_chunk_ack"); assert(ack.ok);
        if(chunks===3) {
          // Ignore the first ACK, then resend exactly the same transaction.
          const repeated=await exchange(packet,"offline_audio_chunk_ack");
          assert.equal(repeated.next_offset,ack.next_offset); assert(repeated.ok);
        }
        offset=ack.next_offset;
        if(chunks===7) {
          await disconnect(); await connect();
          assert.equal((await begin()).next_offset,offset);
        }
        chunks++;
      }
      const commit=()=>exchange({type:"offline_audio_commit",request_id:`commit_${segment}`,...meta},"offline_audio_commit_ack");
      assert((await commit()).ok);
      // Lost final ACK/reconnect must be an idempotent finalized commit.
      assert.equal((await begin()).next_offset,data.length); assert((await commit()).ok);
      const durable=fs.readFileSync(path.join(root,`stability-test__segment_${segment}.complete`));
      assert(durable.equals(data));
      const checked=archiveCrc(durable); assert.equal(checked.missingFrames,0);
      assert.equal(checked.firstSequence,checkedFrames); checkedFrames+=checked.frameCount;
    }
    assert.equal(checkedFrames,15000);
    assert(await args.flushReport());
    const report=JSON.parse(fs.readFileSync(args.reportFile));
    assert.equal(report.replay.crcFailures,0); assert.equal(report.pending.length,0);
    require("node:child_process").execFileSync(process.execPath,
      [path.join(__dirname,"audit-offline-session.js"),root,"five_minute_integrity","15000"]);
    // POSIX/Windows writes are allowed to be short. Test the actual receiver
    // with short writes and verify that ACK covers only fully durable bytes.
    const receiver=new ReplayReceiver(path.join(root,"short-writes"));
    const data=archive(0,10), meta={type:"offline_audio_begin",request_id:"b",segment_id:"short",total_bytes:data.length,crc32:archiveCrc(data).crc};
    receiver.begin("short-test",meta);
    const write=fs.writeSync;
    try {
      fs.writeSync=(fd,buf,off,len,pos)=>write(fd,buf,off,Math.min(len,137),pos);
      assert.equal(receiver.chunk("short-test",makeChunk(data,0,"c")).next_offset,data.length);
    } finally { fs.writeSync=write; }
    assert(receiver.commit("short-test",{...meta,type:"offline_audio_commit",request_id:"done"}).ok);
    console.log("PASS: 300 seconds / 15000 frames / 10 archives over real WebSocket; repeated chunks and commits; 10 reconnects; byte equality, CRC and contiguous sequences; short writes");
  } finally {
    if(ws && ws.readyState===WebSocket.OPEN) await disconnect();
    await new Promise(resolve=>server.close(resolve));
    await args.closeReport();
    // Only the temporary directory returned by mkdtemp above is removed.
    fs.rmSync(root,{recursive:true,force:true});
  }
}
if (require.main === module) main().catch(error=>{console.error(error);process.exitCode=1;});
module.exports = { archive };
