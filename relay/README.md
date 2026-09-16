# Agent limits relay

One Vercel function. The Mac POSTs a usage snapshot, the reader GETs it.

It exists because the two devices are almost never on the same network and the
Mac has no fixed address, so the reader cannot call it directly. Neither side
needs to know where the other is — both just reach this.

## Deploy

    npm install
    npx vercel link
    npx vercel blob create-store <name> --access public --yes
    npx vercel deploy --prod

Then set three environment variables on the project (any long random strings):

    PUSH_TOKEN         the Mac sends this to write
    READ_KEY           the reader sends this to read, in the query string
    BLOB_PATH_SECRET   random segment in the stored object's path

`BLOB_READ_WRITE_TOKEN` is added automatically when the Blob store is created.

## Endpoints

    POST /api/limits          Authorization: Bearer $PUSH_TOKEN
    GET  /api/limits?k=$READ_KEY

`GET` returns the stored snapshot with its `subtitle` restamped with how long
ago the push happened, so the reader can show a stale reading honestly rather
than presenting day-old numbers as current.

Single slot, last write wins: this is a dashboard, not a log.
