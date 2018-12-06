/*******************************************************************************
 * Copyright (c) 2010, 2018 IBM Corp. and others
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
package com.ibm.j9ddr;

import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.regex.Pattern;

import com.ibm.j9ddr.StructureReader.StructureDescriptor;

/**
 * Helper class for working with structure data.
 * 
 * @author andhall
 */
public class StructureTypeManager
{
	
	public static final int TYPE_UNKNOWN = -1;
	
	public static final int TYPE_VOID = 0;

	public static final int TYPE_U8 = 1;
	public static final int TYPE_U16 = 2;
	public static final int TYPE_U32 = 3;
	public static final int TYPE_U64 = 4;
	public static final int TYPE_UDATA = 5;
	
	public static final int TYPE_I8 = 6;
	public static final int TYPE_I16 = 7;
	public static final int TYPE_I32 = 8;
	public static final int TYPE_I64 = 9;
	public static final int TYPE_IDATA = 10;
	
	/* Values between this represent simple types */ 
	public static final int TYPE_SIMPLE_MIN = 1;
	public static final int TYPE_SIMPLE_MAX = 99;

	/* Semi-simple types */
	public static final int TYPE_BOOL = 100;
	public static final int TYPE_ENUM = 101;
	public static final int TYPE_DOUBLE = 102;
	public static final int TYPE_FLOAT = 103;
	public static final int TYPE_BITFIELD = 104;
	
	public static final int TYPE_ENUM_POINTER = 105;

	/* Simple pointers */
	public static final int TYPE_POINTER = 110;
	public static final int TYPE_J9SRP = 111;
	public static final int TYPE_J9WSRP = 112;
	public static final int TYPE_ARRAY = 113;
	
	/* Pointers to SRPs */
	public static final int TYPE_J9SRP_POINTER = 114;
	public static final int TYPE_J9WSRP_POINTER = 115;

	/* struct/class */
	public static final int TYPE_STRUCTURE = 120;
	public static final int TYPE_STRUCTURE_POINTER = 121;
	
	/* Compressed headers & pointers */
	public static final int TYPE_FJ9OBJECT = 130;
	public static final int TYPE_FJ9OBJECT_POINTER = 131;
	public static final int TYPE_J9OBJECTCLASS = 132;
	public static final int TYPE_J9OBJECTCLASS_POINTER = 133;
	public static final int TYPE_J9OBJECTMONITOR = 134;
	public static final int TYPE_J9OBJECTMONITOR_POINTER = 135;

	public static final Map<String, Integer> simpleTypeCodeMap;
	public static final Map<Integer, String> simpleTypeAccessorMap;
	
	private final Set<String> structureNames = new HashSet<String>();
	private final Set<String> enumNames = new HashSet<String>();
	
	static 
	{
		Map<String, Integer> localSimpleTypeCodeMap = new HashMap<String, Integer>();
		localSimpleTypeCodeMap.put("void", TYPE_VOID);
		
		localSimpleTypeCodeMap.put("U8", TYPE_U8);
		localSimpleTypeCodeMap.put("U16", TYPE_U16);
		localSimpleTypeCodeMap.put("U32", TYPE_U32);
		localSimpleTypeCodeMap.put("U64", TYPE_U64);
		localSimpleTypeCodeMap.put("UDATA", TYPE_UDATA);

		localSimpleTypeCodeMap.put("I8", TYPE_I8);
		localSimpleTypeCodeMap.put("I16", TYPE_I16);
		localSimpleTypeCodeMap.put("I32", TYPE_I32);
		localSimpleTypeCodeMap.put("I64", TYPE_I64);
		localSimpleTypeCodeMap.put("IDATA", TYPE_IDATA);
		
		localSimpleTypeCodeMap.put("char", TYPE_U8);
		
		localSimpleTypeCodeMap.put("bool", TYPE_BOOL);
		localSimpleTypeCodeMap.put("double", TYPE_DOUBLE);
		localSimpleTypeCodeMap.put("float", TYPE_FLOAT);
		localSimpleTypeCodeMap.put("fj9object_t", TYPE_FJ9OBJECT);
		localSimpleTypeCodeMap.put("iconv_t", TYPE_IDATA);
		localSimpleTypeCodeMap.put("j9objectclass_t", TYPE_J9OBJECTCLASS);
		localSimpleTypeCodeMap.put("j9objectmonitor_t", TYPE_J9OBJECTMONITOR);

		simpleTypeCodeMap = Collections.unmodifiableMap(localSimpleTypeCodeMap);
		
		Map<Integer, String> localSimpleTypeAccessorMap = new HashMap<Integer, String>();
		
		localSimpleTypeAccessorMap.put(TYPE_U8, "getByteAtOffset");
		localSimpleTypeAccessorMap.put(TYPE_U16, "getShortAtOffset");
		localSimpleTypeAccessorMap.put(TYPE_U32, "getIntAtOffset");
		localSimpleTypeAccessorMap.put(TYPE_U64, "getLongAtOffset");
		localSimpleTypeAccessorMap.put(TYPE_UDATA, "getUDATAAtOffset");

		localSimpleTypeAccessorMap.put(TYPE_I8, "getByteAtOffset");
		localSimpleTypeAccessorMap.put(TYPE_I16, "getShortAtOffset");
		localSimpleTypeAccessorMap.put(TYPE_I32, "getIntAtOffset");
		localSimpleTypeAccessorMap.put(TYPE_I64, "getLongAtOffset");
		localSimpleTypeAccessorMap.put(TYPE_IDATA, "getIDATAAtOffset");
		
		simpleTypeAccessorMap = Collections.unmodifiableMap(localSimpleTypeAccessorMap);
	}
	
	public StructureTypeManager(Collection<StructureDescriptor> structures)
	{
		//For the purposes of matching types, we assume that any "structure" with constants but no fields is an enum. (It could also be a bag of constants,
		//but that doesn't matter here).
		for (StructureDescriptor structure : structures) {
			if (structure.getFields().size() == 0 && structure.getConstants().size() > 0) {
				enumNames.add(structure.getName());
			} else {
				structureNames.add(structure.getName());
			}
		}
	}

	private static final Pattern CVQualifierPattern = Pattern.compile("\\b(const|volatile)\\s+");

	public int getType(String rawType)
	{
		// strip out any const/volatile qualifiers
		String type = CVQualifierPattern.matcher(rawType).replaceAll("").trim();

		// look for the simple types
		if (simpleTypeCodeMap.containsKey(type)) {
			return simpleTypeCodeMap.get(type);
		}

		CTypeParser parser = new CTypeParser(type);

		// array?
		if (parser.getSuffix().endsWith("]")) {
			return TYPE_ARRAY;
		}

		// bit field?
		if (parser.getSuffix().contains(":")) {
			return TYPE_BITFIELD;
		}

		// pointer?
		if (type.endsWith("*")) {
			int pointerType = getType(type.substring(0, type.length() - 1).trim());
			switch (pointerType) {
			case TYPE_UNKNOWN:
				return TYPE_UNKNOWN;
			case TYPE_STRUCTURE:
				return TYPE_STRUCTURE_POINTER;
			case TYPE_FJ9OBJECT:
				return TYPE_FJ9OBJECT_POINTER;
			case TYPE_J9OBJECTCLASS:
				return TYPE_J9OBJECTCLASS_POINTER;
			case TYPE_J9OBJECTMONITOR:
				return TYPE_J9OBJECTMONITOR_POINTER;
			case TYPE_J9SRP:
				return TYPE_J9SRP_POINTER;
			case TYPE_J9WSRP:
				return TYPE_J9WSRP_POINTER;
			case TYPE_ENUM:
				return TYPE_ENUM_POINTER;
			default:
				return TYPE_POINTER;
			}
		}

		/* SRP?
		 * Match types like J9SRP or J9SRP(UDATA) but not J9SRP* or J9SRPHashTable.
		 */
		if (type.equals("J9SRP") || type.startsWith("J9SRP(")) {
			return TYPE_J9SRP;
		}

		/* WSRP?
		 * Match types like J9WSRP or J9WSRP(UDATA) but not J9WSRP*.
		 */
		if (type.equals("J9WSRP") || type.startsWith("J9WSRP(")) {
			return TYPE_J9WSRP;
		}

		// struct?
		if (type.startsWith("struct ") || type.startsWith("class ") || structureNames.contains(type.trim())) {
			return TYPE_STRUCTURE;
		}

		if (type.startsWith("enum ") || enumNames.contains(type.trim())) {
			return TYPE_ENUM;
		}

		return decodeOther(type);
	}

	private static final Pattern WordSeparator = Pattern.compile("\\s+");

	private static int decodeOther(String type) {
		// we break out of this block as soon as the input is no longer considered 'good'
		good: {
			int bits = 0;
			int num_char = 0;
			int num_short = 0;
			int num_int = 0;
			int num_long = 0;
			int num_signed = 0;
			int num_unsigned = 0;
			int num_std_signed = 0;
			int num_std_unsigned = 0;

			/*
			 * C allows types and modifiers in any order, so we count the number of
			 * occurrences of each word to verify the combination is reasonable.
			 */
			for (String word : WordSeparator.split(type)) {
				int num = 0;

				switch (word) {
				// built-in types and modifiers 
				case "char":
					num_char += 1;
					num = num_char;
					break;
				case "short":
					num_short += 1;
					num = num_short;
					break;
				case "int":
					num_int += 1;
					num = num_int;
					break;
				case "long":
					// 'long' can be repeated twice: test the old count
					num = num_long;
					num_long += 1;
					break;
				case "signed":
					num_signed += 1;
					num = num_signed;
					break;
				case "unsigned":
					num_unsigned += 1;
					num = num_unsigned;
					break;

				/*
				 * Capture the width of a standard type and whether it is signed or
				 * unsigned. This also disallows the combination of a standard type
				 * together with 'signed' or 'unsigned' keywords.
				 */

				// standard signed types
				case "int8_t":
					bits = 8;
					num_signed += 1;
					num_std_signed += 1;
					num = num_std_signed;
					break;
				case "int16_t":
					bits = 16;
					num_signed += 1;
					num_std_signed += 1;
					num = num_std_signed;
					break;
				case "int32_t":
					bits = 32;
					num_signed += 1;
					num_std_signed += 1;
					num = num_std_signed;
					break;
				case "int64_t":
					bits = 64;
					num_signed += 1;
					num_std_signed += 1;
					num = num_std_signed;
					break;

				// standard unsigned types
				case "uint8_t":
					bits = 8;
					num_unsigned += 1;
					num_std_unsigned += 1;
					num = num_std_unsigned;
					break;
				case "uint16_t":
					bits = 16;
					num_unsigned += 1;
					num_std_unsigned += 1;
					num = num_std_unsigned;
					break;
				case "uint32_t":
					bits = 32;
					num_unsigned += 1;
					num_std_unsigned += 1;
					num = num_std_unsigned;
					break;
				case "uint64_t":
					bits = 64;
					num_unsigned += 1;
					num_std_unsigned += 1;
					num = num_std_unsigned;
					break;

				// standard pointer types
				case "intptr_t":
					bits = -1;
					num_signed += 1;
					num_std_signed += 1;
					num = num_std_signed;
					break;
				case "uintptr_t":
					bits = -1;
					num_unsigned += 1;
					num_std_unsigned += 1;
					num = num_std_unsigned;
					break;

				default:
					break good;
				}

				if (num > 1) {
					break good;
				}
			}

			if ((num_signed + num_unsigned) > 1) {
				// at most one of 'signed' or 'unsigned' is allowed
				break good;
			}

			int num_std = num_std_signed + num_std_unsigned;

			if (num_std > 1) {
				// at most one standard type is allowed
				break good;
			} else if (num_std > 0) {
				if ((num_char + num_short + num_int + num_long) > 0) {
					// standard types cannot be combined with built-in types
					break good;
				}
			} else if ((1 == num_char) && (0 == num_short) && (0 == num_int) && (0 == num_long)) {
				bits = 8;
			} else if ((0 == num_char) && (1 == num_short) && (1 >= num_int) && (0 == num_long)) {
				bits = 16;
			} else if ((0 == num_char) && (0 == num_short) && (1 == num_int) && (0 == num_long)) {
				bits = 32;
			} else if ((0 == num_char) && (0 == num_short) && (1 >= num_int) && (1 == num_long)) {
				bits = -1; // assume 'long' and 'intptr_t' are the same size
			} else if ((0 == num_char) && (0 == num_short) && (1 >= num_int) && (2 == num_long)) {
				bits = 64;
			} else {
				break good;
			}

			// types are signed unless explicitly 'unsigned'
			switch (bits) {
			case -1:
				return (0 == num_unsigned) ? TYPE_IDATA : TYPE_UDATA;
			case 8:
				return (0 == num_unsigned) ? TYPE_I8 : TYPE_U8;
			case 16:
				return (0 == num_unsigned) ? TYPE_I16 : TYPE_U16;
			case 32:
				return (0 == num_unsigned) ? TYPE_I32 : TYPE_U32;
			case 64:
				return (0 == num_unsigned) ? TYPE_I64 : TYPE_U64;
			default:
				throw new IllegalStateException();
			}
		}

		// we found an unreasonable combination of keywords
		return TYPE_UNKNOWN;
	}

}
