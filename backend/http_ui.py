#!/usr/bin/python3 -i

# Copyright (C) 2023 Calvin Owens <jcalvinowens@gmail.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

import sqlite3
import subprocess
import traceback

import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler
import plots

class UIRequestHandler(BaseHTTPRequestHandler):
	"""
	User facing HTTP server: only serves graphs of data.
	"""

	@staticmethod
	def date_parse(string):
		"""
		Parse lots of relative date strings into an int epoch. Nothing
		available in python is as good as date, sorry not sorry...
		"""
		return int(subprocess.check_output(
			["date", "+%s", "-d", string], stderr=subprocess.DEVNULL,
			stdin=subprocess.DEVNULL, shell=False,
		).decode('ascii', 'ignore').rstrip())

	def send_error(self, code, explain=""):
		self.send_response(code)

		if explain:
			self.send_header("Content-Length", len(explain))
			self.send_header("Content-Type", "text/plain;charset=utf-8")

		self.end_headers()

		if explain:
			self.wfile.write(explain)

	def gnuplot_temperature(self, ext, start_epoch, end_epoch, x, y, locs=None):
		xarr = [
			plots.gnuplot_preamble(ext or "svg", x, y),
			plots.gnuplot_colors(),
			plots.gnuplot_plot_preamble("Temperature Sensor Data",
						    "UTC Time", "Temperature (F)"),
		]

		if locs is None or (len(locs) == 1 and locs[0] == ''):
			locs = self.server.get_db_locations(start_epoch, end_epoch)

		for i, loc in enumerate(locs):
			xarr.append(plots.gnuplot_plot(loc, "temp", start_epoch,
						       end_epoch, "($1*1.8+32)",
						       first=(i == 0),
						       more=not (i == len(locs) - 1)))

		return plots.gnuplot(*xarr)

	def gnuplot_humidity(self, ext, start_epoch, end_epoch, x, y, locs=None):
		xarr = [
			plots.gnuplot_preamble(ext or "svg", x, y),
			plots.gnuplot_colors(),
			plots.gnuplot_plot_preamble("Humidity Sensor Data",
					      "UTC Time", "Relative Humidity (%)"),
		]

		if locs is None or (len(locs) == 1 and locs[0] == ''):
			locs = self.server.get_db_locations(start_epoch, end_epoch)

		for i, loc in enumerate(locs):
			xarr.append(plots.gnuplot_plot(loc, "humidity", start_epoch,
						       end_epoch, "1",
						       first=(i == 0),
						       more=not (i == len(locs) - 1)))

		return plots.gnuplot(*xarr)

	def parse_get_querydict(self):
		ret = {}

		if "?" in self.path:
			self.path, query = self.path.split('?')
			params = query.split(',')
			for p in params:
				k, v = p.split('=')
				ret[urllib.parse.unquote(k)] = \
					urllib.parse.unquote(v)

		return ret

	def do_GET(self):
		try:
			querydict = self.parse_get_querydict()
			start_epoch = UIRequestHandler.date_parse(querydict.get("start", "yesterday"))
			end_epoch = UIRequestHandler.date_parse(querydict.get("end", "now"))
			locs = [x for x in querydict.get("locs", "").split('+')]

		except Exception as e:
			self.send_error(400, explain=traceback.format_exc().encode('utf-8'))
			return

		if self.path.startswith("/graph/"):
			req = self.path[len("/graph/"):]

			try:
				if '.' in req:
					name, extension = req.split('.')
				else:
					name, extension = req, None

				ttype, ctype = {
					"svg": ("svg", "image/svg+xml"),
					"txt": ("dumb", "text/plain"),
				}.get(extension, ("svg", "image/svg+xml"))

				if ttype == "dumb":
					x = 95
					y = 32
				else:
					x = 800
					y = 800

				plotdata = {
					"temp": self.gnuplot_temperature,
					"humidity": self.gnuplot_humidity,
				}[name](ttype, start_epoch, end_epoch, x, y, locs)

				self.send_response(200)
				self.send_header("Content-Length", len(plotdata))
				self.send_header("Content-Type", ctype)
				self.end_headers()
				self.wfile.write(plotdata)
				return

			except subprocess.CalledProcessError as e:
				self.send_error(503, explain=e.stderr)
				return

			except KeyError as e:
				self.send_error(404, explain=traceback.format_exc().encode('utf-8'))
				return

			except Exception as e:
				self.send_error(500, explain=traceback.format_exc().encode('utf-8'))
				return

		self.send_error(403)

	def do_POST(self):
		self.send_error(403)

class UIHTTPServer(HTTPServer):
	"""
	Server state object for the user-facing HTTP server.
	"""

	def __init__(self, server_address, request_handler_class, db_path):
		super().__init__(server_address, request_handler_class,
				 bind_and_activate=False)
		self.db_path = db_path

	def get_db_locations(self, epoch_start, epoch_end):
		"""
		Dump all locations that have data between start/end.
		"""
		con = sqlite3.connect(self.db_path)
		cur = con.cursor()
		cur.execute(f"""
		SELECT location FROM data WHERE c_epoch >= {epoch_start}
		and c_epoch <= {epoch_end} GROUP BY location;
		""")
		rows = cur.fetchall()
		con.close()

		return [x[0] for x in rows]
