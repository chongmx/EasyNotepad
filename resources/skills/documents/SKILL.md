---
name: documents
description: read PDF, Word, Excel and PowerPoint files - their text by page, and their pages as pictures
---

# Reading documents

## Text first
`read_document {"path": "report.pdf"}` gives pages 1-3 and says how many pages there are. Read on
with `from_page` / `to_page`, a few pages at a time; stop as soon as you have the answer.
- PDF, PowerPoint (slides) and Word laid out as pages: by page.
- Excel and Word without pages: by part (`from_page` is the part).
- A long document: read page 1 (title, contents), then jump to the pages the contents name.
- A question about a word or number: `search_files` does not look inside documents, so read the
  pages, or look at them.

## When the text is not enough: look at the page
Tables, charts, drawings, diagrams, scanned pages and layout lose their meaning as text. Then
`look_at_page {"path": "report.pdf", "page": 4}` shows the page in the viewer and gives you a
picture of it. For small print, zoom and take a region:
`look_at_page {"path": "report.pdf", "page": 4, "zoom": 200, "region": "top"}`.
See the pictures-and-sound skill for zooming well.

## Locked documents
A PDF with a password cannot be read until the user opens it in the viewer and types the password.
Say so; after that, `look_at_page` sees its pages.

## Answering
Name the pages you used ("page 12", "slide 3", "sheet Sales"). Quote numbers exactly as the
document has them. If the document does not say, say it does not.
