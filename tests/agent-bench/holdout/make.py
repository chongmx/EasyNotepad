"""Makes the held-out set for the local agent: data files and questions, with the expected answers
worked out here from the data (never typed by hand).

    python3 tests/agent-bench/holdout/make.py

Writes tests/agent-bench/holdout/data/ (the folder the agent works in) and
tests/agent-bench/holdout/questions.json (the run.ps1 format). Everything is made up; the same
seed gives the same files.

This set is for measuring only. The harness and the skills are not changed to pass one of its
questions: a failure here points at a general weakness, fixed in general (and then checked on a
fresh set), never by teaching the model these files.
"""
import datetime
import json
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
rng = random.Random(20261122)


def write(path, text):
    full = os.path.join(DATA, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def csv(path, header, rows):
    write(path, ",".join(header) + "\n" + "".join(",".join(str(c) for c in r) + "\n" for r in rows))


questions = []


def ask(qid, kind, question, *checks):
    questions.append({"id": qid, "kind": kind, "question": question, "checks": list(checks)})


def number(v, tol):
    return {"number": round(v, 6), "tol": tol}


def text(*words):
    return {"text": list(words)}


# --- Library loans: a table with several categories -------------------------------------------
branches = ["Riverside", "Hillcrest", "Old Town", "Harbour"]
genres = ["Fantasy", "History", "Science", "Crime", "Cookery", "Poetry"]
members = ["Adult", "Child", "Senior"]
loans = []
for i in range(1, 181):
    days = rng.randint(1, 40)
    fee = round(max(0, days - 21) * 0.25, 2)
    loans.append([f"L{i:04d}", rng.choice(branches), rng.choice(genres), rng.choice(members), days, f"{fee:.2f}"])
csv("data/loans.csv", ["loan_id", "branch", "genre", "member", "days_out", "late_fee"], loans)

per_branch = {b: sum(1 for r in loans if r[1] == b) for b in branches}
top_branch = max(per_branch, key=per_branch.get)
assert sorted(per_branch.values())[-1] != sorted(per_branch.values())[-2], "a tie for the most loans"
ask("loans-top-branch", "table", "Which branch in data/loans.csv lent the most books, and how many loans did it make?",
    text(top_branch), number(per_branch[top_branch], 0))

kids_fantasy = [r[4] for r in loans if r[3] == "Child" and r[2] == "Fantasy"]
ask("loans-child-fantasy", "table", "On average, how many days did children keep Fantasy books, according to data/loans.csv?",
    number(sum(kids_fantasy) / len(kids_fantasy), 0.01))

fees = {b: sum(float(r[5]) for r in loans if r[1] == b) for b in branches}
all_fees = sum(fees.values())
ask("loans-harbour-fees", "table",
    "How much did the Harbour branch collect in late fees (data/loans.csv), and what percentage of all late fees is that?",
    number(fees["Harbour"], 0.01), number(100 * fees["Harbour"] / all_fees, 0.1))

late = sum(1 for r in loans if r[4] > 21)
ask("loans-late-count", "table", "How many loans in data/loans.csv were kept longer than 21 days?", number(late, 0))

# --- Weather: dates, two months, four cities ---------------------------------------------------
cities = {"Oslo": (12, 1.6), "Bergen": (11, 4.2), "Trondheim": (9, 2.4), "Stavanger": (12, 3.1)}
weather = []
day = datetime.date(2026, 4, 1)
while day <= datetime.date(2026, 5, 31):
    for city, (warm, wet) in cities.items():
        t = round(warm + (day - datetime.date(2026, 4, 1)).days * 0.12 + rng.uniform(-4, 4), 1)
        rain = round(max(0.0, rng.gauss(wet, wet * 1.1)), 1)
        weather.append([day.isoformat(), day.strftime("%B"), city, t, rain])
    day += datetime.timedelta(days=1)
csv("data/weather.csv", ["date", "month", "city", "temp_max_c", "rain_mm"], weather)

may_rain = {c: sum(r[4] for r in weather if r[2] == c and r[1] == "May") for c in cities}
wettest = max(may_rain, key=may_rain.get)
ask("weather-wettest-may", "table", "Which city in data/weather.csv had the most rain in May, and how much rain was that?",
    text(wettest), number(may_rain[wettest], 0.05))

bergen_april = [r for r in weather if r[2] == "Bergen" and r[1] == "April"]
hottest = max(bergen_april, key=lambda r: r[3])
assert sum(1 for r in bergen_april if r[3] == hottest[3]) == 1, "a tie for the hottest day"
when = datetime.date.fromisoformat(hottest[0])
ask("weather-bergen-hottest", "table",
    "What was the highest maximum temperature in Bergen in April (data/weather.csv), and on which date was it?",
    number(hottest[3], 0.05),
    text(hottest[0], f"{when.day} April", f"April {when.day}"))

rain_total = {c: sum(r[4] for r in weather if r[2] == c) for c in cities}
ask("weather-bergen-vs-oslo", "table",
    "Over the two months in data/weather.csv, how much more rain fell in Bergen than in Oslo?",
    number(rain_total["Bergen"] - rain_total["Oslo"], 0.05))

# --- Stock: a product per row the table cannot total itself -------------------------------------
items = ["Desk lamp", "Office chair", "Monitor arm", "Keyboard", "Mouse", "Webcam", "Headset", "Notebook",
         "Stapler", "Whiteboard", "Marker set", "Paper ream", "Toner", "Cable kit", "USB hub"]
stock = []
low = ["Webcam", "Toner", "Monitor arm"]
for n, item in enumerate(items):
    for wh in ["North", "South"]:
        qty = rng.randint(2, 9) if (item in low and wh == "North") else rng.randint(12, 80)
        stock.append([f"SKU-{100 + n}-{wh[0]}", item, wh, qty, f"{rng.choice([4.5, 9.9, 12.0, 24.75, 39.0, 89.0, 149.0]):.2f}"])
csv("data/stock.csv", ["sku", "item", "warehouse", "qty", "unit_cost"], stock)
north_value = sum(r[3] * float(r[4]) for r in stock if r[2] == "North")
ask("stock-north-value", "table",
    "What is the total value of the stock (quantity times unit cost) in the North warehouse, in data/stock.csv?",
    number(north_value, 0.05))
few = sorted(r[1] for r in stock if r[3] < 10)
assert few == sorted(low)
ask("stock-low", "table", "Which items in data/stock.csv have fewer than 10 units in a warehouse?",
    *[text(i) for i in low])

# --- A handbook: one document, facts, a sum, and something it does not say ---------------------
write("docs/handbook.md", """# Staff handbook

## Annual leave
| Years with us | Days of leave a year |
|---|---|
| 0 to 2 | 22 |
| 3 to 5 | 25 |
| 6 or more | 28 |

Unused leave of up to 5 days can be carried into the next year. Leave is booked in the HR portal
at least two weeks ahead.

## Notice period
During the first year the notice period is 4 weeks on either side; after that it is 8 weeks.

## Working from home
Everyone may work from home up to 3 days a week, agreed with their team lead.

## Travel and expenses
- Meals on a business trip: up to 35 EUR a day.
- Hotels: up to 140 EUR a night in capitals, 110 EUR elsewhere.
- Trains are booked in second class; flights only for trips over 600 km.
- Receipts go into the expense tool within 30 days of the trip.

## Equipment
Each new starter gets a laptop and a headset. A second monitor can be requested from IT.
""")
ask("handbook-leave", "document", "According to docs/handbook.md, how many days of annual leave a year does someone get who has worked here for 4 years?",
    number(25, 0))
ask("handbook-trip", "document",
    "Using docs/handbook.md, what is the most someone can claim for meals and hotel nights on a 3-night trip to a capital city, with meals for 3 days?",
    number(3 * 35 + 3 * 140, 0.01))
ask("handbook-missing", "document", "What does docs/handbook.md say about parental leave?",
    text("does not", "doesn't", "no mention", "not mention", "not cover", "no information", "not found", "nothing about",
         "isn't", "is not", "no section", "not include", "not contain"))

# --- Meeting minutes: owners, dates, what is left ----------------------------------------------
write("docs/minutes-2026-03-12.md", """# Events committee, 12 March 2026

Present: Priya Nair, Tomasz Wolski, Amara Okafor, Leon Brandt (notes)

## Summer open day
The committee confirmed the open day for 6 June. The budget approved by the board is 4,800 EUR.
So far 1,250 EUR has been spent on the stage hire deposit and the printed flyers.

## Decisions
- The open day runs from 10:00 to 16:00.
- Food comes from two local vendors; the committee will not run its own stall this year.

## Action items
- Priya: update the supplier list and send it round by 20 March.
- Tomasz: book the venue's main hall by 3 April.
- Amara: draft the newsletter announcement by 27 March.
- Leon: ask the board about a rain plan before the next meeting.

Next meeting: 9 April 2026.
""")
ask("minutes-venue", "document", "In docs/minutes-2026-03-12.md, who has to book the venue, and by when?",
    text("Tomasz"), text("3 April", "April 3", "2026-04-03", "03/04"))
ask("minutes-budget", "document", "How much of the open day budget in docs/minutes-2026-03-12.md has not been spent yet?",
    number(4800 - 1250, 0.01))

# --- Contracts: the same facts from several documents, and dates -------------------------------
contracts = [
    ("cleaning-brightway.txt", "Brightway Cleaning", "office cleaning, five days a week", 640, datetime.date(2025, 2, 1), 12),
    ("security-sentinel.txt", "Sentinel Guard", "night security and alarm response for both buildings, including weekends "
     "and public holidays, with a monthly report on incidents and patrols", 1150, datetime.date(2024, 9, 1), 24),
    ("catering-freshtable.txt", "Fresh Table", "lunch catering for the canteen", 890, datetime.date(2025, 6, 15), 18),
    ("it-bytecare.txt", "ByteCare", "IT support desk and laptop repairs", 1480, datetime.date(2025, 11, 1), 6),
]
for name, vendor, service, fee, start, months in contracts:
    write("contracts/" + name, f"""SERVICE AGREEMENT

Supplier: {vendor}
Customer: Alder & Finch Ltd
Service: {service}
Monthly fee: {fee:,} EUR (excluding VAT)
Start date: {start.strftime('%d %B %Y')}
Term: {months} months from the start date
Payment: within 30 days of each monthly invoice
""")


def end_of(start, months):
    m = start.month - 1 + months
    first_after = datetime.date(start.year + m // 12, m % 12 + 1, start.day)
    return first_after - datetime.timedelta(days=1), first_after


ask("contracts-monthly", "documents", "What do the contracts in the contracts folder cost per month altogether?",
    number(sum(c[3] for c in contracts), 0.01))
first = min(contracts, key=lambda c: end_of(c[4], c[5])[0])
last_day, day_after = end_of(first[4], first[5])
ask("contracts-ends-first", "documents", "Which contract in the contracts folder ends first, and on what date?",
    text(first[1].split()[0]),
    text(*{d.isoformat() for d in (last_day, day_after)} | {f"{d.day} {d.strftime('%B')} {d.year}" for d in (last_day, day_after)} |
         {f"{d.strftime('%B')} {d.day}" for d in (last_day, day_after)}))
sizes = {name: len(open(os.path.join(DATA, "contracts", name), encoding="utf-8").read().encode()) for name, *_ in contracts}
largest = max(sizes, key=sizes.get)
ask("contracts-folder", "folder", "How many .txt files are in the contracts folder, and which one is the largest?",
    number(len(contracts), 0), text(largest))

# --- A settings file and a program ---------------------------------------------------------------
write("config/app.json", json.dumps({
    "name": "orders-api",
    "services": {
        "payments": {"url": "https://pay.internal/v2", "timeout_ms": 4500, "retries": 3},
        "search": {"url": "https://search.internal", "timeout_ms": 1200, "retries": 1},
        "mail": {"url": "smtp://mail.internal", "timeout_ms": 8000, "retries": 5},
    },
    "log_level": "info",
}, indent=2) + "\n")
ask("config-timeout", "code", "In config/app.json, what is the payments service's timeout in seconds?", number(4.5, 0.001))

write("src/stock.py", '''"""Keeps the shop's stock levels and orders more when they run low."""

REORDER_LEVEL = 12   # order more below this many units
WEEKS_OF_COVER = 3   # how many weeks of sales an order covers


def weekly_sales(item, history):
    """The average units of `item` sold a week, over the weeks in `history`."""
    weeks = history.get(item, [])
    return sum(weeks) / len(weeks) if weeks else 0


def restock(item, on_hand, history):
    """How many units of `item` to order now: none while there are REORDER_LEVEL or more,
    else enough for WEEKS_OF_COVER weeks of sales."""
    if on_hand >= REORDER_LEVEL:
        return 0
    return round(weekly_sales(item, history) * WEEKS_OF_COVER)
''')
ask("code-restock", "code", "In src/stock.py, below what stock level does restock order more, and how many weeks of sales does an order cover?",
    number(12, 0), number(3, 0))

# --- Long and odd files: a log, a long report, an HTML export ----------------------------------
services = ["billing", "search", "auth", "mailer"]
log, billing_errors = [], 0
start = datetime.datetime(2026, 3, 2, 8, 0, 0)
restart_line = 1137
for n in range(1, 1501):
    t = start + datetime.timedelta(seconds=n * 47)
    svc = rng.choice(services)
    level = rng.choices(["INFO", "WARN", "ERROR"], [80, 14, 6])[0]
    if n == restart_line:
        svc, level, msg = "auth", "WARN", "restarted after the certificate store ran out of memory"
    elif level == "ERROR":
        msg = rng.choice(["request timed out", "upstream returned 502", "could not write the audit record"])
    else:
        msg = rng.choice(["request served", "cache refreshed", "job queued", "connection pool resized", "health check passed"])
    billing_errors += svc == "billing" and level == "ERROR"
    log.append(f"{t.isoformat(sep=' ')} [{level:5}] {svc}: {msg} (req {rng.randint(10000, 99999)})")
write("logs/app.log", "\n".join(log) + "\n")
ask("log-billing-errors", "long", "How many ERROR lines from the billing service are in logs/app.log?", number(billing_errors, 0))
restart_at = start + datetime.timedelta(seconds=restart_line * 47)
ask("log-auth-restart", "long", "When did the auth service restart according to logs/app.log, and why?",
    text(restart_at.strftime("%H:%M")), text("certificate", "out of memory", "memory"))

topics = ["the northern region", "the retail stores", "the online shop", "the logistics centre", "customer service",
          "the product range", "energy use", "the supplier base", "the training programme", "the IT systems"]
verbs = ["grew steadily", "was reorganised", "met its targets", "faced higher costs", "was reviewed in depth",
         "saw new investment", "kept its course", "improved its margins"]
paragraphs = []
for k in range(160):
    sentences = [f"In the quarter, {rng.choice(topics)} {rng.choice(verbs)}." for _ in range(rng.randint(3, 6))]
    if k == 71:
        sentences.insert(2, "The board approved a dividend of 0.42 EUR per share, payable in June.")
    if k == 118:
        sentences.insert(1, "Headcount at the end of the year was 1,284 people, against 1,190 a year before.")
    paragraphs.append(" ".join(sentences))
write("docs/annual-report.md", "# Annual report 2025\n\n" + "\n\n".join(
    (f"## Section {k // 10 + 1}\n\n" if k % 10 == 0 else "") + p for k, p in enumerate(paragraphs)) + "\n")
ask("report-dividend-headcount", "long",
    "According to docs/annual-report.md, what dividend per share did the board approve, and what was the headcount at the end of the year?",
    number(0.42, 0.001), number(1284, 0))

customers = ["Velmont", "Okra Foods", "Lindqvist", "Parada", "Hollis & Co"]
rows, velmont = [], 0.0
for n in range(1, 181):
    c = rng.choice(customers)
    amount = round(rng.uniform(40, 900), 2)
    velmont += amount if c == "Velmont" else 0
    rows.append(f"<tr><td>ORD-{5000 + n}</td><td>2026-0{rng.randint(1, 6)}-{rng.randint(10, 28)}</td><td>{c}</td>"
                f"<td class=\"num\">{amount:,.2f}</td></tr>")
write("exports/orders.html", "<!doctype html><html><head><meta charset=\"utf-8\"><title>Orders export</title>"
      "<style>td.num{text-align:right}</style></head><body><h1>Orders, first half of 2026</h1><table>"
      "<tr><th>Order</th><th>Date</th><th>Customer</th><th>Amount (EUR)</th></tr>" + "".join(rows) + "</table></body></html>\n")
ask("html-velmont-total", "long", "In exports/orders.html, what do the orders from Velmont add up to?", number(velmont, 0.05))

# --- Calculations alone -------------------------------------------------------------------------
ask("calc-recipe", "calc", "A recipe for 4 people needs 300 g of flour. How much flour do I need for 10 people?", number(750, 0.01))
ask("calc-savings", "calc",
    "If I put 2,500 euros into savings at 3.5% interest a year, added once a year, how much will there be after 4 years?",
    number(2500 * 1.035 ** 4, 0.01))

with open(os.path.join(HERE, "questions.json"), "w", encoding="utf-8", newline="\n") as f:
    json.dump({"about": "Held-out questions (made by make.py from its own data): for measuring only, never for tuning.",
               "questions": questions}, f, indent=1)
print(len(questions), "questions")
