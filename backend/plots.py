#!/usr/bin/python3

# Ugly python to generate gnuplot scripts on the fly
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

import subprocess
import tempfile

def gnuplot_preamble(fmt="svg", x=1600, y=800):
	return f"""
	set terminal {fmt} size {x} {y}
	set datafile separator '|'
	set xdata time
	set timefmt "%s"
	set format x "%d%H%MZ"
	"""

def gnuplot_colors():
	return """
	set linetype  1 lc rgb "dark-violet" lw 1
	set linetype  2 lc rgb "#009e83" lw 1
	set linetype  3 lc rgb "#56b4e9" lw 1
	set linetype  4 lc rgb "#e69f00" lw 1
	set linetype  5 lc rgb "#f0e442" lw 1
	set linetype  6 lc rgb "#0072b2" lw 1
	set linetype  7 lc rgb "#32cd32"lw 1
	set linetype  8 lc rgb "black"   lw 1
	set linetype  9 lc rgb "gray50"  lw 1
	set linetype 10 lc rgb "#e51e10" lw 1
	set linetype 11 lc rgb "dark-violet" lw 1 dashtype 4
	set linetype 12 lc rgb "#009e83" lw 1 dashtype 4
	set linetype 13 lc rgb "#56b4e9" lw 1 dashtype 4
	set linetype 14 lc rgb "#e69f00" lw 1 dashtype 4
	set linetype 15 lc rgb "#f0e442" lw 1 dashtype 4
	set linetype 16 lc rgb "#0072b2" lw 1 dashtype 4
	set linetype 17 lc rgb "#32cd32"lw 1 dashtype 4
	set linetype 18 lc rgb "black"   lw 1 dashtype 4
	set linetype 19 lc rgb "gray50"  lw 1 dashtype 4
	set linetype 20 lc rgb "#e51e10" lw 1 dashtype 4
	set linetype 21 lc rgb "dark-violet" lw 1 dashtype 7
	set linetype 22 lc rgb "#009e83" lw 1 dashtype 7
	set linetype 23 lc rgb "#56b4e9" lw 1 dashtype 7
	set linetype 24 lc rgb "#e69f00" lw 1 dashtype 7
	set linetype 25 lc rgb "#f0e442" lw 1 dashtype 7
	set linetype 26 lc rgb "#0072b2" lw 1 dashtype 7
	set linetype 27 lc rgb "#32cd32"lw 1 dashtype 7
	set linetype 28 lc rgb "black"   lw 1 dashtype 7
	set linetype 29 lc rgb "gray50"  lw 1 dashtype 7
	set linetype 30 lc rgb "#e51e10" lw 1 dashtype 7
	set linetype cycle 30
	"""

def gnuplot_multiplot_begin(x=1, y=1):
	return f"""
	set multiplot layout {y},{x} rowsfirst
	"""

def gnuplot_plot_preamble(title, xlabel, ylabel, xtics=1800, ytics=1):
	return f"""
	set title '{title}'
	set xlabel '{xlabel}'
	set ylabel '{ylabel}'
	set key font ",6" outside rmargin
	set xtics font ",6"
	set xtics rotate by 270
	set xtics {xtics} out nomirror
	set ytics {ytics} out
	set ytics mirror
	set grid ytics noxtics
	"""

def gnuplot_plot(location, field, epoch_start, epoch_end, spec="1", first=True,
		 more=False, dbname="data.db"):
	return ("plot " if first else "") + f"""'< sqlite3 {dbname} "select {field},c_epoch from data where location=''{location}'' and c_epoch >= {epoch_start} and c_epoch <= {epoch_end} order by c_epoch;"' using 2:{spec} with lines title '{location}'""" + (", \\" if more else "")

def gnuplot_multiplot_end():
	return f"""
	unset multiplot
	"""

def gnuplot(*script):
	with tempfile.NamedTemporaryFile() as f:
		f.write('\n'.join(script).encode('utf-8'))
		f.flush()

		return subprocess.check_output(
			["/usr/bin/gnuplot", "-p", "-c", f.name],
			stdin=subprocess.DEVNULL,
			stderr=subprocess.PIPE,
		)
