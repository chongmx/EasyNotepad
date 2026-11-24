---
name: data-and-calculations
description: filter, sort, group and total tables (CSV, Excel) and calculate exactly
---

# Tables and numbers

## Never calculate in your head
Every number you work out - sum, difference, total, average, percentage, share, conversion,
rounding, even 2 + 3 - goes through `calculate`. Only counting a few things you see is yours.
The app checks your answer: a figure no tool gave you is sent back to you.
`calculate {"expression": "(4200 - 3150) / 3150 * 100"}` - percent change.
Several at once, named, each using the ones before (separated by `;`):
`calculate {"expression": "total = 42.50 * 1.08; each = total / 4; round(each, 2)"}`
Functions: sqrt abs round(x, places) floor ceil min max sum avg pow log ln exp, pi, e.
Take the figures from the tools' replies exactly as they are, then answer with what calculate
returns. Write figures as digits (18000, not "eighteen thousand").
Mind the units a heading gives: "Weight (thousand tonnes)" 4.2 is 4.2 * 1000 tonnes. A rate, a
share or a price per unit is worked out from the whole figures.

## Tables: query_table
1. See the columns first: `query_table {"path": "data/sales.csv", "limit": 3}`.
   For Excel it also lists the sheets; pick one with `"sheet": "Sales"` - only a name it listed.
   A year, a segment or a region is a value in a column: filter it with where (`Year = 2019`).
2. Pick rows: `"where": "Region = North and Quantity > 1000"` (also `,` or `&&` between conditions).
   Operators: = != > < >= <= contains. Text matches ignore case; numbers may have $ and commas.
3. Totals: `"aggregate": "sum(Sales), avg(Price), count"`, by group: `"group_by": "Channel"`.
4. Top rows: `"sort": "Sales desc", "limit": 5, "columns": "Region, Product, Sales"`.
5. Which group has the most (or least): group, total, and sort by the total -
   `"group_by": "Region", "aggregate": "sum(Sales)", "sort": "sum(Sales) desc", "limit": 1`.
   Do not take the largest single row for "the most overall".

Use the column names exactly as listed. If a call fails, the reply lists the columns: fix the name
and call again.

## Answering
Give the numbers with their units, say which rows they cover ("North, 2023, 140 rows"), and
show a small table in Markdown when it helps.
