"""Keeps the shop's stock levels and orders more when they run low."""

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
