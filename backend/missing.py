#!/usr/bin/python3

import sqlite3
import itertools
import time

def get_missing_text():
	txt = ""
	con = sqlite3.connect("data.db")
	cur = con.cursor()

	def to_ranges(iterable):
		iterable = sorted(set(iterable))
		for key, group in itertools.groupby(enumerate(iterable),
						    lambda t: t[1] - t[0]):
			group = list(group)
			yield group[0][1], group[-1][1]

	def date_parse(epoch):
		return time.strftime("%c", time.localtime(epoch))

	locs = [x[0] for x in cur.execute(
		"select location from data group by location;"
	)]
	ts = [x[0] for x in cur.execute(
		"select (c_epoch / 60) as c_epoch_base from data "
		"where c_epoch > 0 group by c_epoch_base;"
	)]

	txt += f"Earliest entry: {date_parse(min(ts) * 60)}\n\n"

	for location in locs:
		if location.startswith("(") and location.endswith(")"):
			continue

		got_ts = set(x[0] for x in cur.execute(
			f"select (c_epoch / 60) as c_epoch_base from data where location='{location}' order by c_epoch_base;"
		))
		missing = []

		for timestamp in ts:
			if timestamp not in got_ts:
				missing.append(timestamp)

		txt += f"{location} missing {len(missing)} entries:\n"
		for miss_start, miss_end in to_ranges(missing):
			miss_len = miss_end - miss_start + 1
			txt += f"\t{date_parse(miss_start * 60)} --- "
			txt += f"{date_parse(miss_end * 60)}\t{miss_len}min!\n"
		txt += "\n"

	con.close()
	return txt
