// node-smoke.js — NanOS Node acceptance oracle (plan 02 Task 2.4): versions, fs, net, dns, timers,
// promises. Prints "node-smoke: PASS" and exits 0 on success; the QEMU smoke greps for that marker.
const fs = require('fs');
const net = require('net');
const dns = require('dns');

let failures = 0;
const ok = (cond, name) => {
  console.log((cond ? '  OK  : ' : '  FAIL: ') + name);
  if (!cond) failures++;
};

console.log('=== node-smoke ===');
console.log('node ' + process.versions.node + ' v8 ' + process.versions.v8);

// fs round-trip
const p = '/tmp/node-smoke.txt';
fs.writeFileSync(p, 'nanos');
ok(fs.readFileSync(p, 'utf8') === 'nanos', 'fs write/read round-trip');
fs.renameSync(p, p + '.2'); ok(fs.existsSync(p + '.2'), 'fs rename');
fs.unlinkSync(p + '.2');

// timers + promises
let timerFired = false;
setTimeout(() => { timerFired = true; }, 50);

// tcp loopback echo
const srv = net.createServer(s => s.pipe(s));
srv.listen(0, '127.0.0.1', () => {
  const c = net.connect(srv.address().port, '127.0.0.1', () => c.write('ping'));
  c.on('data', d => {
    ok(d.toString() === 'ping', 'tcp loopback echo');
    c.end(); srv.close();
    dns.lookup('localhost', (err, addr) => {
      ok(!err && addr, 'dns lookup localhost');
      Promise.resolve(41).then(v => {
        ok(v + 1 === 42, 'promise resolves');
        ok(timerFired, 'timer fired');
        console.log(failures ? 'node-smoke: FAIL' : 'node-smoke: PASS');
        process.exit(failures ? 1 : 0);
      });
    });
  });
});
