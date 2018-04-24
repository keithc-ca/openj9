/*******************************************************************************
 * Copyright (c) 2018, 2018 IBM Corp. and others
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
package com.ibm.j9ddr.vm29.tools.ddrinteractive.structureformat.base;

import java.io.PrintStream;

import com.ibm.j9ddr.CorruptDataException;
import com.ibm.j9ddr.StructureTypeManager;
import com.ibm.j9ddr.tools.ddrinteractive.BaseFieldFormatter;
import com.ibm.j9ddr.tools.ddrinteractive.Context;
import com.ibm.j9ddr.tools.ddrinteractive.FormatWalkResult;
import com.ibm.j9ddr.tools.ddrinteractive.IStructureFormatter;
import com.ibm.j9ddr.vm29.pointer.I8Pointer;
import com.ibm.j9ddr.vm29.pointer.I16Pointer;
import com.ibm.j9ddr.vm29.pointer.I32Pointer;
import com.ibm.j9ddr.vm29.pointer.I64Pointer;
import com.ibm.j9ddr.vm29.pointer.U8Pointer;
import com.ibm.j9ddr.vm29.pointer.U16Pointer;
import com.ibm.j9ddr.vm29.pointer.U32Pointer;
import com.ibm.j9ddr.vm29.pointer.U64Pointer;
import com.ibm.j9ddr.vm29.types.Scalar;

public class BitfieldFormatter extends BaseFieldFormatter {

	@Override
	public FormatWalkResult format(String name, String type, String declaredType, int typeCode,
			long address, PrintStream out, Context context, IStructureFormatter structureFormatter)
			throws CorruptDataException {
		if (typeCode != StructureTypeManager.TYPE_BITFIELD) {
			return FormatWalkResult.KEEP_WALKING;
		}

		int colonIndex = type.indexOf(':');

		if (colonIndex < 0) {
			out.print("<<Missing ':' in bitfield type " + type + ">>");
			return FormatWalkResult.STOP_WALKING;
		}

		String baseType = type.substring(0, colonIndex).trim();
		String format;
		Scalar value;

		switch (baseType) {
		case "I8":
			format = "0x%1$02X (%1$d)";
			value = I8Pointer.cast(address).at(0);
			break;
		case "U8":
			format = "0x%1$02X (%1$d)";
			value = U8Pointer.cast(address).at(0);
			break;
		case "I16":
			format = "0x%1$04X (%1$d)";
			value = I16Pointer.cast(address).at(0);
			break;
		case "U16":
			format = "0x%1$04X (%1$d)";
			value = U16Pointer.cast(address).at(0);
			break;
		case "I32":
			format = "0x%1$08X (%1$d)";
			value = I32Pointer.cast(address).at(0);
			break;
		case "U32":
			format = "0x%1$08X (%1$d)";
			value = U32Pointer.cast(address).at(0);
			break;
		case "I64":
			format = "0x%1$016X (%1$d)";
			value = I64Pointer.cast(address).at(0);
			break;
		case "U64":
			format = "0x%1$016X (%1$d)";
			value = U64Pointer.cast(address).at(0);
			break;
		default:
			out.print("<<Unhandled bitfield type: " + type + ">>");
			return FormatWalkResult.STOP_WALKING;
		}

		out.print(String.format(format, value.longValue()));

		return FormatWalkResult.STOP_WALKING;
	}

}
