"""Makes the real-life set for the local agent: one person's messy folder (downloads, an inbox,
recordings, photos of paper, spreadsheets, a calendar) and questions asked the way people ask them,
with the expected answers worked out here from the data (never typed by hand).

    python3 tests/agent-bench/reallife/make.py
    powershell -ExecutionPolicy Bypass -File tests/agent-bench/reallife/make-media.ps1

The first writes tests/agent-bench/reallife/data/ (text, PDF, Word, Excel, e-mail, calendar files),
media.json (what the recordings say and what the photos show) and questions.json (the run.ps1
format). The second speaks the recordings (Windows' voices, two people in a call) and draws the
photographed papers, from media.json. Everything is made up; the same seed gives the same files.

Like the held-out set, this is for measuring only: a failure points at a general weakness, fixed in
general, never by teaching the model these files.
"""
import datetime
import json
import os
import random
import zipfile
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
rng = random.Random(20261123)

questions = []
media = {"speech": [], "pictures": []}


def path_of(rel):
    full = os.path.join(DATA, rel)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    return full


def write(rel, text):
    with open(path_of(rel), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def ask(qid, kind, question, *checks):
    questions.append({"id": qid, "kind": kind, "question": question, "checks": list(checks)})


def number(v, tol=0.01):
    return {"number": round(v, 6), "tol": tol}


def text(*words):
    return {"text": list(words)}


def money(v):
    return f"{v:,.2f}"


# --- File formats, written with the standard library -------------------------------------------

def pdf(rel, lines):
    """A one-page PDF with the lines in Helvetica (a text layer, as a PDF printed from an app)."""
    def esc(s):
        return s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
    body = ["BT", "/F1 11 Tf", "14 TL", "56 790 Td"]
    for line in lines:
        if line.startswith("# "):
            body += ["/F1 16 Tf", f"({esc(line[2:])}) Tj", "T*", "/F1 11 Tf", "T*"]
        else:
            body += [f"({esc(line)}) Tj", "T*"]
    body.append("ET")
    stream = "\n".join(body).encode("latin-1")
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        b"<< /Length " + str(len(stream)).encode() + b" >>\nstream\n" + stream + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
    ]
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for i, o in enumerate(objects, 1):
        offsets.append(len(out))
        out += f"{i} 0 obj\n".encode() + o + b"\nendobj\n"
    xref = len(out)
    out += f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode()
    for off in offsets:
        out += f"{off:010d} 00000 n \n".encode()
    out += f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    with open(path_of(rel), "wb") as f:
        f.write(out)


def docx(rel, paragraphs):
    """A Word document: one paragraph per item; "# " makes a heading (bold, larger)."""
    paras = []
    for p in paragraphs:
        if p.startswith("# "):
            paras.append('<w:p><w:r><w:rPr><w:b/><w:sz w:val="32"/></w:rPr><w:t xml:space="preserve">'
                         + escape(p[2:]) + "</w:t></w:r></w:p>")
        else:
            paras.append('<w:p><w:r><w:t xml:space="preserve">' + escape(p) + "</w:t></w:r></w:p>")
    document = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>'
                + "".join(paras) + "</w:body></w:document>")
    with zipfile.ZipFile(path_of(rel), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml",
                   '<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                   '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                   '<Default Extension="xml" ContentType="application/xml"/>'
                   '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
                   "</Types>")
        z.writestr("_rels/.rels",
                   '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                   '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>'
                   "</Relationships>")
        z.writestr("word/document.xml", document)


def xlsx(rel, sheet, rows):
    """An Excel workbook with one sheet: numbers as numbers, the rest as shared strings."""
    strings, index = [], {}

    def col(i):
        s = ""
        i += 1
        while i:
            i, r = divmod(i - 1, 26)
            s = chr(65 + r) + s
        return s

    cells = []
    for r, row in enumerate(rows, 1):
        cs = []
        for c, v in enumerate(row):
            ref = f"{col(c)}{r}"
            if isinstance(v, (int, float)):
                cs.append(f'<c r="{ref}"><v>{v}</v></c>')
            else:
                if v not in index:
                    index[v] = len(strings)
                    strings.append(v)
                cs.append(f'<c r="{ref}" t="s"><v>{index[v]}</v></c>')
        cells.append(f'<row r="{r}">' + "".join(cs) + "</row>")
    ns = 'xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"'
    rns = 'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"'
    with zipfile.ZipFile(path_of(rel), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml",
                   '<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                   '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                   '<Default Extension="xml" ContentType="application/xml"/>'
                   '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
                   '<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>'
                   '<Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>'
                   "</Types>")
        z.writestr("_rels/.rels",
                   '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                   '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>'
                   "</Relationships>")
        z.writestr("xl/workbook.xml", f'<?xml version="1.0" encoding="UTF-8"?><workbook {ns} {rns}><sheets>'
                   f'<sheet name="{escape(sheet)}" sheetId="1" r:id="rId1"/></sheets></workbook>')
        z.writestr("xl/_rels/workbook.xml.rels",
                   '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                   '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>'
                   '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>'
                   "</Relationships>")
        z.writestr("xl/worksheets/sheet1.xml", f'<?xml version="1.0" encoding="UTF-8"?><worksheet {ns}><sheetData>'
                   + "".join(cells) + "</sheetData></worksheet>")
        z.writestr("xl/sharedStrings.xml", f'<?xml version="1.0" encoding="UTF-8"?><sst {ns} count="{len(strings)}" '
                   f'uniqueCount="{len(strings)}">' + "".join(f"<si><t>{escape(s)}</t></si>" for s in strings) + "</sst>")


def eml(rel, sender, to, subject, date, body):
    write(rel, f"From: {sender}\nTo: {to}\nSubject: {subject}\nDate: {date}\n"
               "MIME-Version: 1.0\nContent-Type: text/plain; charset=utf-8\n\n" + body.strip() + "\n")


ME = "Dana Whitlock"
ME_MAIL = "dana.whitlock@example.com"

# --- The team call: two people, decisions and action items -------------------------------------
LAUNCH = datetime.date(2026, 4, 14)
CAP = 18000
VENUE_DAY = datetime.date(2026, 3, 20)
call = [
    ("David", "Morning Dana. Thanks for joining. Let's go through the launch quickly, I have a hard stop at ten thirty."),
    ("Zira", "Morning Marco. Sure. Where did we land on the date?"),
    ("David", "We agreed to move the launch to the fourteenth of April. The original date in March is off, the supplier "
              "cannot deliver the packaging in time."),
    ("Zira", "OK, the fourteenth of April. And the budget?"),
    ("David", f"Finance approved a cap of eighteen thousand dollars for the whole launch, including the venue."),
    ("Zira", "Good. What do you need from me?"),
    ("David", "Dana, can you send everyone the revised timeline by Friday? That's your action."),
    ("Zira", "Yes, I'll send the revised timeline by Friday."),
    ("David", "I will book the venue by the twentieth of March, and Priya is updating the press kit."),
    ("Zira", "Great. Anything else?"),
    ("David", "No, that's it. Same time next Tuesday. Thanks Dana."),
]
media["speech"].append({"file": "Recordings/2026-03-03 team call.wav", "lines": call})
ask("call-launch-action", "sound",
    "I missed the team call on 3 March (it's in Recordings). What's the new launch date, and what was I asked to do?",
    text("14 April", "April 14", "14th of April", "14th April", "April 14th", "fourteenth of April", "14 Apr", "2026-04-14"), text("timeline"))
ask("call-budget", "sound", "In the recording of the 3 March team call, what budget cap did finance approve?",
    number(CAP, 0))

# --- A voicemail and a voice memo ---------------------------------------------------------------
media["speech"].append({"file": "Recordings/voicemail from landlord.wav", "lines": [
    ("David", "Hi Dana, it's Tom, your landlord. Just to let you know the plumber is coming on Thursday between nine and "
              "eleven in the morning to fix the boiler. If you're not home, please leave the key with your neighbour in "
              "flat four B. Thanks, bye."),
]})
ask("voicemail-plumber", "sound",
    "My landlord left me a voicemail. When is the plumber coming, and where should I leave the key?",
    text("Thursday"), text("4B", "4 B", "four B"))
GATE = 4417
media["speech"].append({"file": "Recordings/memo 0412.wav", "lines": [
    ("Zira", "Note to self. The storage unit is number two one two, and the gate code is four, four, one, seven."),
]})
ask("memo-gate-code", "sound", "What was the gate code for the storage unit? I recorded a voice memo about it.",
    text("4417", "4 4 1 7", "4,417"))

# --- The inbox: who waits for a reply ------------------------------------------------------------
INV = "INV-2291"
INV_AMOUNT = 4350.00
REPORT_DUE = datetime.date(2026, 3, 13)
eml("Inbox/01 newsletter.eml", "news@cityarts.example.org", ME_MAIL, "This month at City Arts", "Mon, 2 Mar 2026 07:00:00 +0000",
    "Our spring programme is out. No reply needed - see you at the gallery!")
eml("Inbox/02 report request.eml", "Priya Nair <priya.nair@example.com>", ME_MAIL, "Q1 report - can you send it?",
    "Tue, 3 Mar 2026 09:12:00 +0000",
    f"Hi Dana,\n\nCould you send me the Q1 report by {REPORT_DUE.strftime('%A %d %B')}? I need it for the board pack.\n"
    "Let me know if that works.\n\nThanks,\nPriya")
eml("Inbox/03 bank notice.eml", "alerts@northbank.example.com", ME_MAIL, "Your statement is ready",
    "Tue, 3 Mar 2026 11:00:00 +0000", "Your March statement is ready in online banking. This is an automated message.")
eml("Inbox/04 client invoice.eml", "Leo Grant <leo.grant@brightwater.example.com>", ME_MAIL, f"Re: {INV}",
    "Wed, 4 Mar 2026 16:40:00 +0000",
    f"Hi Dana,\n\nWe can't find invoice {INV} in our system. Could you resend it, with the bank details? We'd like to pay "
    "it this month.\n\nBest,\nLeo Grant\nBrightwater Ltd")
eml("Inbox/05 thanks.eml", "Marco Silva <marco.silva@example.com>", ME_MAIL, "Thanks for today",
    "Wed, 4 Mar 2026 18:05:00 +0000", "Thanks for the help with the slides today. Nothing needed, just saying thanks!\n\nMarco")
eml("Inbox/06 dentist.eml", "reminders@brightsmile.example.com", ME_MAIL, "Appointment reminder",
    "Thu, 5 Mar 2026 08:00:00 +0000", "This is a reminder of your appointment on Thursday 12 March at 14:00. "
    "No need to reply unless you need to change it.")
ask("inbox-needs-reply", "email",
    "Which e-mails in my Inbox still need a reply from me? Just the ones where someone is waiting on me.",
    text("Priya"), text("Leo", "Brightwater"))
ask("inbox-report-due", "email", "When does Priya need the Q1 report?",
    text("13 March", "March 13", "13th", "Friday 13", "2026-03-13"))

# --- Photos of paper: a receipt and a parking fine -------------------------------------------------
DINNER = [("Grilled salmon", 32.50), ("Risotto", 24.00), ("Sparkling water", 6.50), ("House red (bottle)", 24.40)]
DINNER_TOTAL = round(sum(p for _, p in DINNER), 2)
LIMIT = 75.00
media["pictures"].append({"file": "Downloads/IMG_2041.png", "kind": "receipt", "lines":
    ["THE HARBOUR KITCHEN", "12 Quay Street", "Tel 555 0142", "", "Date: 05/03/2026  20:41", "Table 7   Covers: 1", ""]
    + [f"{name:<20}{money(p):>8}" for name, p in DINNER]
    + ["", f"{'TOTAL':<20}{money(DINNER_TOTAL):>8}", "Paid by card", "", "Thank you!"]})
docx("Work/Expense policy 2026.docx", [
    "# Expense policy 2026",
    "This policy covers travel and meals for client work.",
    "# Meals",
    f"A dinner with a client or on a work trip may be claimed up to {money(LIMIT)} per person, drinks included. "
    "Anything above the limit is paid by the employee.",
    "# Taxis",
    "Taxis are claimed with a receipt. Rides above 20.00 need the reason written on the claim.",
    "# How to claim",
    "Send the receipts with the claim form by the 5th of the next month.",
])
ask("receipt-claim", "picture",
    "Can I claim the whole dinner on the receipt photo in Downloads (IMG_2041.png)? I ate alone. Check our expense policy.",
    number(DINNER_TOTAL), number(LIMIT), number(round(DINNER_TOTAL - LIMIT, 2)))
FINE, FINE_REF = 60.00, "PCN-88172"
FINE_DUE = datetime.date(2026, 3, 26)
media["pictures"].append({"file": "Downloads/scan0003.png", "kind": "letter", "lines": [
    "CITY OF HARBOURVIEW - PARKING SERVICES", "", "PENALTY CHARGE NOTICE", "", f"Reference: {FINE_REF}",
    "Vehicle: KX21 LMV", "Date of contravention: 26/02/2026", "Location: Quay Street (bay 14)", "",
    f"Amount due: GBP {money(FINE)}", f"Pay by: {FINE_DUE.strftime('%d/%m/%Y')}",
    "If paid within 14 days, the amount is reduced by half.", "", "Pay online at harbourview.example.gov/pay"]})
ask("fine-amount-due", "picture",
    "There's a photo of a parking fine in my Downloads. How much is it, and by when do I have to pay?",
    number(FINE), text("26/03/2026", "26 March", "March 26", "26.03", "2026-03-26"))

# --- Downloads: duplicates under other names ------------------------------------------------------
invoice = [f"# Invoice {INV}", "", "From: Dana Whitlock Design, 4 Mill Lane", "To: Brightwater Ltd", "",
           "Date: 20/02/2026", "Due: 20/03/2026", "",
           "Brand guidelines (24 hours)                3,600.00", "Two workshop sessions                       750.00", "",
           f"Total due                                  {money(INV_AMOUNT)}", "", "Bank: Northbank, account 40-22-19 77310452"]
pdf(f"Downloads/Invoice_{INV}.pdf", invoice)
pdf(f"Downloads/Invoice_{INV} (1).pdf", invoice)
pdf("Downloads/Invoice_INV-2288.pdf", [l.replace(INV, "INV-2288").replace(money(INV_AMOUNT), "1,200.00")
                                       .replace("3,600.00", "1,200.00").replace("Two workshop sessions                       750.00", "")
                                       for l in invoice])
write("Downloads/notes (copy).txt", "Call the bank about the card limit.\nBuy printer ink.\n")
write("Downloads/notes.txt", "Call the bank about the card limit.\nBuy printer ink.\n")
ask("downloads-duplicates", "workspace",
    "Are there any duplicate files in my Downloads folder (the same file saved twice)?",
    text(f"Invoice_{INV} (1)", f"{INV} (1)"), text("notes (copy)"))

# --- Money: a messy expenses workbook and a bank export -------------------------------------------
expenses = [["Date", "Description", "Category", "Amount"]]
taxi_march = 0.0
for day in range(1, 32):
    d = datetime.date(2026, 3, day) if day <= 31 else None
    if rng.random() < 0.45:
        kind = rng.choice(["taxi", "taxi", "lunch", "train", "coffee", "books"])
        amount = round(rng.uniform(8, 38), 2) if kind == "taxi" else round(rng.uniform(3, 60), 2)
        desc, cat = {
            "taxi": (rng.choice(["Taxi to client", "Uber ride", "Cab home", "Taxi from station"]),
                     rng.choice(["Taxi", "taxi", "Taxi ", "Transport - taxi"])),
            "lunch": ("Lunch", "Meals"), "train": ("Train ticket", "Transport - train"),
            "coffee": ("Coffee", "Meals"), "books": ("Book", "Office"),
        }[kind]
        if kind == "taxi":
            taxi_march += amount
        expenses.append([d.isoformat(), desc, cat, amount])
for day in (3, 9, 17, 26):  # February: not in the question
    expenses.append([datetime.date(2026, 2, day).isoformat(), "Taxi to client", "Taxi", round(rng.uniform(10, 30), 2)])
xlsx("Finance/expenses 2026.xlsx", "Expenses", expenses)
ask("taxi-march", "table", "How much did I spend on taxis in March? It's in my expenses workbook in Finance.",
    number(round(taxi_march, 2), 0.05))

bank = [["Date", "Payee", "Reference", "Amount"]]
groceries = 0.0
for day in range(1, 32):
    if rng.random() < 0.5:
        payee = rng.choice(["FreshMart", "Corner Grocer", "FreshMart", "City Pharmacy", "Coffee House", "Petrol Plus"])
        amount = -round(rng.uniform(5, 95), 2)
        if payee in ("FreshMart", "Corner Grocer"):
            groceries += -amount
        bank.append([f"2026-03-{day:02d}", payee, "CARD", amount])
bank.append(["2026-03-02", "Brightwater Ltd", "INV-2275", 2100.00])
bank.append(["2026-03-11", "Gymworks", "MEMBERSHIP", -39.00])
bank.sort(key=lambda r: r[0] if r[0] != "Date" else "")
with open(path_of("Finance/bank export March.csv"), "w", encoding="utf-8", newline="\n") as f:
    for r in bank:
        f.write(",".join(str(c) for c in r) + "\n")
ask("groceries-march", "table",
    "What did I spend on groceries in March, going by my bank export? FreshMart and Corner Grocer are my grocery shops.",
    number(round(groceries, 2), 0.05))
ask("brightwater-owes", "documents",
    f"Has Brightwater paid invoice {INV} yet? If not, how much do they still owe me?",
    number(INV_AMOUNT))

# --- Work: two versions of a price list, a proposal, standup notes --------------------------------
items = [("Logo design", 950), ("Brand guidelines", 3600), ("Business cards", 180), ("Website banner", 240),
         ("Social media kit", 620), ("Workshop session", 375)]
changed = {"Business cards": 210, "Social media kit": 680}
removed, added = "Website banner", ("Packaging design", 1450)
write("Work/price list v1.csv", "Service,Price\n" + "".join(f"{n},{p}\n" for n, p in items))
v2 = [(n, changed.get(n, p)) for n, p in items if n != removed] + [added]
write("Work/price list v2.csv", "Service,Price\n" + "".join(f"{n},{p}\n" for n, p in v2))
ask("price-list-changes", "documents", "What changed between price list v1 and v2 in Work?",
    text("Business cards"), text("Social media kit"), text("Website banner"), text("Packaging design"))
PROPOSAL_PRICE, PROPOSAL_DATE = 14500, datetime.date(2026, 5, 29)
docx("Work/Project Lumen - proposal v3.docx", [
    "# Project Lumen - proposal (version 3)",
    "Prepared for Lumen Coffee Co. by Dana Whitlock Design.",
    "# Scope",
    "A new visual identity: logo, colours, type, packaging for three blends, and a short brand book.",
    "# Timeline",
    f"Kick-off on 6 April 2026; final files delivered by {PROPOSAL_DATE.strftime('%d %B %Y')}.",
    "# Price",
    f"The fixed price is {PROPOSAL_PRICE:,} EUR, paid 40% at kick-off and 60% on delivery.",
    "Version 2 quoted 16,200 EUR; the packaging for a fourth blend was taken out of scope.",
])
ask("proposal-price-date", "documents", "What price did we quote Lumen in the latest proposal, and when do we deliver?",
    number(PROPOSAL_PRICE, 0), text("29 May", "May 29", "2026-05-29", "29/05"))
ask("proposal-deposit", "documents", "How much does Lumen pay at kick-off, according to the proposal?",
    number(PROPOSAL_PRICE * 0.4, 0.5))
write("Work/standup notes week 11.md", "# Standup notes, week 11\n\n"
      "## Monday\n- Marco: finishing the packaging mock-ups.\n- Priya: press kit draft.\n- Dana: Lumen proposal v3.\n\n"
      "## Wednesday\n- Marco: mock-ups done; waiting for the printer's colour proofs.\n"
      "- Priya: press kit needs the final launch date.\n- Dana: sent the proposal.\n\n"
      "## Friday\n- Blocker: the release is waiting on the printer's colour proofs, promised for next Tuesday.\n"
      "- Priya still needs legal sign-off on the press kit.\n")
ask("standup-blocker", "documents", "Going by this week's standup notes, what is the release waiting on?",
    text("colour proof", "color proof", "proofs"))

# --- Home: a gym contract, a recipe, the wifi, a calendar, a flight --------------------------------
GYM_FEE, NOTICE = 39.00, 30
pdf("Home/Gymworks membership terms.pdf", [
    "# Gymworks - membership terms", "",
    f"1. The membership costs {money(GYM_FEE)} per month, taken on the 11th.",
    "2. The minimum term is 6 months from the start date.",
    f"3. To cancel, give {NOTICE} days' notice in writing (a letter, or an e-mail to",
    "   members@gymworks.example.com). Notice given by phone is not accepted.",
    "4. Fees already paid are not refunded.",
    "5. A membership can be frozen for up to 2 months a year for medical reasons."])
ask("gym-cancel", "documents", "How do I cancel my gym membership, and how much notice do I have to give?",
    number(NOTICE, 0), text("writing", "written", "e-mail", "email", "letter"))
write("Home/recipes/pancakes.md", "# Pancakes (serves 4)\n\n- 200 g plain flour\n- 2 eggs\n- 300 ml milk\n"
      "- 1 tbsp sugar\n- a pinch of salt\n- butter for the pan\n\nWhisk, rest 20 minutes, fry in a hot pan.\n")
ask("pancakes-for-six", "calc", "How much flour do I need for the pancakes if we're 6 people?", number(300, 0))
write("Home/house notes.txt", "Bins: green on Tuesday, black on Friday.\nWifi: network HomeNest-5G, password "
      "maple-river-208\nBoiler: service due in October.\nSpare key: with Mrs Patel at 4B.\n")
ask("wifi-password", "workspace", "What's our wifi password at home? I wrote it down somewhere.", text("maple-river-208"))
write("Home/calendar.ics", "BEGIN:VCALENDAR\nVERSION:2.0\nPRODID:-//made up//EN\n"
      "BEGIN:VEVENT\nSUMMARY:Lumen kick-off prep\nDTSTART:20260312T093000\nDTEND:20260312T110000\nEND:VEVENT\n"
      "BEGIN:VEVENT\nSUMMARY:Dentist\nDTSTART:20260312T140000\nDTEND:20260312T150000\nLOCATION:Bright Smile\nEND:VEVENT\n"
      "BEGIN:VEVENT\nSUMMARY:Yoga\nDTSTART:20260312T183000\nDTEND:20260312T193000\nEND:VEVENT\n"
      "BEGIN:VEVENT\nSUMMARY:Lunch with Marco\nDTSTART:20260313T123000\nDTEND:20260313T133000\nEND:VEVENT\n"
      "END:VCALENDAR\n")
ask("calendar-thursday", "documents", "Am I free on Thursday 12 March in the afternoon? Check my calendar in Home.",
    text("Dentist"))
DEPART = datetime.datetime(2026, 4, 9, 7, 35)
CLOSE = 45
pdf("Travel/booking confirmation.pdf", [
    "# Booking confirmation", "", "Passenger: DANA WHITLOCK", "Booking reference: QX7TLM", "",
    f"Flight NE 412   {DEPART.strftime('%d %b %Y')}   Harbourview (HBV) - Lisbon (LIS)",
    f"Departs {DEPART.strftime('%H:%M')}  Terminal 2      Arrives 10:20 local time", "",
    f"Check-in and bag drop close {CLOSE} minutes before departure.",
    "Boarding closes 20 minutes before departure."])
latest = DEPART - datetime.timedelta(minutes=CLOSE)
ask("flight-latest-arrival", "documents",
    "What's the latest I can get to the airport for my Lisbon flight and still check in my bag?",
    text(latest.strftime("%H:%M"), latest.strftime("%-H:%M"), "6.50", "6:50 am", "6:50am"))

# --- Across several files --------------------------------------------------------------------------
ask("owe-this-month", "documents",
    "This month I have to pay the parking fine and my gym fee. How much is that together?",
    number(FINE + GYM_FEE))
ask("call-vs-proposal", "documents",
    "Is the launch date from the 3 March team call before or after the Lumen kick-off date in the proposal, and by how many days?",
    text("after"), number((LAUNCH - datetime.date(2026, 4, 6)).days, 0))

os.makedirs(DATA, exist_ok=True)
with open(os.path.join(HERE, "media.json"), "w", encoding="utf-8") as f:
    json.dump(media, f, indent=1)
with open(os.path.join(HERE, "questions.json"), "w", encoding="utf-8") as f:
    json.dump({"about": "Real-life questions (made by make.py and make-media.ps1 from their own data): one person's "
                        "messy folder, asked as people ask. For measuring only, never for tuning.",
               "questions": questions}, f, indent=1)
print(len(questions), "questions;", len(media["speech"]), "recordings;", len(media["pictures"]), "photos to make")
