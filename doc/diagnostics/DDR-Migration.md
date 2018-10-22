<!--
Copyright (c) 2018, 2020 IBM Corp. and others

This program and the accompanying materials are made available under
the terms of the Eclipse Public License 2.0 which accompanies this
distribution and is available at https://www.eclipse.org/legal/epl-2.0/
or the Apache License, Version 2.0 which accompanies this distribution and
is available at https://www.apache.org/licenses/LICENSE-2.0.

This Source Code may also be made available under the following
Secondary Licenses when the conditions for such availability set
forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
General Public License, version 2 with the GNU Classpath
Exception [1] and GNU General Public License, version 2 with the
OpenJDK Assembly Exception [2].

[1] https://www.gnu.org/software/classpath/license.html
[2] http://openjdk.java.net/legal/assembly-exception.html

SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
-->

# DDR Plugin Migration Guide

OpenJ9 includes tools that can be used to examine system core files in aid
of understanding the cause of unwanted behavior. One such tool, `jdmpview`
can be extended with 'plugins'. Plugins are classes with the
`com.ibm.j9ddr.tools.ddrinteractive.annotations.DebugExtension`
annotation found by searching files and directories in locations listed in
a search path. That search path is defined by using the system property
`plugins`; or by the environment variable `com.ibm.java.diagnostics.plugins`
or `com_ibm_java_diagnostics_plugins`.

The code in those plugins may access data structures of the JVM via
generated 'pointer' types. With the migration to using the `ddrgen` tool
of the OMR project, some changes to these generated classes was required.
This document describes the changes that have occurred, the motivation for
those changes and the impacts on plugins.

## Conditional Information

Most of the migration issues arise because the generated classes are derived
from information gather during a build for a single platform, rather than
from the union of all supported platforms.

## Conditional Fields

Some data structures have different sets of fields in different configurations.
An example is `MM_HeapLinkedFreeHeader` declared in `HeapLinkedFreeHeader.hpp`:

```c
    class MM_HeapLinkedFreeHeader
    {
    #if defined(SPLIT_NEXT_POINTER)
        uint32_t _next; /**< tagged pointer to the next free list entry, or a tagged pointer to NULL */
        uint32_t _nextHighBits; /**< the high 32 bits of the next pointer on 64-bit builds */
    #else /* defined(SPLIT_NEXT_POINTER) */
        uintptr_t _next; /**< tagged pointer to the next free list entry, or a tagged pointer to NULL */
    #endif /* defined(SPLIT_NEXT_POINTER) */
        /* [...snip...] */
    };
```

This has effects on `GCHeapLinkedFreeHeader_V1.java`. Because the `_nextHighBits` field is present
only in 'compressedrefs' configurations, the information must be accessed indirectly.

```java
    // GCHeapLinkedFreeHeader_V1.java
    private UDATA getNextImpl() throws CorruptDataException
    {
        Pointer nextEA = heapLinkedFreeHeaderPointer._nextEA();

        if (J9BuildFlags.interp_compressedObjectHeader) {
            U32Pointer nextPointer = U32Pointer.cast(nextEA);
            U32 lowBits = nextPointer.at(0);
            U32 highBits = nextPointer.at(1);

            return new UDATA(highBits).leftShift(32).bitOr(lowBits);
        } else {
            return UDATAPointer.cast(nextEA).at(0);
        }
    }
```

## Type Hierarchy Changes

The OMR `ddrgen` tool works by examining debugging information written to
object files by compilers. Consider the basic definition of the `UDATA` type:

```c
#if defined(OMR_ENV_DATA64)
  typedef U64 UDATA;
#else
  typedef U32 UDATA;
#endif
```

The tool used internally at IBM prior to open-sourcing OpenJ9 examined
the source code and was able to retain the references to `UDATA`. Now
we are at the mercy of the various compilers used to build OpenJ9 on
each platform. Some expand the references to `UDATA` and record
a use of `U32` or `U64` depending on whether we're dealing with a 32-bit
or 64-bit platform.

To deal with this, changes to the DDR type hierarchy were required.

| Old Hierarchy   | New Hierarchy |
| :--             | :-- |
| DataType        | DataType |
| +- Scalar       | +- Scalar |
| +- +- UScalar   | +- +- UScalar |
| +- +- +- U8     | +- +- +- U8 |
| +- +- +- U16    | +- +- +- U16 |
| +- +- +- UDATA  | +- +- +- UDATA |
| +- +- +- U32    | +- +- +- +- U32 |
| +- +- +- U64    | +- +- +- +- U64 |
| +- +- IScalar   | +- +- IScalar |
| +- +- +- I8     | +- +- +- I8 |
| +- +- +- I16    | +- +- +- I16 |
| +- +- +- IDATA  | +- +- +- IDATA |
| +- +- +- I32    | +- +- +- +- I32 |
| +- +- +- I64    | +- +- +- +- I64 |
| +- +- Void      | +- +- Void |

As a consequence, DDR code can no longer assume that the return type of a generated
field accessor matches the type in the native code. If the native declaration is
`U32` or `U64`, it is safe to use a Java cast to the type of the same name.

Consider this declaration from `mgmtinit.h`:

```c
    struct J9MemoryNotification {
        UDATA type;
        U_64 sequenceNumber;
        /* [...snip...] */
    };
```

which yields this generated code in `J9MemoryNotificationPointer.java`:

```java
    public UDATA type() throws CorruptDataException {
        return new UDATA(getUDATAAtOffset(J9MemoryNotification._typeOffset_));
    }
    public UDATA sequenceNumber() throws CorruptDataException {
        return new U64(getLongAtOffset(J9MemoryNotification._sequenceNumberOffset_));
    }
```

Notice that while `sequenceNumber()` is declared to return `UDATA` it is safe to cast
the object returned to `U64`.

On the other hand, on 64-bit platforms, `type` is also a 64-bit field, and on some platforms
the compiler will record that its type is `U64`, but it isn't safe to cast the object returned
to `U64` because on other platforms it really will be a `UDATA` object.
