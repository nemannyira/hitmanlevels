// The upload's checks: the client's own package reader and strict level check (shared/levelfmt,
// h2/src/levelfmt) built to WebAssembly, so the site lists exactly what the client accepts
// (docs/site_api.md "Checks on upload"). Not built yet: until it is, every upload is refused.

export async function checkPackage(bytes) {
  return {
    ok: false,
    error: "Uploads are not open yet: the package check is still being built.",
    checks: [],
  };
}
