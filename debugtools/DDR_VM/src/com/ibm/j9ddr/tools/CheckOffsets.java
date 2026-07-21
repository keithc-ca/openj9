/*
 * Copyright IBM Corp. and others 2026
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
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 */
package com.ibm.j9ddr.tools;

import java.io.File;
import java.io.IOException;
import java.nio.ByteOrder;
import java.util.HashMap;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.stream.Stream;

import javax.imageio.stream.FileImageInputStream;
import javax.imageio.stream.ImageInputStream;

import com.ibm.j9ddr.StructureReader;
import com.ibm.j9ddr.StructureReader.FieldDescriptor;
import com.ibm.j9ddr.StructureReader.StructureDescriptor;

/**
 * A tool to check that fields of certain types are naturally aligned.
 */
@SuppressWarnings("boxing")
public final class CheckOffsets {

	/**
	 * This pattern is used to strip trailing "[]" array specifiers.
	 */
	private static final Pattern ArrayPattern = Pattern.compile("([^\\[]+)(?:\\[\\])*");

	/**
	 * This pattern is used to strip a leading "class" or "struct" qualifier or
	 * a bitfield-width specifier. For example, the field J9JavaVM.vmInterface
	 * has type "struct J9VMInterface": The part that matches group(1) is
	 * "J9VMInterface".
	 */
	private static final Pattern BasePattern = Pattern.compile("(?:\\s*(?:class|struct)\\s+)*([^:\\[]+)(?::\\d+)?");

	/**
	 * Return the first group matching the supplied pattern or the original input.
	 */
	private static String firstGroup(String input, Pattern pattern) {
		Matcher matcher = pattern.matcher(input);

		return matcher.matches() ? matcher.group(1) : input;
	}

	public static void main(String[] args) {
		if (args.length < 2) {
			System.err.println("Usage: CheckOffsets blob type ...");
			return;
		}

		try {
			CheckOffsets checker = new CheckOffsets(args[0]);

			if (Stream.of(args).skip(1).allMatch(checker::check)) {
				return;
			}
		} catch (IOException e) {
			System.err.format("Cannot read blob: %s%n", e.getLocalizedMessage());
		}

		System.exit(1);
	}

	/**
	 * Create a map with the basic types and their alignments.
	 */
	private static Map<String, Integer> makeAlignments(StructureReader reader) {
		int sizeOfUDATA = reader.getSizeOfUDATA();
		Map<String, Integer> map = new HashMap<>();

		map.put("I8", 1);
		map.put("U8", 1);
		map.put("I16", 2);
		map.put("U16", 2);
		map.put("I32", 4);
		map.put("U32", 4);
		map.put("I64", 8);
		map.put("U64", 8);
		map.put("IDATA", sizeOfUDATA);
		map.put("UDATA", sizeOfUDATA);

		return map;
	}

	/**
	 * A helper method to open the blob as an ImageInputStream,
	 * without loading the awt shared library.
	 */
	private static ImageInputStream openImageStream(String fileName) throws IOException {
		// Extend FileImageInputStream to avoid loading the awt shared library.
		final class BlobStream extends FileImageInputStream {
			BlobStream(File file) throws IOException {
				super(file);
			}
		}

		return new BlobStream(new File(fileName));
	}

	/**
	 * A map from type name to alignment. Initially, only the predefined types
	 * are included; entries for structures are added by computing the maximum
	 * alignment of their fields.
	 */
	private final Map<String, Integer> alignments;

	/**
	 * The size (and alignment) of a pointer, derived from the size of UDATA
	 * as recorded in the blob.
	 */
	private final int pointerSize;

	/**
	 * A map from structure name to descriptor, as read from the blob.
	 */
	private final Map<String, StructureDescriptor> types;

	/**
	 * Create an offset checker from the blob named by fileName.
	 */
	private CheckOffsets(String fileName) throws IOException {
		super();

		try (ImageInputStream image = openImageStream(fileName)) {
			image.setByteOrder(ByteOrder.LITTLE_ENDIAN);

			StructureReader reader = new StructureReader(image);

			alignments = makeAlignments(reader);
			pointerSize = reader.getSizeOfUDATA();
			types = new HashMap<>();

			for (StructureDescriptor structure : reader.getStructures()) {
				types.put(structure.getName(), structure);
			}
		}
	}

	/**
	 * Check the alignments of fields of the named structure. Returns true
	 * if the type is known and all its fields are naturally aligned.
	 */
	private boolean check(String typeName) {
		StructureDescriptor info = types.get(typeName);

		if (info == null) {
			System.err.format("Unknown type: %s%n", typeName);
			return false;
		}

		boolean good = true;

		for (FieldDescriptor field : info.getFields()) {
			String fieldType = field.getType();
			int alignment = getAlignment(fieldType);

			if (alignment == 0) {
				// skip fields of unknown alignment
			} else if ((field.getOffset() % alignment) != 0) {
				System.err.format("Field %s.%s not naturally aligned%n", typeName, field.getName());
				good = false;
			}
		}

		return good;
	}

	/**
	 * Compute the alignment of the structure of the given name.
	 */
	private int computeAlignment(String type) {
		int alignment = 0;
		StructureDescriptor info = types.get(type);

		if (info != null) {
			for (FieldDescriptor field : info.getFields()) {
				String fieldType = field.getType();

				alignment = Math.max(getAlignment(fieldType), alignment);
			}
		}

		return alignment;
	}

	/**
	 * Compute the alignment of the specified type.
	 */
	private int getAlignment(String type) {
		/* An array type has the same alignment as its component type. */
		type = firstGroup(type, ArrayPattern);

		/* All pointer types have the same alignment. */
		if (type.endsWith("*")) {
			return pointerSize;
		}

		/* Remove any class/struct prefix or any bitfield suffix. */
		type = firstGroup(type, BasePattern);

		Integer alignment = alignments.get(type);

		if (alignment == null) {
			alignment = Integer.valueOf(computeAlignment(type));
			alignments.put(type, alignment);
		}

		return alignment.intValue();
	}

}
