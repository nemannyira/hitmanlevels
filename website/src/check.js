// The upload's checks: the client's own package reader and strict level check (shared/levelfmt,
// h2/src/levelfmt) built to WebAssembly (check/build.ps1 -> check.wasm), so the site lists exactly what the
// client accepts (docs/site_api.md "Checks on upload"). Each package gets a new instance: whatever a hostile
// one does to the module's memory ends with its check.

import wasm from "./check.wasm";

// the WASI calls the module links to (libc's): no environment, no files, a clock; output dropped
function wasi(memory) {
  const view = () => new DataView(memory().buffer);
  return {
    environ_sizes_get: (count, size) => { view().setUint32(count, 0, true); view().setUint32(size, 0, true); return 0; },
    environ_get: () => 0,
    clock_time_get: (_id, _precision, out) => { view().setBigUint64(out, BigInt(Date.now()) * 1000000n, true); return 0; },
    fd_close: () => 8,  // EBADF
    fd_seek: () => 8,
    fd_write: (_fd, iovs, count, written) => {
      let n = 0;
      for (let i = 0; i < count; i++) n += view().getUint32(iovs + i * 8 + 4, true);
      view().setUint32(written, n, true);
      return 0;
    },
    proc_exit: (code) => { throw new Error(`the check stopped (${code})`); },
  };
}

const CHECKS = [
  "A level package and nothing else (only the files a level may hold, no game files)",
  "Signed by its author, every file as signed",
  "The level's file passes the game's own check",
];

// { ok, error?, manifest?, checks: [names passed] }
export async function checkPackage(bytes) {
  let instance;
  const imports = { wasi_snapshot_preview1: wasi(() => instance.exports.memory) };
  try {
    instance = await WebAssembly.instantiate(wasm, imports);
    const x = instance.exports;
    x._initialize();
    const at = x.check_alloc(bytes.length);
    new Uint8Array(x.memory.buffer, at, bytes.length).set(bytes);
    const out = x.check_package(at, bytes.length);
    const text = new TextDecoder().decode(new Uint8Array(x.memory.buffer, out, x.check_result_size()));
    const r = JSON.parse(text);
    return r.ok ? { ok: true, manifest: r.manifest, files: r.files, checks: CHECKS } : { ok: false, error: r.error, checks: [] };
  } catch (e) {
    return { ok: false, error: "The package could not be checked: it is not a level package this site takes.", checks: [] };
  }
}
