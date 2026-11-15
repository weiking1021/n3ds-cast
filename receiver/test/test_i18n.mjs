// The page's language choice and its catalogs. The TypeScript is run as it is (Node strips the types).
import assert from 'node:assert';
import { CATALOGS, DEFAULT_LOCALE, matchLocale, resolveLocale, translate } from '../src/i18n.ts';

// language tags -> supported languages
assert.strictEqual(matchLocale('en'), 'en');
assert.strictEqual(matchLocale('en-US'), 'en');
assert.strictEqual(matchLocale('zh-TW'), 'zh-TW');
assert.strictEqual(matchLocale('zh_hant_tw'), 'zh-TW');
assert.strictEqual(matchLocale('zh-HK'), 'zh-TW');
assert.strictEqual(matchLocale('zh-CN'), 'zh-TW'); // Traditional is the only Chinese there is
assert.strictEqual(matchLocale('ja'), 'ja');
assert.strictEqual(matchLocale('ja-JP'), 'ja');
assert.strictEqual(matchLocale('ko'), null);
assert.strictEqual(matchLocale(''), null);
assert.strictEqual(matchLocale(undefined), null);

// the first candidate that is supported wins; missing or unsupported ones are skipped; English when none is
assert.strictEqual(resolveLocale(['zh-TW', 'en']), 'zh-TW');
assert.strictEqual(resolveLocale([null, 'ko', 'zh-TW', 'en']), 'zh-TW'); // the 3DS asked for Korean: use the next one
assert.strictEqual(resolveLocale(['ja', 'zh-TW', 'en']), 'ja'); // the 3DS asked for Japanese
assert.strictEqual(resolveLocale([undefined, 'en-GB', 'zh-TW']), 'en');
assert.strictEqual(resolveLocale(['ko', 'fr']), DEFAULT_LOCALE);
assert.strictEqual(resolveLocale([]), DEFAULT_LOCALE);

// parameters are put in
assert.strictEqual(translate('en', 'audio.waiting', { kbps: 64 }), 'Audio: 64 kbps, waiting');
assert.strictEqual(translate('zh-TW', 'audio.waiting', { kbps: 64 }), '音訊：64 kbps，等待中');
assert.strictEqual(translate('ja', 'audio.waiting', { kbps: 64 }), '音声：64 kbps、待機中');

// every language says the same things: the same keys, the same placeholders, nothing empty
const placeholders = (text) => [...text.matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort().join(',');
const keys = Object.keys(CATALOGS.en);
for (const [locale, catalog] of Object.entries(CATALOGS)) {
  assert.deepStrictEqual(Object.keys(catalog).sort(), [...keys].sort(), `${locale} has other keys than en`);
  for (const key of keys) {
    assert(catalog[key].trim(), `${locale} ${key} is empty`);
    assert.strictEqual(placeholders(catalog[key]), placeholders(CATALOGS.en[key]), `${locale} ${key}: other placeholders`);
  }
}

console.log('PASS: page language choice (?lang, 3DS, browser/TV preference, English fallback) and catalogs');
