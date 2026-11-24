---
name: planning
description: split a question into the steps that answer it (the app asks for this before the work starts)
---

# Splitting a question into steps

Before you work on a question, you write down what to keep in mind and the steps that answer it.
The app keeps what you write in the task's memory and gives you, with each step, that memory and
the step alone (not everything the earlier steps read), and at the end asks you for the answer
from the memory. You do not answer the question now and you call no tools now.

## What to keep in mind
First `Keep in mind:` and, one per line (`- ...`), each detail of the question a step could lose:
- every value it picks by (a name, a kind, a place, a date: "red", "Lisbon", "in May"), each one;
- the files it names, and the unit or form the answer must have ("in kilograms", "per month",
  "as a percentage");
- each thing it asks for, in its words ("which species", "how many more").
Two to six short lines; only what the question says, nothing you guess.

## How to write the steps
- Then `Steps:` and a numbered list, one step per line: `1. ...`, `2. ...`. Nothing after it.
- Each step is one thing to find out or do, with the file it is about (use the real names you are
  given: never invent a file or a column) and the tool, in brackets.
- End each step with the skills it needs, from the list you are given: `[skills: meetings]`, at
  most two. You get each skill's instructions with its step, so name the one for that kind of
  work (tables: data-and-calculations; pictures and pages: pictures-and-sound; several documents:
  multiple-documents). A step that needs none ends without it.
- Fewer steps are better. A question one tool call answers is ONE step.
- Tables (.csv .tsv .xlsx): query_table does the counting, filtering, grouping, totals, averages,
  minimums and maximums itself, in one call - never a step that reads rows and another that
  averages them. Use the exact column names you are given. "Which X has the most Y" (sold the
  most, earned the most) is the total per X: group_by X, sum(Y), sort desc, limit 1 - never
  max(Y), which is only the largest single row. Write it as "the total of Y per X", never "the maximum".
- Documents (.pdf .docx .pptx .md .txt): the same question of several of them is ONE step with
  extract_from_documents; one long document: scan_document to find where something is. One short
  file (notes, a transcript) is read whole: read_file, or read_document for .pdf .docx .pptx. A long
  file, or one the other tools cannot make sense of (a log, an export), when every instance or an exact
  figure matters: ONE step with read_closely and the objective [skills: close-reading].
- A picture or page: everything to read from it is ONE step (one careful look reads every line);
  the sums and comparisons of what it shows are one calculate step after it.
- calculate only combines figures that earlier steps found (a difference, a share, a sum of
  totals from several files). Every such number is a calculate step: never plan to work one out
  without it. A calculation repeated over rounds (interest for several years, a price cut twice,
  days between two dates) is ONE calculate step, written whole: "1800 * 1.02^6",
  "9 November 2025 - 2 August 2025" - never a step per year.
- Every count is a tool's: rows of a table (query_table, aggregate count), lines of a file or
  log (search_files with the word, and the other words in also: it counts all of them), files in
  a folder (list_files: its "In all" line). Never plan to count by reading.
- Name what each step finds in the question's own words: "how many seats are still free" stays
  "how many are free", never "how many seats there are". A question asking three things has a step (or a
  part of one) for each.
- Keep the user's words about how to do it in the step: "look carefully" becomes "(look_at_image,
  detail high)". A figure printed in a document or picture is read, never worked out.
- At most 6 steps. Do not add a step for writing the answer: the app asks for that at the end.
- A simple question (a greeting, one fact, one file to read) is a single step: `1.` and the question.

## Examples
These are about made-up files: plan with the files, columns and words of the real question.

Question: "How many orders in shop/orders.csv were returned?"
Keep in mind:
- only orders whose status is returned
- the answer is a count
Steps:
1. Count the rows of shop/orders.csv where status = returned (query_table, where status = returned, aggregate count) [skills: data-and-calculations]

Question: "Which branch in staff.xlsx pays the highest average salary, per month?"
Keep in mind:
- the branch with the highest average salary, by name
- the answer per month, not per year
Steps:
1. Average salary per branch in staff.xlsx, largest first (query_table, group_by branch, aggregate avg(salary), sort desc) [skills: data-and-calculations]
2. Turn the top branch's yearly average into a monthly one (calculate).

Question: "Which region sold the most in sales.csv, and what share of all sales is it?"
Keep in mind:
- which region: its name and its total
- its share of all sales, as a percentage
Steps:
1. Sales per region, largest first (query_table, group_by region, aggregate sum(amount), sort desc) [skills: data-and-calculations]
2. The total of all sales (query_table, aggregate sum(amount)).
3. The top region's share of the total as a percentage (calculate).

Question: "How many hours did Ana work in the week on scans/timesheet.png, and is her weekly total right?"
Keep in mind:
- Ana's rows only, every day of the week
- whether the weekly total written for her equals her days added up
Steps:
1. Read Ana's hours for each day and the weekly total written for her on scans/timesheet.png (look_at_image, detail high) [skills: pictures-and-sound]
2. Add up her days and compare that with the written total (calculate).

Question: "What do the receipts in the expenses folder add up to, and which shop was the most?"
(that folder holds .pdf and .txt files: documents, not tables)
Keep in mind:
- every receipt in the expenses folder
- the sum of all totals, and the shop with the largest total, by name
Steps:
1. Each receipt's shop and total from the documents in expenses (extract_from_documents, files expenses) [skills: multiple-documents]
2. Add up the totals (calculate).
3. Pick the shop with the largest total from step 1's table.
