/*[INCLUDE-IF Sidecar18-SE]*/
/*******************************************************************************
 * Copyright (c) 2020, 2020 IBM Corp. and others
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
 *
 * Portions Copyright (c) 1999-2003 Apple Computer, Inc. All Rights
 * Reserved.
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *******************************************************************************/
package com.ibm.dtfj.corereaders;

import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.IOException;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.Set;
import java.util.TreeSet;

import javax.imageio.stream.ImageInputStream;

import com.ibm.dtfj.image.ImageModule;

/**
 * TODO document this class
 */
@SuppressWarnings("boxing")
public final class NewMachoDump extends CoreReaderSupport {

	private static abstract class Command {

		static final int LC_SEGMENT_64 = 0x19;

		static final int LC_THREAD = 0x4;

		static Command read(DumpReader reader) throws IOException {
			long type = reader.readUnsignedInt();
			long size = reader.readUnsignedInt();

			if (type == LC_SEGMENT_64) {
				return new Segment(reader, size);
			} else if (type == LC_THREAD) {
				return new Thread(reader, size);
			} else {
				final class OtherCommand extends Command {
					OtherCommand(DumpReader reader, long type, long size) throws IOException {
						super(type, size);

						if (DEBUG) {
							long start = reader.getPosition() - 8;

							System.out.printf("Segment @ %016X: type=%d size=%d%n", //$NON-NLS-1$
									Long.valueOf(start), Long.valueOf(type), Long.valueOf(size));
						}

						reader.seek(reader.getPosition() - 8 + size);
					}
				}

				return new OtherCommand(reader, type, size);
			}
		}

		final long cmdType;
		final long cmdSize;

		Command(long type, long size) {
			super();
			this.cmdType = type;
			this.cmdSize = size;
		}

	}

	@SuppressWarnings("unused")
	private static final class Header {

		// Mach-O magic numbers
		static final int MACHO_64 = 0xFEEDFACF;
		static final int MACHO_64_REV = 0xCFFAEDFE;

		static boolean isMACHO(int magic) {
			return (magic == MACHO_64) || (magic == MACHO_64_REV);
		}

		final int cpuType;
		final int cpuSubType;
		final int fileType;
		final int numCommands;
		final long sizeOfCommands;
		final int flags;

		Header(DumpReader reader) throws IOException {
			super();
			reader.readInt(); // magic
			this.cpuType = reader.readInt();
			this.cpuSubType = reader.readInt();
			this.fileType = reader.readInt();
			this.numCommands = reader.readInt();
			this.sizeOfCommands = reader.readUnsignedInt();
			this.flags = reader.readInt();
			reader.readInt(); // reserved
		}

	}

	private static final class MachFile {

		private static void dumpSegments(Command[] commands) {
			for (Command command : commands) {
				if (command.cmdType == Command.LC_SEGMENT_64) {
					Segment segment = (Segment) command;
					String format;

					if (segment.fileSize == 0) {
						format = "Segment: %s %016X-%016X%n"; //$NON-NLS-1$
					} else {
						format = "Segment: %s %016X-%016X %016X-%016X%n"; //$NON-NLS-1$
					}

					System.out.printf(format, segment.segmentName, //
							segment.vmaddr, segment.vmaddr + segment.vmsize - 1, // virtual address range
							segment.fileOffset, segment.fileOffset + segment.fileSize - 1);

					for (Section section : segment.sections) {
						System.out.printf("  Section: %s %016X-%016X%n", //$NON-NLS-1$
								section.sectionName, section.address, section.address + section.size - 1);
					}
				} else if (command.cmdType == Command.LC_THREAD) {
					Thread thread = (Thread) command;

					for (Thread.State state : thread.states) {
						System.out.printf("Thread flavor=%d%n", state.flavor); //$NON-NLS-1$

						for (Map.Entry<String, Number> entry : state.registers.entrySet()) {
							System.out.printf("  %-20s 0x%X%n", entry.getKey(), entry.getValue()); //$NON-NLS-1$
						}
					}
				}
			}
		}

		final DumpReader reader;

		final Header header;

		final Command[] commands;

		MachFile(DumpReader reader, long offset) throws IOException {
			super();

			reader.seek(offset);

			this.reader = reader;
			this.header = new Header(reader);
			this.commands = new Command[header.numCommands];

			for (int i = 0; i < commands.length; ++i) {
				commands[i] = Command.read(reader);
			}

			if (DEBUG) {
				dumpSegments(commands);
			}
		}

		long getFileOffset(long vmOffset) {
			for (Command command : commands) {
				if (command.cmdType == Command.LC_SEGMENT_64) {
					Segment segment = (Segment) command;
					long base = segment.vmaddr;

					if ((base <= vmOffset) && (vmOffset < base + segment.vmsize)) {
						if (segment.fileSize != 0) {
							return vmOffset - base + segment.fileOffset;
						}
					}
				}
			}

			return 0;
		}

		List<MemoryRange> getMemoryRangesWithOffset(long vmOffset) {
			List<MemoryRange> ranges = new ArrayList<>();

			for (Command command : commands) {
				if (command.cmdType == Command.LC_SEGMENT_64) {
					Segment segment = (Segment) command;

					ranges.add(segment.asMemoryRangeWithOffset(vmOffset));
				}
			}

			return ranges;
		}

	}

	@SuppressWarnings("unused")
	private static class Section {

		final String sectionName;
		final String segmentName;
		final long address;
		final long size;
		final long offset;
		final long alignment;
		final long relocOffset;
		final long numReloc;
		final long flags;

		Section(DumpReader reader) throws IOException {
			super();
			this.sectionName = getStringFromAsciiChars(reader.readBytes(16));
			this.segmentName = getStringFromAsciiChars(reader.readBytes(16));
			this.address = reader.readLong();
			this.size = reader.readLong();
			this.offset = reader.readUnsignedInt();
			this.alignment = reader.readUnsignedInt();
			this.relocOffset = reader.readUnsignedInt();
			this.numReloc = reader.readUnsignedInt();
			this.flags = reader.readUnsignedInt();
			reader.readUnsignedInt(); // reserved
			reader.readUnsignedInt(); // reserved
			reader.readUnsignedInt(); // reserved
		}

	}

	@SuppressWarnings("unused")
	private static final class Segment extends Command {

		static final int VM_PROT_READ = 0x1;
		static final int VM_PROT_WRITE = 0x2;
		static final int VM_PROT_EXECUTE = 0x4;

		final String segmentName;
		final long vmaddr;
		final long vmsize;
		final long fileOffset;
		final long fileSize;
		final int maxProt;
		final int initProt;
		final int numSections;
		final long flags;
		final Section[] sections;

		Segment(DumpReader reader, long size) throws IOException {
			super(LC_SEGMENT_64, size);
			this.segmentName = getStringFromAsciiChars(reader.readBytes(16));
			this.vmaddr = reader.readLong();
			this.vmsize = reader.readLong();
			this.fileOffset = reader.readLong();
			this.fileSize = reader.readLong();
			this.maxProt = reader.readInt();
			this.initProt = reader.readInt();
			this.numSections = reader.readInt();
			this.flags = reader.readUnsignedInt();
			this.sections = new Section[numSections];

			for (int i = 0; i < numSections; ++i) {
				sections[i] = new Section(reader);
			}
		}

		MemoryRange asMemoryRange() {
			return asMemoryRangeWithOffset(0);
		}

		MemoryRange asMemoryRangeWithOffset(long vmOffset) {
			int asid = 0;
			boolean shared = false;
			boolean readOnly = (maxProt & VM_PROT_WRITE) == 0;
			boolean executable = (maxProt & VM_PROT_EXECUTE) != 0;
			boolean inCore = fileSize != 0;

			return new MemoryRange(vmaddr + vmOffset, fileOffset, vmsize, asid, shared, readOnly, executable, inCore);
		}

	}

	@SuppressWarnings("unused")
	private static final class Thread extends Command {

		private static final class State {

			// thread flavors
			static final int X86_THREAD_STATE64 = 4;
			static final int X86_FLOAT_STATE64 = 5;
			static final int X86_EXCEPTION_STATE64 = 6;
			static final int X86_THREAD_STATE = 7;
			static final int X86_FLOAT_STATE = 8;
			static final int X86_EXCEPTION_STATE = 9;
			static final int X86_DEBUG_STATE64 = 11;
			static final int X86_DEBUG_STATE = 12;
			static final int THREAD_STATE_NONE = 13;
			static final int X86_AVX_STATE64 = 17;
			static final int X86_AVX_STATE = 18;
			static final int X86_AVX512_STATE64 = 20;
			static final int X86_AVX512_STATE = 21;

			private static void readException(DumpReader reader, Map<String, Number> registers) throws IOException {
				registers.put("trapno", reader.readShort()); //$NON-NLS-1$
				registers.put("cpu", reader.readShort()); //$NON-NLS-1$
				registers.put("err", reader.readInt()); //$NON-NLS-1$
				registers.put("faultvaddr", reader.readLong()); //$NON-NLS-1$
			}

			private static Number readFPValue(DumpReader reader, int bytes) throws IOException {
				byte[] data = reader.readBytes(bytes);

				if (reader.isLittleEndian()) {
					reverse(data);
				}

				if (data.length > 0 && data[0] < 0) {
					byte[] unsigned = new byte[data.length + 1];

					System.arraycopy(data, 0, unsigned, 1, data.length);

					data = unsigned;
				}

				return new BigInteger(data);
			}

			private static void readFloatState(DumpReader reader, Map<String, Number> registers) throws IOException {
				registers.put("fpu_reserved[0]", reader.readUnsignedInt()); //$NON-NLS-1$
				registers.put("fpu_reserved[1]", reader.readUnsignedInt()); //$NON-NLS-1$

				registers.put("fpu_fcw", reader.readUnsignedShort()); //$NON-NLS-1$
				registers.put("fpu_fsw", reader.readUnsignedShort()); //$NON-NLS-1$
				registers.put("fpu_ftw", reader.readUnsignedByte()); //$NON-NLS-1$
				registers.put("fpu_fsrv1", reader.readUnsignedByte()); //$NON-NLS-1$
				registers.put("fpu_fop", reader.readUnsignedShort()); //$NON-NLS-1$
				registers.put("fpu_ip", reader.readUnsignedInt()); //$NON-NLS-1$
				registers.put("fpu_cs", reader.readUnsignedShort()); //$NON-NLS-1$
				registers.put("fpu_rsrv2", reader.readUnsignedShort()); //$NON-NLS-1$
				registers.put("fpu_dp", reader.readUnsignedInt()); //$NON-NLS-1$
				registers.put("fpu_ds", reader.readUnsignedShort()); //$NON-NLS-1$
				registers.put("fpu_rsrv3", reader.readUnsignedShort()); //$NON-NLS-1$
				registers.put("fpu_mxcsr", reader.readUnsignedInt()); //$NON-NLS-1$
				registers.put("fpu_mxcsrmask", reader.readUnsignedInt()); //$NON-NLS-1$

				// xmm registers are 16 bytes each; 10 bytes used and 6 bytes reserved
				for (int i = 0; i < 8; ++i) {
					registers.put("fpu_stmm" + i, readFPValue(reader, 10)); //$NON-NLS-1$
					readFPValue(reader, 6);
				}

				// xmm registers are 16 bytes each
				for (int i = 0; i < 16; ++i) {
					registers.put("fpu_xmm" + i, readFPValue(reader, 16)); //$NON-NLS-1$
				}

				for (int i = 0; i < 6; ++i) {
					registers.put("fpu_rsrv4[" + i + "]", readFPValue(reader, 16)); //$NON-NLS-1$ //$NON-NLS-2$
				}

				registers.put("fpu_reserved1", reader.readUnsignedInt()); //$NON-NLS-1$
			}

			private static void readRegisters(DumpReader reader, Map<String, Number> registers) throws IOException {
				String names = "rax,rbx,rcx,rdx,rdi,rsi,rbp,rsp,r8,r9,r10,r11,r12,r13,r14,r15,rip,rflags,cs,fs,gs"; //$NON-NLS-1$

				for (String name : names.split(",")) { //$NON-NLS-1$
					registers.put(name, reader.readLong());
				}
			}

			private static void reverse(byte[] data) {
				for (int lo = 0, hi = data.length - 1; lo < hi; lo += 1, hi -= 1) {
					byte tmp = data[lo];

					data[lo] = data[hi];
					data[hi] = tmp;
				}
			}

			final long flavor;
			final Map<String, Number> registers;

			State(DumpReader reader, long flavor, long count) throws IOException {
				super();
				this.flavor = flavor;
				this.registers = new LinkedHashMap<>();

				long nextPosition = reader.getPosition() + (count * 4);

				reader.readUnsignedInt(); // detail flavor
				reader.readUnsignedInt(); // detail length

				if (flavor == X86_EXCEPTION_STATE) {
					readException(reader, registers);
				} else if (flavor == X86_FLOAT_STATE) {
					readFloatState(reader, registers);
				} else if (flavor == X86_THREAD_STATE) {
					readRegisters(reader, registers);
				} else {
					if (DEBUG) {
						System.out.printf("Unsupported thread state flavor=%d, count=%d%n", flavor, count); //$NON-NLS-1$
					}
					reader.seek(nextPosition);
				}

				if (DEBUG) {
					if (reader.getPosition() != nextPosition) {
						System.out.printf("Unexpected position after flavor=%d, count=%d%n", flavor, count); //$NON-NLS-1$
						reader.seek(nextPosition);
					}
				}
			}

		}

		final State[] states;

		Thread(DumpReader reader, long size) throws IOException {
			super(LC_THREAD, size);

			List<State> stateList = new ArrayList<>();

			for (long offset = 8; offset < size;) {
				if (DEBUG) {
					System.out.printf("Reading thread state @ 0x%016X%n", reader.getPosition()); //$NON-NLS-1$
				}

				long flavor = reader.readUnsignedInt();
				long count = reader.readUnsignedInt();

				stateList.add(new State(reader, flavor, count));
				offset += (count * 4) + 8;
			}

			this.states = stateList.toArray(new State[stateList.size()]);
		}

	}

	private static final class Symbol {
		//
	}

	private static final boolean DEBUG = true;

	// Mach-O file types
	public static final int MH_OBJECT = 0x1;
	public static final int MH_EXECUTE = 0x2;
	public static final int MH_FVMLIB = 0x3;
	public static final int MH_CORE = 0x4;
	public static final int MH_PRELOAD = 0x5;
	public static final int MH_DYLIB = 0x6;
	public static final int MH_DYLINKER = 0x7;
	public static final int MH_BUNDLE = 0x8;
	public static final int MH_DYLIB_STUB = 0x9;
	public static final int MH_DSYM = 0xa;
	public static final int MH_KEXT_BUNDLE = 0xb;

	private static final String SYSTEM_PROP_EXECUTABLE = "com.ibm.dtfj.corereaders.executable"; //$NON-NLS-1$

	public static ICoreFileReader dumpFromFile(ImageInputStream stream) throws IOException {
		stream.seek(0);

		int magic = stream.readInt();
		boolean isLittleEndian;
		DumpReader reader;

		switch (magic) {
		case Header.MACHO_64:
			isLittleEndian = false;
			reader = new DumpReader(stream, true);
			break;
		case Header.MACHO_64_REV:
			isLittleEndian = true;
			reader = new LittleEndianDumpReader(stream, true);
			break;
		default:
			throw new IllegalArgumentException("Not a supported Macho core file"); //$NON-NLS-1$
		}

		MachFile file = new MachFile(reader, 0);

		return new NewMachoDump(file, reader, isLittleEndian);
	}

	private static MachFile machFileFrom(ClosingFileReader file) throws IOException {
		int magic = file.readInt();
		DumpReader reader;

		switch (magic) {
		case Header.MACHO_64:
			reader = new DumpReader(file, true);
			break;
		case Header.MACHO_64_REV:
			reader = new LittleEndianDumpReader(file, true);
			break;
		default:
			return null;
		}

		return new MachFile(reader, 0);
	}

	/**
	 * Attempts to track down the file with given filename in :-delimited path
	 *
	 * @param builder
	 * @param executableName
	 * @param property
	 * @return The file if it is found or null if the search failed
	 */
	@SuppressWarnings("resource")
	private static ClosingFileReader findFileInPath(Builder builder, String filename, String path) {
		ClosingFileReader file = null;

		try {
			// first try it as whatever we were given first
			file = builder.openFile(filename);
		} catch (IOException e1) {
			// it wasn't there so fall back to the path
			for (String component : path.split(File.pathSeparator)) {
				try {
					file = builder.openFile(component + File.separator + filename);
					if (file != null) {
						break;
					}
				} catch (IOException e2) {
					// do nothing, most of these will be file not found
				}
			}
		}

		return file;
	}

	static String getStringFromAsciiChars(byte[] chars) {
		return getStringFromAsciiChars(chars, 0);
	}

	static String getStringFromAsciiChars(byte[] chars, int start) {
		if ((0 <= start) && (start < chars.length)) {
			int end = start;
			for (; end < chars.length; ++end) {
				if (0 == chars[end]) {
					break;
				}
			}
			return new String(chars, start, end - start, StandardCharsets.US_ASCII);
		}
		return null;
	}

	public static boolean isSupportedDump(ImageInputStream stream) throws IOException {
		stream.seek(0);

		int magic = stream.readInt();
		DumpReader reader;

		switch (magic) {
		case Header.MACHO_64:
			reader = new DumpReader(stream, true);
			break;
		case Header.MACHO_64_REV:
			reader = new LittleEndianDumpReader(stream, true);
			break;
		default:
			return false;
		}

		reader.seek(0);

		Header header = new Header(reader);

		return header.fileType == MH_CORE;
	}

	private final Set<String> additionalFileNames;

	private final MachFile file;

	private final boolean isLittleEndian;

	private final List<MemoryRange> memoryRanges;

	private NewMachoDump(MachFile file, DumpReader reader, boolean isLittleEndian) {
		super(reader);
		this.additionalFileNames = new TreeSet<>();
		this.file = file;
		this.isLittleEndian = isLittleEndian;
		this.memoryRanges = new ArrayList<>();
	}

	//	private Iterator<?> buildModuleSections(Builder builder, Object addressSpace, MachFile executable,
	//			long loadedBaseAddress) throws IOException {
	//		List<Object> sections = new Vector<>();
	//		Iterator<SectionHeaderEntry> shentries;
	//		shentries = executable.sectionHeaderEntries();
	//		byte[] strings = null;
	//
	//		// Find the string table by looking for a section header entry of type string
	//		// table which, when resolved against itself, has the name ".shstrtab".
	//		// NB : If the ELF file is invalid, the method stringFromBytesAt() may return null.
	//		while (shentries.hasNext()) {
	//			SectionHeaderEntry entry = shentries.next();
	//
	//			if (SHT_STRTAB == entry._type) {
	//				executable.seek(entry.offset);
	//				byte[] attempt = executable.readBytes((int) entry.size);
	//				String peek = stringFromBytesAt(attempt, entry._name);
	//
	//				if (".shstrtab".equals(peek)) { //$NON-NLS-1$
	//					strings = attempt;
	//					break; // found it
	//				}
	//			}
	//		}
	//
	//		// Loop through the section header entries building a module section for each one
	//		// NB : If the ELF file is invalid, the method stringFromBytesAt() may return null.
	//		shentries = executable.sectionHeaderEntries();
	//		while (shentries.hasNext()) {
	//			SectionHeaderEntry entry = shentries.next();
	//			String name = ""; //$NON-NLS-1$
	//
	//			if (strings != null) {
	//				name = stringFromBytesAt(strings, entry._name);
	//				if (name == null) {
	//					name = ""; //$NON-NLS-1$
	//				}
	//			}
	//
	//			Object section = builder.buildModuleSection(addressSpace, name, loadedBaseAddress + entry.offset,
	//					loadedBaseAddress + entry.offset + entry.size);
	//
	//			sections.add(section);
	//		}
	//		
	//		return sections.iterator();
	//	}

	private Object buildProcess(Builder builder, Object addressSpace, String pid, String commandLine,
			Properties environmentVariables, Object currentThread, String executableName)
			throws IOException, MemoryAccessException {
		List<?> threads = Collections.emptyList();
		List<?> modules = readModules(builder, addressSpace, executableName);
		Iterator<?> libraries = modules.iterator();
		Object executable = libraries.hasNext() ? libraries.next() : null;

		return builder.buildProcess(addressSpace, pid, commandLine, environmentVariables, currentThread,
				threads.iterator(), executable, libraries, 8);
	}

	//	private Object buildThread(Builder builder, Object addressSpace, String pid, Map<String, Address> registers, Properties properties, int signalNumber) throws MemoryAccessException, IOException {
	//		List<Object> frames = new ArrayList<>();
	//		List<Object> sections = new ArrayList<>();
	//		long stackPointer = file.getStackPointerFrom(registers).asAddress();
	//		long basePointer = file.getBasePointerFrom(registers).asAddress();
	//		long instructionPointer = file.getInstructionPointerFrom(registers).asAddress();
	//		long previousBasePointer = 0;
	//
	//		if (0 == instructionPointer || false == isValidAddressInProcess(instructionPointer)) {
	//			instructionPointer = file.getLinkRegisterFrom(registers).asAddress();
	//		}
	//
	//		if (0 != instructionPointer && 0 != basePointer && isValidAddressInProcess(instructionPointer)
	//				&& isValidAddressInProcess(stackPointer)) {
	//			MemoryRange range = memoryRangeFor(stackPointer);
	//
	//			sections.add(builder.buildStackSection(addressSpace, range.getVirtualAddress(),
	//					range.getVirtualAddress() + range.getSize()));
	//			frames.add(builder.buildStackFrame(addressSpace, basePointer, instructionPointer));
	//
	//			// Loop through stack frames, starting from current frame base pointer
	//			try {
	//				while (range.contains(basePointer) && (basePointer != previousBasePointer)) {
	//					previousBasePointer = basePointer;
	//					seekToAddress(basePointer);
	//					basePointer = coreReadAddress();
	//					instructionPointer = coreReadAddress();
	//					frames.add(builder.buildStackFrame(addressSpace, basePointer, instructionPointer));
	//				}
	//			} catch (IOException e) {
	//				// CMVC 161796
	//				// catch the IO exception, give up trying to extract the frames and attempt to carry on parsing the core file
	//
	//				// CMVC 164739
	//				// Keep going instead.
	//				frames.add(builder.buildCorruptData(addressSpace,
	//						"Linux ELF core dump corruption " + "detected during native stack frame walk", basePointer));
	//			}
	//		}
	//
	//		return builder.buildThread(pid, registersAsList(builder, registers).iterator(), sections.iterator(),
	//				frames.iterator(), properties, signalNumber);
	//	}

	/* (non-Javadoc)
	 * @see com.ibm.jvm.j9.dump.systemdump.Dump#extract(com.ibm.jvm.j9.dump.systemdump.Builder)
	 */
	@Override
	public void extract(Builder builder) {
		try {
			builder.setOSType("OSX"); //$NON-NLS-1$
			builder.setCPUType("X86_64"); //$NON-NLS-1$
			builder.setCPUSubType(""); //$NON-NLS-1$

			Object addressSpace = builder.buildAddressSpace("Mach-O Address Space", 0); //$NON-NLS-1$

			List<Object> threads = new ArrayList<>();

			for (Command command : file.commands) {
				if (command.cmdType == Command.LC_THREAD) {
					threads.add(buildThread(builder, (Thread) command));
				}
			}

			List<Object> processes = new ArrayList<>();

			// TODO

			//	for (Iterator<DataEntry> iter = file.processEntries(); iter.hasNext();) {
			//		DataEntry entry = iter.next();
			//	
			//		processes.add(readProcess(entry, builder, addressSpace, threads));
			//	}

			//	for (Iterator<DataEntry> iter = file.auxiliaryVectorEntries(); iter.hasNext();) {
			//		DataEntry entry = iter.next();
			//
			//		readAuxiliaryVector(entry);
			//	}

			{
				// FIXME remove this
				if ("FIXME".isEmpty()) { //$NON-NLS-1$
					if (file.reader.readInt() == 0) {
						throw new CorruptCoreException(""); //$NON-NLS-1$
					} else {
						throw new MemoryAccessException(0, 0);
					}
				}
			}
		} catch (CorruptCoreException e) {
			// TODO throw exception or notify builder?
		} catch (IOException e) {
			// TODO throw exception or notify builder?
		} catch (MemoryAccessException e) {
			// TODO throw exception or notify builder?
		}
	}

	@Override
	public Iterator<String> getAdditionalFileNames() {
		return additionalFileNames.iterator();
	}

	private Properties getEnvironmentVariables(Builder builder) {
		// builder.getEnvironmentAddress() points to a pointer to a null
		// terminated list of addresses which point to strings of form x=y

		// get environment variable addresses
		long environmentAddress = builder.getEnvironmentAddress();
		DumpReader reader = file.reader;
		List<Long> addresses = new ArrayList<>();

		if (environmentAddress != 0) {
			long fileOffset = file.getFileOffset(environmentAddress);

			if (fileOffset != 0)
				try {
					reader.seek(fileOffset);

					long address = reader.readAddress();

					reader.seek(address);

					for (;;) {
						address = reader.readAddress();

						if (address == 0) {
							break;
						}

						addresses.add(Long.valueOf(address));
					}
				} catch (IOException e) {
					// get as many environment variables as we can
				}
		}

		Properties environment = new Properties();

		for (Long address : addresses) {
			long fileOffset = file.getFileOffset(address.longValue());

			if (fileOffset != 0) {
				try {
					String pair = readStringAt(reader, fileOffset);

					if (pair != null) {
						// pair is a string: name=value
						int equal = pair.indexOf('=');

						if (equal != -1) {
							String key = pair.substring(0, equal);
							String value = pair.substring(equal + 1);

							environment.put(key, value);
						}
					}
				} catch (IOException e) {
					// carry on getting other environment variables
				}
			}
		}

		return environment;
	}

	@Override
	protected MemoryRange[] getMemoryRangesAsArray() {
		return memoryRanges.toArray(new MemoryRange[memoryRanges.size()]);
	}

	@Override
	protected boolean is64Bit() {
		return true;
	}

	@Override
	protected boolean isLittleEndian() {
		return isLittleEndian;
	}

	/**
	 * Is this virtual address valid and present in the core file.
	 * @param address Virtual address to test
	 * @return true if valid and present
	 */
	private boolean isValidAddress(long address) {
		for (Command command : file.commands) {
			if (command.cmdType == Command.LC_SEGMENT_64) {
				Segment segment = (Segment) command;

				if (segment.fileSize != 0) {
					if (segment.vmaddr <= address && address < segment.vmaddr + segment.vmsize) {
						return true;
					}
				}
			}
		}

		return false;
	}

	/**
	 * Is this virtual address valid in the process, though perhaps not present in the core file?
	 * @param address Virtual address to test
	 * @return if valid
	 */
	private boolean isValidAddressInProcess(long address) {
		for (Command command : file.commands) {
			if (command.cmdType == Command.LC_SEGMENT_64) {
				Segment segment = (Segment) command;

				if (segment.vmaddr <= address && address < segment.vmaddr + segment.vmsize) {
					return true;
				}
			}
		}

		return false;
	}

	private MemoryRange memoryRangeFor(long address) {
		for (MemoryRange range : memoryRanges) {
			if (range.contains(address)) {
				return range;
			}
		}

		return null;
	}

	//	private List<?> readLibrariesAt(Builder builder, Object addressSpace, long imageStart) throws MemoryAccessException, IOException {
	//		// create the method return value
	//		List<Object> libraries = new ArrayList<>();
	//
	//		// seek to the start of the dynamic section
	//		seekToAddress(imageStart);
	//
	//		// loop reading the dynamic section tag-value/pointer pairs until a 'debug' entry is found
	//		long tag;
	//		long address;
	//
	//		do {
	//			// read the tag and the value/pointer (only used after the loop has terminated)
	//			tag = file.readElfWord();
	//			address = file.readElfWord();
	//
	//			// check that the tag is valid. As there may be some debate about the
	//			// set of valid values, a message will be issued but reading will continue
	//			/*
	//			 * CMVC 161449 - SVT:70:jextract invalid tag error
	//			 * the range of valid values in the header has been expanded, so increasing the valid range
	//			 * http://refspecs.freestandards.org/elf/gabi4+/ch5.dynamic.html
	//			 * DT_RUNPATH 			29 	d_val 	optional 	optional
	//			 * DT_FLAGS 			30 	d_val 	optional 	optional
	//			 * DT_ENCODING 			31 	unspecified 	unspecified 	unspecified
	//			 * DT_PREINIT_ARRAY 	32 	d_ptr 	optional 	ignored
	//			 * DT_PREINIT_ARRAYSZ 	33 	d_val 	optional 	ignored
	//			 */
	//			if (_verbose) {
	//				if (!((tag >= 0) && (tag <= 33)) && !((tag >= 0x60000000) && (tag <= 0x6FFFFFFF))
	//						&& !((tag >= 0x70000000) && (tag <= 0x7FFFFFFF))) {
	//					System.err.println("Error reading dynamic section. Invalid tag value '0x" + Long.toHexString(tag));
	//				}
	//			}
	//		} while ((tag != DT_NULL) && (tag != DT_DEBUG));
	//
	//		// if there is no debug section, there is nothing to do
	//		if (tag != DT_DEBUG) {
	//			return libraries;
	//		}
	//
	//		// seek to the start of the debug data
	//		seekToAddress(address);
	//
	//		// NOTE the rendezvous structure is described in /usr/include/link.h
	//		//	struct r_debug {
	//		//		int r_version;
	//		//		struct link_map *r_map;
	//		//		ElfW(Addr) r_brk;		/* Really a function pointer */
	//		//		enum { ... } r_state;
	//		//		ElfW(Addr) r_ldbase;
	//		//	};
	//		//	struct link_map {
	//		//		ElfW(Addr) l_addr;		/* Base address shared object is loaded at. */
	//		//		char *l_name;			/* Absolute file name object was found in. */
	//		//		ElfW(Dyn) *l_ld;		/* Dynamic section of the shared object. */
	//		//		struct link_map *l_next, *l_prev;	/* Chain of loaded objects. */
	//		//	};
	//
	//		file.readElfWord(); // ignore version (and alignment padding)
	//		long next = file.readElfWord();
	//
	//		while (0 != next) {
	//			seekToAddress(next);
	//			long loadedBaseAddress = file.readElfWord();
	//			long nameAddress = file.readElfWord();
	//			file.readElfWord(); // ignore dynamicSectionAddress
	//			next = file.readElfWord();
	//			if (0 != loadedBaseAddress) {
	//				MemoryRange range = memoryRangeFor(loadedBaseAddress);
	//
	//				// NOTE There is an apparent bug in the link editor on Linux x86-64.
	//				// The loadedBaseAddress for libnuma.so is invalid, although it is
	//				// correctly loaded and its symbols are resolved.  The library is loaded
	//				// around 0x2A95C4F000 but its base is recorded in the link map as
	//				// 0xFFFFFFF09F429000.
	//
	//				Properties properties = new Properties();
	//				List<Object> symbols = new ArrayList<>();
	//				String name = readStringAt(nameAddress);
	//				Iterator<?> sections = Collections.emptyIterator();
	//
	//				// If we have a valid link_map entry, open up the actual library file to get the symbols and
	//				// library sections.
	//				// Note: on SLES 10 we have seen a link_map entry with a null name, in non-corrupt dumps, so we now
	//				// ignore link_map entries with null names rather than report them as corrupt. See defect 132140
	//				// Note : on SLES 11 we have seen a link_map entry with a module name of "7". Using the base address and correlating
	//				// with the libraries we find by iterating through the program header table of the core file, we know that
	//				// this is in fact linux-vdso64.so.1
	//				// Detect this by spotting that the name does not begin with "/" - the names should
	//				// always be full pathnames. In this case just ignore this one.
	//				// See defect CMVC 184115
	//				if ((null != range) && (range.getVirtualAddress() == loadedBaseAddress) && (null != name) && name.startsWith("/")) {
	//					try {
	//						ClosingFileReader file = builder.openFile(name);
	//
	//						if (null != file) {
	//							if (_verbose) {
	//								System.err.println("Reading library file " + name);
	//							}
	//
	//							ElfFile library = elfFileFrom(file);
	//
	//							if (range.isInCoreFile() == false) {
	//								// the memory range has not been loaded into the core dump, but it should be
	//								// resident in the library file on disk, so add a reference to the real file.
	//								range.setLibraryReader(library._reader);
	//							}
	//							_additionalFileNames.add(name);
	//							if (null != library) {
	//								symbols = readSymbolsFrom(builder, addressSpace, library, loadedBaseAddress);
	//								properties = library.getProperties();
	//								sections = buildModuleSections(builder, addressSpace, library, loadedBaseAddress);
	//							} else {
	//								symbols.add(builder.buildCorruptData(addressSpace, "unable to find module " + name,
	//										loadedBaseAddress));
	//							}
	//						}
	//					} catch (FileNotFoundException e) {
	//						// As for null file return above, if we can't find the library file we follow the normal
	//						// DTFJ convention and leave the properties, sections and symbols iterators empty.
	//					}
	//					libraries.add(builder.buildModule(name, properties, sections, symbols.iterator(), loadedBaseAddress));
	//				}
	//			}
	//		}
	//
	//		return libraries;
	//	}

	private ImageModule processExecutableFile(MachFile executableFile, long offset) throws IOException {
		List<Symbol> symbols = new ArrayList<>();
		Collection<? extends MemoryRange> ranges = executableFile.getMemoryRangesWithOffset(offset);

		// Module m = new Module(process, "executable", symbols, memoryRanges, executableFile.streamOffset, new Properties());

		// public ImageModule(String moduleName, Properties properties, Iterator sections, Iterator symbols, long loadAddress) {

		return null;
	}

	/**
	 * Returns a list of the ImageModules in the address space.  The executable will be element 0
	 * @param builder
	 * @param addressSpace
	 * @param executableName
	 * @return
	 * @throws IOException
	 * @throws MemoryAccessException
	 */
	private List<?> readModules(Builder builder, Object addressSpace, String executableName)
			throws IOException, MemoryAccessException {
		List<ImageModule> executables = new ArrayList<>();
		List<ImageModule> dylibs = new ArrayList<>();
		List<Object> dylinkers = new ArrayList<>();

		{
			String exeName = System.getProperty(SYSTEM_PROP_EXECUTABLE, executableName);
			String classpath = System.getProperty("java.class.path", "."); //$NON-NLS-1$ //$NON-NLS-2$
			@SuppressWarnings("resource")
			ClosingFileReader fileReader = findFileInPath(builder, exeName, classpath);

			if (fileReader == null) {
				builder.setExecutableUnavailable("File \"" + exeName + "\" not found"); //$NON-NLS-1$ //$NON-NLS-2$
			} else {
				MachFile executable = machFileFrom(fileReader);

				if ((executable == null) || (executable.header.fileType != MH_EXECUTE)) {
					builder.setExecutableUnavailable("File \"" + exeName + "\" is not an executable"); //$NON-NLS-1$ //$NON-NLS-2$
				} else {
					Properties properties = new Properties();
					Iterator<Object> sections = Collections.emptyIterator();
					Iterator<Object> symbols = Collections.emptyIterator();
					long startAddress = 0;

					//	executables.add(executable);

					builder.buildModule(exeName, properties, sections, symbols, startAddress);

					//	if (null != dynamic) {
					//		List symbols = readSymbolsFrom(builder, addressSpace, executable, dynamic.virtualAddress);
					//		long imageStart = dynamic.virtualAddress;
					//		if (isValidAddress(imageStart)) {
					//			Iterator sections = _buildModuleSections(builder, addressSpace, executable, dynamic.virtualAddress);
					//			Properties properties = executable.getProperties();
					//			modules.add(builder.buildModule(executableName, properties, sections, symbols.iterator(),imageStart));
					//			modules.addAll(readLibrariesAt(builder, addressSpace, imageStart));
					//			_additionalFileNames.add(executableName);
					//		}
					//	}

				}
			}

			// Get all the loaded modules .so names to make sure we have the
			// full list of libraries for jextract to zip up. CMVC 194366
			//	for (Object e : _file._programHeaderEntries) {
			//		ProgramHeaderEntry entry = (ProgramHeaderEntry) e;
			//		if (!entry.isLoadable()) {
			//			continue;
			//		}
			//		try {
			//			ElfFile moduleFile = null;
			//			if (_is64Bit) {
			//				moduleFile = new Elf64File(_file._reader, entry.fileOffset);
			//			} else {
			//				moduleFile = new Elf32File(_file._reader, entry.fileOffset);
			//			}
			//			// Now search the dynamic entries for this .so file to get it's SONAME.
			//			// "lib.so" is returned for the executable and should be ignored.
			//			String soname = moduleFile.getSONAME();
			//			if (soname != null && !"lib.so".equals(soname)) {
			//				_additionalFileNames.add(soname);
			//			}
			//		} catch (Exception ex) {
			//			// We can't tell a loaded module from a loaded something else without trying to open it
			//			// as an ElfFile so this will happen. We are looking for extra library names, it's not
			//			// critical if this fails.
			//		}
			//	}

			//	return modules;
		}

		for (Command command : file.commands) {
			if (command.cmdType != Command.LC_SEGMENT_64) {
				continue;
			}

			Segment segment = (Segment) command;
			DumpReader reader = file.reader;

			try {
				int magic = reader.readInt();

				if (Header.isMACHO(magic)) {
					MachFile innerFile = new MachFile(reader, segment.fileOffset);

					switch (innerFile.header.fileType) {
					case MH_EXECUTE:
						// TODO executable = processExecutableFile(innerFile, segment.vmaddr);
						break;
					case MH_DYLIB:
						// TODO modules.add(processModuleFile(innerFile, segment));
						break;
					case MH_DYLINKER:
						// TODO dylinkerMachFile = innerFile;
						break;
					default:
						break;
					}
				}
			} catch (EOFException e) {
				// nothing to process if we have a truncated file
			}

		}

		//	String overrideExecutableName = System.getProperty(SYSTEM_PROP_EXECUTABLE);
		//	ClosingFileReader file;
		//
		//	if (overrideExecutableName == null) {
		//		file = findFileInPath(builder, executableName, System.getProperty("java.class.path", "."));
		//	} else {
		//		// Use override for the executable name. This supports the jextract -f <executable> option, for
		//		// cases where the launcher path+name is truncated by the 80 character OS limit, AND it was a
		//		// custom launcher, so the alternative IBM_JAVA_COMMAND_LINE property was not set.
		//		file = findFileInPath(builder, overrideExecutableName, System.getProperty("java.class.path", "."));
		//	}

		//	if (null != file) {
		//		// System.err.println("NewMachoDump.readModules() opened file");
		//		ElfFile executable = elfFileFrom(file);
		//
		//		if (null != executable) {
		//			ProgramHeaderEntry dynamic = null;
		//
		//			for (Iterator<ProgramHeaderEntry> iter = executable.programHeaderEntries(); iter.hasNext();) {
		//				ProgramHeaderEntry entry = iter.next();
		//
		//				if (entry.isDynamic()) {
		//					dynamic = entry;
		//					break;
		//				}
		//			}
		//
		//			if (null != dynamic) {
		//				List<?> symbols = readSymbolsFrom(builder, addressSpace, executable, dynamic.virtualAddress);
		//				long imageStart = dynamic.virtualAddress;
		//
		//				if (isValidAddress(imageStart)) {
		//					Iterator<?> sections = buildModuleSections(builder, addressSpace, executable,
		//							dynamic.virtualAddress);
		//					Properties properties = executable.getProperties();
		//
		//					modules.add(builder.buildModule(executableName, properties, sections, symbols.iterator(),
		//							imageStart));
		//					modules.addAll(readLibrariesAt(builder, addressSpace, imageStart));
		//					_additionalFileNames.add(executableName);
		//				}
		//			}
		//		} else {
		//			builder.setExecutableUnavailable("Executable file \"" + executableName + "\" not found");
		//		}
		//	} else {
		//		builder.setExecutableUnavailable("File \"" + executableName + "\" not found");
		//		if (_verbose) {
		//			System.err.println("Warning: executable " + executableName + " not found, unable to collect libraries."
		//					+ " Please retry with jextract -f option.");
		//		}
		//	}
		//
		//	// get all the loaded modules .so names to make sure we have the
		//	// full list of libraries for jextract to zip up. CMVC 194366
		//	for (ProgramHeaderEntry entry : file._programHeaderEntries) {
		//		if (!entry.isLoadable()) {
		//			continue;
		//		}
		//
		//		try {
		//			ElfFile moduleFile = new Elf64File(file._reader, entry.fileOffset);
		//
		//			// Now search the dynamic entries for this .so file to get it's SONAME.
		//			// "lib.so" is returned for the executable and should be ignored.
		//			String soname = moduleFile.getSONAME();
		//
		//			if (soname != null && !"lib.so".equals(soname)) {
		//				_additionalFileNames.add(soname);
		//			}
		//		} catch (Exception ex) {
		//			// We can't tell a loaded module from a loaded something else without trying to open it
		//			// as an ElfFile so this will happen. We are looking for extra library names, it's not
		//			// critical if this fails.
		//		}
		//	}

		List<Object> modules = new ArrayList<>();

		modules.addAll(executables);
		modules.addAll(dylibs);
		modules.addAll(dylinkers);

		return modules;
	}

	private Object readProcess(Builder builder, Object addressSpace, List<Object> threads)
			throws IOException, CorruptCoreException, MemoryAccessException {
		//		// get the PID from the J9RAS structure, if possible
		//		// otherwise use the one of the forked process
		//		if (null != _j9rasReader) {
		//			boolean didFork = true;
		//
		//			try {
		//				long rasPID = _j9rasReader.getProcessID();
		//
		//				if (elfPID != rasPID) {
		//					// PIDs are different, therefore fork+dump has happened.
		//					// In this case, rasPID is the correct PID to report
		//					pid = Long.toString(rasPID);
		//				} else {
		//					// PIDs are equal, so the dump must have been generated externally.
		//					// In this case, the failing thread is the one reported by ELF,
		//					// not the one read from J9RAS.
		//					didFork = false;
		//				}
		//			} catch (UnsupportedOperationException use) {
		//			}
		//
		//			if (didFork) {
		//				try {
		//					long tid = _j9rasReader.getThreadID();
		//					// fake a thread with no other useful info than the TID.
		//					failingThread = builder.buildThread(Long.toString(tid), Collections.EMPTY_LIST.iterator(),
		//							Collections.EMPTY_LIST.iterator(), Collections.EMPTY_LIST.iterator(), new Properties(), 0);
		//					threads.add(0, failingThread);
		//				} catch (UnsupportedOperationException uoe) {
		//				}
		//			}
		//		}

		String pid = "<pid>";
		String commandLine = "<command>";
		String failingThread = "<thread>";
		String executable = "<exe>";

		return buildProcess(builder, addressSpace, pid, commandLine, getEnvironmentVariables(builder), failingThread,
				executable);
	}

	private static String readStringAt(DumpReader reader, long fileOffset) throws IOException {
		reader.seek(fileOffset);

		for (ByteArrayOutputStream buffer = new ByteArrayOutputStream();;) {
			byte ascii = reader.readByte();

			if (ascii == 0) {
				return new String(buffer.toByteArray(), StandardCharsets.US_ASCII);
			}

			buffer.write(ascii);
		}
	}

	private String readStringAt(long address) throws IOException {
		if (isValidAddress(address)) {
			return readStringAt(file.reader, address);
		}

		return null;
	}

	//	private List<Object> readSymbolsFrom(Builder builder, Object addressSpace, ElfFile file, long baseAddress) throws IOException {
	//		List<Object> symbols = new ArrayList<>();
	//
	//		for (Iterator<SectionHeaderEntry> iter = file.sectionHeaderEntries(); iter.hasNext();) {
	//			SectionHeaderEntry entry = iter.next();
	//
	//			if (entry.isSymbolTable()) {
	//				symbols.addAll(readSymbolsFrom(builder, addressSpace, file, entry, baseAddress));
	//			}
	//		}
	//
	//		return symbols;
	//	}

	//	private List<?> readSymbolsFrom(Builder builder, Object addressSpace, ElfFile file, SectionHeaderEntry entry, long baseAddress) throws IOException {
	//		List<Object> symbols = new ArrayList<>();
	//		SectionHeaderEntry stringTable = file.sectionHeaderEntryAt((int) entry.link);
	//
	//		file.seek(stringTable.offset);
	//
	//		byte[] strings = file.readBytes((int) stringTable.size);
	//
	//		for (Iterator<Symbol> iter = file.readSymbolsAt(entry); iter.hasNext();) {
	//			Symbol sym = iter.next();
	//
	//			if (sym.isFunction() && sym.value != 0) {
	//				String name = stringFromBytesAt(strings, sym.name);
	//
	//				if (name != null) {
	//					symbols.add(builder.buildSymbol(addressSpace, name, baseAddress + sym.value));
	//				}
	//			}
	//		}
	//
	//		return symbols;
	//	}

	/*
	 * NOTE: Only one native thread is expected due to the way core dumps are created.
	 */
	private Object buildThread(Builder builder, Thread thread) throws IOException, MemoryAccessException {
		String pid = "<pid>"; // FIXME

		List<Object> registers = new ArrayList<>();

		for (Thread.State state : thread.states) {
			for (Map.Entry<String, Number> entry : state.registers.entrySet()) {
				registers.add(builder.buildRegister(entry.getKey(), entry.getValue()));
			}
		}

		List<Object> sections = Collections.emptyList();

		List<Object> frames = Collections.emptyList();

		Properties properties = new Properties();

		//	properties.setProperty("Thread user time secs", Long.toString(utimeSec));
		//	properties.setProperty("Thread user time usecs", Long.toString(utimeUSec));
		//	properties.setProperty("Thread sys time secs", Long.toString(stimeSec));
		//	properties.setProperty("Thread sys time usecs", Long.toString(stimeUSec));

		int signalNumber = 0;

		return builder.buildThread(pid, //
				registers.iterator(), //
				sections.iterator(), //
				frames.iterator(), //
				properties, //
				signalNumber);
	}

	/**
	 * Method for extracting a string from a string table
	 *
	 * @param strings : The byte array from which the strings are to be extracted
	 * @param offset  : The offset of the required string
	 * @return
	 */
	private static String stringFromBytesAt(byte[] stringTableBytes, long offset) {
		// check that the offset is valid
		if (!((0 <= offset) && (offset < stringTableBytes.length))) {
			return null;
		}

		int startOffset = (int) offset;
		int endOffset = startOffset;

		// scan along the characters checking they are valid (else undefined behaviour
		// converting to a string) and locating the terminating null (if any!!)
		for (endOffset = startOffset; endOffset < stringTableBytes.length; ++endOffset) {
			if (stringTableBytes[endOffset] >= 128) {
				return null;
			} else if (stringTableBytes[endOffset] == 0) {
				break;
			}
		}

		// check that the string is null terminated
		if (stringTableBytes[endOffset] != 0) {
			return null;
		}

		// convert the bytes to a string
		try {
			return new String(stringTableBytes, startOffset, endOffset - startOffset, StandardCharsets.US_ASCII);
		} catch (IndexOutOfBoundsException exception) {
			System.err.println(
					"Error (IndexOutOfBoundsException) converting string table characters. The core file is invalid and the results may unpredictable"); //$NON-NLS-1$
		}

		return null;
	}

}
