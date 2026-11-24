"""Works out the answers of the table questions in questions.json from the data itself, with the
standard library only (no pandas, no openpyxl), so the expected answers do not come from the app's
own query_table. Usage: python3 compute_answers.py <agent-samples folder>"""
import csv
import re
import sys
import zipfile
import xml.etree.ElementTree as ET
from collections import defaultdict

root = sys.argv[1] if len(sys.argv) > 1 else "/mnt/c/cpp/agent-samples"


def rows(name):
    with open(f"{root}/data/{name}", newline="") as f:
        return list(csv.DictReader(f))


def xlsx(name):
    ns = {"m": "http://schemas.openxmlformats.org/spreadsheetml/2006/main"}
    z = zipfile.ZipFile(f"{root}/data/{name}")
    shared = []
    if "xl/sharedStrings.xml" in z.namelist():
        for si in ET.fromstring(z.read("xl/sharedStrings.xml")).findall("m:si", ns):
            shared.append("".join(t.text or "" for t in si.iter(f"{{{ns['m']}}}t")))
    sheet = ET.fromstring(z.read("xl/worksheets/sheet1.xml"))
    table = []
    for r in sheet.iter(f"{{{ns['m']}}}row"):
        cells = {}
        for c in r.findall("m:c", ns):
            col = re.match(r"[A-Z]+", c.get("r")).group()
            v = c.find("m:v", ns)
            text = "" if v is None else v.text
            if c.get("t") == "s":
                text = shared[int(text)]
            cells[col] = text
        table.append(cells)
    header = table[0]
    return [{header[k].strip(): v for k, v in r.items() if k in header} for r in table[1:]]


def num(s):
    return float(str(s).replace("$", "").replace(",", "").strip() or 0)


def group(data, key, value, fn):
    g = defaultdict(list)
    for r in data:
        if r[value] not in ("", "NA"):
            g[r[key]].append(num(r[value]))
    return {k: fn(v) for k, v in g.items()}


avg = lambda v: sum(v) / len(v)

tips = rows("tips.csv")
print("tips rows", len(tips))
print("tips smokers", sum(r["smoker"] == "Yes" for r in tips))
print("tips avg total_bill by time", group(tips, "time", "total_bill", avg))
print("tips avg tip by day", group(tips, "day", "tip", avg))
print("tips biggest party", max(int(r["size"]) for r in tips),
      [(r["total_bill"], r["tip"]) for r in tips if int(r["size"]) == 6])

pen = rows("penguins.csv")
print("penguins rows", len(pen))
print("penguins by species", {k: len(v) for k, v in group(pen, "species", "body_mass_g", list).items()})
print("penguins avg mass by island", group(pen, "island", "body_mass_g", avg))
print("gentoo male flipper avg", avg([num(r["flipper_length_mm"]) for r in pen
                                      if r["species"] == "Gentoo" and r["sex"] == "MALE"]))
print("penguins missing sex", sum(r["sex"] in ("", "NA") for r in pen))

fin = xlsx("Financial Sample.xlsx")
print("financial rows", len(fin), "columns", list(fin[0].keys()))
profit = group(fin, "Country", "Profit", sum)
total = sum(profit.values())
print("profit by country", profit, "total", total)
print("government 2014 sales", sum(num(r["Sales"]) for r in fin if r["Segment"] == "Government" and r["Year"] == "2014"))
print("units by product", group(fin, "Product", "Units Sold", sum))
print("discount band none sales", sum(num(r["Sales"]) for r in fin if r["Discount Band"].strip() == "None"))
species = defaultdict(int)
for r in pen:
    species[r["species"]] += 1
print("penguins rows by species", dict(species))
print("pdf density finland", 5.5e6 / 338424)
print("price rise", (5050 - 4200) / 4200 * 100, 5050 * 5050 / 4200)
print("tip split", 86.40 * 1.18 / 3)
