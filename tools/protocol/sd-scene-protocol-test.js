"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const { once } = require("node:events");
const { ReplayReceiver, archiveCrc, startServer, parseArgs } = require("./offline-audio-replay-gateway");
const { fixture } = require("./sd-audio-sessions-test");
const { failure, parseScenePacket } = require("./sd-scene-receiver");
function packet(data, session, index, offset = 0, size = 8064) {
  const length = Math.min(size, data.length - offset), p = Buffer.alloc(64 + length);
  p.write("NWXCH1"); p.writeUInt32LE(3, 8); p.writeUInt32LE(offset, 12); p.writeUInt32LE(length, 16);
  p.write(session, 20); p.writeUInt32LE(index, 56); p.writeUInt32LE(offset + length === data.length ? 1 : 0, 60);
  data.copy(p, 64, offset, offset + length); return p;
}
let serial = 0;
const control = (type, session, extra = {}) => ({ type, request_id: `r${++serial}`, audio_session_id: session, ...extra });
const meta = (data, index) => ({ segment_index: index, total_bytes: data.length, crc32: archiveCrc(data).crc });
async function main() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-scene-"));
  let receiver = new ReplayReceiver(dir), scene = receiver.scenes, connection = 1;
  const device = "SCENE-TEST";
  const send = (data, id, index, offset = 0) => { for (let at = offset; at < data.length; at += 8064)
    assert.equal(scene.chunk(device, packet(data, id, index, at), connection), null); };
  const begin = (id, monitoring = false, extra = {}) => scene.begin(device,
    control(`${monitoring ? "monitoring" : "meeting"}_audio_begin`, id, extra), connection);
  const commit = (data, id, index, route = "meeting") => scene.commit(device,
    control(`${route === "meeting" ? "meeting_audio" : route.replace("monitoring_", "monitoring_audio_")}_commit`, id, meta(data, index)), connection);
  const end = (id, count, complete = true, monitoring = false) => scene.end(device,
    control(`${monitoring ? "monitoring" : "meeting"}_audio_session_end`, id,
      { segment_count: count, recording_complete: complete }), connection);
  const stored = (id, index) => receiver.paths(device, `sd_${id}_${index.toString(16).padStart(8,"0")}`).complete;
  let server;
  try {
    // 4 full files and one tail: one logical begin, five file confirmations.
    const id = "meeting_five"; assert(begin(id).ok);
    for (let i = 0; i < 5; i++) {
      const data = fixture(id, i, false, i * 125940, i === 4 ? 3148 : 6297, i * 6297);
      send(data, id, i); assert(commit(data, id, i).ok); assert.deepEqual(fs.readFileSync(stored(id,i)), data);
    }
    assert.equal(receiver.stats.begins, 1); assert.equal(receiver.stats.commits, 5);
    assert((await end(id, 5)).ok); assert((await end(id, 5)).ok);
    const manifest = JSON.parse(fs.readFileSync(path.join(receiver.audioSessions.sessionDir(device,id),"session.json")));
    assert.equal(manifest.audioDurationMs, (6297 * 4 + 3148) * 20); assert.deepEqual(manifest.missingSegments, []);
    // Full-size monitor files retain live/history preemption, one begin and exact time order.
    const monitor = "monitor_interleave", history = fixture(monitor,0,true,0,6297), live = fixture(monitor,1,true,125940,6297,6297);
    assert(begin(monitor,true).ok); scene.chunk(device,packet(history,monitor,0),connection);
    send(live,monitor,1); assert(commit(live,monitor,1,"monitoring_realtime").ok);
    send(history,monitor,0,8064); assert(commit(history,monitor,0,"monitoring_history").ok);
    const tail = fixture(monitor,2,true,251880,7,12594);
    send(tail,monitor,2); assert(commit(tail,monitor,2,"monitoring_realtime").ok);
    assert.equal(receiver.stats.begins, 2); assert((await end(monitor,3,true,true)).ok);
    for (const [index, bytes] of [history,live,tail].entries()) assert.deepEqual(fs.readFileSync(stored(monitor,index)),bytes);
    const monitorManifest = JSON.parse(fs.readFileSync(path.join(receiver.audioSessions.sessionDir(device,monitor),"session.json")));
    assert.equal(monitorManifest.audioDurationMs,(6297*2+7)*20);
    assert.deepEqual(monitorManifest.missingSegments,[]);
    assert.deepEqual(monitorManifest.segments.map(s=>s.type),["history","realtime","realtime"]);
    // Process restart discards only unacknowledged RAM. Reply advertises the durable prefix.
    const retry = "resume_scene", data = fixture(retry,0,true,0,1500);
    begin(retry,true); for(let at=0;at<400000;at+=8064) scene.chunk(device,packet(data,retry,0,at),connection);
    scene.release(device, connection); await receiver.audioSessions.close();
    receiver = new ReplayReceiver(dir); scene = receiver.scenes; ++connection;
    const resumed = begin(retry,true,{resume:[meta(data,0)]});
    const offset = resumed.resume[0].next_offset; assert(offset >= 262144 && offset < 400000);
    send(data,retry,0,offset); assert(commit(data,retry,0,"monitoring_history").ok);
    // Lost commit ACK: recovery reports full file, repeat commit never repeats analysis.
    scene.release(device, connection); ++connection;
    assert.equal(begin(retry,true,{resume:[meta(data,0)]}).resume[0].next_offset,data.length);
    assert(commit(data,retry,0,"monitoring_realtime").ok);
    const record = JSON.parse(fs.readFileSync(stored(retry,0)+".json")); assert.equal(record.processingClass,"history");
    assert((await end(retry,1,true,true)).ok);
    // Immutable conflict preserves prior object; unknown fields are harmless.
    const bad = control("monitoring_audio_history_commit",retry,{...meta(data,0),crc32:1});
    let conflict;
    assert.throws(()=>scene.commit(device,bad,connection), error=>{conflict=error;return /immutable/.test(error.message);});
    assert.deepEqual(fs.readFileSync(stored(retry,0)),data);
    const rejected = failure(bad,conflict); assert.equal(rejected.error_code,"conflict");
    assert.equal(failure(bad,new Error("unexpected internal mismatch")).error_code,"storage_unavailable");
    // CRC failure clears only the corrupt uncommitted prefix; then recover from zero.
    const corruptId = "bad_crc", good = fixture(corruptId,0,false), corrupt = Buffer.from(good); corrupt[200]^=1;
    begin(corruptId); send(corrupt,corruptId,0);
    assert.throws(()=>commit(good,corruptId,0), e=>e.code==="crc_mismatch");
    assert.equal(begin(corruptId,false,{resume:[meta(good,0)]}).resume[0].next_offset,0);
    send(good,corruptId,0); assert(commit(good,corruptId,0).ok);
    // Incomplete scene may be filled later without turning it into a complete recording.
    const late = "late_scene", lateData = fixture(late,0,false);
    begin(late); assert((await end(late,1,false)).ok); send(lateData,late,0); assert(commit(lateData,late,0).ok);
    // File ACK follows durable data/index/refresh intent; an explicit end joins the refresh.
    assert((await end(late,1,false)).ok);
    const lateManifest = JSON.parse(fs.readFileSync(path.join(receiver.audioSessions.sessionDir(device,late),"session.json")));
    assert.equal(lateManifest.recordingComplete,false); assert.equal(lateManifest.receivedSegments,1);
    // A successor owns the scene; an old socket close must not erase its state.
    const fence="connection_fence", fenceData=fixture(fence,0,false);begin(fence);
    const oldConnection=connection; ++connection;
    begin(fence,false,{resume:[meta(fenceData,0)]});
    assert.throws(()=>scene.chunk(device,packet(fenceData,fence,0),oldConnection),/not registered/);
    scene.release(device,oldConnection);send(fenceData,fence,0);assert(commit(fenceData,fence,0).ok);
    // Full recovery batches; no scalar next_offset and no implicit missing entries.
    const many="resume_batch";begin(many);
    const files=Array.from({length:16},(_,i)=>fixture(many,i,false,i*960));
    const batch=begin(many,false,{resume:files.map((bytes,i)=>meta(bytes,i))});
    assert.equal(batch.next_offset,undefined);assert.equal(batch.resume.length,16);
    assert(batch.resume.every((entry,i)=>entry.segment_index===i && entry.next_offset===0));
    // A long realtime attempt keeps the class selected at its start.
    const timely="long_realtime", timelyData=fixture(timely,0,true);begin(timely,true);send(timelyData,timely,0);
    const originalBegin=receiver.audioSessions.begin;
    receiver.audioSessions.begin=function(...args){const info=originalBegin.apply(this,args);info.started-=10000000000n;return info;};
    assert(commit(timelyData,timely,0,"monitoring_realtime").ok);receiver.audioSessions.begin=originalBegin;
    assert.equal(JSON.parse(fs.readFileSync(stored(timely,0)+".json")).processingClass,"realtime");
    const malformed = packet(lateData,late,0); malformed.writeUInt32LE(2,60); assert.throws(()=>parseScenePacket(malformed));
    assert.throws(()=>begin("oversized",false,{resume:Array.from({length:17},(_,i)=>meta(lateData,i))}));
    // The complete C sender fixture must use exactly this receiver, without reinterpretation.
    const fixtureRoot = path.resolve(__dirname,"../../build-host-checks");
    const controlsPath = path.join(fixtureRoot,"sd-file.controls.jsonl");
    if (fs.existsSync(controlsPath)) {
      const controls = fs.readFileSync(controlsPath,"utf8").trim().split(/\r?\n/).map(JSON.parse);
      assert.equal(controls[0].segment_index,undefined,"regenerate host sender fixture for scene protocol");
      assert(scene.begin(device,controls[0],connection).ok);
      const packets = fs.readFileSync(path.join(fixtureRoot,"sd-file.packets"));
      for(let at=0;at<packets.length;) { const n=packets.readUInt32LE(at);at+=4;scene.chunk(device,packets.subarray(at,at+n),connection);at+=n; }
      assert(scene.commit(device,controls[1],connection).ok);
      assert.deepEqual(fs.readFileSync(stored(controls[1].audio_session_id,controls[1].segment_index)),fs.readFileSync(path.join(fixtureRoot,"sd-file.bin")));
    }
    // Real WebSocket: ordered back-to-back binary frames, exact ACKs, no chunk ACK.
    const args = parseArgs(["node","gateway","--host","127.0.0.1","--storage-dir",path.join(dir,"wire")]);args.port=0;
    server=startServer(args);await once(server,"listening");
    const ws=new WebSocket(`ws://127.0.0.1:${server.address().port}/api/device/ws?device_id=WIRE-TEST`);
    await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
    const received=[];ws.addEventListener("message",e=>received.push(JSON.parse(e.data)));
    const exchange=message=>new Promise((resolve,reject)=>{
      const timer=setTimeout(()=>reject(new Error("wire ACK timeout")),10000);
      const receive=e=>{const ack=JSON.parse(e.data);if(ack.request_id!==message.request_id)return;
        clearTimeout(timer);ws.removeEventListener("message",receive);resolve(ack);};
      ws.addEventListener("message",receive);ws.send(JSON.stringify(message));
    });
    assert((await exchange(control("meeting_audio_begin","wire_scene"))).ok);
    for(let i=0;i<2;i++) {const bytes=fixture("wire_scene",i,false,i*960);for(let at=0;at<bytes.length;at+=8064)ws.send(packet(bytes,"wire_scene",i,at));
      assert((await exchange(control("meeting_audio_commit","wire_scene",meta(bytes,i)))).ok);}
    assert((await exchange(control("meeting_audio_session_end","wire_scene",{segment_count:2,recording_complete:true}))).ok);
    assert.equal(received.length,4);
    assert((await exchange(control("monitoring_audio_begin","wire_monitor_full"))).ok);
    for(let i=0;i<3;i++) {
      const bytes=fixture("wire_monitor_full",i,true,i*125940,i===2?7:6297,i*6297);
      for(let at=0;at<bytes.length;at+=8064)ws.send(packet(bytes,"wire_monitor_full",i,at));
      const prefix=i===0?"monitoring_audio_history":"monitoring_audio_realtime";
      assert((await exchange(control(`${prefix}_commit`,"wire_monitor_full",meta(bytes,i)))).ok);
    }
    assert((await exchange(control("monitoring_audio_session_end","wire_monitor_full",{segment_count:3,recording_complete:true}))).ok);
    assert.equal(received.length,9); // Only begin, file commits and session end produce ACKs.
    ws.close();await new Promise(resolve=>ws.addEventListener("close",resolve,{once:true}));
    console.log("PASS: scene begin once; 4MiB meeting/monitor files and short tails; byte/time integrity; preemption; restart/resume; lost ACK; CRC/conflict; late files; C producer; WebSocket");
  } finally {
    if(server)await new Promise(resolve=>server.close(resolve));
    await receiver.audioSessions.close();
    fs.rmSync(dir,{recursive:true,force:true});
  }
}
if(require.main===module)main().catch(error=>{console.error(error);process.exitCode=1;});
module.exports={packet};
