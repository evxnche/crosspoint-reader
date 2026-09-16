# PDF Reading

CrossPoint opens `.pdf` files as books. Put one on the SD card next to your
EPUBs and it appears in the library and the file browser like any other title.

## What actually happens

A PDF has no paragraphs. It is a list of glyphs at absolute positions on a
fixed-size page — which is why shrinking an A4 page to a 4.3" screen is
unreadable no matter whose firmware draws it.

So CrossPoint does not draw the page. It reads the text out of it and rebuilds
the structure from geometry:

1. Runs sharing a baseline join into a line.
2. Consecutive lines at the document's own leading join into a paragraph.
3. Each paragraph is written out as **one long line**.

The reader then wraps that paragraph to your font, size, and margins, exactly as
it wraps an EPUB. That is the whole point: a reflowed PDF is an ordinary book, a
rendered one is a postage stamp.

The result is cached at `/.crosspoint/pdf_<hash>/text.txt`, so the parse happens
once. Page turns after that cost no more than a plain text file.

### Where paragraph breaks come from

No single signal is reliable across producers, so several are combined:

| Signal | Meaning |
|---|---|
| Vertical gap larger than the paragraph's own leading | Extra space between blocks |
| Type size change | A heading is not a continuation of the text above it |
| First-line indent | The classic typeset paragraph marker |
| Baseline moving *up* | A new column, or a new block |
| A line containing only spaces | The layout's own blank line |

The leading is measured from the paragraph in hand rather than assumed from the
font size: books set leading anywhere from 1.1 to 1.6 em, and a fixed guess
either splits every line or never splits at all.

### Headers, footers, and hyphens

Running heads and page numbers are dropped: short runs pinned to the top or
bottom 5.5% of the page would otherwise reflow into the middle of a sentence. A
word split across a line break by a hyphen is rejoined.

## First open

Opening a PDF for the first time shows a progress bar while the document is
parsed. Expect roughly a second per ten pages — a 200-page book takes about
20 seconds. Every open after that is instant.

Re-extraction happens only if the file itself changes (its size or timestamp).

## What is supported

- Classic cross-reference tables, cross-reference streams, and object streams
- `FlateDecode`, `LZWDecode`, `ASCIIHexDecode`, `ASCII85Decode`, `RunLengthDecode`,
  with PNG and TIFF predictors
- `/ToUnicode` CMaps, WinAnsi and Standard encodings, `/Differences` arrays
- Type0/CID fonts with two-byte codes
- Form XObjects (text inside them is body text in many generators)
- Damaged files: if the cross-reference table is unusable, the whole file is
  scanned for objects instead, which recovers most truncated downloads

## What is not

**Encrypted PDFs are refused.** Most carry only an owner password and restrict
printing rather than reading, so they could in principle be opened — but that
needs RC4, AES, and MD5/SHA-2 on a device with 380KB of RAM, and a half-built
decryptor that silently produces mojibake is worse than a clear refusal. Strip
the encryption on a computer first.

**Scanned PDFs have no text to read.** Pages that are photographs of paper carry
no text layer; there is nothing to extract and nothing to reflow. CrossPoint
detects this and says so rather than opening a blank book. Reading them needs
OCR, which does not fit on this hardware.

**Images and page layout are dropped.** Tables, figures, sidebars, and
multi-column layouts come through as running text. A PDF's visual design does
not survive reflow, and on a 4.3" screen it could not have survived anyway.

## When the result looks wrong

Delete the cache and let it re-parse:

    rm -rf /path/to/sd/.crosspoint/pdf_*

If a specific document extracts badly, the fastest fix is still to convert it to
EPUB on a computer (Calibre does this well) — a real EPUB always beats a
reconstructed one.
