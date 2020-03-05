/*[INCLUDE-IF Sidecar18-SE]*/
/*******************************************************************************
 * Copyright (c) 2004, 2020 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/
package com.ibm.jvm.dtfjview.commands.infocommands;

import java.io.PrintStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.Iterator;
import java.util.LinkedList;
import java.util.List;
import java.util.Properties;

import com.ibm.dtfj.image.ImagePointer;
import com.ibm.dtfj.image.ImageSection;
import com.ibm.java.diagnostics.utils.IContext;
import com.ibm.java.diagnostics.utils.commands.CommandException;
import com.ibm.java.diagnostics.utils.plugins.DTFJPlugin;
import com.ibm.jvm.dtfjview.commands.BaseJdmpviewCommand;
import com.ibm.jvm.dtfjview.commands.helpers.Utils;

@DTFJPlugin(version = "1.*", runtime = false)
public class InfoMmapCommand extends BaseJdmpviewCommand {

	{
		addCommand("info mmap", "[address] [-verbose] [-sort:<size|address>]",
				"Outputs a list of all memory segments in the address space");
	}

	public void run(String command, String[] args, IContext context, PrintStream out) throws CommandException {
		boolean verbose = false;
		ImagePointer addressPointer = null;
		Comparator<ImageSection> sortOrder = null;

		if (initCommand(command, args, context, out)) {
			return; //processing already handled by super class
		}

		for (String arg : args) {
			if (Utils.SORT_BY_SIZE_FLAG.equals(arg)) {
				sortOrder = new SizeComparator();
			} else if (Utils.SORT_BY_ADDRESS_FLAG.equals(arg)) {
				sortOrder = new AddressComparator();
			} else if (Utils.VERBOSE_FLAG.equals(arg)) {
				verbose = true;
			} else {
				// longFromString will return a Long or null if we couldn't parse
				// the argument as a number.
				Long address = Utils.longFromString(arg);
				if (address == null) {
					out.println("\"info mmap\" -unknown parameter " + arg);
					return;
				} else {
					addressPointer = ctx.getAddressSpace().getPointer(address);
				}
			}
		}

		List<ImageSection> sortedSections = new LinkedList<>();
		Iterator<ImageSection> imageSections = ctx.getAddressSpace().getImageSections();
		while (imageSections.hasNext()) {
			sortedSections.add(imageSections.next());
		}
		if (sortOrder != null) {
			Collections.sort(sortedSections, sortOrder);
		}

		// Width for decimals can vary as we use the locales format (via %,d)
		long maxValue = ctx.getProcess().getPointerSize() <= 32 ? Integer.MAX_VALUE : Long.MAX_VALUE;
		int decWidth = String.format("(%,d)", maxValue).length(); //$NON-NLS-1$
		int hexWidth = String.format("%x", maxValue).length(); //$NON-NLS-1$

		out.printf("%-" + hexWidth + "s\t%-" + hexWidth + "s\t%-" + hexWidth + "s\t%-" + decWidth
				+ "s\tRead/Write/Execute", "Start Address", "End Address", "Size", "Size");
		out.println();
		long totalSize = 0;
		long totalSizeRwx = 0;
		Iterator<?> sortedIterator = sortedSections.iterator();
		while (sortedIterator.hasNext()) {
			ImageSection imageSection = (ImageSection) sortedIterator.next();
			if (addressPointer != null) {
				if (imageSection.getBaseAddress().getAddress() <= addressPointer.getAddress()
				&& imageSection.getBaseAddress().add(imageSection.getSize()).getAddress() > addressPointer.getAddress()) {
					// Print this address
				} else {
					continue;
				}
			}
			long startAddress = imageSection.getBaseAddress().getAddress();
			long size = imageSection.getSize();
			long endAddress = startAddress + size - 1;
			totalSize += size;
			String decSize = String.format("(%,d)", size);
			out.printf("0x%0" + hexWidth + "x\t0x%0" + hexWidth + "x\t0x%0" + hexWidth + "x\t%-" + decWidth + "s\t",
					startAddress, endAddress, size, decSize);

			Properties props = imageSection.getProperties();
			if (props != null) {
				boolean rwx = false;
				if (Boolean.TRUE.toString().equals(props.get("readable"))) {
					out.print("R");
					rwx = true;
				}
				if (Boolean.TRUE.toString().equals(props.get("writable"))) {
					out.print("W");
					rwx = true;
				}
				if (Boolean.TRUE.toString().equals(props.get("executable"))) {
					out.print("X");
					rwx = true;
				}
				if (rwx) {
					totalSizeRwx += size;
				}
			}
			if (verbose || addressPointer != null) {
				out.println();
				out.println("Name:\t" + imageSection.getName());
				if (props != null) {
					String[] keys = props.keySet().toArray(new String[0]);
					Arrays.sort(keys);
					ArrayList<String> table = new ArrayList<>(keys.length);
					int maxLen = 0;
					// We may have a lot of properties so print them out in two columns.
					for (String key : keys) {
						String formatted = String.format("%s=%s", key, props.get(key));
						table.add(formatted);
						maxLen = Math.max(maxLen, formatted.length());
					}
					Iterator<String> tableIterator = table.iterator();
					String tableFormatString = "\t%-" + maxLen + "s\t%-" + maxLen + "s\n";
					while (tableIterator.hasNext()) {
						out.printf(tableFormatString, tableIterator.next(),
								tableIterator.hasNext() ? tableIterator.next() : "");
					}
				}
			}
			out.println();
		}
		if (addressPointer == null) {
			if (totalSizeRwx > 0 && totalSize != totalSizeRwx) {
				out.printf("Total size (Readable, Writable or Executable): 0x%1$x (%1$,d) bytes\n", totalSizeRwx);
			}
			out.printf("Total size: 0x%1$x (%1$,d) bytes\n", totalSize);
		}
	}

	private static final class SizeComparator implements Comparator<ImageSection> {
		@Override
		public int compare(ImageSection o1, ImageSection o2) {
			long s1 = o1.getSize();
			long s2 = o2.getSize();
			return Long.compare(s1, s2);
		}
	}

	private static final class AddressComparator implements Comparator<ImageSection> {
		@Override
		public int compare(ImageSection o1, ImageSection o2) {
			long a1 = o1.getBaseAddress().getAddress();
			long a2 = o2.getBaseAddress().getAddress();
			return Long.compare(a1, a2);
		}
	}

	@Override
	public void printDetailedHelp(PrintStream out) {
		out.println("outputs a list of all memory segments in the address space\n\n" + 
				"parameters: none, an address in memory, sort flags, verbose output\n\n" +
				"If no address is specified outputs a list all memory segments (ImageSections) " +
				"in the address space with start address, size and properties\n" +
				"This output may be sorted using the " + Utils.SORT_BY_ADDRESS_FLAG + ", " +
				" or " + Utils.SORT_BY_SIZE_FLAG + " flags.\n" +
				"If " + Utils.VERBOSE_FLAG + " is specified then each memory segment is followed by any further detailed information." +
				"If an address is specified then just the memory segment containing that address is output.\n"
				 );
		
	}
}
