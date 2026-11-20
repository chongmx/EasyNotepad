---
name: pictures-and-sound
description: look at pictures and pages (how carefully - low, normal, high detail and OCR), and listen
---

# Seeing and hearing

You can only see and hear when your eyes and ears are downloaded (the guide says). If not, tell the
user they can download them in the agent set-up (the gear), and use text tools meanwhile.

## How carefully to look: detail
Each look costs tokens (room in your memory for this conversation). Choose:
- `"detail": "low"` (about 70 tokens) - what kind of picture it is, the layout, colours, a chart's
  shape; a zoomed-in region of a page (it is already large).
- `"detail": "normal"` (about 280, the default) - most questions about a picture or a page.
- `"detail": "high"` (about 1100) - reading text (OCR), small print, receipts, forms, tables of
  numbers, handwriting, fine details; whenever the user says "look carefully" or "read exactly".
If an answer is unsure at a lower detail, look again with more.

## Pictures
`look_at_image {"path": "photos/board.jpg", "detail": "normal"}`. Describe what is there; copy text
exactly; say when something is too small or unclear instead of guessing.

## Pages of documents
`look_at_page` shows a page in the viewer as the user sees it, and gives you that picture.
- `zoom`: 0 fits the page to the width (the default); 200 or 400 for small print, with a `region`.
- `region`: all, top, bottom, left, right, top-left, top-right, bottom-left, bottom-right, center.
- Coarse to fine: the whole page at low or normal detail, then zoom on the part that matters.
  A zoomed region often needs only low or normal detail.

Example - the numbers in a chart at the bottom of page 7:
1. `look_at_page {"path": "q3.pdf", "page": 7, "detail": "low"}` (where is the chart?)
2. `look_at_page {"path": "q3.pdf", "page": 7, "zoom": 200, "region": "bottom", "detail": "high"}`

## Recordings
`listen_to {"path": "voicemail.wav"}` for a short clip (up to about 30 seconds). For meetings and
anything longer, use `transcribe` (see the meetings skill).

## Showing pictures to the user
- What you look at with look_at_image and look_at_page is shown to the user under the tool's step.
- `show_image {"path": "photos/board.jpg"}` puts a picture in the chat when it helps your answer;
  you can also write `![caption](path)` in your answer.
- Tools that make pictures (later: drawing, segmenting, pose estimation) save them in the app's
  `temp/agent-images` folder and give you the path: show it with show_image or ![...](path).
