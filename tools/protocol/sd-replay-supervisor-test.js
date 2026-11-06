"use strict";
const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const net = require("net");
const { spawn } = require("child_process");
const { once } = require("events");
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(fn) {
  for (let i = 0; i < 80; i++) {
    try { const result = fn(); if (result) return result; } catch {}
    await delay(100);
  }
  throw new Error("supervisor test timed out");
}
async function main() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-supervisor-test-"));
  const storage = path.join(dir, "archives");
  fs.mkdirSync(storage);
  const partial = path.join(storage, "retained.part");
  fs.writeFileSync(partial, "durable-prefix");
  const probe = net.createServer();
  probe.listen(0, "127.0.0.1");
  await once(probe, "listening");
  const port = probe.address().port;
  await new Promise(resolve => probe.close(resolve));
  const preload = path.join(dir, "inject-spawn.js");
  fs.writeFileSync(preload, `const cp=require('child_process');const original=cp.spawn;let n=0;
    cp.spawn=function(exe,args,options){
      if(++n===1)throw Object.assign(new Error('injected synchronous spawn UNKNOWN'),{code:'UNKNOWN'});
      if(n===2)return original('nuwax-test-nonexistent-executable',args,options);
      return original(exe,args,options);
    };`);
  const supervisor = spawn(process.execPath, ["--require", preload, path.join(__dirname, "sd-replay-supervisor.js"),
    dir, storage, "127.0.0.1", String(port)], { windowsHide: true, stdio: "ignore" });
  const state = () => JSON.parse(fs.readFileSync(path.join(dir, "supervisor.json")));
  try {
    const first = await until(() => { const s = state(); return s.state === "running" && s.childPid && s; });
    assert.equal(first.attempt, 3); // Sync throw + async spawn error, one retry each.
    await until(() => fs.existsSync(path.join(dir, `${first.logPrefix}-report.json`)));
    process.kill(first.childPid); // Fault injection into this test's own child.
    const second = await until(() => { const s = state(); return s.childPid && s.childPid !== first.childPid && s; });
    await until(() => fs.existsSync(path.join(dir, `${second.logPrefix}-report.json`)));
    assert.equal(second.attempt, 4);
    assert.equal(fs.readFileSync(partial, "utf8"), "durable-prefix");
    assert(fs.existsSync(path.join(dir, `${first.logPrefix}-report.json`)));
    fs.writeFileSync(path.join(dir, "stop.request"), "stop");
    await until(() => supervisor.exitCode !== null);
    assert.equal(state().state, "stopped");
    console.log("PASS: synchronous/async spawn failure retries once; abnormal exit restarts gateway; preserves prefix/reports; explicit stop prevents restart");
  } finally {
    fs.writeFileSync(path.join(dir, "stop.request"), "stop");
    await until(() => supervisor.exitCode !== null);
    fs.rmSync(dir, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
