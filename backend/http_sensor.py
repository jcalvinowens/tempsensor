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

import json
import os
import sqlite3
import time
import traceback

from http.server import HTTPServer, BaseHTTPRequestHandler

class SensorRequestHandler(BaseHTTPRequestHandler):
	"""
	The sensor facing HTTP handler: accepts data from sensors, and also
	serves the freshest firmware blob when requested.
	"""

	def send_error(self, code, explain=""):
		self.send_response(code)

		if explain:
			self.send_header("Content-Length", len(explain))
			self.send_header("Content-Type", "text/plain;charset=utf-8")

		self.end_headers()

		if explain:
			self.wfile.write(explain)

	def do_GET(self):
		if self.path.startswith("/fw/"):
			if self.path.endswith(self.server.fw_sha):
				self.send_response(200)
				self.send_header("Content-Length", len(self.server.fw_blob))
				self.send_header("Content-Type", "application/octet-stream")
				self.end_headers()
				self.wfile.write(self.server.fw_blob)
				return

			elif self.path.endswith(self.server.beta_fw_sha):
				self.send_response(200)
				self.send_header("Content-Length", len(self.server.beta_fw_blob))
				self.send_header("Content-Type", "application/octet-stream")
				self.end_headers()
				self.wfile.write(self.server.beta_fw_blob)
				return

			self.send_error(404)

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
				"next_epoch": (rxtime // 60 + 5) * 60,
				"queue_interval": 300,
				"queue_count": 60,
			}

			location, want_send_delay_us = self.server.lookup_location(ser)

			if "outdoor" in location.lower():
				rjsond["queue_interval"] = 60
				rjsond["queue_count"] = 5

			if send_delay_us != want_send_delay_us:
				rjsond["next_send_delay_us"] = want_send_delay_us

			if sha != self.server.fw_sha:
				rjsond["new_fw"] = self.server.new_fw_url

			if location.startswith("(") and location.endswith(")"):
				if self.server.beta_fw_sha is not None:
					if sha != self.server.beta_fw_sha:
						rjsond["new_fw"] = self.server.beta_fw_url

				rjsond["queue_count"] = 0

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
	"""
	Server state object for the sensor facing HTTP server.
	"""

	def __init__(self, server_address, request_handler_class, db_path,
		     db_lock, locs_path, locs_lock, fw_sha, fw_blob, fw_url,
		     beta_fw_sha, beta_fw_blob, beta_fw_url):
		super().__init__(server_address, request_handler_class,
				 bind_and_activate=False)

		self.db_path = db_path
		self.db_lock = db_lock
		self.locs_path = locs_path
		self.locs_lock = locs_lock
		self.fw_sha = fw_sha
		self.fw_blob = fw_blob
		self.fw_url = fw_url
		self.beta_fw_sha = beta_fw_sha
		self.beta_fw_blob = beta_fw_blob
		self.beta_fw_url = beta_fw_url
		self.LOCS_MTIME = 0

	def init_db(self):
		"""
		Initialize the DB, if nobody else has yet.
		"""
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
