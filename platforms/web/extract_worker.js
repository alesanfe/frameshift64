/* Torch extraction worker — runs the ROM→o2r pipeline off the page thread.
 *
 * Protocol (postMessage):
 *   page → worker: { id, rom: ArrayBuffer }            (transferred)
 *   worker → page: { id, phase }                       phase: 'load'|'recipes'|'extract'
 *                  { id, done:i, total:n }             recipe staging ticks
 *                  { id, ok:true,  o2r:ArrayBuffer }   (transferred)
 *                  { id, ok:false, code, message }
 *
 * The page kills the job with worker.terminate() — since Extract() blocks
 * only this worker's thread, cancel is clean and leaves no half-state in
 * the game FS. A fresh worker is spawned per attempt, so nothing leaks.
 *
 * Module workers: torch.js is EXPORT_ES6 (`export default createModule`),
 * so this file is loaded as { type: 'module' } and can static-import it.
 */
import createModule from './torch/torch.js';

function post(msg, transfer) {
  (self).postMessage(msg, transfer || []);
}

async function stageRecipes(FS, id) {
  const man = await fetch('asset-recipes/files.txt');
  if (!man.ok) {
    throw Object.assign(new Error('asset-recipes/files.txt missing — serve the build dir'),
                        { code: 'RECIPE_MANIFEST_MISSING' });
  }
  const rels = (await man.text()).split('\n').map(s => s.trim()).filter(Boolean);
  FS.mkdirTree('/recipes');
  for (let i = 0; i < rels.length; i++) {
    const r = await fetch('asset-recipes/' + rels[i]);
    if (!r.ok) {
      throw Object.assign(new Error('recipe fetch failed: ' + rels[i]),
                          { code: 'RECIPE_FETCH_FAILED' });
    }
    const slash = rels[i].lastIndexOf('/');
    if (slash > 0) FS.mkdirTree('/recipes/' + rels[i].slice(0, slash));
    FS.writeFile('/recipes/' + rels[i], new Uint8Array(await r.arrayBuffer()));
    if ((i & 15) === 0) post({ id, phase: 'recipes', done: i + 1, total: rels.length });
  }
  return rels.length;
}

self.onmessage = async (e) => {
  const { id, rom } = e.data || {};
  try {
    if (typeof id !== 'number' || !(rom instanceof ArrayBuffer) || !rom.byteLength) {
      post({ id: id || -1, ok: false, code: 'BAD_REQUEST', message: 'worker got no ROM bytes' });
      return;
    }
    post({ id, phase: 'load' });
    const mod = await createModule({
      print: (t) => post({ id, phase: 'log', text: t }),
      printErr: (t) => post({ id, phase: 'log', text: t }),
    });
    const FS = mod.FS;

    post({ id, phase: 'recipes' });
    await stageRecipes(FS, id);

    FS.writeFile('/baserom', new Uint8Array(rom));
    FS.mkdirTree('/out');
    post({ id, phase: 'extract' });

    if (mod.Extract('/baserom', '/recipes', '/out') !== 0) {
      post({ id, ok: false, code: 'EXTRACT_FAILED',
             message: 'extraction failed — is this a supported Smash 64 ROM?' });
      return;
    }
    const out = FS.readdir('/out').filter(n => n.toLowerCase().endsWith('.o2r'));
    if (!out.length) {
      post({ id, ok: false, code: 'NO_O2R', message: 'extractor produced no .o2r' });
      return;
    }
    const bytes = FS.readFile('/out/' + out[0]);
    post({ id, ok: true, name: out[0], o2r: bytes.buffer }, [bytes.buffer]);
  } catch (err) {
    post({ id: (e.data && e.data.id) || -1, ok: false,
           code: (err && err.code) || 'EXTRACT_ERROR',
           message: (err && err.message) || String(err) });
  }
};
