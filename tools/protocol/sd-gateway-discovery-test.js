"use strict";
const assert = require("node:assert/strict"), fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const dgram = require("node:dgram"), { once } = require("node:events"), { spawnSync } = require("node:child_process");
const { answer, mac, startDiscovery } = require("./sd-gateway-discovery");
const { loadConfig, prepareBuild } = require("./sd-test-gateway-config");
async function main() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "nuwax-discovery-"));
  let responder, client;
  try {
    const file = path.join(root, "settings.json");
    const config = loadConfig(file, true); assert.deepEqual(loadConfig(file, true), config);
    const query = Buffer.alloc(72); query.write("NWXSDQ1\0", 0, "ascii"); query.fill(0xab, 8, 24);
    Buffer.from(config.gatewayId, "hex").copy(query, 24); mac(query.subarray(0, 40), config.discoveryKey).copy(query, 40);
    const reply = answer(query, config);
    assert(reply && reply.length === 76); assert.equal(reply.readUInt16BE(40), 8810);
    assert.deepEqual(reply.subarray(8, 40), query.subarray(8, 40)); assert.deepEqual(reply.subarray(44), mac(reply.subarray(0, 44), config.discoveryKey));
    for (let i = 0; i < query.length; i++) { const bad = Buffer.from(query); bad[i] ^= 1; assert.equal(answer(bad, config), null); }
    assert.equal(answer(query.subarray(0, 71), config), null);
    assert.equal(answer(Buffer.concat([query, Buffer.alloc(1)]), config), null);
    assert.equal(answer(query, { ...config, discoveryKey: "00".repeat(32) }), null);
    responder = startDiscovery(file);
    client = dgram.createSocket("udp4"); client.bind(0, "127.0.0.1"); await once(client, "listening");
    async function exchange() {
      const timeout = setTimeout(() => client.emit("error", new Error("discovery timed out")), 2000);
      try { const incoming = once(client, "message"); client.send(query, 8811, "127.0.0.1"); return (await incoming)[0]; }
      finally { clearTimeout(timeout); }
    }
    assert.deepEqual(await exchange(), reply);
    responder.close(); responder = null;
    await new Promise(resolve => setTimeout(resolve, 30));
    responder = startDiscovery(file); await new Promise(resolve => setTimeout(resolve, 30));
    assert.deepEqual(await exchange(), reply); // identity survives gateway restart
    const sdk = path.join(root, "sdkconfig");
    fs.writeFileSync(sdk, 'CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE=n\nCONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE=y\n');
    prepareBuild(sdk); const generated = fs.readFileSync(sdk, "utf8");
    assert(generated.includes('CONFIG_NUWAX_SD_GATEWAY_DISCOVERY_ENABLE=y'));
    assert(!generated.includes('CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE=y'));
    assert.equal(generated.match(/^CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE=/gm).length, 1);
    // Exercise actual ESP-IDF discovery control flow against bounded socket/PSA stubs.
    const fixture = { id: config.gatewayId, key: config.discoveryKey, query: query.toString("hex"), reply: reply.toString("hex") };
    fs.writeFileSync(path.join(root, "fixture.json"), JSON.stringify(fixture));
    const check = spawnSync("C:/Espressif/python_env/idf6.0_py3.13_env/Scripts/python.exe",
      [path.join(__dirname, "../test-sd-gateway-discovery.py"), root], { encoding: "utf8", windowsHide: true });
    process.stdout.write(check.stdout || ""); process.stderr.write(check.stderr || ""); assert.equal(check.status, 0);
    console.log("PASS: authenticated UDP, every query-byte corruption, wrong key/identity, gateway restart, persistent build pairing and device discovery control flow");
  } finally {
    if (client) client.close(); if (responder) responder.close();
    assert.equal(path.dirname(root), path.resolve(os.tmpdir())); assert(path.basename(root).startsWith("nuwax-discovery-"));
    fs.rmSync(root, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
