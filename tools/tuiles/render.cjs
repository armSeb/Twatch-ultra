'use strict';
/*
 * render.cjs — tuiles raster 256 × 256 pour l'app Cartes de la montre.
 *
 * Tourne dans l'image Docker (voir Dockerfile) : MapLibre Native dessine les
 * tuiles vectorielles OpenMapTiles (fichier .mbtiles fait par Planetiler) avec
 * le style « montre », sharp les découpe et les écrit en PNG palette.
 *
 *   node render.cjs estimer [options]   compte les tuiles, place sur la carte SD
 *   node render.cjs paquet [options]    refait seulement tiles.pmtiles depuis tiles/
 *   node render.cjs dessiner [options]  dessine dans <sortie>/tiles/<z>/<x>/<y>.png,
 *                                       puis range tout dans <sortie>/tiles.pmtiles
 *
 * La montre lit tiles.pmtiles (PMTiles v3, un seul fichier : rapide à copier
 * sur la carte SD) ; le dossier tiles/ sert d'étape et à la reprise.
 *
 * Les tuiles d'une seule couleur (mer, hors de l'extrait) dont la tuile
 * parente est de la même couleur ne sont pas écrites : la montre agrandit la
 * parente (jusqu'à 4 niveaux), c'est identique et ça économise la carte SD.
 *
 * Reprise : un journal dans <sortie>/.journal-tuiles retient les blocs finis ;
 * relancer la même commande continue où elle s'était arrêtée.
 */
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const os = require('os');
const crypto = require('crypto');

const NM = '/usr/src/app/node_modules/';

const TILE = 256;
const META = 8;            /* bloc de 8 × 8 tuiles dessiné d'un coup */
const MARGIN = 128;        /* marge autour du bloc : libellés entiers */
const UPSCALE_MAX = 4;     /* MT_UPSCALE du firmware (main/maptiles.h) */
const DETAIL_ZOOMS = [13, 16];

/* ------------------------------------------------------------ options */

function die(msg) {
    console.error('Erreur : ' + msg);
    process.exit(2);
}

function parseSize(s, what) {
    const m = /^(\d+(?:[.,]\d+)?)\s*([kKmMgGtT]?)[oOiIbB]*$/.exec(String(s).trim());
    if (!m) die(`${what} illisible : « ${s} » (exemples : 2G, 512M, 32K)`);
    const mul = { '': 1, k: 1024, m: 1024 ** 2, g: 1024 ** 3, t: 1024 ** 4 }[m[2].toLowerCase()];
    return Math.round(parseFloat(m[1].replace(',', '.')) * mul);
}

function parseZooms(s, what) {
    const m = /^(\d+)(?:-(\d+))?$/.exec(String(s).trim());
    if (!m) die(`${what} : niveaux illisibles « ${s} » (exemple : 13-16)`);
    const a = +m[1], b = m[2] === undefined ? a : +m[2];
    if (a > b || b > 20) die(`${what} : niveaux ${s} invalides (0 à 20, du plus petit au plus grand)`);
    return [a, b];
}

function parseArgs(argv) {
    const o = {
        cmd: argv[0], zooms: '0-12', details: [], bboxes: [], taille: '2G',
        libre: null, jobs: Math.max(1, os.cpus().length - 1), forcer: false, refaire: false,
        couleurs: 128, pbf: null, mbtiles: null, style: '/opt/twatch/style-montre.json',
        fonts: NM + 'tileserver-gl-styles/fonts', sortie: '/sortie',
    };
    if (!['estimer', 'dessiner', 'paquet'].includes(o.cmd)) die('commande « estimer », « dessiner » ou « paquet » attendue');
    argv = argv.flatMap((a) => (/^--[a-z]+=/.test(a) ? [a.slice(0, a.indexOf('=')), a.slice(a.indexOf('=') + 1)] : [a]));
    for (let i = 1; i < argv.length; i++) {
        const a = argv[i];
        const val = () => {
            if (i + 1 >= argv.length) die(`valeur manquante après ${a}`);
            return argv[++i];
        };
        switch (a) {
        case '--zooms': o.zooms = val(); break;
        case '--detail': o.details.push(val()); break;
        case '--bbox': o.bboxes.push(val()); break;
        case '--taille': o.taille = val(); break;
        case '--libre': o.libre = val(); break;
        case '--jobs': o.jobs = Math.max(1, parseInt(val(), 10) || 1); break;
        case '--couleurs': o.couleurs = Math.min(256, Math.max(2, parseInt(val(), 10) || 128)); break;
        case '--forcer': o.forcer = true; break;
        case '--refaire': o.refaire = true; break;
        case '--pbf': o.pbf = val(); break;
        case '--mbtiles': o.mbtiles = val(); break;
        case '--style': o.style = val(); break;
        case '--fonts': o.fonts = val(); break;
        case '--sortie': o.sortie = val(); break;
        default: die(`option inconnue : ${a}`);
        }
    }
    return o;
}

/* ------------------------------------------------- géométrie des tuiles */

function lon2x(lon, z) { return (lon + 180) / 360 * 2 ** z; }
function lat2y(lat, z) {
    const r = Math.max(-85.0511, Math.min(85.0511, lat)) * Math.PI / 180;
    return (1 - Math.asinh(Math.tan(r)) / Math.PI) / 2 * 2 ** z;
}
function x2lon(x, z) { return x / 2 ** z * 360 - 180; }
function y2lat(y, z) { return Math.atan(Math.sinh(Math.PI * (1 - 2 * y / 2 ** z))) * 180 / Math.PI; }

/* rectangle de tuiles [x0, y0, x1, y1] (inclus) couvrant bbox au niveau z */
function tileRect(b, z) {
    const n = 2 ** z, c = (v) => Math.max(0, Math.min(n - 1, Math.floor(v)));
    return [c(lon2x(b[0], z)), c(lat2y(b[3], z)), c(lon2x(b[2], z) - 1e-9), c(lat2y(b[1], z) - 1e-9)];
}

/* bbox [ouest, sud, est, nord] d'un fichier .osm.pbf, lue dans son en-tête */
function pbfBounds(file) {
    const fd = fs.openSync(file, 'r');
    try {
        const head = Buffer.alloc(4);
        fs.readSync(fd, head, 0, 4, 0);
        const hlen = head.readUInt32BE(0);
        if (hlen > 64 * 1024) throw new Error('en-tête PBF invalide');
        const hb = Buffer.alloc(hlen);
        fs.readSync(fd, hb, 0, hlen, 4);
        let datasize = 0, type = '';
        for (const [f, v] of pbMessage(hb)) {
            if (f === 1) type = v.toString();
            if (f === 3) datasize = v;
        }
        if (type !== 'OSMHeader' || !datasize || datasize > 32 * 1024 * 1024) throw new Error('pas un fichier .osm.pbf');
        const blob = Buffer.alloc(datasize);
        fs.readSync(fd, blob, 0, datasize, 4 + hlen);
        let data = null;
        for (const [f, v] of pbMessage(blob)) {
            if (f === 1) data = v;
            if (f === 3) data = zlib.inflateSync(v);
        }
        if (!data) throw new Error('en-tête PBF compressé autrement qu\'en zlib');
        for (const [f, v] of pbMessage(data)) {
            if (f !== 1) continue;
            const b = {};
            for (const [g, w] of pbMessage(v)) b[g] = zigzag(w) / 1e9;
            if ([1, 2, 3, 4].every((k) => k in b)) return [b[1], b[4], b[2], b[3]];
        }
        return null;
    } finally {
        fs.closeSync(fd);
    }
}

function zigzag(n) { return n % 2 ? -(n + 1) / 2 : n / 2; }

/* lecteur protobuf minimal : [numéro de champ, valeur (nombre ou Buffer)] */
function* pbMessage(buf) {
    let p = 0;
    const varint = () => {
        let v = 0, mul = 1, b;
        do {
            if (p >= buf.length) throw new Error('protobuf tronqué');
            b = buf[p++];
            v += (b & 0x7f) * mul;
            mul *= 128;
        } while (b & 0x80);
        return v;
    };
    while (p < buf.length) {
        const key = varint(), f = Math.floor(key / 8), wt = key % 8;
        if (wt === 0) yield [f, varint()];
        else if (wt === 2) {
            const n = varint();
            yield [f, buf.subarray(p, p + n)];
            p += n;
        } else if (wt === 1) p += 8;
        else if (wt === 5) p += 4;
        else throw new Error('protobuf : type ' + wt);
    }
}

/* --------------------------------------------------------- les zones */

function parseBbox(s, what) {
    const v = s.split(',').map((t) => t.trim());
    if (v.length !== 4 && v.length !== 5) die(`${what} : « ${s} » — attendu OUEST,SUD,EST,NORD[,ZMIN-ZMAX]`);
    const b = v.slice(0, 4).map(Number);
    if (b.some(isNaN) || b[0] >= b[2] || b[1] >= b[3]) die(`${what} : rectangle invalide « ${s} »`);
    return { bbox: b, zooms: v[4] ? parseZooms(v[4], what) : DETAIL_ZOOMS, nom: what };
}

function parseDetail(s, what) {
    const v = s.split(',').map((t) => t.trim());
    if (v.length !== 3 && v.length !== 4) die(`${what} : « ${s} » — attendu LAT,LON,RAYON_KM[,ZMIN-ZMAX]`);
    const [lat, lon, r] = v.slice(0, 3).map(Number);
    if ([lat, lon, r].some(isNaN) || Math.abs(lat) > 85 || Math.abs(lon) > 180 || r <= 0) die(`${what} : valeurs invalides « ${s} »`);
    const dLat = r / 111.32, dLon = r / (111.32 * Math.cos(lat * Math.PI / 180));
    return { bbox: [lon - dLon, lat - dLat, lon + dLon, lat + dLat], zooms: v[3] ? parseZooms(v[3], what) : DETAIL_ZOOMS, nom: what };
}

/* Plan : pour chaque niveau, les rectangles de tuiles demandés */
function makePlan(o, extentBbox) {
    const zones = [];
    if (o.zooms !== 'aucun') {
        if (!extentBbox) die('emprise de l\'extrait inconnue');
        zones.push({ bbox: extentBbox, zooms: parseZooms(o.zooms, '--zooms'), nom: 'extrait entier' });
    }
    o.details.forEach((s, i) => zones.push(parseDetail(s, `--detail n°${i + 1}`)));
    o.bboxes.forEach((s, i) => zones.push(parseBbox(s, `--bbox n°${i + 1}`)));
    if (!zones.length) die('rien à dessiner (--zooms aucun, sans --detail ni --bbox)');
    const plan = new Map();
    for (const zn of zones) {
        for (let z = zn.zooms[0]; z <= zn.zooms[1]; z++) {
            if (!plan.has(z)) plan.set(z, []);
            plan.get(z).push(tileRect(zn.bbox, z));
        }
    }
    return { zones, plan: new Map([...plan.entries()].sort((a, b) => a[0] - b[0])) };
}

function inRects(rects, x, y) {
    for (const r of rects) if (x >= r[0] && x <= r[2] && y >= r[1] && y <= r[3]) return true;
    return false;
}

/* blocs de META × META tuiles touchés au niveau z, avec leurs tuiles */
function metatiles(rects) {
    const seen = new Set(), out = [];
    for (const r of rects) {
        for (let my = Math.floor(r[1] / META); my <= Math.floor(r[3] / META); my++) {
            for (let mx = Math.floor(r[0] / META); mx <= Math.floor(r[2] / META); mx++) {
                const k = mx + ',' + my;
                if (seen.has(k)) continue;
                seen.add(k);
                out.push([mx, my]);
            }
        }
    }
    out.sort((a, b) => a[1] - b[1] || a[0] - b[0]);
    return out;
}

/* nombre de tuiles (union des rectangles) et de dossiers <z>/<x> */
function countZoom(rects) {
    let tiles = 0;
    const cols = new Set();
    let x0 = Infinity, x1 = -Infinity;
    for (const r of rects) { x0 = Math.min(x0, r[0]); x1 = Math.max(x1, r[2]); }
    for (let x = x0; x <= x1; x++) {
        const iv = rects.filter((r) => x >= r[0] && x <= r[2]).map((r) => [r[1], r[3]]).sort((a, b) => a[0] - b[0]);
        if (!iv.length) continue;
        cols.add(x);
        let [a, b] = iv[0];
        for (const [c, d] of iv.slice(1)) {
            if (c > b + 1) { tiles += b - a + 1; a = c; b = d; } else b = Math.max(b, d);
        }
        tiles += b - a + 1;
    }
    return { tiles, dirs: cols.size };
}

/* ----------------------------------------------------------- estimation */

/* Taille moyenne d'une tuile PNG palette avec le style montre (mesurée sur
 * des villes et des campagnes ; la mer et les zones vides pèsent ~200 o). */
function avgTileBytes(z) {
    if (z <= 6) return 14000;
    if (z <= 10) return 11000;
    if (z <= 13) return 9000;
    return 7000;
}

function human(n) {
    if (n >= 1024 ** 3) return (n / 1024 ** 3).toFixed(2).replace('.', ',') + ' Go';
    if (n >= 1024 ** 2) return Math.round(n / 1024 ** 2) + ' Mo';
    if (n >= 1024) return Math.round(n / 1024) + ' Ko';
    return n + ' o';
}

function fmtInt(n) { return n.toLocaleString('fr-FR').replace(/ | /g, ' '); }

function estimate(o, P) {
    const budget = o.libre !== null ? parseSize(o.libre, '--libre') : Math.floor(parseSize(o.taille, '--taille') * 0.9);
    console.log('\nZones demandées :');
    for (const zn of P.zones) {
        const b = zn.bbox.map((v) => v.toFixed(3)).join(', ');
        console.log(`  - ${zn.nom} : niveaux ${zn.zooms[0]} à ${zn.zooms[1]}  [${b}]`);
    }
    console.log('\nEstimation (un seul fichier tiles.pmtiles) :');
    console.log('  niveau      tuiles     taille');
    let tiles = 0, total = 0;
    for (const [z, rects] of P.plan) {
        const c = countZoom(rects);
        const sz = c.tiles * avgTileBytes(z);
        tiles += c.tiles;
        total += sz;
        console.log(`  ${String(z).padStart(6)}  ${fmtInt(c.tiles).padStart(10)}     ${human(sz).padStart(8)}`);
    }
    console.log(`  ${'total'.padStart(6)}  ${fmtInt(tiles).padStart(10)}     ${human(total).padStart(8)}`);
    console.log(`\nPlace disponible : ${human(budget)}` + (o.libre === null ? ` (90 % de ${o.taille}, le reste pour les logs et l'historique)` : ' (libre sur la carte SD)'));
    console.log('Maximum : la mer et les zones vides ne prennent presque rien (tuiles identiques stockées une fois).');
    if (total > budget) {
        console.log(`\n⚠  Trop gros : ≈ ${human(total)} pour ${human(budget)} disponibles.`);
        console.log('   Réduire --zooms (ex. 0-11), le rayon ou les niveaux des --detail, ou ajouter --forcer.');
        return { ok: o.forcer, tiles };
    }
    console.log('\nÇa rentre.');
    return { ok: true, tiles };
}

/* ------------------------------------------------------------- dessin */

function openMbtiles(file) {
    const sqlite3 = require(NM + 'sqlite3');
    return new Promise((res, rej) => {
        const db = new sqlite3.Database(file, sqlite3.OPEN_READONLY, (e) => (e ? rej(e) : res(db)));
    });
}

function dbGet(db, sql, args) {
    return new Promise((res, rej) => db.get(sql, args, (e, r) => (e ? rej(e) : res(r))));
}

async function mbtilesBounds(file) {
    const db = await openMbtiles(file);
    const r = await dbGet(db, 'SELECT value FROM metadata WHERE name = ?', ['bounds']);
    db.close();
    if (!r) return null;
    const b = r.value.split(',').map(Number);
    return b.length === 4 && !b.some(isNaN) ? b : null;
}

function makeMap(o, styleJson, db) {
    const mbgl = require(NM + '@maplibre/maplibre-gl-native');
    const map = new mbgl.Map({
        mode: 'static',
        ratio: 1,
        request: (req, cb) => {
            let done = false;
            const reply = (...a) => { if (!done) { done = true; cb(...a); } };
            try {
                const url = req.url;
                if (url.startsWith('mbtiles://')) {
                    const [z, x, y] = url.slice(10).split('/').map((v) => parseInt(v, 10));
                    db.get('SELECT tile_data FROM tiles WHERE zoom_level = ? AND tile_column = ? AND tile_row = ?',
                        [z, x, 2 ** z - 1 - y], (e, row) => {
                            if (e || !row) return reply();       /* absente : agrandir la parente */
                            let d = row.tile_data;
                            if (d[0] === 0x1f && d[1] === 0x8b) d = zlib.gunzipSync(d);
                            reply(null, { data: d });
                        });
                } else if (url.startsWith('fonts://')) {
                    const parts = url.slice(8).split('/');
                    const stack = decodeURIComponent(parts[0]).split(',')[0].trim();
                    const file = path.join(o.fonts, stack, path.basename(parts[1]));
                    fs.readFile(file, (e, d) => reply(null, { data: e ? Buffer.alloc(0) : d }));
                } else {
                    reply(new Error('ressource inattendue : ' + url));
                }
            } catch (e) {
                reply(e);
            }
        },
    });
    map.load(styleJson);
    return map;
}

function renderMap(map, opts) {
    return new Promise((res, rej) => map.render(opts, (e, buf) => (e ? rej(e) : res(buf))));
}

/* ancienne sortie partielle : journal des blocs finis et des tuiles unies */
function loadJournal(file, sig) {
    const done = new Set(), uni = new Map();
    if (!fs.existsSync(file)) return { done, uni };
    const lines = fs.readFileSync(file, 'utf8').split('\n');
    if (lines[0] !== 'v1 ' + sig) {
        console.log('Style ou réglages changés depuis la dernière fois : tout est redessiné.');
        return { done, uni };
    }
    for (const l of lines.slice(1)) {
        const p = l.split(' ');
        if (p[0] === 'M' && p.length === 4) done.add(p.slice(1).join('/'));
        else if (p[0] === 'U' && p.length === 6) uni.set(`${p[1]}/${p[2]}/${p[3]}`, { c: +p[4], d: +p[5] });
    }
    return { done, uni };
}

async function draw(o, P) {
    const sharp = require(NM + 'sharp');
    sharp.concurrency(1);
    const styleText = fs.readFileSync(o.style, 'utf8');
    const style = JSON.parse(styleText);
    for (const s of Object.values(style.sources)) {
        if (s.type === 'vector') { delete s.url; s.tiles = ['mbtiles://{z}/{x}/{y}']; s.maxzoom = s.maxzoom || 14; }
    }
    style.glyphs = 'fonts://{fontstack}/{range}.pbf';
    const styleJson = JSON.stringify(style);

    const out = path.join(o.sortie, 'tiles');
    fs.mkdirSync(out, { recursive: true });
    const sig = crypto.createHash('sha1').update(styleText + o.couleurs + META + MARGIN + fs.statSync(o.mbtiles).mtimeMs).digest('hex').slice(0, 12);
    const jfile = path.join(o.sortie, '.journal-tuiles');
    if (o.refaire && fs.existsSync(jfile)) fs.unlinkSync(jfile);
    const J = loadJournal(jfile, sig);
    if (!J.done.size) fs.writeFileSync(jfile, 'v1 ' + sig + '\n');
    const journal = fs.createWriteStream(jfile, { flags: 'a' });

    const db = await openMbtiles(o.mbtiles);
    const nmaps = Math.max(1, o.jobs);
    const maps = Array.from({ length: nmaps }, () => makeMap(o, styleJson, db));

    const stats = { written: 0, skipped: 0, bytes: 0, dirs: new Set() };
    const t0 = Date.now();
    for (const [z, rects] of P.plan) {
        const metas = metatiles(rects).filter(([mx, my]) => !J.done.has(`${z}/${mx}/${my}`));
        const total = countZoom(rects).tiles;
        if (!metas.length) { console.log(`niveau ${z} : déjà fait`); continue; }
        let done = 0, last = 0;
        const tz = Date.now();
        let next = 0;
        const worker = async (map) => {
            while (next < metas.length) {
                const [mx, my] = metas[next++];
                done += await drawMeta(map, sharp, o, z, mx, my, rects, J.uni, out, stats, journal);
                const now = Date.now();
                if (now - last > 2000 || done === total) {
                    last = now;
                    const rate = done / Math.max(0.001, (now - tz) / 1000);
                    const left = rate > 0 ? Math.round((total - done) / rate) : 0;
                    process.stdout.write(`\rniveau ${z} : ${fmtInt(done)} / ${fmtInt(total)} tuiles  (${rate.toFixed(0)}/s, reste ≈ ${fmtDur(left)})      `);
                }
            }
        };
        await Promise.all(maps.map(worker));
        process.stdout.write(`\rniveau ${z} : ${fmtInt(total)} tuiles en ${fmtDur(Math.round((Date.now() - tz) / 1000))}                          \n`);
    }
    journal.end();
    db.close();
    for (const m of maps) m.release();

    console.log(`\nDessin terminé en ${fmtDur(Math.round((Date.now() - t0) / 1000))} : ${fmtInt(stats.written)} tuiles écrites, ${fmtInt(stats.skipped)} tuiles unies sautées.`);
}

function fmtDur(s) {
    if (s < 60) return s + ' s';
    if (s < 3600) return Math.floor(s / 60) + ' min ' + String(s % 60).padStart(2, '0');
    return Math.floor(s / 3600) + ' h ' + String(Math.floor(s / 60) % 60).padStart(2, '0');
}

async function drawMeta(map, sharp, o, z, mx, my, rects, uni, out, stats, journal) {
    const n = 2 ** z;
    /* tuiles du bloc à faire (rectangle englobant pour le dessin) */
    const want = [];
    let tx0 = Infinity, ty0 = Infinity, tx1 = -1, ty1 = -1;
    for (let y = my * META; y < Math.min(n, (my + 1) * META); y++) {
        for (let x = mx * META; x < Math.min(n, (mx + 1) * META); x++) {
            if (!inRects(rects, x, y)) continue;
            want.push([x, y]);
            tx0 = Math.min(tx0, x); ty0 = Math.min(ty0, y); tx1 = Math.max(tx1, x); ty1 = Math.max(ty1, y);
        }
    }
    if (!want.length) return 0;
    const nx = tx1 - tx0 + 1, ny = ty1 - ty0 + 1;
    /* MapLibre compte en tuiles de 512 : le niveau z en 256 est son z - 1.
     * Le niveau 0 se dessine en double taille au niveau 0, puis réduit. */
    const k = z === 0 ? 2 : 1;
    const margin = z === 0 ? 0 : MARGIN;
    const W = nx * TILE + 2 * margin, H = ny * TILE + 2 * margin;
    const cx = tx0 + nx / 2, cy = ty0 + ny / 2;
    let buf = await renderMap(map, {
        zoom: Math.max(0, z - 1), center: [x2lon(cx, z), y2lat(cy, z)],
        width: W * k, height: H * k, bearing: 0, pitch: 0,
    });
    if (k !== 1) {
        buf = await sharp(buf, { raw: { width: W * k, height: H * k, channels: 4, premultiplied: true } })
            .resize(W, H, { kernel: 'lanczos3' }).raw().toBuffer();
    }
    const stride = W * 4;
    for (const [x, y] of want) {
        const ox = margin + (x - tx0) * TILE, oy = margin + (y - ty0) * TILE;
        const tile = Buffer.allocUnsafe(TILE * TILE * 4);
        for (let r = 0; r < TILE; r++) buf.copy(tile, r * TILE * 4, (oy + r) * stride + ox * 4, (oy + r) * stride + (ox + TILE) * 4);
        const col = uniformColor(tile);
        const key = `${z}/${x}/${y}`;
        if (col !== null) {
            const par = z > 0 ? uni.get(`${z - 1}/${x >> 1}/${y >> 1}`) : null;
            if (par && par.c === col && par.d + 1 <= UPSCALE_MAX) {
                uni.set(key, { c: col, d: par.d + 1 });
                journal.write(`U ${z} ${x} ${y} ${col} ${par.d + 1}\n`);
                const f = path.join(out, String(z), String(x), y + '.png');
                if (fs.existsSync(f)) fs.unlinkSync(f);           /* ancienne version */
                stats.skipped++;
                continue;
            }
            uni.set(key, { c: col, d: 0 });
            journal.write(`U ${z} ${x} ${y} ${col} 0\n`);
        }
        const dir = path.join(out, String(z), String(x));
        if (!stats.dirs.has(dir)) { fs.mkdirSync(dir, { recursive: true }); stats.dirs.add(dir); }
        const png = await sharp(tile, { raw: { width: TILE, height: TILE, channels: 4 } })
            .removeAlpha()
            .png({ palette: true, colors: col !== null ? 2 : o.couleurs, dither: 0, compressionLevel: 9, effort: 10 })
            .toBuffer();
        fs.writeFileSync(path.join(dir, y + '.png'), png);
        stats.written++;
        stats.bytes += png.length;
    }
    journal.write(`M ${z} ${mx} ${my}\n`);
    return want.length;
}

/* couleur 0xRRGGBB si toute la tuile est d'une seule couleur, sinon null */
function uniformColor(t) {
    const v = new Uint32Array(t.buffer, t.byteOffset, t.length / 4);
    const c = v[0];
    for (let i = 1; i < v.length; i++) if (v[i] !== c) return null;
    return ((t[0] << 16) | (t[1] << 8) | t[2]) >>> 0;
}

/* ------------------------------------------------------- PMTiles v3 */
/* Un seul fichier pour la carte SD (des milliers de petits fichiers s'y
 * copient très lentement). Spécification : github.com/protomaps/PMTiles,
 * même disposition que l'écrivain Python officiel : en-tête, répertoire
 * racine, métadonnées, répertoires feuilles, tuiles (rangées par numéro de
 * tuile, les tuiles identiques une seule fois). */

function zxyToTileId(z, x, y) {
    let acc = (4 ** z - 1) / 3;
    for (let a = z - 1; a >= 0; a--) {
        const s = 1 << a;
        const rx = (s & x) ? 1 : 0, ry = (s & y) ? 1 : 0;
        acc += s * s * ((3 * rx) ^ ry);
        if (!ry) {
            if (rx) { x = s - 1 - x; y = s - 1 - y; }
            const t = x; x = y; y = t;
        }
    }
    return acc;
}

function varint(arr, n) {
    while (n >= 128) { arr.push((n % 128) | 128); n = Math.floor(n / 128); }
    arr.push(n);
}

function serializeDir(ents) {
    const b = [];
    varint(b, ents.length);
    let last = 0;
    for (const e of ents) { varint(b, e.id - last); last = e.id; }
    for (const e of ents) varint(b, e.run);
    for (const e of ents) varint(b, e.len);
    ents.forEach((e, i) => {
        if (i > 0 && e.off === ents[i - 1].off + ents[i - 1].len) varint(b, 0);
        else varint(b, e.off + 1);
    });
    return zlib.gzipSync(Buffer.from(b), { level: 9 });
}

function buildDirs(ents, maxRoot) {
    const root = serializeDir(ents);
    if (root.length < maxRoot) return { root, leaves: Buffer.alloc(0) };
    for (let leaf = 4096; ; leaf *= 2) {
        const rootEnts = [], parts = [];
        let off = 0;
        for (let i = 0; i < ents.length; i += leaf) {
            const d = serializeDir(ents.slice(i, i + leaf));
            rootEnts.push({ id: ents[i].id, off, len: d.length, run: 0 });
            parts.push(d);
            off += d.length;
        }
        const r = serializeDir(rootEnts);
        if (r.length < maxRoot) return { root: r, leaves: Buffer.concat(parts) };
    }
}

function writePmtiles(o, P, file) {
    const dir = path.join(o.sortie, 'tiles');
    const list = [];
    for (const [z, rects] of P.plan) {
        const zd = path.join(dir, String(z));
        if (!fs.existsSync(zd)) continue;
        for (const xs of fs.readdirSync(zd)) {
            const x = parseInt(xs, 10);
            if (!(x >= 0)) continue;
            for (const ys of fs.readdirSync(path.join(zd, xs))) {
                const m = /^(\d+)\.png$/.exec(ys);
                if (!m || !inRects(rects, x, +m[1])) continue;
                list.push({ z, x, y: +m[1], id: zxyToTileId(z, x, +m[1]) });
            }
        }
    }
    if (!list.length) die('aucune tuile à mettre dans le fichier');
    list.sort((a, b) => a.id - b.id);

    const tmp = file + '.donnees';
    const fd = fs.openSync(tmp, 'w');
    const seen = new Map(), ents = [];
    let off = 0;
    for (const t of list) {
        const data = fs.readFileSync(path.join(dir, String(t.z), String(t.x), t.y + '.png'));
        const h = crypto.createHash('sha1').update(data).digest('hex');
        let at = seen.get(h);
        if (at === undefined) {
            fs.writeSync(fd, data, 0, data.length, off);
            at = off;
            seen.set(h, at);
            off += data.length;
        }
        const prev = ents[ents.length - 1];
        if (prev && prev.off === at && prev.id + prev.run === t.id) prev.run++;
        else ents.push({ id: t.id, off: at, len: data.length, run: 1 });
    }
    fs.closeSync(fd);

    const HEADER = 127;
    const { root, leaves } = buildDirs(ents, 16384 - HEADER);
    const meta = zlib.gzipSync(Buffer.from(JSON.stringify({
        name: 'T-Watch Ultra', format: 'png', type: 'baselayer',
        attribution: '© contributeurs OpenStreetMap, © OpenMapTiles',
        generator: 'tools/tuiles/generer-tuiles.sh',
    })), { level: 9 });

    let bb = null;
    for (const zn of P.zones) {
        const b = zn.bbox;
        bb = bb ? [Math.min(bb[0], b[0]), Math.min(bb[1], b[1]), Math.max(bb[2], b[2]), Math.max(bb[3], b[3])] : b.slice();
    }
    const det = P.zones.find((zn) => zn.nom !== 'extrait entier') || P.zones[0];
    const zs = [...P.plan.keys()];
    const e7 = (v) => Math.round(Math.max(-180, Math.min(180, v)) * 1e7);

    const h = Buffer.alloc(HEADER);
    h.write('PMTiles', 0, 'ascii');
    h[7] = 3;
    let p = 8;
    const u64 = (v) => { h.writeBigUInt64LE(BigInt(v), p); p += 8; };
    const rootOff = HEADER, metaOff = rootOff + root.length, leafOff = metaOff + meta.length, dataOff = leafOff + leaves.length;
    u64(rootOff); u64(root.length);
    u64(metaOff); u64(meta.length);
    u64(leafOff); u64(leaves.length);
    u64(dataOff); u64(off);
    u64(list.length);                     /* tuiles adressées */
    u64(ents.length);                     /* entrées */
    u64(seen.size);                       /* contenus différents */
    h[p++] = 1;                           /* rangées par numéro */
    h[p++] = 2;                           /* répertoires : gzip */
    h[p++] = 1;                           /* tuiles : sans compression (PNG) */
    h[p++] = 2;                           /* PNG */
    h[p++] = Math.min(...zs);
    h[p++] = Math.max(...zs);
    for (const v of [e7(bb[0]), e7(bb[1]), e7(bb[2]), e7(bb[3])]) { h.writeInt32LE(v, p); p += 4; }
    h[p++] = det.zooms[0];
    h.writeInt32LE(e7((det.bbox[0] + det.bbox[2]) / 2), p); p += 4;
    h.writeInt32LE(e7((det.bbox[1] + det.bbox[3]) / 2), p); p += 4;

    const part = file + '.part';
    const out = fs.openSync(part, 'w');
    fs.writeSync(out, Buffer.concat([h, root, meta, leaves]));
    const src = fs.openSync(tmp, 'r'), buf = Buffer.alloc(4 << 20);
    for (let n; (n = fs.readSync(src, buf, 0, buf.length, null)) > 0;) fs.writeSync(out, buf, 0, n);
    fs.closeSync(src);
    fs.closeSync(out);
    fs.unlinkSync(tmp);
    fs.renameSync(part, file);
    return { tiles: list.length, contents: seen.size, size: fs.statSync(file).size };
}

/* -------------------------------------------------------------- main */

(async () => {
    const o = parseArgs(process.argv.slice(2));
    let ext = null;
    if (o.pbf) {
        try { ext = pbfBounds(o.pbf); } catch (e) { die(`${o.pbf} : ${e.message}`); }
    }
    if (!ext && o.mbtiles && fs.existsSync(o.mbtiles)) ext = await mbtilesBounds(o.mbtiles);
    if (!ext && o.zooms !== 'aucun') die('l\'extrait n\'indique pas son emprise : donner les zones avec --bbox et --zooms aucun');
    const P = makePlan(o, ext);
    const est = estimate(o, P);
    if (o.cmd === 'estimer') process.exit(est.ok ? 0 : 3);
    if (o.cmd === 'dessiner') {
        if (!est.ok) process.exit(3);
        if (!o.mbtiles || !fs.existsSync(o.mbtiles)) die('fichier .mbtiles manquant');
        console.log('');
        await draw(o, P);
    }
    const file = path.join(o.sortie, 'tiles.pmtiles');
    process.stdout.write('\nÉcriture du fichier unique tiles.pmtiles… ');
    const r = writePmtiles(o, P, file);
    console.log(`${fmtInt(r.tiles)} tuiles (${fmtInt(r.contents)} différentes), ${human(r.size)}.`);
})().catch((e) => {
    console.error('\nErreur : ' + (e && e.stack || e));
    process.exit(1);
});
