# Local agent questions

27 questions with known answers for the local agent (the offline model in the agent panel), over
the small sample set in `C:\cpp\agent-samples` (outside the repository: licences and sources in its
`SOURCES.md`). They cover what the agent is for: tables (CSV and Excel), sums, PDF documents,
photographed receipts (OCR), speech, a meeting transcript, a DLL's exports and imports, the
folder itself, and four invoices to work across (`invoices/`). Most need several tool calls, and a third need several steps (a lookup, another
lookup, then `calculate`).

| File | What |
|---|---|
| `questions.json` | the questions and how each answer is checked (numbers within a tolerance, phrases, tools that must be called) |
| `compute_answers.py` | works out the table answers from the data with the Python standard library, not with the app's `query_table` |
| `run.ps1` | asks each question in the running app (automation endpoint), grades the final answer, writes `results/<time>.md` and `.json` |

Running: start EasyNotepad, set the agent to Local (with the model downloaded), then

```powershell
powershell -ExecutionPolicy Bypass -File tests\agent-bench\run.ps1            # all
powershell -ExecutionPolicy Bypass -File tests\agent-bench\run.ps1 -Kind table
powershell -ExecutionPolicy Bypass -File tests\agent-bench\run.ps1 -Only receipt-vat,pdf-capital -Repeat 3
```

It opens the data folder as the workspace (it stops if documents are unsaved) and starts a new chat
for each question. A small model's answers vary from run to run: use `-Repeat` before reading much
into one result.

Two more sets, made by a script from their own made-up data (for measuring only, never for
tuning): `holdout/` (25 questions: tables, documents, a log, an HTML export, code) and `reallife/`
(23 questions over one person's messy folder: downloads with duplicates, an inbox of e-mails,
recordings of a call, a voicemail and a voice memo spoken by Windows' voices, photos of a receipt
and a parking fine, an expenses workbook with messy categories, a bank export, a calendar, two
versions of a price list, a proposal, a gym contract, a flight booking; asked as people ask, some
across several files). Make them with `python3 holdout/make.py`, and `python3 reallife/make.py`
then `reallife/make-media.ps1`; run with `-QuestionFile <set>/questions.json -Data <set>/data`.

Public benchmarks for the same kinds of work, for later: GAIA (multi-step questions with attached
spreadsheets, PDFs, pictures and audio; gated on Hugging Face), the Berkeley Function Calling
Leaderboard (tool-call syntax), tau-bench (tool use in conversations), OfficeBench (office tasks),
WikiTableQuestions and TabMWP (questions over tables), DocVQA and ChartQA (documents and charts as
pictures). None fits as it is: they need web access, another tool set or an answer format of their
own, so these questions borrow their kinds of questions on local files instead.
