/*[INCLUDE-IF Sidecar18-SE]*/
/*******************************************************************************
 * Copyright (c) 2000, 2020 IBM Corp. and others
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
package com.ibm.jvm.trace.format.api;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.PrintStream;
import java.io.RandomAccessFile;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Collections;
import java.util.ConcurrentModificationException;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.NoSuchElementException;
import java.util.Properties;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.Vector;

public class TraceContext {
	protected static final int traceFormatMajorVersion = 2;
	protected static final int traceFormatMinorVersion = 0;

	public static final int INTERNAL = 0;
	public static final int EXTERNAL = 1;

	public static final int BYTE = 1;
	public static final int INT = 4;
	public static final int LONG = 8;
	/* This is used to represent a pointer sized data item when we don't yet know
	 * the platform so can't reify the value
	 */
	public static final int SIZE_T = -1;

	protected static final BigInteger MILLIS2SECONDS = BigInteger.valueOf(1000);
	protected static final BigInteger SECONDS2MINUTES = BigInteger.valueOf(60);
	protected static final BigInteger MINUTES2HOURS = BigInteger.valueOf(60);
	protected static final BigInteger HOURS2DAYS = BigInteger.valueOf(24);
	protected static final BigInteger MILLION = BigInteger.valueOf(1000000);

	float version;

	BigInteger highPrecisionTicksPerMillisecond = BigInteger.valueOf(1000);

	BigInteger lastWritePlatform = BigInteger.ZERO;
	BigInteger lastWriteSystem = BigInteger.ZERO;

	long totalTracePoints = 0;
	long totalRecords = 0;

	/* The message file being used by this particular context */
	protected MessageFile messageFile;
	protected Vector<MessageFile> auxiliaryMessageFiles;

	/* The trace headers used to initialize this context */
	TraceFileHeader metadata;

	PrintStream errorStream = System.out;
	long errorCount = 0;

	PrintStream warningStream = System.out;
	long warningCount = 0;

	PrintStream messageStream = System.out;
	PrintStream debugStream = System.out;
	int debugLevel = 0;

	/* time ordered list of live threads */
	List<TraceThread> threads = new ArrayList<>();
	/* live thread map */
	Map<Long, TraceThread> threadMap = new HashMap<>();
	boolean sorted = false;

	/* Map of thread IDs to list of associated names */
	Map<Long, Set<String>> knownThreads = new HashMap<>();

	/* The subset of threads we're interested in */
	Set<Long> filteredThreads;

	/* The time zone offset (in +/- minutes) to be added when formatting the time stamps */
	int timezoneOffset = 0;

	/*
	 * Internal constructor for testing.
	 */
	TraceContext(int recordSize, ByteOrder endian) {
		metadata = new TraceFileHeader(recordSize, endian);
	}

	/**
	 * Private constructor
	 * @see com.ibm.jvm.trace.format.api.TraceContext#getContext(byte[], int, InputStream, PrintStream, PrintStream, PrintStream, PrintStream)
	 * @throws IOException
	 */
	private TraceContext(ByteBuffer data, PrintStream message, PrintStream error, PrintStream warning, PrintStream debug) throws IOException {
		this.auxiliaryMessageFiles = new Vector<>();
		this.messageStream = message;
		this.warningStream = warning;
		this.errorStream = error;
		this.debugStream = debug;

		TraceFileHeader header = new TraceFileHeader(this, data);
		/*
		 * this assignment is also done in the TraceFileHeader
		 * constructor so it can be used for debug output.
		 */
		this.metadata = header;
	}

	/**
	 * Private constructor
	 * @see com.ibm.jvm.trace.format.api.TraceContext#getContext(byte[], int, InputStream, PrintStream, PrintStream, PrintStream, PrintStream)
	 * @throws IOException
	 */
	private TraceContext(ByteBuffer data, InputStream messageFile, PrintStream message, PrintStream error, PrintStream warning, PrintStream debug) throws IOException {
		this(data, message, error, warning, debug);

		this.messageFile = MessageFile.getMessageFile(messageFile, this);
	}

	/**
	 * Private constructor
	 * @see com.ibm.jvm.trace.format.api.TraceContext#getContext(byte[], int, InputStream, PrintStream, PrintStream, PrintStream, PrintStream)
	 * @throws IOException
	 */
	private TraceContext(ByteBuffer data, File messageFile, PrintStream message, PrintStream error, PrintStream warning, PrintStream debug) throws IOException {
		this(data, message, error, warning, debug);

		this.messageFile = MessageFile.getMessageFile(messageFile, this);
	}

	/**
	 * Reports a warning message
	 * @param source - the object generating the message
	 * @param message - the message to report
	 */
	public void warning(Object source, Object message) {
		warningCount++;

		if (warningStream != null) {
			warningStream.println(message);
		}
	}

	/**
	 * Reports an error message
	 * @param source - the object generating the message
	 * @param message - the message to report
	 */
	public void error(Object source, Object message) {
		errorCount++;

		if (errorStream != null) {
			errorStream.println("Error: " + message);
		}
	}

	/**
	 * Reports an informational message
	 * @param source - the object generating the message
	 * @param message - the message to report
	 */
	public void message(Object source, Object message) {
		if (messageStream != null) {
			messageStream.println(message);
		}
	}

	/**
	 * Reports a debug message
	 * @param source - the object generating the message
	 * @param level - the debug level of the message
	 * @param message - the message to report
	 */
	public void debug(Object source, int level, Object message) {
		if (debugStream != null && debugLevel >= level) {
			debugStream.println(message);
		}
	}

	/**
	 * Sets the destination for error messages
	 * @param stream - destination
	 */
	public void setErrorStream(PrintStream stream) {
		errorStream = stream;
	}

	/**
	 * Sets the destination for warning messages
	 * @param stream - destination
	 */
	public void setWarningStream(PrintStream stream) {
		warningStream = stream;
	}

	/**
	 * Sets the destination for debug messages
	 * @param stream - destination
	 */
	public void setDebugStream(PrintStream stream) {
		debugStream = stream;
	}

	/**
	 * Sets the destination for informational messages
	 * @param stream - destination
	 */
	public void setMessageStream(PrintStream stream) {
		messageStream = stream;
	}

	/**
	 * @return
	 */
	public float getVersion() {
		return version;
	}

	/**
	 * A description of the VM that generated the meta-data with which the context was constructed
	 * @return - VM description
	 */
	public String getVmVersionString() {
		return metadata.serviceSection.serviceString;
	}

	/**
	 * This returns the number of high precision ticks per millisecond as calculated based on trace data processed
	 * to date. This value will stabilize over time.
	 * @return - ticks per millisecond
	 */
	public BigInteger getHighPrecisionResolution() {
		return highPrecisionTicksPerMillisecond;
	}

	/**
	 * The size of the trace records expected by the context
	 * @return - size in bytes
	 */
	public int getRecordSize() {
		return metadata.recordSize;
	}

	/**
	 * Returns the size of the meta-data. This allows a file processor to skip to the
	 * offset of the first record.
	 * @return the length of the meta-data
	 */
	public int getHeaderSize() {
		return metadata.dataHeader.length;
	}

	/**
	 * Constructs a temporary TraceFileHeader from the supplied data and returns its size
	 * offset of the first record.
	 * @return the length of the meta-data
	 */
	public int getHeaderSize(ByteBuffer data) {
		data.order(metadata.byteOrder);
		DataHeader header = new DataHeader(this, data, "UTTH", false);
		return header.length;
	}

	/**
	 * The byte order of the trace data
	 * @return - a ByteOrder
	 */
	public ByteOrder order() {
		return metadata.byteOrder;
	}

	/**
	 * Accessor for the trace type, internal (wraps within a single buffer) or external (no wrapping)
	 * @return - trace type
	 */
	public int getTraceType() {
		return metadata.traceSection.type;
	}

	/**
	 * This forces the trace to a given type. This should only be necessary if you have metadata from
	 * a VM when no subscribers were attached and data from a subscriber that was registered afterwards.
	 * The inverted case could be true as well, but is much less likely to occur.
	 *
	 * If you're calling this then you should think about altering the sequence of calls used to get the
	 * metadata and trace data.
	 *
	 * @param type - the type of the trace data to process, either TraceContext.INTERNAL or TraceContext.EXTERNAL
	 *
	 * @deprecated this method is deprecated as its use implies a problem elsewhere
	 */
	@Deprecated
	public void setTraceType(int type) {
		boolean validType = true;
		if (debugStream != null) {
			String typeName;
			switch (type) {
			case TraceContext.EXTERNAL:
				typeName = "External";
				break;
			case TraceContext.INTERNAL:
				typeName = "Internal";
				break;
			default:
				typeName = "<" + type + " is an invalid trace type>";
				validType = false;
			}

			this.debug(this, 1, "Forcing trace type to " + typeName + " at user request");
		}

		if (metadata != null && metadata.traceSection != null && validType) {
			metadata.traceSection.type = type;
		} else {
			if (metadata == null || metadata.traceSection == null) {
				this.error(this, "Unable to set trace type due to incomplete trace context");
			} else {
				this.error(this, "Unable to set trace type to invalid type " + type);
			}
		}
	}

	/**
	 * The total number of trace points returned to date
	 * @return - number of trace points
	 */
	public long getTotalTracePoints() {
		return totalTracePoints;
	}

	/**
	 * The total number of records processed to date
	 * @return - number of records
	 */
	public long getTotalRecords() {
		return totalRecords;
	}

	/**
	 * The number of errors encountered to date
	 * @return - number of errors
	 */
	public long getErrorCount() {
		return errorCount;
	}

	/**
	 * The number of warnings encountered to date
	 * @return - number of warnings
	 */
	public long getWarningCount() {
		return warningCount;
	}

	/**
	 * @see com.ibm.jvm.trace.format.api.TraceContext#addMessageData(InputStream)
	 * @throws IOException
	 */
	public void addMessageData(File file) throws IOException {
		auxiliaryMessageFiles.add(MessageFile.getMessageFile(new FileInputStream(file), this));
	}

	/**
	 * Adds additional formatting strings to the set provided when the context was created.
	 * @param stream - input stream for accessing formatting data
	 * @throws IOException
	 */
	public void addMessageData(InputStream stream) throws IOException {
		auxiliaryMessageFiles.add(MessageFile.getMessageFile(stream, this));
	}

	/**
	 * Accessor for the pointer size associated with the trace data
	 * @return - pointer size in bytes (4 or 8)
	 */
	public int getPointerSize() {
		return metadata.traceSection.pointerSize;
	}

	/**
	 * @see com.ibm.jvm.trace.format.api.TraceContext#getContext(ByteBuffer, InputStream)
	 * @return
	 */
	public static TraceContext getContext(ByteBuffer data, File messageFile) throws IOException {
		return getContext(data, messageFile, System.out, System.err, System.out, null);
	}

	/**
	 * @see com.ibm.jvm.trace.format.api.TraceContext#getContext(ByteBuffer, InputStream)
	 * @throws IOException
	 */
	public static TraceContext getContext(byte[] data, int length, File messageFile) throws IOException {
		return getContext(data, length, messageFile, System.out, System.err, System.out, null);
	}

	/**
	 * @see com.ibm.jvm.trace.format.api.TraceContext#getContext(byte[], int, InputStream, PrintStream, PrintStream, PrintStream, PrintStream)
	 * @throws IOException
	 */
	public static TraceContext getContext(byte[] data, int length, File messageFile, PrintStream message, PrintStream error, PrintStream warning, PrintStream debug) throws IOException {
		return getContext(ByteBuffer.wrap(data, 0, length), messageFile, message, error, warning, debug);
	}

	/**
	 * Message and warning destinations default to stdout, the error destination defaults to stderr.
	 * @see com.ibm.jvm.trace.format.api.TraceContext#getContext(byte[], int, InputStream, PrintStream, PrintStream, PrintStream, PrintStream)
	 * @throws IOException
	 */
	public static TraceContext getContext(ByteBuffer data, InputStream messageFile) throws IOException {
		return getContext(data, messageFile, System.out, System.err, System.out, null);
	}

	/**
	 * Message and warning destinations default to stdout, the error destination defaults to stderr.
	 * @see com.ibm.jvm.trace.format.api.TraceContext#getContext(byte[], int, InputStream, PrintStream, PrintStream, PrintStream, PrintStream)
	 * @throws IOException
	 */
	public static TraceContext getContext(byte[] data, int length, InputStream messageFile) throws IOException {
		return getContext(data, length, messageFile, System.out, System.err, System.out, null);
	}

	/**
	 * This method constructs a context that can be used to format trace records produced by the VM instance that created the meta-data provided.
	 * The message file is used to format trace points into a human readable form and the print streams provided are where messages of that type are written to
	 * @param data - trace meta-data
	 * @param length - the length of the meta-data in the array
	 * @param messageFile - a file containing format strings
	 * @param message - informational message destination
	 * @param error - error message destination
	 * @param warning - warning message destination
	 * @param debug - debug message destination
	 * @return - a context to use for formatting trace records
	 * @throws IOException - if the message data can't be accessed
	 */
	public static TraceContext getContext(byte[] data, int length, InputStream messageFile, PrintStream message, PrintStream error, PrintStream warning, PrintStream debug) throws IOException {
		return getContext(ByteBuffer.wrap(data, 0, length), messageFile, message, error, warning, debug);
	}

	/**
	 * This method constructs a context that can be used to format trace records produced by the VM instance that created the meta-data provided.
	 * The message file is used to format trace points into a human readable form and the print streams provided are where messages of that type are written to
	 * @param data - trace meta-data
	 * @param messageFile - a file containing format strings
	 * @param message - informational message destination
	 * @param error - error message destination
	 * @param warning - warning message destination
	 * @param debug - debug message destination
	 * @return - a context to use for formatting trace records
	 * @throws IOException
	 */
	public static TraceContext getContext(ByteBuffer data, File messageFile, PrintStream message, PrintStream error, PrintStream warning, PrintStream debug) throws IOException {
		return new TraceContext(data, messageFile, message, warning, error, debug);
	}

	/**
	 * This method constructs a context that can be used to format trace records produced by the VM instance that created the meta-data provided.
	 * The message file is used to format trace points into a human readable form and the print streams provided are where messages of that type are written to
	 * @param data - trace meta-data
	 * @param messageFile - an input stream providing access to format strings
	 * @param message - informational message destination
	 * @param error - error message destination
	 * @param warning - warning message destination
	 * @param debug - debug message destination
	 * @return - a context to use for formatting trace records
	 * @throws IOException
	 */
	public static TraceContext getContext(ByteBuffer data, InputStream messageFile, PrintStream message, PrintStream error, PrintStream warning, PrintStream debug) throws IOException {
		return new TraceContext(data, messageFile, message, warning, error, debug);
	}

	/**
	 * @see com.ibm.jvm.trace.format.api.TraceContext#createByteStream(byte[], int, int)
	 */
	ByteStream createByteStream() {
		ByteStream stream = new ByteStream();
		stream.order(metadata.byteOrder);

		return stream;
	}

	/**
	 * @see com.ibm.jvm.trace.format.api.TraceContext#createByteStream(byte[], int, int)
	 * @param data - initial contents of the stream
	 */
	ByteStream createByteStream(byte data[]) {
		return createByteStream(data, 0);
	}

	/**
	 * @see com.ibm.jvm.trace.format.api.TraceContext#createByteStream(byte[], int, int)
	 * @param data - initial contents of the stream
	 * @param offset - offset into the data for the first byte of the stream
	 */
	ByteStream createByteStream(byte data[], int offset) {
		return createByteStream(data, offset, data.length - offset);
	}

	/**
	 * Constructs a ByteStream with an endian representation suitable for use with data related
	 * to the context creating it.
	 *
	 * @param data - initial contents of the stream
	 * @param offset - offset into the data for the first byte of the stream
	 * @param length - length past the offset for the last byte of data in the stream
	 * @return
	 */
	ByteStream createByteStream(byte data[], int offset, int length) {
		ByteStream stream = new ByteStream(data);
		stream.order(metadata.byteOrder);

		return stream;
	}

	/**
	 * This method is called to inform the context that we think the specified thread has
	 * terminated. This allows us to tidy up after dead threads so we don't leak memory.
	 *
	 * If the thread has been re-used later on then we do not remove it as there will still be
	 * more data available.
	 *
	 * @param thread - the terminated thread
	 * @param moreData - true if the thread has more trace points after this one, false otherwise
	 */
	synchronized void threadTerminated(TraceThread thread, boolean moreData) {
		if (debugStream != null) {
			debug(this, 2, "Thread " + thread + " terminated, removing thread from map" + (moreData ? "" : "and list"));
		}
		threadMap.remove(Long.valueOf(thread.getThreadID()));
		if (!moreData) {
			threads.remove(thread);
		}
	}

	/**
	 * This method takes a trace record, retrieves or creates the corresponding TraceThread object
	 * then appends the record to the threads data. This method may result in the first trace point from
	 * the record being read in and cached for thread sorting purposes.
	 * @param record - a trace record
	 * @return - TraceThread object that corresponds to the input record
	 * @throws IllegalArgumentException
	 */
	private synchronized TraceThread addData(TraceRecord record) {
		TraceThread thread;

		/* which thread does it belong to? */
		// Use the J9VMThread pointer as the unique id.
		Long ident = Long.valueOf(record.threadID);

		if (record.threadName.equals("Exception trace pseudo-thread")) {
			/*
			 * TODO: record the "low frequency" buffer count
			 * somewhere and warn if it gets 'high'. This was >= 10
			 * per sec in the 26 formatter.
			 */
		}

		/* record the threads current name for historical reference */
		Set<String> names = knownThreads.get(ident);
		if (names == null) {
			names = new HashSet<>();
			knownThreads.put(ident, names);
		}
		names.add(record.threadName);

		thread = threadMap.get(ident);
		if (thread == null || thread.stream == null) {
			thread = new TraceThread(this, record.threadID, record.threadSyn1, record.threadName);

			threads.add(thread);
			threadMap.put(ident, thread);

			/*
			 * TODO: can we refactor this method so that we
			 * append the data and refresh the thread prior to
			 * adding it to the thread list. That way we can insert
			 * it at the front of the list and it'll be bubbled to
			 * its correct location rather than the full sort we
			 * currently do
			 */
			sorted = false;
		} else {
			/* This is a workaround for the fact that presenting us with external data but
			 * have created a context with metadata from before they subscribed.
			 * If we've got multiple buffers from a single thread then we're external
			 * regardless of what the metadata said.
			 *
			 * TODO: create a proper heuristic for TraceRecord to determine the wrapping style
			 */
			metadata.traceSection.type = TraceContext.EXTERNAL;
		}

		/* update the timestamps and hi-precision ticks per ms */
		if (record.writePlatform.compareTo(lastWritePlatform) > 0) {
			lastWritePlatform = record.writePlatform;
		}

		if (record.writeSystem.compareTo(lastWriteSystem) > 0) {
			lastWriteSystem = record.writeSystem;
		}

		// Check the start time has been set, if not this calculation is not valid.
		if (metadata.traceSection.startPlatform.compareTo(BigInteger.ZERO) != 0 &&
			metadata.traceSection.startSystem != lastWriteSystem) {
			BigInteger uptime = lastWriteSystem.subtract(metadata.traceSection.startSystem);
			if (uptime.compareTo(BigInteger.ZERO) == 0) {
				highPrecisionTicksPerMillisecond = lastWritePlatform.subtract(metadata.traceSection.startPlatform);
			} else {
				highPrecisionTicksPerMillisecond = (lastWritePlatform.subtract(metadata.traceSection.startPlatform)).divide(uptime);
			}
		}

		/* if there's a filter set and this thread isn't part of it, discard the data */
		if (filteredThreads == null || filteredThreads.contains(Long.valueOf(record.threadID))) {
			thread.addRecord(record);
		}

		return thread;
	}

	/**
	 * @see com.ibm.jvm.trace.format.api.TraceContext#addData(TraceRecord)
	 * @param file - file containing trace data
	 * @param offset - the offset in the file of the buffer
	 * @return - the thread that generated the buffer
	 * @throws IOException
	 */
	public TraceThread addData(RandomAccessFile file, long offset) throws IOException {
		return addData(new TraceRecord(this, file, offset));
	}

	/**
	 * @see com.ibm.jvm.trace.format.api.TraceContext#addData(TraceRecord)
	 * @param data - a trace buffer generated by the JVM corresponding the the context
	 * @return - the thread that generated the buffer
	 */
	public TraceThread addData(byte[] data) {
		return addData(new TraceRecord(this, data));
	}

	/**
	 * This method tells the formatter that there was data discarded at this point in
	 * the stream of records. This has the affect of discarding any trace point fragments
	 * that are awaiting data for completion across all threads. When a trace point iterator
	 * encounters one of the locations where data was discarded it will throw a MissingDataException
	 * as for records discarded by the trace engine.
	 *
	 * This makes the assumption that the records are being supplied chronologically.
	 */
	public synchronized void discardedData() {
		Iterator<?> itr = threads.iterator();
		while (itr.hasNext()) {
			TraceThread thread = (TraceThread)itr.next();

			thread.userDiscardedData();
		}
	}

	/**
	 * The time of trace initialization in the traced JVM in high precision format
	 * This should be used in conjunction with the system start time
	 *
	 * @return - high precision start time
	 */
	public BigInteger getStartPlatform() {
		return metadata.traceSection.startPlatform;
	}

	/**
	 * The system time of trace initialization in the traced JVM
	 * @return - start time
	 */
	public BigInteger getStartSystem() {
		return metadata.traceSection.startSystem;
	}

	/**
	 * Provides access to the current set of active threads.
	 */
	static final class ThreadListIterator implements Iterator<TraceThread> {
		/* The next thread to return via getThreads. This is needed because we delete threads once
		 * they've been completely processed which results in ConMod Exceptions
		 */
		TraceThread threadCursor = null;
		List<TraceThread> threads;
		Iterator<TraceThread> iterator;

		ThreadListIterator(List<TraceThread> list) {
			iterator = list.iterator();
			threads = list;
			threadCursor = null;
		}

		/* (non-Javadoc)
		 * @see java.util.Iterator#hasNext()
		 */
		@Override
		public boolean hasNext() {
			return iterator.hasNext() || (threadCursor != null);
		}

		/* (non-Javadoc)
		 * @see java.util.Iterator#next()
		 */
		@Override
		public TraceThread next() {
			TraceThread thread = null;

			synchronized (threads) {
				try {
					if (threadCursor == null) {
						threadCursor = iterator.next();
					}

					thread = threadCursor;
					threadCursor = iterator.next();

					return thread;
				} catch (ConcurrentModificationException e) {
					if (threadCursor == null) {
						throw e;
					}

					Iterator<TraceThread> itr = threads.iterator();

					/* skip to our cursor again - use object equality as threads can recur */
					while (itr.hasNext() && itr.next() != threadCursor) {
						/* searching */
					}
					iterator = itr;

					return next();
				} catch (NoSuchElementException e) {
					threadCursor = null;

					if (thread == null) {
						throw e;
					}

					return thread;
				}
			}
		}

		/* (non-Javadoc)
		 * @see java.util.Iterator#remove()
		 */
		@Override
		public void remove() {
			throw new UnsupportedOperationException();
		}
	}

	/**
	 * Allows chronologically ordered access to trace points across all threads present in
	 * the data set at the time of the call to next.
	 */
	final class SortedTracepointIterator implements Iterator<Object> {

		/**
		 * True if a call to next() will return a trace point, false if it will return null.
		 * The return from this method may change from false to true as data is added to the
		 * context.
		 * @see java.util.Iterator#hasNext()
		 */
		@Override
		public boolean hasNext() {
			sort();
			if (threads.size() > 0) {
				return threads.get(0).getIterator().hasNext();
			}

			return false;
		}

		/**
		 * The next trace point in chronological order from the current data set.
		 * May return null.
		 * @see java.util.Iterator#next()
		 */
		@Override
		public Object next() {
			sort();

			TracePoint tp = (TracePoint) threads.get(0).getIterator().next();

			/*
			 * we set sorted to true because we know we're a well
			 * behaved user of the thread iterators. The thread
			 * iterators can't know this so they set it to false.
			 */
			sorted = true;
			return tp;
		}

		/**
		 * This function is used to sort the known threads based on the time stamp of
		 * their next trace point. When possible we preserve the ordering of the thread list so that
		 * we only have to bubble the thread at the head of the list to its new location and we're
		 * once again sorted. This obviates the need for a full sort of the entire thread list each
		 * time a trace point is returned.
		 * Use of the thread level iterators breaks this ordering and mandates a full sort.
		 */
		private void sort() {
			/* are the first two records out of order? */
			if (!sorted) {
				/*
				 * we've been accessed via the thread
				 * level iterators so all our ordering
				 * guarantees are void, full sort
				 * needed.
				 * Ensure that all threads are up to date first
				 */
				for (int i = 0; i < threads.size(); i++) {
					threads.get(i).refresh();
				}

				Collections.sort(threads);
				sorted = true;
			} else if (threads.size() > 1 && threads.get(0).compareTo(threads.get(1)) > 0) {
				TraceThread bubble = threads.get(0);
				threads.remove(0);
				Iterator<TraceThread> itr = threads.iterator();

				for (int i = 0; itr.hasNext(); i++) {
					if (bubble.compareTo(itr.next()) <= 0) {
						threads.add(i, bubble);
						bubble = null;
						break;
					}
				}

				if (bubble != null) {
					threads.add(bubble);
				}

				sorted = true;
			}
		}

		/**
		 * Removal through this iterator is not supported
		 * @see java.util.Iterator#remove()
		 */
		public void remove() {
			throw new UnsupportedOperationException();
		}
	}

	/**
	 * This method provides an iterator to walk the set of known threads; those that have not
	 * returned trace points that indicate the thread is exiting. This iterator may be invalidated
	 * by adding new trace data to the context.
	 *
	 * @return - iterator over non-dead threads
	 */
	public Iterator getThreads() {
		return new ThreadListIterator(threads);
	}

	/**
	 * This method returns trace points in chronological order on the current data set across threads.
	 * This operates on the data available at the time the method is called. If new data is added
	 * the oldest trace point from the expanded data set will be returned, irrespective if newer
	 * trace points have already been returned.
	 * @return
	 */
	public Iterator getTracepoints() {
		return new SortedTracepointIterator();
	}

	/**
	 * This method adds a thread id to the thread filter. Only those threads in the filter will have data
	 * returned via any of the iterators.
	 * @param threadID - the id of the thread to include in the filter
	 */
	public void addThreadToFilter(Long threadID) {
		if (filteredThreads == null) {
			filteredThreads = new TreeSet<>();
		}

		filteredThreads.add(threadID);
	}

	/**
	 * Sets the timezone offset from GMT in +/- minutes to be applied to the time stamp when formatted.
	 * @param minutes - timezone offset in minutes
	 */
	public void setTimeZoneOffset(int minutes) {
		timezoneOffset = minutes;
	}

	/**
	 * Formats a time stamp into a human readable form to nanosecond precision (not accuracy)
	 * @param time - 64bit time stamp
	 * @return - a time in the form hh:mm:ss.xxxxxxxxx
	 */
	final String getFormattedTime(BigInteger time) {
		switch (metadata.processorSection.counter) {
		case 2:
		case 4:
		case 5:
		case 7: {
			/*
			 * X86 with RDTSC, MFTB, MSPR, J9
			 */
			if (version >= 1.1) {
				long timeRaw = time.subtract(metadata.traceSection.startPlatform).longValue();
				long hiprec = highPrecisionTicksPerMillisecond.longValue();
				long millis = (timeRaw / hiprec) + metadata.traceSection.startSystem.longValue() + (timezoneOffset * 60 * 1000);
				long hours = ((((millis) / 1000) / 60) / 60) % 24;
				long minutes = (((millis) / 1000) / 60) % 60;
				long seconds = ((millis) / 1000) % 60;
				long nanos = (((millis) % 1000) * 1000000) + (((timeRaw % hiprec) * 1000000) / hiprec);

				return String.format("%02d:%02d:%02d.%09d", hours, minutes, seconds, nanos);
			} else {
				return String.format("%016x", time);
			}
		}

		case 3: {
			/*
			 * Power/Power PC
			 */
			long seconds = (time.shiftRight(32).longValue() & 0xffffffffL) + (timezoneOffset * 60);
			long hh = (seconds / 3600) % 24;
			long mm = (seconds / 60) % 60;
			long ss = seconds % 60;
			long nanos = time.longValue() & 0xffffffffL;

			return String.format("%02d:%02d:%02d.%09d", hh, mm, ss, nanos);
		}
		case 6: {
			/*
			 * System/390
			 */
			long micros = (time.shiftRight(12).longValue() & 0xfffffffffffffL) + (timezoneOffset * 60 * 1000000);
			long hh = ((micros / 3600000000L) + timezoneOffset) % 24;
			long mm = (micros / 60000000) % 60;
			long ss = (micros / 1000000) % 60;
			long picos = (((time.longValue() & 0xfffL) | ((micros % 1000000) << 12)) * 244140625) / 1000000;

			return String.format("%02d:%02d:%02d.%012d", hh, mm, ss, picos);
		}
		default: {
			return String.format("%016x", time);
		}
		}
	}

	public String formatPointer(long value) {
		final String template;

		if (getPointerSize() == 4) {
			template = "0x%08x";
		} else {
			template = "0x%016x";
		}

		return String.format(template, value);
	}

	public String summary() {
		String nl = System.lineSeparator();
		StringBuilder sb = new StringBuilder("                Trace Summary").append(nl);
		sb.append(nl);
		sb.append(metadata.serviceSection.summary()).append(nl);
		sb.append(metadata.startupSection.summary());
		/* startup section already has a trailing line separator */
		sb.append(metadata.processorSection.summary()).append(nl);
		sb.append(metadata.activeSection.summary()).append(nl);
		sb.append(metadata.traceSection.summary()).append(nl);

		/* add the known threads */
		sb.append("Active threads").append(nl);

		for (Map.Entry<Long, Set<String>> entry : knownThreads.entrySet()) {
			Long key = entry.getKey();
			Set<String> names = entry.getValue();

			sb.append("        ").append(formatPointer(key.longValue()));
			if (names.size() <= 1) {
				for (String name : names) {
					sb.append("  ").append(name).append(nl);
				}
			} else {
				String spaces = getPointerSize() == 4 ? "" : "        ";
				sb.append("  (id reused)").append(nl);
				for (String name : names) {
					sb.append("                    ").append(spaces).append(name).append(nl);
				}
			}
		}

		sb.append(nl);

		return sb.toString();
	}

	public void setDebugLevel(int level) {
		debugLevel = level;
	}

	public String statistics() {
		int nameWidth = 0;
		long hitMax = 0;
		long bytesMax = 0;

		Map<String, Properties> stats = this.messageFile.getStatistics();
		for (int i = 0; i < this.auxiliaryMessageFiles.size(); i++) {
			stats.putAll(this.auxiliaryMessageFiles.get(i).getStatistics());
		}

		TreeMap<String, Long> componentByteTotals = new TreeMap<>();
		List<NameValueTuple<Long>> componentTotals = new Vector<>();
		TreeMap<String, Long> hitCount = new TreeMap<>();
		List<NameValueTuple<Long>> tracePointCounts = new Vector<>();
		List<NameValueTuple<Long>> tracePointBytes = new Vector<>();

		/* generate totals per component and trace point */
		long totalBytes = 0;

		for (Map.Entry<String, Properties> entry : stats.entrySet()) {
			String tp = entry.getKey();
			Properties props = entry.getValue();

			String component = props.getProperty("component", "");
			int width = tp.length();
			if (nameWidth < width) {
				nameWidth = width;
			}

			if (props.containsKey("count")) {
				long count = (Long) props.get("count");
				if (hitMax < count) {
					hitMax = count;
				}

				hitCount.put(tp, Long.valueOf(count));
				tracePointCounts.add(new NameValueTuple<>(tp, (Long) props.get("count")));
			}

			if (props.containsKey("bytes")) {
				long bytes = (Long) props.get("bytes");
				if (bytesMax < bytes) {
					bytesMax = bytes;
				}
				totalBytes += bytes;
				tracePointBytes.add(new NameValueTuple<>(tp, (Long) props.get("bytes")));

				long total = 0;
				if (componentByteTotals.containsKey(component)) {
					total = (Long) componentByteTotals.get(component);
				}
				componentByteTotals.put(component, Long.valueOf(total + bytes));
			}
		}

		StringBuffer sb = new StringBuffer();
		String nl = System.lineSeparator();

		/* Write out the component byte totals */
		sb.append("Component totals (bytes)").append(nl);

		for (Map.Entry<String, Long> entry : componentByteTotals.entrySet()) {
			String component = entry.getKey();
			Long total = entry.getValue();

			componentTotals.add(new NameValueTuple<>(component, total));
		}

		Collections.sort(componentTotals);

		String totalFormat = "%-" + nameWidth + "s %d%n";
		for (NameValueTuple<Long> tuple : componentTotals) {
			sb.append(String.format(totalFormat, tuple.name() + ":", tuple.value()));
		}

		sb.append("Total bytes: " + totalBytes).append(nl);
		sb.append(nl);

		/* Write out the trace point hit count totals */
		sb.append("Trace point counts:").append(nl);
		Collections.sort(tracePointCounts);

		String countFormat = "%-" + nameWidth + "s %d (level: %2d)%n";
		for (NameValueTuple<Long> tuple : tracePointCounts) {
			String name = tuple.name();
			int dotIndex = name.indexOf('.');
			int tpId = Integer.parseInt(name.substring(dotIndex + 1));
			Message msg = messageFile.getMessageFromID(name.substring(0, dotIndex), tpId);

			sb.append(String.format(countFormat, name + ":", tuple.value(), msg.getLevel()));
		}
		sb.append(nl);

		/* Write out the trace point byte totals */
		sb.append("Trace point totals (bytes):").append(nl);
		Collections.sort(tracePointBytes);

		String bytesFormat = "%-" + nameWidth + "s %-" + (Math.log10(bytesMax) + 1) + "d (%6.2f%%, level: %2d, hit count: %" + (Math.log10(hitMax) + 1) + "d)%n";
		for (NameValueTuple<Long> tuple : tracePointBytes) {
			String name = tuple.name();
			int dotIndex = name.indexOf('.');
			int tpId = Integer.parseInt(name.substring(dotIndex + 1));
			Message msg = messageFile.getMessageFromID(name.substring(0, dotIndex), tpId);
			Long bytes = tuple.value();
			double bytesPercent = (bytes.doubleValue() * 100.0) / totalBytes;

			sb.append(String.format(bytesFormat, name + ":",
					bytes, bytesPercent, msg.getLevel(), hitCount.get(name)));
		}

		return sb.toString();
	}
}

final class NameValueTuple<T extends Comparable<T>> implements Comparable<NameValueTuple<T>> {
	String name;
	T value;

	NameValueTuple(String name, T value) {
		this.name = name;
		this.value = value;
	}

	@Override
	public int compareTo(NameValueTuple<T> o) {
		return value.compareTo(o.value);
	}

	public String name() {
		return name;
	}

	public T value() {
		return value;
	}

	public void setValue(T value) {
		this.value = value;
	}
}
