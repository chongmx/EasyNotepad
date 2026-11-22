---
name: pictures-and-sound
description: look at pictures and pages (how carefully, and zooming into a part: OCR, small print), and listen
---

# Seeing and hearing

You can only see and hear when your eyes and ears are downloaded (the guide says). If not, tell the
user they can download them in the agent set-up (the gear), and use text tools meanwhile.

## How carefully to look: detail
Each look costs tokens (room in your memory for this conversation). Choose:
- `"detail": "low"` (about 70 tokens) - what kind of picture it is, the layout, colours, a chart's shape.
- `"detail": "normal"` (about 280, the default) - most questions about a picture or a page.
- `"detail": "high"` (about 1100) - reading text (OCR), small print, receipts, forms, tables of
  numbers, handwriting, fine details; whenever the user says "look carefully" or "read exactly".
- Or a number of tokens: `"detail": "600"`.
The user may have set a minimum: then every look is at least that careful.
If an answer is unsure at a lower detail, look again with more, or zoom in.

## Zooming into a part
Both `look_at_image` and `look_at_page` take the same three arguments:
- `region`: the part - `top`, `bottom`, `left`, `right`, `top-left`, `top-right`, `bottom-left`,
  `bottom-right`, `center` (a half), with `third` or `quarter` for less: `"bottom third"`,
  `"top-right quarter"`, `"middle-left third"`. Or a box in percent of the whole,
  `"left,top,right,bottom"`: `"55,60,100,80"` is the right part, 60 to 80% of the way down.
- `zoom`: percent magnification of that part: 200 its middle half, 400 a quarter of it across.
- `at`: with zoom, the point to zoom around, `"x,y"` in percent of the whole: `"70,85"`.
A part you zoom into is given to you with the picture's own detail (a page is zoomed in the viewer),
so small text becomes readable. The reply says which part you saw ("55-100% across, 60-80% down").

Coarse to fine: look at the whole at low or normal detail, find where the thing is, then zoom on
that part at high detail. Example - the VAT line near the bottom of a receipt:
1. `look_at_image {"path": "receipt.jpg"}` (the layout: the totals are 55-70% down)
2. `look_at_image {"path": "receipt.jpg", "region": "0,55,100,70", "detail": "high"}`

## Receipts, invoices, order lists
Read every line at high detail: its quantity, item and amount. Then use calculate for both totals,
in one call: the number of items is the sum of the quantities (a line "3x Pens" is three items),
and the amounts' sum is compared with the printed total
(`items = 3 + 1 + 2; lines = 7.50 + 12.00 + 8.40`). Never count them in your head.

## Finding a small thing: scan_image
When you do not know where in a picture something is (a stamp, a signature, a serial number, one
label on a big drawing), scan it: it shows you the picture in overlapping tiles, one at a time,
looks again finer where you saw it, and returns the regions.
- `scan_image {"path": "site-photo.jpg", "find": "the serial number plate"}`
- `"grid": 3` for a busy picture (3 x 3 tiles first), `"levels": 3` to go finer, `"detail": "high"`
  for small print in each tile.
Then read it: `look_at_image {"path": "site-photo.jpg", "region": "<the region it gave>", "detail": "high"}`.

## Pages of documents
`look_at_page {"path": "q3.pdf", "page": 7}` shows the page in the viewer as the user sees it, whole,
and gives you that picture. The numbers in a chart at the bottom of page 7:
`look_at_page {"path": "q3.pdf", "page": 7, "region": "bottom", "detail": "high"}`.

## Recordings
`listen_to {"path": "voicemail.wav"}` for a short clip (up to about 30 seconds). For meetings and
anything longer, use `transcribe` (see the meetings skill).

## Showing pictures to the user
- What you look at with look_at_image and look_at_page is shown to the user under the tool's step
  (a part you zoomed into, as you saw it).
- `show_image {"path": "photos/board.jpg"}` puts a picture in the chat when it helps your answer;
  you can also write `![caption](path)` in your answer.
- Tools that make pictures (later: drawing, segmenting, pose estimation) save them in the app's
  `temp/agent-images` folder and give you the path: show it with show_image or ![...](path).

Pictures it reads: PNG, JPEG, GIF, BMP, TIFF (also scanned black-and-white fax pages), WebP, TGA,
QOI, ICO, PNM, Radiance HDR.
