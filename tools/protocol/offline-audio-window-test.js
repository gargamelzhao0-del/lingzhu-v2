"use strict";
const assert=require("node:assert/strict"), fs=require("node:fs"), os=require("node:os"), path=require("node:path");
const {once}=require("node:events");
const {performance}=require("node:perf_hooks");
const {archive}=require("./offline-audio-stability-test");
const {ReplayReceiver,archiveCrc,startServer,parseArgs,makeChunk}=require("./offline-audio-replay-gateway");
const root=fs.mkdtempSync(path.join(os.tmpdir(),"nuwax-window-"));
function packets(data,start,end,id) {
  const result=[];
  for(let at=start;at<end;) {
    const n=Math.min(8064,end-at), p=Buffer.alloc(64+n);
    p.write("NWXCH1");p.writeUInt32LE(2,8);p.writeUInt32LE(at,12);p.writeUInt32LE(n,16);
    p.write(id,20);p.writeUInt32LE(at+n===end?1:0,60);data.copy(p,64,at,at+n);
    result.push(p);at+=n;
  }
  return result;
}
function meta(data,id,v2=true) {return {type:"offline_audio_begin",request_id:"b",segment_id:id,
  total_bytes:data.length,crc32:archiveCrc(data).crc,...(v2?{replay_version:2,replay_revision:2,window_bytes:524288}:{})};}
function receiveWindow(receiver,data,start,end,id="w") {
  const batch=packets(data,start,end,id);let ack;
  for(let i=0;i<batch.length;i++) {
    ack=receiver.chunk("d",batch[i]);
    if(i<batch.length-1) assert.equal(ack,null,"no ACK before batch persistence");
  }
  assert(ack.ok);assert.equal(ack.next_offset,end);return ack;
}
async function main() {
  const data=archive(0,1574), m=meta(data,"integrity");
  let r=new ReplayReceiver(path.join(root,"faults"));
  for(const revision of [undefined,1,3,"2"]) {
    const old=meta(data,`old-${revision}`);old.replay_revision=revision;
    const ack=r.begin("d",old);assert.equal(ack.replay_version,undefined);
    assert(r.chunk("d",makeChunk(data,0,"legacy-fallback")).ok);
  }
  assert.equal(r.begin("d",m).window_bytes,524288);
  const initial=packets(data,0,65536,"partial");
  r.chunk("d",initial[0]);
  assert(!fs.existsSync(r.paths("d","integrity").part),"unconfirmed RAM window must not be written");
  r=new ReplayReceiver(path.join(root,"faults"));
  assert.equal(r.begin("d",m).next_offset,0);
  let writes=0,syncs=0;
  const write=fs.writeSync,sync=fs.fsyncSync;
  try {
    fs.writeSync=(fd,b,o,n,p)=>{writes++;return write(fd,b,o,Math.min(n,997),p);};
    fs.fsyncSync=fd=>{syncs++;return sync(fd);};
    receiveWindow(r,data,0,65536);
  } finally {fs.writeSync=write;fs.fsyncSync=sync;}
  assert(writes>1);assert.equal(syncs,1,"one fsync per window");
  receiveWindow(r,data,0,65536); // ACK lost, whole batch repeated.
  r=new ReplayReceiver(path.join(root,"faults"));
  assert.equal(r.begin("d",m).next_offset,65536);
  for(let at=65536;at<data.length;) {const end=Math.min(data.length,at+262144);receiveWindow(r,data,at,end);at=end;}
  assert(r.commit("d",{...m,type:"offline_audio_commit",request_id:"c"}).ok);
  assert(fs.readFileSync(r.paths("d","integrity").complete).equals(data));
  assert.equal(r.begin("d",m).next_offset,data.length);
  assert(r.commit("d",{...m,type:"offline_audio_commit",request_id:"c2"}).ok);
  // Lost process during a positional write: actual recovered prefix is fsynced
  // by begin, and resume is permitted at arbitrary byte offsets.
  const torn=meta(data,"torn");r.begin("d",torn);
  fs.writeFileSync(r.paths("d","torn").part,data.subarray(0,12345));
  r=new ReplayReceiver(path.join(root,"faults"));assert.equal(r.begin("d",torn).next_offset,12345);
  for(let at=12345;at<data.length;) {const end=Math.min(data.length,at+524288);receiveWindow(r,data,at,end);at=end;}
  assert(r.commit("d",{...torn,type:"offline_audio_commit",request_id:"ct"}).ok);
  // A failed fsync must never produce an ACK or advance confirmed progress.
  const fail=meta(data,"sync-fail");r.begin("d",fail);
  try {fs.fsyncSync=()=>{throw new Error("injected sync failure");};
    assert.throws(()=>receiveWindow(r,data,0,65536),/injected sync failure/);
  } finally {fs.fsyncSync=sync;}
  assert.equal(r.transactions.get(r.key("d","sync-fail")).nextOffset,0);
  // Corrupt bytes never become a completed archive.
  const bad=meta(data,"bad");r.begin("d",bad);const corrupt=Buffer.from(data);corrupt[500]^=1;
  for(let at=0;at<data.length;) {const end=Math.min(data.length,at+524288);receiveWindow(r,corrupt,at,end);at=end;}
  assert.throws(()=>r.commit("d",{...bad,type:"offline_audio_commit",request_id:"cb"}));
  assert(!fs.existsSync(r.paths("d","bad").complete));
  r.begin("d",meta(data,"legacy",false));
  assert.throws(()=>r.chunk("d",packets(data,0,8064,"x")[0]),/not negotiated/);
  r.begin("d",{...meta(data,"small"),window_bytes:8064});
  const over=packets(data,0,16128,"over");r.chunk("d",over[0]);assert.throws(()=>r.chunk("d",over[1]),/limit/);

  const results=[];
  for(const delay of [0,25]) {
    const args=parseArgs(["node","test","--host","127.0.0.1","--storage-dir",path.join(root,`network-${delay}`)]);
    args.port=0;args.delayAckMs=delay;
    const server=startServer(args);await once(server,"listening");
    let ws;
    async function connect() {ws=new WebSocket(`ws://127.0.0.1:${server.address().port}/api/device/ws`);
      await new Promise((res,rej)=>{ws.onopen=res;ws.onerror=rej;});await exchange({type:"hello",device_id:"network"},"hello");}
    function exchange(payload,type) {
      return new Promise((resolve,reject)=>{
        const timeout=setTimeout(()=>{ws.removeEventListener("message",on);reject(new Error(`timeout ${type}`));},5000);
        function on(event) {const msg=JSON.parse(event.data);if(msg.type!==type)return;
          clearTimeout(timeout);ws.removeEventListener("message",on);resolve(msg);}
        ws.addEventListener("message",on);
        for(const p of Array.isArray(payload)?payload:[payload]) ws.send(Buffer.isBuffer(p)?p:JSON.stringify(p));
      });
    }
    async function disconnect() {const closed=once(ws,"close");ws.close();await closed;}
    try {
      await connect();
      for(const version of [1,2]) {
        const request=meta(data,`v${version}`,version===2), started=performance.now();
        let offset=(await exchange(request,"offline_audio_begin_ack")).next_offset, window=65536;
        while(offset<data.length) {
          const batch=version===1 ? makeChunk(data,offset,`c${offset}`) : packets(data,offset,Math.min(data.length,offset+window),`w${offset}`);
          const ack=await exchange(batch,"offline_audio_chunk_ack");assert(ack.ok);offset=ack.next_offset;
          window=Math.min(524288,window*2);
        }
        assert((await exchange({...request,type:"offline_audio_commit",request_id:"commit"},"offline_audio_commit_ack")).ok);
        const elapsed=performance.now()-started;
        assert(fs.readFileSync(path.join(args.storageDir,`network__v${version}.complete`)).equals(data));
        results.push({version,ackDelayMs:delay,bytes:data.length,elapsedMs:Math.round(elapsed),bytesPerSecond:Math.round(data.length*1000/elapsed)});
      }
      // Disconnect mid-window over actual WebSocket, then negotiate/resume.
      const request=meta(data,"network-resume");await exchange(request,"offline_audio_begin_ack");
      const batch=packets(data,0,65536,"lost");ws.send(batch[0]);
      await exchange({type:"ping"},"pong");await disconnect();await connect();
      assert.equal((await exchange(request,"offline_audio_begin_ack")).next_offset,0);
      const ack=await exchange(batch,"offline_audio_chunk_ack");assert.equal(ack.next_offset,65536);
      assert.equal((await exchange(batch,"offline_audio_chunk_ack")).next_offset,65536);
    } finally {if(ws&&ws.readyState===WebSocket.OPEN)await disconnect();await new Promise(res=>server.close(res));}
  }
  const delayed=results.filter(r=>r.ackDelayMs===25);
  assert(delayed[1].bytesPerSecond/delayed[0].bytesPerSecond>=10,"window must exceed 10x in the controlled ACK-delay benchmark");
  const output={checkedAt:new Date().toISOString(),scope:"host WebSocket benchmark; excludes ESP32 SD/CPU/Wi-Fi",results,
    delayedSpeedup:delayed[1].bytesPerSecond/delayed[0].bytesPerSecond};
  if(process.argv[2])fs.writeFileSync(process.argv[2],JSON.stringify(output,null,2));
  console.log(JSON.stringify(output,null,2));
  console.log("PASS: v1/v2 durable windows, lost ACK, process resume, short writes, fsync failure, corrupt archive, capability/window bounds, real WebSocket reconnect and throughput");
}
main().catch(error=>{console.error(error);process.exitCode=1;}).finally(()=>fs.rmSync(root,{recursive:true,force:true}));
