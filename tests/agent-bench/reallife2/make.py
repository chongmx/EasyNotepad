"""Makes the second real-life set: another made-up person's folder (a parent with a car, a flat, a
part-time job), with the same kinds of tasks as reallife/ but other files and other questions. It
was written after the harness was tuned on reallife/, to measure whether that tuning carries over:
it is for measuring only, never for tuning.

    python3 tests/agent-bench/reallife2/make.py
    powershell -ExecutionPolicy Bypass -File tests/agent-bench/reallife/make-media.ps1 -Set tests/agent-bench/reallife2

Expected answers are worked out here from the data, never typed by hand.
"""
import datetime
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import benchfiles as b  # noqa: E402
from benchfiles import ask, docx, eml, media, money, number, path_of, pdf, text, write, xlsx  # noqa: E402

b.start(HERE)
rng = random.Random(20261124)
ME_MAIL = "sam.okafor@example.net"

# --- Recordings: a parents' evening, a garage voicemail, a memo ------------------------------------
TEST_DAY = datetime.date(2026, 6, 2)
media["speech"].append({"file": "Audio/parents evening Mia.wav", "lines": [
    ("Zira", "Thanks for coming in, Mr Okafor. Mia is doing really well in English, her reading has come on a lot."),
    ("David", "That's good to hear. How is she getting on with maths?"),
    ("Zira", "Maths is where she needs a bit more practice. Fractions in particular. If she could do ten minutes of "
             "fractions a few evenings a week, that would help."),
    ("David", "Fractions, OK. Is there a test coming up?"),
    ("Zira", "Yes, the end of year maths test is on the second of June."),
    ("David", "Second of June. We'll work on it. Anything else?"),
    ("Zira", "Just that the school trip form needs to come back signed. Otherwise she's a pleasure to teach."),
]})
ask("teacher-practise-test", "sound",
    "At Mia's parents' evening, what did the teacher say she should practise, and when is her test? It's in Audio.",
    text("fraction"), text("2 June", "June 2", "2nd of June", "second of June", "2nd June", "June 2nd", "2026-06-02"))
GARAGE_BILL = 245
media["speech"].append({"file": "Audio/voicemail 0417.wav", "lines": [
    ("David", "Hello, this is Kevin from Millbrook Motors calling for Sam Okafor. Your car is ready to collect. We replaced "
              "the front brake pads and did the service. The bill comes to two hundred and forty five pounds. On "
              "Saturday we close at one o'clock, so please come before then. Thanks."),
]})
ask("garage-bill-saturday", "sound",
    "The garage left me a voicemail. How much is the bill, and until what time can I pick the car up on Saturday?",
    number(GARAGE_BILL, 0), text("1 pm", "1pm", "one o'clock", "1 o'clock", "13:00", "1:00", "13.00"))
media["speech"].append({"file": "Audio/memo 2.wav", "lines": [
    ("Zira", "Reminder for mum's birthday. Get the blue scarf from Larkin's, size medium. It's about forty pounds."),
]})
ask("memo-present", "sound", "I left myself a voice memo about mum's birthday present. What was I going to get her, and from where?",
    text("scarf"), text("Larkin"))

# --- Photos: a pharmacy label and a concert ticket ------------------------------------------------
EVERY, MAXDAY = 8, 6
media["pictures"].append({"file": "Phone/PXL_20260411_183002.png", "kind": "receipt", "lines": [
    "LANEWAY PHARMACY", "", "Patient: S OKAFOR", "Amoxicillin 500 mg capsules", "Qty: 21", "",
    f"Take 1 capsule every {EVERY} hours", f"Do not take more than {MAXDAY} in 24 hours",
    "Complete the course", "", "Dispensed 11/04/2026"]})
media["pictures"].append({"file": "Phone/PXL_20260402_201544.png", "kind": "letter", "lines": [
    "THE ARCADIA - LIVE", "", "NORTHERN LIGHTS ORCHESTRA", "Saturday 16 May 2026", "", "Doors 19:00   Show 19:45", "",
    "STALLS   ROW F   SEAT 12", "", "Price GBP 38.50", "Ticket no. 0048817"]})
ask("ticket-seat", "picture", "Where am I sitting at the orchestra concert? I took a photo of the ticket.",
    text("Row F", "row F", "F12", "F 12", "F-12", "F, seat", "F seat"), number(12, 0))
ask("label-dose", "picture",
    "How often should I take the capsules from the pharmacy, and what's the most I can take in a day? I took a photo of the label.",
    number(EVERY, 0), number(MAXDAY, 0))

# --- The inbox --------------------------------------------------------------------------------------
TRIP_COST, TRIP_FORM_DUE = 35, datetime.date(2026, 4, 24)
OLD_RENT, NEW_RENT, RENT_FROM = 1080, 1150, datetime.date(2026, 5, 1)
eml("Mail/01 school trip.eml", "Office <office@hillside-primary.example.org>", ME_MAIL, "Year 4 trip to the science museum",
    "Mon, 13 Apr 2026 08:30:00 +0100",
    f"Dear parents,\n\nYear 4 visit the science museum on 7 May. The trip costs {TRIP_COST} pounds. Please reply to this "
    f"e-mail to confirm your child's place by {TRIP_FORM_DUE.strftime('%A %d %B')}, and return the signed form.\n\n"
    "Hillside Primary office")
eml("Mail/02 rent.eml", "Harbor Lettings <lettings@harbor.example.com>", ME_MAIL, "Your rent from May",
    "Tue, 14 Apr 2026 10:02:00 +0100",
    f"Dear Sam,\n\nFollowing the annual review, the rent for 9 Elm Court will be {NEW_RENT:,} pounds a month from "
    f"{RENT_FROM.strftime('%d %B %Y')}. No action is needed: your standing order will be updated by us.\n\nHarbor Lettings")
eml("Mail/03 shift swap.eml", "Jo Reyes <jo.reyes@example.com>", ME_MAIL, "Swap Saturday?",
    "Tue, 14 Apr 2026 19:40:00 +0100",
    "Hi Sam, could you swap your Saturday 25th shift with my Sunday 26th? Let me know either way so I can tell the "
    "manager.\n\nJo")
eml("Mail/04 parcel.eml", "Parcelgo <noreply@parcelgo.example.com>", ME_MAIL, "Your parcel is on its way",
    "Wed, 15 Apr 2026 07:15:00 +0100", "Your parcel will arrive tomorrow between 10:00 and 14:00. No reply needed.")
eml("Mail/05 offers.eml", "Larkin's <offers@larkins.example.com>", ME_MAIL, "Spring sale: 20% off scarves",
    "Wed, 15 Apr 2026 12:00:00 +0100", "Our spring sale is on. Unsubscribe at any time.")
ask("mail-to-answer", "email", "Which of my e-mails do I still have to answer? Only the ones where someone is waiting for me.",
    text("school", "Hillside", "trip"), text("Jo", "swap"))
docx("Home/Tenancy agreement - 9 Elm Court.docx", [
    "# Assured shorthold tenancy agreement",
    "Property: 9 Elm Court. Tenant: Sam Okafor. Landlord's agent: Harbor Lettings.",
    "# Rent",
    f"The rent is {OLD_RENT:,} pounds per calendar month, paid by standing order on the 1st.",
    "# Deposit",
    f"A deposit of {OLD_RENT * 1.5:,.0f} pounds is held in a government-backed scheme.",
    "# Notice",
    "Either party may end the tenancy after the fixed term with two months' notice in writing.",
])
ask("rent-increase", "documents", "By how much is my rent going up, and from when?",
    number(NEW_RENT - OLD_RENT, 0), text("1 May", "May 1", "1st of May", "1st May", "2026-05-01", "01/05"))
ask("deposit", "documents", "How much deposit did I pay on the flat?", number(OLD_RENT * 1.5, 0))

# --- Tables -----------------------------------------------------------------------------------------
# Card export: one shop under several names.
card = [["Date", "Merchant", "Amount"]]
tesco_april = 0.0
for day in range(1, 31):
    for _ in range(rng.choice([0, 1, 1, 2])):
        m = rng.choice(["TESCO STORES 2231", "Tesco Express", "TESCO.COM", "ALDI 118", "BOOTS 0442", "SHELL HILL RD",
                        "Costa Coffee", "Tesco Express"])
        amount = round(rng.uniform(3, 80), 2)
        if "tesco" in m.lower():
            tesco_april += amount
        card.append([f"2026-04-{day:02d}", m, amount])
for day in (12, 27):  # March: not asked
    card.append([f"2026-03-{day}", "TESCO STORES 2231", round(rng.uniform(20, 60), 2)])
with open(path_of("Money/card statement.csv"), "w", encoding="utf-8", newline="\n") as f:
    for r in card:
        f.write(",".join(str(c) for c in r) + "\n")
ask("tesco-april", "table", "How much did I spend at Tesco in April? My card statement is in Money.",
    number(round(tesco_april, 2), 0.05))
# Electricity: one row a month; the largest row is the answer.
months = ["2025-10", "2025-11", "2025-12", "2026-01", "2026-02", "2026-03"]
power = [["Month", "kWh", "Cost"]]
for m in months:
    kwh = rng.randint(180, 420)
    power.append([m, kwh, round(kwh * 0.27 + 14.5, 2)])
top = max(power[1:], key=lambda r: r[1])
xlsx("Money/electricity.xlsx", "Usage", power)
ask("electricity-peak", "table", "Which month did we use the most electricity, and what did it cost?",
    text(top[0], datetime.date(int(top[0][:4]), int(top[0][5:]), 1).strftime("%B")), number(top[2], 0.01))
# Pocket money: totals per child.
kids = [["Week", "Child", "Chore", "Coins"]]
coins = {"Mia": 0, "Theo": 0, "Ada": 0}
for week in range(1, 9):
    for child in coins:
        for chore in rng.sample(["dishes", "bins", "tidy room", "feed cat", "hoover"], rng.randint(1, 3)):
            c = rng.choice([1, 2, 2, 3])
            coins[child] += c
            kids.append([week, child, chore, c])
best = max(coins, key=coins.get)
xlsx("Home/pocket money.xlsx", "Chores", kids)
ask("pocket-money-most", "table", "Which of the kids earned the most coins from chores in total, and how many?",
    text(best), number(coins[best], 0))

# --- Documents ----------------------------------------------------------------------------------------
RENEWAL, EXCESS = datetime.date(2026, 7, 14), 250
pdf("Car/insurance schedule 2025-26.pdf", [
    "# Motor insurance schedule", "", "Policyholder: Sam Okafor", "Vehicle: 2019 hatchback, reg LM19 KTP", "",
    "Period of cover: 15 July 2025 to 14 July 2026", f"Renewal date: {RENEWAL.strftime('%d %B %Y')}", "",
    f"Compulsory excess: {money(EXCESS)}", "Voluntary excess: 0.00", "Windscreen excess: 75.00"])
ask("insurance-renewal-excess", "documents", "When does my car insurance renew, and what's the excess if I claim?",
    text("14 July", "July 14", "14/07", "2026-07-14"), number(EXCESS, 0))
HALF_START, HALF_END = datetime.date(2026, 5, 25), datetime.date(2026, 5, 29)
pdf("School/term dates 2025-26.pdf", [
    "# Hillside Primary - term dates 2025-26", "",
    "Autumn term: 3 September - 19 December 2025 (half term 27 - 31 October)",
    "Spring term: 5 January - 27 March 2026 (half term 16 - 20 February)",
    f"Summer term: 13 April - 17 July 2026 (half term {HALF_START.day} - {HALF_END.day} May)", "",
    "INSET days: 2 September, 5 January, 8 June"])
ask("half-term", "documents", "When is the summer half term at Mia's school?",
    text("25", "25th"), text("29"))
LOAN_BALANCE, LOAN_MONTHLY = 6400, 320
pdf("Car/loan statement April.pdf", [
    "# Finance agreement - statement", "", "Agreement: CF-220931", "Statement date: 01/04/2026", "",
    f"Balance outstanding: {money(LOAN_BALANCE)}", f"Monthly payment: {money(LOAN_MONTHLY)}", "Interest rate: fixed, included in the payments"])
ask("loan-payments-left", "calc", "How many more monthly payments until the car loan is paid off?",
    number(LOAN_BALANCE / LOAN_MONTHLY, 0))
write("Home/recipes/lasagne.md", "# Lasagne (serves 4)\n\n- 500 g beef mince\n- 1 onion\n- 2 tins chopped tomatoes\n"
      "- 9 lasagne sheets\n- 500 ml white sauce\n- 100 g cheddar\n\nLayer, bake 40 minutes at 190 C.\n")
ask("lasagne-for-ten", "calc", "I'm making the lasagne for 10 people. How much mince do I need?", number(1250, 0))
write("Home/manuals/Vaillant boiler - user guide.txt", "Boiler user guide\n\nTo repressurise: open the filling loop until the "
      "gauge reads 1.5 bar, then close it.\nService once a year.\n")
write("Home/notes.txt", "Window cleaner: every 4th Wednesday, 18 pounds.\nBins: blue Monday, grey Thursday.\n")
ask("boiler-pressure", "workspace", "The boiler pressure is low. What should the gauge read after I top it up? There's a manual somewhere.",
    number(1.5, 0))
ask("bins-day", "workspace", "Which day do the blue bins go out?", text("Monday"))

# --- Across several files ------------------------------------------------------------------------------
LEFT = 300
ask("afford-garage-trip", "documents",
    f"I have {LEFT} pounds left this week. If I pay the garage bill and Mia's school trip, how much will I have left?",
    number(LEFT - GARAGE_BILL - TRIP_COST, 0))
ask("renewal-vs-halfterm", "documents",
    "Does my car insurance renew before or after the summer half term, and by how many days after its last day?",
    text("after"), number((RENEWAL - HALF_END).days, 0))
ask("test-before-halfterm", "documents",
    "Is Mia's maths test (the teacher mentioned it at parents' evening) before or after the summer half term?",
    text("after"))

b.finish("A second real-life set (made by make.py and reallife/make-media.ps1): another person's folder, written "
         "after the tuning on reallife/. For measuring only, never for tuning.")
