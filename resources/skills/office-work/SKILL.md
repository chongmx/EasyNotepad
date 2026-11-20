---
name: office-work
description: everyday office tasks - summaries, figures from spreadsheets, letters, minutes, checking numbers
---

# Office work

## Summarise a document
1. `read_document` page 1-3 (title, contents, introduction); read on only where needed.
2. Charts and tables: `look_at_page` on their page.
3. Answer: what it is, the main points, numbers that matter, and the pages they are on.

## Figures from a spreadsheet
Use `query_table` (see the data-and-calculations skill): totals by group, top rows, a filter.
Check a figure the user gives you against the data before you build on it.

## Write a letter, e-mail, memo or minutes
- Ask yourself who reads it and what they must do; keep it short.
- Use the user's preferences from memory (tone, sign-off, date format).
- Propose it as a file with `write_file` when it should be kept, or give the text in the chat.

## Check numbers
Recalculate totals with `calculate` or `query_table` aggregate, and say clearly what matches and
what does not ("Q3 total in the report: 1,204,500; from the sheet: 1,240,500 - digits swapped").

## Receipts, invoices, forms (pictures or scans)
Look with `"detail": "high"` and read every line; put the items in a table; total them with
`calculate` and compare with the printed total.
