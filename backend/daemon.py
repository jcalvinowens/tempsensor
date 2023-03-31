#!/usr/bin/python3 -i

# Tempsensor HTTPS Backend
#
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

import argparse
import hashlib
import json
import os
import socket
import sqlite3
import ssl
import subprocess
import multiprocessing
import time
import traceback

import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler

import plots

# Simple server for tempsensors: client POSTs JSON with measurements, server
# repiles with ack and optionally a new FW verison to download and install.
#
# 1) Generate self-signed certs like this:
#
#	openssl req -x509 -noenc -newkey rsa:2048 -days 365 \
#		-keyout key.pem -out cert.pem
#
# 2) Generate a CSR like this:
#
#	openssl req -noenc -newkey rsa:2048 -keyout key.pem -out req.csr
#
# If you use self-signed certificates, use the cert from (1) as cert.pem in the
# source directory (main/). If you use an authority, use their root cert chain.

def date_parse(string):
	"""
	Parse lots of relative date strings. Nothing available in python is as
	good as date, sorry not sorry...
	"""
	return int(subprocess.check_output(
		["date", "+%s", "-d", string], stderr=subprocess.DEVNULL,
		stdin=subprocess.DEVNULL, shell=False,
	).decode('ascii', 'ignore').rstrip())

class SensorRequestHandler(BaseHTTPRequestHandler):
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

			start_epoch = date_parse(querydict.get("start", "yesterday"))
			end_epoch = date_parse(querydict.get("end", "now"))
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

		elif self.path.startswith("/fw/"):
			if not self.path.endswith(self.server.fw_sha):
				self.send_error(404)
				return

			self.send_response(200)
			self.send_header("Content-Length", len(self.server.fw_blob))
			self.send_header("Content-Type", "application/octet-stream")
			self.end_headers()
			self.wfile.write(self.server.fw_blob)
			return

		self.send_error(403)

	def do_POST(self):
		if 'Content-Length' not in self.headers:
			self.send_error(411)
			return

		if self.path.startswith("/data/"):
			ser = self.path.replace("/data/", "")
			if len(ser) != 12:
				self.send_error(403)
				return

			data = self.rfile.read(
				int(self.headers['Content-Length'])
			).decode('ascii', 'ignore')

			try:
				jsond = json.loads(data)
				sha = jsond["fw_sha"]
				ap_rssi = int(jsond["ap_rssi"])
				wake_reason = jsond["wake_reason"]
				send_delay_us = int(jsond["send_delay_us"])
				retries = int(jsond.get("retries", 0))
				wifi_retries = int(jsond.get("wifi_retries", 0))
				data = jsond["data"]

			except Exception as e:
				self.send_error(400, explain=traceback.format_exc().encode('utf-8'))
				return

			rxtime = int(time.time())
			rjsond = {
				"next_epoch": (rxtime // 60 + 1) * 60,
				"queue_interval": 60,
				"queue_count": 5,
			}

			location, want_send_delay_us = self.server.lookup_location(ser)

			if send_delay_us != want_send_delay_us:
				rjsond["next_send_delay_us"] = want_send_delay_us

			if sha != self.server.fw_sha:
				rjsond["new_fw"] = self.server.fw_url

			self.server.insert_db(sha, ser, rxtime,
				  ":".join(str(x) for x in self.client_address),
				  ap_rssi, wake_reason, location, retries,
				  wifi_retries, data)

			response = json.dumps(rjsond)
			self.send_response(200)
			self.send_header("Content-Length", len(response))
			self.end_headers()
			self.wfile.write(response.encode('ascii', 'ignore'))
			self.wfile.flush()
			return

		self.send_error(403)

class SensorHTTPServer(HTTPServer):
	def __init__(self, server_address, request_handler_class, db_path,
		     db_lock, locs_path, locs_lock, fw_sha, fw_blob, fw_url):
		super().__init__(server_address, request_handler_class,
				 bind_and_activate=False)

		self.db_path = db_path
		self.db_lock = db_lock
		self.locs_path = locs_path
		self.locs_lock = locs_lock
		self.fw_sha = fw_sha
		self.fw_blob = fw_blob
		self.fw_url = fw_url
		self.LOCS_MTIME = 0

	def init_db(self):
		with self.db_lock:
			con = sqlite3.connect(self.db_path)
			cur = con.cursor()
			cur.execute("""
			CREATE TABLE IF NOT EXISTS data(
				location, c_epoch, s_epoch,
				r_temp, temp,
				r_humidity, humidity
			);
			""")
			cur.execute("""
			CREATE TABLE IF NOT EXISTS submissions(
				fw_sha, hw_serial, s_epoch, src_ip, ap_rssi,
				wake_reason, location, retries, wifi_retries,
				nr_samples
			);
			""")
			con.commit()
			con.close()

	def lookup_location(self, hw_serial):
		"""
		Cache the contents of locations.json based on its mtime.

		This also takes care of assigning each sensor a unique
		send_delay value by simply counting up from zero.
		"""
		with self.locs_lock:
			if os.stat(self.locs_path).st_mtime != self.LOCS_MTIME:
				with open(self.locs_path, "rb") as f:
					self.LOCS = json.loads(f.read())
					self.LOCS_MTIME = os.fstat(f.fileno()).st_mtime
					self.LOCS_DELAYS = {
						k: 100 * 1000 * i for i, k
						in enumerate(self.LOCS.keys())
					}

		return (
			self.LOCS.get(hw_serial, "(unknown)"),
			self.LOCS_DELAYS.get(hw_serial, 0),
		)

	def get_db_locations(self, epoch_start, epoch_end):
		"""
		Dump all locations that have data between start/end.
		"""
		with self.db_lock:
			con = sqlite3.connect(self.db_path)
			cur = con.cursor()
			cur.execute(f"""
			SELECT location FROM data WHERE c_epoch >= {epoch_start}
			and c_epoch <= {epoch_end} GROUP BY location;
			""")
			rows = cur.fetchall()
			con.close()

			return [x[0] for x in rows]

	def insert_db(self, fw_sha, hw_serial, s_epoch, src_ip, ap_rssi,
		      wake_reason, location, retries, wifi_retries, samples_arr):
		"""
		Insert a submission and its associated samples into the database.
		"""
		with self.db_lock:
			con = sqlite3.connect(self.db_path)
			cur = con.cursor()
			cur.execute(f"""
			INSERT INTO submissions VALUES(
				'{fw_sha}', '{hw_serial}', {s_epoch}, '{src_ip}',
				{ap_rssi}, '{wake_reason}', '{location}', {retries},
				{wifi_retries}, {len(samples_arr)}
			);
			""")

			for d in samples_arr:
				c_epoch = int(d["epoch"])
				r_temp = d["temperature"]
				r_humi = d["humidity"]

				# Temperature: 14-bit Resolution (unit 0.01007080078C)
				# 16-digit result, with two least sig bits always zero
				# Raw mapping: [0, 16384] == [-40C, 125C]
				f_temp = round(float(r_temp) / 65536.0 * 165.0 - 40.0, 2)

				# Humidity: 14-bit Resolution (unit 0.00610351562 %RH)
				# 16-digit result, with two least sig bits always zero
				# Raw mapping: [0, 16384] == [0%, 100%]
				f_humi = round(float(r_humi) / 65536.0 * 100.0, 2)

				cur.execute(f"""
				INSERT INTO data VALUES(
					'{location}', {c_epoch}, {s_epoch},
					{r_temp >> 2}, {f_temp},
					{r_humi >> 2}, {f_humi}
				);
				""")

			con.commit()
			con.close()

def worker(db_lock, locs_lock, addr, ssl_cert, ssl_key, fw_sha, fw_blob, fw_url,
	   sock):
	httpd = SensorHTTPServer(addr, SensorRequestHandler,
				 "data.db", db_lock,
				 "locations.json", locs_lock,
				 fw_sha, fw_blob, fw_url)

	sslctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
	sslctx.load_cert_chain(
		certfile=ssl_cert,
		keyfile=ssl_key,
	)

	httpd.init_db()
	httpd.socket = sslctx.wrap_socket(sock, server_side=True)
	httpd.serve_forever()

parser = argparse.ArgumentParser()
parser.add_argument("--fw", type=str, help="Path to FW binary", required=True)
parser.add_argument("--elf", type=str, help="Path to ELF file", required=True)
parser.add_argument("--dst", type=str, help="Server hostname", required=True)
parser.add_argument("--ssl-cert", type=str, help="SSL Certificate", required=True)
parser.add_argument("--ssl-key", type=str, help="SSL Key", required=True)
parser.add_argument("--locations-file", type=str, help="Locations JSON", required=True)
parser.add_argument("--port", type=int, help="Server port", default=443)
parser.add_argument("--drop-uid", type=int, help="UID/GID to drop to")
parser.add_argument("--threads", type=int, help="Thread count", default=16)
ARGS = parser.parse_args()

with open(ARGS.elf, "rb") as f:
	CURRENT_FW_SHA = hashlib.sha256(f.read()).hexdigest()[:16]

with open(ARGS.fw, "rb") as f:
	CURRENT_FW_BLOB = f.read()

socks = [
	socket.socket(socket.AF_INET, socket.SOCK_STREAM)
	for _ in range(ARGS.threads)
]

for sock in socks:
	sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
	sock.bind(('', ARGS.port))
	sock.listen(10)

if os.getuid() == 0 and ARGS.drop_uid:
	os.setgid(ARGS.drop_uid)
	os.setuid(ARGS.drop_uid)

database_write_lock = multiprocessing.Lock()
locations_file_lock = multiprocessing.Lock()

for sock in socks:
	multiprocessing.Process(
		target=worker,
		args=(
			database_write_lock,
			locations_file_lock,
			('', ARGS.port),
			ARGS.ssl_cert,
			ARGS.ssl_key,
			CURRENT_FW_SHA,
			CURRENT_FW_BLOB,
			f"https://{ARGS.dst}:{ARGS.port}/fw/{CURRENT_FW_SHA}",
			sock,
		),
	).start()
