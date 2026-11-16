const { chromium } = require('playwright'); const fs = require('fs');
(async () => {
  fs.writeFileSync('pub/_t.html', '<!doctype html><html><head><meta charset=utf8><meta name=viewport content="width=device-width,initial-scale=1"></head><body>' + fs.readFileSync('pub/index.html', 'utf8') + '</body></html>');
  const b = await chromium.launch(); const p = await b.newPage({ viewport: { width: 390, height: 800 } });
  const errs = []; p.on('pageerror', e => errs.push(e.message));
  await p.addInitScript(() => { const store = {}, subs = []; window.__store = store;
    const mk = id => ({ id, data: () => store[id], metadata: { hasPendingWrites: false } });
    window.claude = { use: async () => ({ collection: () => ({
      doc: id => ({ set: async d => { await new Promise(r => setTimeout(r, 120)); store[id] = d; subs.forEach(f => f({ docs: Object.keys(store).map(mk), docChanges: () => [{ type: 'modified', doc: mk(id) }] })); } }),
      onSnapshot: f => { subs.push(f); setTimeout(() => f({ docs: [], docChanges: () => [] }), 40); return () => {}; } }) }) }; });
  await p.goto('file://' + process.cwd() + '/pub/_t.html'); await p.waitForTimeout(400);
  const pos = async () => (await p.locator('#pos').textContent()) + '/' + (await p.locator('#done').textContent());
  const first = await p.locator('#img').getAttribute('src');
  console.log('開始', await pos(), first);
  await p.locator('.rb[data-v="4"]').click(); await p.waitForTimeout(500);
  console.log('4を付けた後', await pos(), await p.locator('#img').getAttribute('src'));
  await p.locator('#prev').click(); await p.waitForTimeout(200);
  console.log('前へ→押下状態', await p.locator('.rb[aria-pressed="true"]').evaluateAll(a => a.map(x => x.dataset.v)), await p.locator('#img').getAttribute('src'));
  await p.locator('.rb[data-v="2"]').click(); await p.waitForTimeout(500);
  console.log('2に付け直し', JSON.stringify(await p.evaluate(() => window.__store)), await pos());
  await p.locator('#prev').click(); await p.locator('.tg[data-n="強すぎ"]').click(); await p.waitForTimeout(300);
  console.log('タグ', JSON.stringify(await p.evaluate(() => window.__store)));
  await p.locator('#tolist').click(); console.log('一覧のサムネ数', await p.locator('.th').count(), '集計行', await p.locator('table tr').count());
  await p.screenshot({ path: 'shot_list.png' });
  await p.locator('.th').first().click(); await p.waitForTimeout(200); await p.screenshot({ path: 'shot_card.png' });
  console.log('errors', errs); await b.close();
})();
