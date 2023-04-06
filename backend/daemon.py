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
import os
import socket
import sqlite3
import ssl
import multiprocessing

from http_sensor import SensorHTTPServer, SensorRequestHandler
from http_ui import UIHTTPServer, UIRequestHandler

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

def sensor_worker(db_lock, locs_lock, addr, ssl_cert, ssl_key, fw_sha, fw_blob,
		  fw_url, beta_fw_sha, beta_fw_blob, beta_fw_url, sock):
	"""
	Worker process that handles requests from the actual sensors.
	"""
	httpd = SensorHTTPServer(addr, SensorRequestHandler,
				 "data.db", db_lock,
				 "locations.json", locs_lock,
				 fw_sha, fw_blob, fw_url,
				 beta_fw_sha, beta_fw_blob, beta_fw_url)

	sslctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
	sslctx.load_cert_chain(
		certfile=ssl_cert,
		keyfile=ssl_key,
	)

	httpd.init_db()
	httpd.socket = sslctx.wrap_socket(sock, server_side=True)
	httpd.serve_forever()

def ui_worker(addr, ssl_cert, ssl_key, sock):
	"""
	Worker process that handles requests for graphs from users.
	"""
	httpd = UIHTTPServer(addr, UIRequestHandler, "data.db")

	sslctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
	sslctx.load_cert_chain(
		certfile=ssl_cert,
		keyfile=ssl_key,
	)

	httpd.socket = sslctx.wrap_socket(sock, server_side=True)
	httpd.serve_forever()

parser = argparse.ArgumentParser()
parser.add_argument("--fw", type=str, help="Path to FW binary", required=True)
parser.add_argument("--elf", type=str, help="Path to ELF file", required=True)
parser.add_argument("--beta-fw", type=str, help="Path to FW binary", required=False)
parser.add_argument("--beta-elf", type=str, help="Path to ELF file", required=False)
parser.add_argument("--dst", type=str, help="Server hostname", required=True)
parser.add_argument("--ssl-cert", type=str, help="SSL Certificate", required=True)
parser.add_argument("--ssl-key", type=str, help="SSL Key", required=True)
parser.add_argument("--ui-port", type=int, help="UI port", default=443)
parser.add_argument("--sensor-port", type=int, help="Server port", default=4443)
parser.add_argument("--drop-uid", type=int, help="UID/GID to drop to")
parser.add_argument("--sensor-threads", type=int, help="Sensor thread count", default=16)
parser.add_argument("--ui-threads", type=int, help="UI thread count", default=4)
ARGS = parser.parse_args()

with open(ARGS.elf, "rb") as f:
	CURRENT_FW_SHA = hashlib.sha256(f.read()).hexdigest()[:16]

with open(ARGS.fw, "rb") as f:
	CURRENT_FW_BLOB = f.read()

CURRENT_FW_URL = f"https://{ARGS.dst}:{ARGS.sensor_port}/fw/{CURRENT_FW_SHA}"

BETA_FW_SHA = None
BETA_FW_BLOB = None
BETA_FW_URL = None

if ARGS.beta_elf or ARGS.beta_fw:
	with open(ARGS.beta_elf, "rb") as f:
		BETA_FW_SHA = hashlib.sha256(f.read()).hexdigest()[:16]

	with open(ARGS.beta_fw, "rb") as f:
		BETA_FW_BLOB = f.read()

	BETA_FW_URL = f"https://{ARGS.dst}:{ARGS.sensor_port}/fw/{BETA_FW_SHA}"

sensor_socks = [
	socket.socket(socket.AF_INET, socket.SOCK_STREAM)
	for _ in range(ARGS.sensor_threads)
]

for sock in sensor_socks:
	sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
	sock.bind(('', ARGS.sensor_port))
	sock.listen(10)

ui_socks = [
	socket.socket(socket.AF_INET, socket.SOCK_STREAM)
	for _ in range(ARGS.ui_threads)
]

for sock in ui_socks:
	sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
	sock.bind(('', ARGS.ui_port))
	sock.listen(10)


if os.getuid() == 0 and ARGS.drop_uid:
	os.setgid(ARGS.drop_uid)
	os.setuid(ARGS.drop_uid)

database_write_lock = multiprocessing.Lock()
locations_file_lock = multiprocessing.Lock()

for sock in sensor_socks:
	multiprocessing.Process(
		target=sensor_worker,
		args=(
			database_write_lock,
			locations_file_lock,
			('', ARGS.sensor_port),
			ARGS.ssl_cert,
			ARGS.ssl_key,
			CURRENT_FW_SHA,
			CURRENT_FW_BLOB,
			CURRENT_FW_URL,
			BETA_FW_SHA,
			BETA_FW_BLOB,
			BETA_FW_URL,
			sock,
		),
	).start()

for sock in ui_socks:
	multiprocessing.Process(
		target=ui_worker,
		args=(
			('', ARGS.ui_port),
			ARGS.ssl_cert,
			ARGS.ssl_key,
			sock,
		),
	).start()
