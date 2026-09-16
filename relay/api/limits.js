import { put, head } from '@vercel/blob';

// Relay between the Mac and the e-reader.
//
// The reader has no account with any AI provider and cannot ask one how much
// quota is left. The Mac can (CodexBar already knows), but the two are rarely
// on the same network and never at a stable address. So the Mac PUSHes a
// snapshot here whenever it can, and the reader GETs it from wherever it is.
//
// Deliberately last-write-wins with a single slot: this is a dashboard, not a
// log, and only the latest reading is ever displayed.

// The store is public (this SDK version cannot read a private blob), so the
// object's own URL carries a secret segment. The GET below is still key-gated;
// this only stops the raw object being reachable by guessing the path.
const BLOB_PATH = `agent-limits/${process.env.BLOB_PATH_SECRET || 'unset'}/latest.json`;
const MAX_BODY_BYTES = 16 * 1024;

// "CodexBar · live" as pushed, plus how long ago that push actually happened.
function withAge(subtitle, pushedAt) {
  const base = (subtitle || '').replace(/\s*·\s*\d+[smhd].*$/, '').trim() || 'CodexBar';
  const pushed = Date.parse(pushedAt || '');
  if (!pushed) return base;

  const seconds = Math.max(0, Math.round((Date.now() - pushed) / 1000));
  if (seconds < 120) return `${base} · live`;
  const minutes = Math.round(seconds / 60);
  if (minutes < 60) return `${base} · ${minutes}m old`;
  const hours = Math.round(minutes / 60);
  if (hours < 48) return `${base} · ${hours}h old`;
  return `${base} · ${Math.round(hours / 24)}d old`;
}

function unauthorized(res) {
  // Same response for a missing and a wrong credential, so the endpoint cannot
  // be probed to learn which part was right.
  res.status(401).json({ error: 'unauthorized' });
}

export default async function handler(req, res) {
  res.setHeader('Cache-Control', 'no-store');

  if (req.method === 'POST') {
    const token = (req.headers.authorization || '').replace(/^Bearer\s+/i, '');
    if (!process.env.PUSH_TOKEN || token !== process.env.PUSH_TOKEN) return unauthorized(res);

    const body = typeof req.body === 'string' ? req.body : JSON.stringify(req.body ?? {});
    if (body.length > MAX_BODY_BYTES) return res.status(413).json({ error: 'too large' });

    let parsed;
    try {
      parsed = JSON.parse(body);
    } catch {
      return res.status(400).json({ error: 'invalid json' });
    }
    if (!Array.isArray(parsed.limits)) return res.status(400).json({ error: 'missing limits array' });

    // pushedAt lets the reader show how stale the numbers are even when the
    // Mac has been asleep for a day.
    parsed.pushedAt = new Date().toISOString();

    await put(BLOB_PATH, JSON.stringify(parsed), {
      access: 'public',
      addRandomSuffix: false,
      allowOverwrite: true,
      contentType: 'application/json',
      cacheControlMaxAge: 0,
    });
    return res.status(200).json({ ok: true, pushedAt: parsed.pushedAt });
  }

  if (req.method === 'GET') {
    // The reader cannot set headers, so its key travels in the query string.
    const key = req.query.k || '';
    if (!process.env.READ_KEY || key !== process.env.READ_KEY) return unauthorized(res);

    let meta;
    try {
      meta = await head(BLOB_PATH);
    } catch {
      return res.status(200).json({ subtitle: 'No data pushed yet', limits: [] });
    }

    const upstream = await fetch(meta.url, { cache: 'no-store' });
    if (!upstream.ok) return res.status(502).json({ error: 'store unavailable' });

    const snapshot = await upstream.json();
    // The reader has no reliable clock, and the Mac may have been asleep for a
    // day. Stamping the age here is the only place it can be computed honestly.
    snapshot.subtitle = withAge(snapshot.subtitle, snapshot.pushedAt);
    return res.status(200).json(snapshot);
  }

  res.setHeader('Allow', 'GET, POST');
  return res.status(405).json({ error: 'method not allowed' });
}
