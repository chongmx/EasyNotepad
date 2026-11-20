---
name: pictures-and-sound
description: look at pictures and pages closely (zoom, regions), and listen to recordings
---

# Seeing and hearing

You can only see and hear when your eyes and ears are downloaded (the guide says). If not, tell the
user they can download them in the agent set-up (the gear), and use text tools meanwhile.

## Pictures
`look_at_image {"path": "photos/board.jpg"}` - you see the picture, shrunk to at most 1024 pixels
on its longest side. Describe what is there; read text in it carefully; say when something is too
small or unclear instead of guessing.

## Pages of documents
`look_at_page` shows a page in the viewer as the user would see it, and gives you that picture.
- `zoom`: 0 fits the page to the width (the default, good for the layout); 200 or 400 for small
  print. At a high zoom only part of the page is in view, so pair it with a `region`.
- `region`: all, top, bottom, left, right, top-left, top-right, bottom-left, bottom-right, center.
  A region is that part of the viewer, so its picture is sharper than the whole.
- Work from coarse to fine: first the whole page at zoom 0, then zoom on the part that matters.

Example - the numbers in a chart at the bottom of page 7:
1. `look_at_page {"path": "q3.pdf", "page": 7}` (where is the chart?)
2. `look_at_page {"path": "q3.pdf", "page": 7, "zoom": 200, "region": "bottom"}`

## Recordings
`listen_to {"path": "meeting.wav"}` - WAV, MP3 or FLAC, up to 25 MB. You can transcribe speech,
summarise it, or describe music and sounds. For long recordings say you heard the start only if
the answer stops making sense.
