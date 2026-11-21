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
- Each step is one thing to find out or do, small enough for one or two tool calls, and says
  which file it is about and what to get from it ("the profit of each country in
  data/Financial Sample.xlsx").
- A question with two parts gets a step for each part. A figure the answer needs gets its own
  step: never leave a number to be guessed.
- Keep the user's words about how to do it in the step that does it: "look carefully" or "read
  the small print" becomes "(look_at_image, detail high)".
- A figure printed in a document or picture (a total, a tax, a date) is read from it, never worked
  out: "read the VAT printed on the receipt".
- Sums, differences and percentages are a step of their own (with `calculate`).
- Put the steps in order: a step may use what the steps before it found.
- At most 6 steps. Do not add a step for writing the answer: the app asks for that at the end.
- A simple question (a greeting, one fact, one file to read) is a single step: write it as `1.`
  followed by the question.

## Example
Question: "Which country made the most profit in Financial Sample.xlsx, and how much is that
of the total profit?"

1. Find the profit of each country in Financial Sample.xlsx, largest first (query_table, group by Country).
2. Find the total profit of all rows in Financial Sample.xlsx (query_table, sum of Profit).
3. Work out the top country's share of the total profit as a percentage (calculate).
