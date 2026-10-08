// Drives the real cefui/index.html + character.js in Chromium. The bridge stands in for
// JNI (CharacterManager) and the test plays the gamemode (character_system.inc).
let chromium; try { ({ chromium } = require('playwright')); } catch (e) { ({ chromium } = require('/opt/node-tools/node_modules/playwright')); }
const fs = require('fs'), path = require('path');
const [,, CEF, NATIVE, CATINC] = process.argv;
const [uiJson, defaultLook] = fs.readFileSync(NATIVE, 'utf8').trim().split('\n');
const inc = fs.readFileSync(CATINC, 'utf8');
const catalog = [...inc.matchAll(/\{(\d+),(\d+),(\d+),(\d+)\}/g)].map(m => m.slice(1).map(Number));
const FIELDS = ['gender','bodyType','skinTone','face','hair','hairColor','eyebrows','beard','makeup','top','jacket','pants','shoes','hat','glasses','mask','watch','necklace','bag','accessory','tattoo','freckles'];
// --- port of character_system.inc EC_Parse / EC_Validate / EC_Commit ownership check ---
function ecParse(text){ if(!/^\d{1,10}(,\d{1,10}){23}$/.test(text)) return null; const v=text.split(',').map(Number); return v.some(n=>n>2147483647)?null:v; }
function ecValidate(l){
  if(l.some(n=>n<0||n>65535)) return 'range';
  if(l[0]>1||l[1]>2||l[2]<1||l[2]>6||l[3]<1||l[3]>12||l[4]>8||l[5]>=11) return 'body';
  if(l[6]>2||l[7]>2||l[8]>2||l[20]>2||l[21]>2||(l[0]&&l[7])) return 'detail';
  for(let f=9;f<=19;f++) if(l[f]){ const r=catalog.find(c=>c[0]===l[f]); if(!r||r[1]!==f||!(r[3]&(1<<(l[0]*3+l[1])))) return 'item '+l[f]; }
  return '';
}
const prices = Object.fromEntries(catalog.map(c=>[String(c[0]),c[2]]));
let failures = 0; const ok=(c,m)=>{console.log((c?'PASS ':'FAIL ')+m); if(!c) failures++;};
(async () => {
  const browser = await chromium.launch({ executablePath: '/opt/pw-browsers/chromium-1194/chrome-linux/chrome' }).catch(()=>chromium.launch());
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
  const errors = [];
  page.on('pageerror', e => errors.push(String(e && e.stack || e)));
  await page.route(/fonts\.(googleapis|gstatic)\.com/, r => r.abort());
  await page.addInitScript(({ uiJson }) => {
    window.__sent = [];
    const T = (ev, d) => setTimeout(() => window.Cef._trigger(ev, typeof d === 'string' ? d : JSON.stringify(d)), 5);
    window.__native = { confirmed: null, revision: 1 };
    window.CefBridge = { sendClientEvent(ev, json) {
      window.__sent.push([ev, json]);
      if (ev !== 'eagle_character_local') return;
      const p = JSON.parse(json), n = window.__native;
      // CharacterManager::ProcessEvent
      if (p.action === 'begin' && n.confirmed) { T('eagle_character_catalog', uiJson); T('eagle_character_local_state', { appearance: n.confirmed, revision: n.revision }); T('eagle_character_preview_status', { ready: true }); }
      if (p.action === 'preview') { T('eagle_character_preview_status', { ready: false }); setTimeout(() => T('eagle_character_preview_status', { ready: true }), 30); }
    } };
  }, { uiJson });
  await page.goto('file://' + path.join(CEF, 'index.html'));
  await page.waitForFunction(() => window.EAGLE && EAGLE.isDone('character'), null, { timeout: 15000 });
  ok(true, 'character.js loaded (build ' + await page.evaluate(() => EAGLE.build) + ')');
  const trigger = (ev, d) => page.evaluate(([ev, d]) => window.Cef._trigger(ev, JSON.stringify(d)), [ev, d]);
  const sent = () => page.evaluate(() => window.__sent.map(([e, j]) => [e, JSON.parse(j)]));
  const uiSent = async (a) => (await sent()).filter(([e, j]) => e === 'ui' && j.v === 'character' && (!a || j.a === a));
  const look0 = JSON.parse(defaultLook);
  const serverState = (look, rev, nonce, owned, money) => trigger('eagle_character_server_state', { revision: rev, nonce, money, catalogRevision: 3, prices, appearance: look, owned });
  // ---------------- creator ----------------
  await page.evaluate(l => { window.__native.confirmed = l; }, look0);
  await serverState(look0, 1, 777, [101, 301, 401], 5000);
  await trigger('eagle_character_open', { mode: 'creator', nonce: 777, required: true });
  await page.waitForSelector('#eagle-character:not([hidden]) .ec-options button');
  ok((await sent()).some(([e, j]) => e === 'eagle_character_local' && j.action === 'begin' && j.mode === 'creator'), 'creator: begin sent to JNI');
  ok(await page.isHidden('#eagle-character .ec-close'), 'creator (required): close button hidden');
  await page.waitForFunction(() => !document.querySelector('#ec-save').disabled);
  await page.click('#eagle-character .ec-options button:text-is("Female")');
  await page.waitForFunction(() => window.__sent.some(([e, j]) => e === 'eagle_character_local' && JSON.parse(j).action === 'preview' && JSON.parse(j).appearance.gender === 1));
  const prev = (await sent()).filter(([e, j]) => e === 'eagle_character_local' && j.action === 'preview').pop()[1];
  ok(Object.keys(prev.appearance).length === 22 && FIELDS.every(k => Number.isInteger(prev.appearance[k])), 'preview carries all 22 integer fields (ParseAppearance contract)');
  await page.click('.ec-nav button:text-is("Rambut")');
  await page.click('#eagle-character .ec-options .ec-choices button >> nth=3');
  await page.click('.ec-nav button:text-is("Kulit")');
  await page.click('#eagle-character .ec-swatches button >> nth=4');
  await page.waitForFunction(() => !document.querySelector('#ec-save').disabled);
  await page.click('#ec-save');
  await page.waitForFunction(() => window.__sent.some(([e, j]) => e === 'ui' && JSON.parse(j).a === 'save'));
  const save = (await uiSent('save')).pop()[1];
  const raw = JSON.stringify(save);
  ok(raw.length <= 1024, `save JSON ${raw.length} B <= 1024 (CharacterManager::HandleCefEvent limit)`);
  const d = ecParse(save.s);
  ok(!!d, 'EC_Parse accepts payload: ' + save.s);
  const look = d.slice(2);
  ok(d[0] === 777 && d[1] === 1, 'nonce/revision match server');
  ok(ecValidate(look) === '', 'EC_Validate accepts creator look (gender=' + look[0] + ' hair=' + look[4] + ' tone=' + look[2] + ')');
  ok(look[0] === 1 && look[2] === 5, 'creator choices reached the payload');
  // server: EC_Commit -> EC_UIState, eagle_character_saved, EC_Close
  const saved = Object.fromEntries(FIELDS.map((k, i) => [k, look[i]]));
  await page.evaluate(l => { window.__native.confirmed = l; window.__native.revision = 2; }, saved);
  await serverState(saved, 2, 777, [101, 301, 401], 5000);
  await trigger('eagle_character_saved', { revision: 2 });
  await trigger('eagle_character_close', { show: false });
  await page.waitForSelector('#eagle-character', { state: 'hidden' });
  ok((await sent()).some(([e, j]) => e === 'eagle_character_local' && j.action === 'cancel'), 'close: preview cancelled in JNI');
  // ---------------- shop (/baju) ----------------
  await serverState(saved, 2, 888, [101, 301, 401], 5000);
  await trigger('eagle_character_open', { mode: 'shop', nonce: 888 });
  await page.waitForSelector('#eagle-character:not([hidden]) .ec-item');
  const cards = await page.$$eval('#eagle-character .ec-item button:first-of-type', b => b.map(x => x.textContent));
  ok(cards.length === 32, `shop shows ${cards.length} tops for ${saved.gender ? 'female' : 'male'}`);
  await page.click('#eagle-character .ec-item button:text-is("Top 05 Coral")');
  await page.waitForFunction(() => { const c = [...document.querySelectorAll('#eagle-character .ec-item')].find(n => n.textContent.includes('Top 05 Coral')); const b = c && [...c.querySelectorAll('button')].find(x => x.textContent === 'Beli'); return b && !b.disabled; });
  await page.evaluate(() => { const c = [...document.querySelectorAll('#eagle-character .ec-item')].find(n => n.textContent.includes('Top 05 Coral')); [...c.querySelectorAll('button')].find(x => x.textContent === 'Beli').click(); });
  await page.waitForFunction(() => window.__sent.some(([e, j]) => e === 'ui' && JSON.parse(j).a === 'buy'));
  const buy = (await uiSent('buy')).pop()[1], bd = ecParse(buy.s), bl = bd.slice(2);
  ok(buy.i === '105' && bl[9] === 105 && bd[0] === 888 && bd[1] === 2, 'buy: item 105 equipped, nonce/revision current');
  ok(ecValidate(bl) === '' && [10,11,12].every(f => !bl[f] || [101,301,401,105].includes(bl[f])), 'EC_Commit precondition holds (only the bought item is unowned)');
  ok(bl.slice(0, 9).every((v, i) => v === look[i]) && bl[20] === look[20] && bl[21] === look[21], 'shop payload keeps body/face fields (EC_OnUI non-creator rule)');
  // ---------------- wardrobe (/clothes) ----------------
  await serverState(Object.assign({}, saved, { top: 105 }), 3, 999, [101, 105, 301, 401], 4740);
  await trigger('eagle_character_saved', { revision: 3 });
  await trigger('eagle_character_close', { show: false });
  await trigger('eagle_character_open', { mode: 'wardrobe', nonce: 999 });
  await page.waitForSelector('#eagle-character:not([hidden]) .ec-item');
  const owned = await page.$$eval('#eagle-character .ec-item button:first-of-type', b => b.map(x => x.textContent));
  ok(owned.length === 2 && owned.every(t => /Top 0[15]/.test(t)), 'wardrobe lists only owned tops: ' + owned.join(', '));
  const charErr = errors.filter(e => /character\.js/.test(e));
  ok(charErr.length === 0, 'no character.js errors' + (charErr.length ? ': ' + charErr[0] : ''));
  if (errors.length) console.log('other page errors (not character.js):', errors.length);
  await page.screenshot({ path: path.join(path.dirname(NATIVE), 'cef_wardrobe.png') });
  await browser.close();
  console.log(failures ? `${failures} FAILED` : 'ALL PASSED'); process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
