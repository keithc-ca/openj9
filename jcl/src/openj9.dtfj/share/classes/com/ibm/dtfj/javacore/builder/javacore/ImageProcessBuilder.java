/*[INCLUDE-IF Sidecar18-SE]*/
/*
 * Copyright IBM Corp. and others 2007
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
package com.ibm.dtfj.javacore.builder.javacore;

import java.util.Map;
import java.util.Properties;

import com.ibm.dtfj.image.ImageModule;
import com.ibm.dtfj.image.ImagePointer;
import com.ibm.dtfj.image.ImageRegister;
import com.ibm.dtfj.image.ImageSection;
import com.ibm.dtfj.image.ImageStackFrame;
import com.ibm.dtfj.image.ImageSymbol;
import com.ibm.dtfj.image.ImageThread;
import com.ibm.dtfj.image.javacore.JCImageAddressSpace;
import com.ibm.dtfj.image.javacore.JCImageModule;
import com.ibm.dtfj.image.javacore.JCImageProcess;
import com.ibm.dtfj.image.javacore.JCImageRegister;
import com.ibm.dtfj.image.javacore.JCImageStackFrame;
import com.ibm.dtfj.image.javacore.JCImageSymbol;
import com.ibm.dtfj.image.javacore.JCImageThread;
import com.ibm.dtfj.java.javacore.JCInvalidArgumentsException;
import com.ibm.dtfj.javacore.builder.BuilderFailureException;
import com.ibm.dtfj.javacore.builder.IBuilderData;
import com.ibm.dtfj.javacore.builder.IImageProcessBuilder;
import com.ibm.dtfj.javacore.builder.IJavaRuntimeBuilder;

public class ImageProcessBuilder extends AbstractBuilderComponent implements IImageProcessBuilder {

	private JCImageAddressSpace fImageAddressSpace;
	private JCImageProcess fImageProcess;
	private BuilderContainer fBuilderContainer;
	private Map<String, Number> registers;

	public ImageProcessBuilder(JCImageAddressSpace imageAddressSpace, String id) throws JCInvalidArgumentsException {
		super(id);
		if (imageAddressSpace == null) {
			throw new IllegalArgumentException("A valid image must be passed");
		}
		fImageAddressSpace = imageAddressSpace;
		fBuilderContainer = getBuilderContainer();
		fImageProcess = new JCImageProcess(fImageAddressSpace);
	}

	/**
	 *
	 */
	@Override
	public IJavaRuntimeBuilder getCurrentJavaRuntimeBuilder() {
		return(IJavaRuntimeBuilder) fBuilderContainer.getLastAdded();
	}

	/**
	 *
	 */
	@Override
	public IJavaRuntimeBuilder getJavaRuntimeBuilder(String builderID) {
		return (IJavaRuntimeBuilder) fBuilderContainer.findComponent(builderID);
	}

	/**
	 *
	 * @param id
	 *
	 */
	@Override
	public IJavaRuntimeBuilder generateJavaRuntimeBuilder(String id) throws BuilderFailureException {
		IJavaRuntimeBuilder javaRuntimeBuilder = null;
		if (getJavaRuntimeBuilder(id) == null) {
			try {
				javaRuntimeBuilder = new JavaRuntimeBuilder(fImageProcess, id);
				if (javaRuntimeBuilder instanceof AbstractBuilderComponent) {
					fBuilderContainer.add((AbstractBuilderComponent)javaRuntimeBuilder);
				}
				else {
					throw new BuilderFailureException(javaRuntimeBuilder.toString() + " must also be an " + AbstractBuilderComponent.class);
				}
			} catch (JCInvalidArgumentsException e) {
				throw new BuilderFailureException(e);
			}
		}
		return javaRuntimeBuilder;
	}

	/**
	 *
	 */
	@Override
	public ImageModule addLibrary(String name) {
		ImageModule module = null;
		if(name != null && (module = fImageProcess.getLibrary(name)) == null) {
			JCImageModule jcmodule = new JCImageModule(name);
			fImageProcess.addLibrary(jcmodule);
			module = jcmodule;
		}
		return module;
	}

	/**
	 *
	 * @param size
	 */
	@Override
	public void setPointerSize(int size) {
		fImageProcess.setPointerSize(size);
	}

	/**
	 *
	 * @param Properties with String key and String value
	 */
	@Override
	public ImageThread addImageThread(long nativeThreadID, long systemThreadID, Properties properties) throws BuilderFailureException {
		try {
			ImagePointer pointer = fImageAddressSpace.getPointer(nativeThreadID);
			JCImageThread thread = fImageProcess.getImageThread(pointer);
			if (thread == null) {
				thread = new JCImageThread(pointer);
				if (registers != null) {
					// Add the registers to the first thread
					for (Map.Entry<String, Number> me : registers.entrySet()) {
						ImageRegister ir = new JCImageRegister(me.getKey(), me.getValue());
						thread.addRegister(ir);
					}
					registers = null;
				}
				fImageProcess.addImageThread(thread);
			}
			for (Map.Entry<?, ?> me : properties.entrySet()) {
				thread.addProperty(me.getKey(), me.getValue());
			}
			if (systemThreadID != IBuilderData.NOT_AVAILABLE) {
				pointer = fImageAddressSpace.getPointer(systemThreadID);
				thread.setSystemThreadID(pointer);
			}
			return thread;
		} catch (JCInvalidArgumentsException e) {
				throw new BuilderFailureException(e);
		}
	}

	/**
	 * Set signal
	 * @param signal number
	 */
	@Override
	public void setSignal(int signal) {
		fImageProcess.setSignal(signal);
	}

	/**
	 * Set command line
	 * @param command line string
	 */
	@Override
	public void setCommandLine(String cmdLine) {
		fImageProcess.setCommandLine(cmdLine);
	}

	/**
	 * Add a stack section to an existing image thread
	 * @param thread The native thread
	 * @param section The section we want to add
	 */
	@Override
	public ImageSection addImageStackSection(ImageThread thread, ImageSection section) {
		JCImageThread thread2 = (JCImageThread)thread;
		thread2.addImageStackSection(section);
		return section;
	}

	/**
	 * Set registers if available in javacore.
	 * @param regs Map of registers
	 */
	@Override
	public void setRegisters(Map<String, Number> regs) {
		registers = regs;
	}

	/**
	 * Add environment variables
	 * @param name
	 * @param value
	 */
	@Override
	public void addEnvironmentVariable(String name, String value) {
		fImageProcess.addEnvironment(name, value);
	}

	@Override
	public ImageSymbol addRoutine(ImageModule library, String name, long address) {
		ImagePointer addr = fImageAddressSpace.getPointer(address);
		ImageSymbol symbol = new JCImageSymbol(name, addr);
		JCImageModule mod = (JCImageModule)library;
		mod.addSymbol(symbol);
		return symbol;
	}

	@Override
	public ImageStackFrame addImageStackFrame(long nativeThreadID, String name,
			long baseAddress, long procAddress) {
		ImagePointer pointer = fImageAddressSpace.getPointer(nativeThreadID);
		JCImageThread thread = fImageProcess.getImageThread(pointer);
		ImagePointer ip = procAddress != IBuilderData.NOT_AVAILABLE ? fImageAddressSpace.getPointer(procAddress) : null;
		JCImageStackFrame stackFrame = new JCImageStackFrame(name, null, ip);
		if (thread != null) {
			thread.addImageStackFrame(stackFrame);
		}
		return stackFrame;
	}

	@Override
	public void setExecutable(ImageModule execMod) {
		fImageProcess.setExecutable(execMod);
	}

	@Override
	public void setID(String pid) {
		fImageProcess.setID(pid);
	}

	@Override
	public void setCurrentThreadID(long imageThreadID) {
		fImageProcess.setCurrentThreadID(imageThreadID);
	}

	@Override
	public void addProperty(ImageModule library, String name, String value) {
		JCImageModule mod = (JCImageModule)library;
		mod.addProperty(name, value);
	}
}
