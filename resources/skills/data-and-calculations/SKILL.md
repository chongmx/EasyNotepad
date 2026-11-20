---
name: data-and-calculations
description: filter, sort, group and total tables (CSV, Excel) and calculate exactly
---

# Tables and numbers

## Never calculate in your head
Every sum, difference, percentage, average or conversion goes through `calculate`:
`calculate {"expression": "(4200 - 3150) / 3150 * 100"}` - percent change.
Functions: sqrt abs round(x, places) floor ceil min max sum avg pow log ln exp, pi, e.

## Tables: query_table
1. See the columns first: `query_table {"path": "data/sales.csv", "limit": 3}`.
   For Excel it also lists the sheets; pick one with `"sheet": "Sales"`.
2. Pick rows: `"where": "Country = Canada and Units Sold > 1000"`.
   Operators: = != > < >= <= contains. Text matches ignore case; numbers may have $ and commas.
3. Totals: `"aggregate": "sum(Profit), avg(Discount), count"`, by group: `"group_by": "Segment"`.
4. Top rows: `"sort": "Profit desc", "limit": 5, "columns": "Country, Product, Profit"`.
5. Which group has the most (or least): group, total, and sort by the total -
   `"group_by": "Country", "aggregate": "sum(Profit)", "sort": "sum(Profit) desc", "limit": 1`.
   Do not take the largest single row for "the most overall".

Use the column names exactly as listed. If a call fails, the reply lists the columns: fix the name
and call again.

## Answering
Give the numbers with their units, say which rows they cover ("Canada, 2014, 140 rows"), and
show a small table in Markdown when it helps.
