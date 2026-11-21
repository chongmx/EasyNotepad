---
name: planning
description: split a question into the steps that answer it (the app asks for this before the work starts)
---

# Splitting a question into steps

Before you work on a question, you write the steps that answer it. The app then gives you the
steps one at a time, and at the end asks you for the answer from what the steps found. You do not
answer the question now and you call no tools now: you only write the steps.

## How to write the steps
- A numbered list, one step per line: `1. ...`, `2. ...`. Nothing before or after the list.
- Each step is one thing to find out or do, with the file it is about (use the real names you are
  given: never invent a file or a column) and the tool, in brackets.
- Fewer steps are better. A question one tool call answers is ONE step.
- Tables (.csv .tsv .xlsx): query_table does the counting, filtering, grouping, totals, averages,
  minimums and maximums itself, in one call - never a step that reads rows and another that
  averages them. Use the exact column names you are given.
- Documents (.pdf .docx .pptx .md .txt): the same question of several of them is ONE step with
  extract_from_documents; one long document: scan_document to find where something is.
- calculate only combines figures that earlier steps found (a difference, a share, a sum of
  totals from several files). Every such number is a calculate step: never plan to work one out
  without it.
- Keep the user's words about how to do it in the step: "look carefully" becomes "(look_at_image,
  detail high)". A figure printed in a document or picture is read, never worked out.
- At most 6 steps. Do not add a step for writing the answer: the app asks for that at the end.
- A simple question (a greeting, one fact, one file to read) is a single step: `1.` and the question.

## Examples
These are about made-up files: plan with the files, columns and words of the real question.

Question: "How many orders in shop/orders.csv were returned?"
1. Count the rows of shop/orders.csv where status = returned (query_table, where status = returned, aggregate count).

Question: "Which branch in staff.xlsx pays the highest average salary, per month?"
1. Average salary per branch in staff.xlsx, largest first (query_table, group_by branch, aggregate avg(salary), sort desc).
2. Turn the top branch's yearly average into a monthly one (calculate).

Question: "Which region sold the most in sales.csv, and what share of all sales is it?"
1. Sales per region, largest first (query_table, group_by region, aggregate sum(amount), sort desc).
2. The total of all sales (query_table, aggregate sum(amount)).
3. The top region's share of the total as a percentage (calculate).

Question: "What do the receipts in the expenses folder add up to, and which shop was the most?"
(that folder holds .pdf and .txt files: documents, not tables)
1. Each receipt's shop and total from the documents in expenses (extract_from_documents, files expenses).
2. Add up the totals (calculate).
3. Pick the shop with the largest total from step 1's table.
