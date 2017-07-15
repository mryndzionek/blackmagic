#!/usr/bin/env python

# hexprog.py: Python application to flash a target with an Intel hex file
# Copyright (C) 2011  Black Sphere Technologies
# Written by Gareth McMullin <gareth@blacksphere.co.nz>
# 
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

import gdb
import struct
import time

CHECKSUM_ADDR = 0x90300000
META_ADDR = 0x90500000

def flash_write_hex(target, hexfile, progress_cb=None):
	target.flash_probe()
	f = open(hexfile)
	addrhi = 0
	calc_checksum = 0
	file_checksum = 0
	for line in f:
		if line[0] != ':': raise Exception("Error in hex file")
		reclen = int(line[1:3], 16)
		addrlo = int(line[3:7], 16)
		rectype = int(line[7:9], 16);
		if sum(ord(x) for x in gdb.unhexify(line[1:11+reclen*2])) & 0xff != 0:
			raise Exception("Checksum error in hex file") 
		if rectype == 0: # Data record
			addr = (addrhi << 16) + addrlo
			data = gdb.unhexify(line[9:9+reclen*2])
			if addr < CHECKSUM_ADDR:
				calc_checksum += sum(ord(x) for x in data)
			elif addr == CHECKSUM_ADDR:
				file_checksum = int(gdb.hexify(data), 16)
			elif addr == META_ADDR:
				hex_chipid = int(gdb.hexify(data[2:4]), 16)
			target.flash_write_prepare(addr, data)
			pass
		elif rectype == 4: # High address record
			addrhi = int(line[9:13], 16)
			pass
		elif rectype == 5: # Entry record
			pass
		elif rectype == 1: # End of file record
			break 
		else:
			raise Exception("Invalid record in hex file")

	if (calc_checksum & 0xFFFF) != file_checksum:
		print("Calculated checksum doesn't match hex file checksum")
		exit(-1)

	chipid = int(target.monitor("siliconid")[0], 16) & 0xFFFF
	chipid = ((chipid & 0xFF) << 8) + (chipid >> 8)
	if chipid != hex_chipid:
		print("Hex file chip id doesn't match the actual chip id")
		exit(-1)

	try:
		target.flash_commit(progress_cb)
	except:
		print "Flash write failed! Is device protected?\n"
		exit(-1)

	checksum = int(target.monitor("checksum")[0], 16)
	if calc_checksum != checksum:
		print("Calculated checksum doesn't match the MCU checksum")
		exit(-1)


if __name__ == "__main__":
	from serial import Serial, SerialException
	from sys import argv, platform, stdout
	from getopt import getopt

	if platform == "linux2":
		print ("\x1b\x5b\x48\x1b\x5b\x32\x4a") # clear terminal screen
	print("Black Magic Probe -- Target Production Programming Tool -- version 1.0")
	print "Copyright (C) 2011  Black Sphere Technologies"
	print "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>"
	print("")

	dev = "COM1" if platform == "win32" else "/dev/ttyACM0"
	baud = 115200
	targetno = 1

	try:
		opts, args = getopt(argv[1:], "sd:b:t:rR")
		for opt in opts:
			if opt[0] == "-b": baud = int(opt[1])
			elif opt[0] == "-d": dev = opt[1]
			elif opt[0] == "-t": targetno = int(opt[1])
			else: raise Exception()

		hexfile = args[0]
	except:
		print("Usage %s [-d <dev>] [-b <baudrate>] [-t <n>] <filename>" % argv[0])
		print("\t-d : Use target on interface <dev> (default: %s)" % dev)
		print("\t-b : Set device baudrate (default: %d)" % baud)
		print("\t-t : Connect to target #n (default: %d)" % targetno)
		print("")
		exit(-1)

	try:
		s = Serial(dev, baud, timeout=3)
		s.setDTR(1)
		while s.read(1024):
			pass

		target = gdb.Target(s)
		
	except SerialException, e:
		print("FATAL: Failed to open serial device!\n%s\n" % e[0])
		exit(-1)

	try:
		r = target.monitor("version")
		for s in r: print s,
	except SerialException, e:
		print("FATAL: Serial communication failure!\n%s\n" % e[0])
		exit(-1)
	except: pass

	target.monitor("srst_before_connect enable")
	print "Target device scan:"
	targetlist = None
	r = target.monitor("swdp_scan")
	for s in r: 
		print s,
	print

	r = target.monitor("targets")
	for s in r: 
		if s.startswith("No. Att Driver"): targetlist = []
		try:
			if type(targetlist) is list: 
				targetlist.append(int(s[:2]))
		except: pass

	#if not targetlist:
	#	print("FATAL: No usable targets found!\n")
	#	exit(-1)

	if targetlist and (targetno not in targetlist):
		print("WARNING: Selected target %d not available, using %d" % (targetno, targetlist[0]))
		targetno = targetlist[0]

	print("Attaching to target %d." % targetno)
	target.attach(targetno)
	time.sleep(0.1)

	for m in target.flash_probe():
		print("FLASH memory -- Offset: 0x%X  BlockSize:0x%X\n" % (m.offset, m.blocksize))

	def progress(percent):
		print ("Progress: %d%%\r" % percent),
		stdout.flush()

	target.monitor("erase_mass")

	print("Programming target")
	flash_write_hex(target, hexfile, progress)

	print("Resetting target")
	target.reset()

	target.detach()

	print("\nAll operations complete!\n")

