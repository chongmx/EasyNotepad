---
name: multiple-documents
description: work across many documents in the folder - find them, ask each the same question, collect, compare and total
---

# Many documents

You can keep only a little in mind at once, so never read many documents into the conversation.
Work in passes, and keep what you find in your notes.

## 1. Find them
- `search_documents {}` lists every document in the folder: kind, pages, title. Start here when
  you do not know what is there.
- `search_documents {"query": "termination notice"}` says which documents and pages mention
  something, best first, with a snippet. Use the words the documents would use.
- `search_documents {"folder": "contracts", "query": "renewal"}` searches one folder only.

## 2. Ask each the same question: extract_from_documents
One call reads each document in a pass of its own (a long one part by part) and returns a table:
one row per document, with pages. The table also goes into your notes.
- `extract_from_documents {"files": "expenses", "question": "receipt number, date and total amount with currency"}`
- `extract_from_documents {"files": "contracts/*.pdf", "question": "end date and notice period"}`
- `extract_from_documents {"files": "q1.pdf, q2.pdf, q3.pdf", "question": "revenue and profit for the quarter"}`
Ask for exactly the facts you need, all in one question. "not in this document" means it was not
there: say so, do not fill it in. Up to 30 documents in one call; for more, call it per folder.

## Where in a long document: scan_document
For "where does it say..." in a long document (or a few), scan it: it shows you the document in
overlapping windows, one at a time, with what you look for, and returns the pages and the quotes.
- `scan_document {"path": "report.pdf", "find": "the notice period for ending the contract"}`
- `"window": "small"` (3,000 characters) for a needle in dense text, `"large"` for long passages;
  `"overlap"` (percent, default 20) so a sentence at an edge is whole in the next window.
Then read just those pages: `read_document {"path": "report.pdf", "from_page": 17, "to_page": 18}`,
or `look_at_page` when it is a table or a chart.
extract_from_documents answers one question per document; scan_document finds every place.

## 3. Keep what you found: notes
- `note {"text": "Contract B (contracts/b.pdf p.3): ends 2025-06-30, 3 months notice"}` for
  anything a later step needs. Your notes are shown to you at each step.
- `read_notes {}` reads them back.

## 4. Combine
- Totals, differences, averages, shares: `calculate`, with the figures from the table exactly as
  written: `calculate {"expression": "march = 1250.00; april = 980.50; total = march + april"}`.
- Which is biggest, earliest, missing: compare the table's rows; say which document each comes from.
- A table or figure that did not come through as text: `look_at_page` on the page the table names.

## 5. Answer
A short answer first, then a small Markdown table (document, the facts, page), then what could not
be found or read. Name every document a figure comes from.

## Examples
"What do the receipts in expenses/ add up to?"
1. `extract_from_documents {"files": "expenses", "question": "receipt number and total amount with currency"}`
2. `calculate` the sum of the totals (by currency if they differ).
3. Answer: the total, and the table of receipts.

"Which contracts end in 2025?"
1. `search_documents {"query": "end date term expiry", "folder": "contracts"}` (which ones say it)
2. `extract_from_documents {"files": "contracts", "question": "the end date of the contract"}`
3. Answer with the ones ending in 2025, and the rest.

"Summarise the three reports and how they differ"
1. `extract_from_documents {"files": "reports", "question": "the main conclusion and the key figures, in 3 lines"}`
2. Compare the rows; answer with a short summary of each and the differences.

One long document (a 60-page report): `extract_from_documents {"files": "report.pdf", "question": ...}`
reads it all part by part, where read_document would show only a few pages.
