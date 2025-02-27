/*******************************************************************************
 * Copyright IBM Corp. and others 1991
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
 *******************************************************************************/

#include <string.h>
#include "j9protos.h"
#include "j9port.h"
#include "vm_internal.h"
#include "j2sever.h"
#include "j9vmnls.h"
#include "j9version.h"
#include "jvminit.h"
#include "ut_j9vm.h"
#include "vmargs_api.h"
#include "vendor_version.h"

#if defined(LINUX)
/* Copy the system properties names and values into malloced memory */
static void copySystemProperties(J9JavaVM* vm);
#endif /* defined(LINUX) */

static U_8*  unicodeEscapeStringToMUtf8(J9JavaVM * vm, const char* escapeString, UDATA escapeLength);
static UDATA getLibSubDir(J9JavaVM *VM, const char *subDir, char **value);

#define JAVA_ENDORSED_DIRS "java.endorsed.dirs"
#define JAVA_EXT_DIRS "java.ext.dirs"

UDATA addSystemProperty(J9JavaVM * vm, const char* propName,  const char* propValue, UDATA flags);
static char * getOptionArg(J9JavaVM *vm, IDATA argIndex, UDATA optionNameLen);
static UDATA addPropertyForOptionWithPathArg(J9JavaVM *vm, const char *optionName, UDATA optionNameLen, const char *propName);
static UDATA addPropertyForOptionWithModuleListArg(J9JavaVM *vm, const char *optionName, IDATA optionNameLen, const char *propName);
static UDATA addPropertiesForOptionWithAssignArg(J9JavaVM *vm, const char *optionName, UDATA optionNameLen, const char *basePropName, UDATA basePropNameLen, UDATA *propertyCount);
static UDATA addPropertyForOptionWithEqualsArg(J9JavaVM *vm, const char *optionName, UDATA optionNameLen, const char *propName);
static UDATA addModularitySystemProperties(J9JavaVM * vm);

/*
 * Create a copy of the given string in allocated memory.
 *
 * @param [in] vm the J9JavaVM
 * @param [in] source the null-terminated string to be copied
 *
 * @return a copy of the string in allocated memory
 *         or NULL if space could not be allocated
 */
static char *
copyToMem(J9JavaVM *vm, const char *source)
{
	PORT_ACCESS_FROM_JAVAVM(vm);
	UDATA length = strlen(source);
	char *dest = (char *)j9mem_allocate_memory(length + 1, OMRMEM_CATEGORY_VM);
	if (NULL != dest) {
		strcpy(dest, source);
	}
	return dest;
}

/**
 * Adds a system property, attempting to ensure that both the
 * name and the value are in allocated memory (if not already
 * there according to the supplied flags).
 *
 * The motivation for this is to improve the likelihood that
 * DDR will be able to access these strings, even without the
 * benefit of augmented information collected via jpackcore.
 *
 * @param [in] vm the J9JavaVM
 * @param [in] name null-terminated property name to be added
 * @param [in] value null-terminated property value to be added
 * @param [in] flags flags as speicified by addSystemProperty()
 *
 * @return J9SYSPROP_ERROR_NONE on success, or a J9SYSPROP_ERROR_* value on failure.
 */
static UDATA
addAllocatedSystemProperty(J9JavaVM *vm, const char *name, const char *value, UDATA flags)
{
	if (J9_ARE_NO_BITS_SET(flags, J9SYSPROP_FLAG_NAME_ALLOCATED)) {
		char *copy = copyToMem(vm, name);
		if (NULL != copy) {
			flags |= J9SYSPROP_FLAG_NAME_ALLOCATED;
			name = copy;
		}
	}

	if (J9_ARE_NO_BITS_SET(flags, J9SYSPROP_FLAG_VALUE_ALLOCATED)) {
		char *copy = copyToMem(vm, value);
		if (NULL != copy) {
			flags |= J9SYSPROP_FLAG_VALUE_ALLOCATED;
			value = copy;
		}
	}

	return addSystemProperty(vm, name, value, flags);
}

#if defined(LINUX)
/* Copy the system properties names and values into malloced memory. */
static void
copySystemProperties(J9JavaVM* vm)
{
	pool_state walkState;

	J9VMSystemProperty *property = pool_startDo(vm->systemProperties, &walkState);
	while (NULL != property) {
		/* copy the name */
		if (J9_ARE_NO_BITS_SET(property->flags, J9SYSPROP_FLAG_NAME_ALLOCATED)) {
			char *copied = copyToMem(vm, property->name);

			if (NULL == copied) {
				/* Give up at this point as the memory allocation will probably continue to fail */
				return;
			}
			property->name = copied;
			property->flags |= J9SYSPROP_FLAG_NAME_ALLOCATED;
		}

		/* copy the value */
		if (J9_ARE_NO_BITS_SET(property->flags, J9SYSPROP_FLAG_VALUE_ALLOCATED)) {
			char *copied = copyToMem(vm, property->value);

			if (NULL == copied) {
				/* Give up at this point as the memory allocation will probably continue to fail */
				return;
			}
			property->value = copied;
			property->flags |= J9SYSPROP_FLAG_VALUE_ALLOCATED;
		}

		property = pool_nextDo(&walkState);
	}
}
#endif /* defined(LINUX) */

/**
 * Returns value of a long option present at given index in option list.
 * A long option may be of the form:
 * 	--<name>=<value>
 * 		or
 * 	--<name> <value>
 *
 * 	Note: the allocated memory of the return value needs to be freed after use.
 *
 * @param [in] vm the J9JavaVM
 * @param [in] argIndex index of the option in the option list
 * @param [in] optionNameLen length of the option
 *
 * @return <value> of the option if present, NULL otherwise
 */
static char *
getOptionArg(J9JavaVM *vm, IDATA argIndex, UDATA optionNameLen)
{
	char *option = NULL;
	char *optionArg = NULL;
	J9VMInitArgs* j9vm_args = vm->vmArgsArray;

	if ((argIndex < 0) || ((UDATA)argIndex >= vm->vmArgsArray->nOptions)) {
		goto _end;
	}

	option = getOptionString(j9vm_args, argIndex);
	if ('=' == option[optionNameLen]) {
		/* Option is specified in the format <optionName>=<optionArg> */
		GET_OPTION_VALUE(argIndex, '=', &optionArg);
	} else {
		/* Option is specified in the format <optionName> <optionArg>. So the next 'option' is <optionArg> */
		if ((UDATA)(argIndex + 1) < j9vm_args->nOptions) {
			optionArg = getOptionString(j9vm_args, argIndex + 1);
			/* If <optionArg> starts with '-' it is not an option argument but some other option */
			if ('-' == optionArg[0]) {
				/* <optionValue> is missing */
				optionArg = NULL;
			}
		} else {
			/* <optionValue> is missing */
		}
	}

_end:
	if (NULL != optionArg) {
		/* Convert the value string from the platform encoding to the modified UTF8 */
		optionArg = (char *)getMUtf8String(vm, optionArg, strlen(optionArg));
	}

	return optionArg;
}


/**
 * This function is used to add system properties for modularity command line options
 * which accept only one argument.
 * For example:
 *
 * 	 '--illegal-access=permit'   good - same for 'deny', 'debug' and 'warn'
 * 	 '--illegal-access=' 		 not good, but the JCL will throw the exception
 * 	 '--illegal-access=asdfs'    not good, but the JCL will throw the exception
 * 	 '--illegal-access'  		 not good, and should fail with 'JVMJ9VM007E Command-line option unrecognised: --illegal-access'
 *
 * system property to be added is:
 * 	property name: jdk.module.illegalAccess    property value: "[permit|warn|debug|deny]"
 *
 * @param [in] vm the J9JavaVM
 * @param [in] optionName name of the modularity command line option
 * @param [in] optionNameLen length of the modularity command line option
 * @param [out] propName name of property to be set
 *
 * @return returns J9SYSPROP_ERROR_NONE on success, any other J9SYSPROP_ERROR code on failure
 */
static UDATA
addPropertyForOptionWithEqualsArg(J9JavaVM *vm, const char *optionName, UDATA optionNameLen, const char *propName)
{
	UDATA rc = J9SYSPROP_ERROR_NONE;
	IDATA argIndex = FIND_AND_CONSUME_VMARG(STARTSWITH_MATCH, optionName, NULL);

	if (argIndex >= 0) {
		/* option name includes the '=' so go back one to get the option arg */
		char *optionArg = getOptionArg(vm, argIndex, optionNameLen - 1);

		if (NULL != optionArg) {
			rc = addSystemProperty(vm, propName, optionArg, J9SYSPROP_FLAG_VALUE_ALLOCATED);
		}
	}

	return rc;
}


/**
 * This function is used to add system properties for modularity command line options
 * which accept module path as argument.
 * For example:
 * 	--upgrade-module-path
 * 	--module-path
 *
 * If the option is specified multiple times, then the last option is considered, and
 * the previous options are consumed and ignored.
 *
 * E.g. for options
 * 	 --module-path /abc/def --module-path /pqr/stu
 * system property to be added is:
 * 	property name: jdk.module.path    property value: /pqr/stu
 *
 * @param [in] vm the J9JavaVM
 * @param [in] optionName name of the modularity command line option
 * @param [in] optionNameLen length of the modularity command line option
 * @param [out] propName name of property to be set
 *
 * @return returns J9SYSPROP_ERROR_NONE on success, any other J9SYSPROP_ERROR code on failure
 */
static UDATA
addPropertyForOptionWithPathArg(J9JavaVM *vm, const char *optionName, UDATA optionNameLen, const char *propName)
{
	IDATA argIndex = -1;
	UDATA rc = J9SYSPROP_ERROR_NONE;
	PORT_ACCESS_FROM_JAVAVM(vm);

	argIndex = FIND_AND_CONSUME_VMARG(OPTIONAL_LIST_MATCH_USING_EQUALS, optionName, NULL);
	if (argIndex >= 0) {
		char *optionArg = getOptionArg(vm, argIndex, optionNameLen);

		if (NULL != optionArg) {
			rc = addSystemProperty(vm, propName, optionArg, J9SYSPROP_FLAG_VALUE_ALLOCATED);
			if (J9SYSPROP_ERROR_NONE != rc) {
				goto _end;
			}
		} else {
			j9nls_printf(PORTLIB, J9NLS_ERROR, J9NLS_VM_PROPERTY_MODULARITY_OPTION_REQUIRES_MODULE_PATH, optionName);
			rc = J9SYSPROP_ERROR_ARG_MISSING;
			goto _end;
		}
	}

_end:
	return rc;
}

/**
 * This function is used to add system properties for modularity command line options
 * which accept module names separated by ',' as argument.
 * For example:
 * 	--limit-modules
 *
 * If the option is specified multiple times, then all its arguments (which are module names)
 * are clubbed together separated by ','.
 * E.g. for options
 * 	 --limit-modules abc,def --limit-modules pqr,stu
 * system property to be added is:
 * 	property name: jdk.module.limitmods    property value: abc,def,pqr,stu
 *
 * @param [in] vm the J9JavaVM
 * @param [in] optionName name of the modularity command line option
 * @param [in] optionNameLen length of the modularity command line option
 * @param [in] propName name of the property to be set
 *
 * @return returns J9SYSPROP_ERROR_NONE on success, any other J9SYSPROP_ERROR code on failure
 */
static UDATA
addPropertyForOptionWithModuleListArg(J9JavaVM *vm, const char *optionName, IDATA optionNameLen, const char *propName)
{
	IDATA argIndex = -1;
	UDATA rc = J9SYSPROP_ERROR_NONE;
	J9VMInitArgs* j9vm_args	= vm->vmArgsArray;
	PORT_ACCESS_FROM_JAVAVM(vm);

	argIndex = FIND_ARG_IN_VMARGS_FORWARD(OPTIONAL_LIST_MATCH_USING_EQUALS, optionName, NULL);
	if (argIndex >= 0) {
		char *modulesList = NULL;

		do {
			char *optionArg = getOptionArg(vm, argIndex, optionNameLen);
			if (NULL != optionArg) {
				IDATA listSize = strlen(optionArg);

				if (NULL == modulesList) {
					modulesList = j9mem_allocate_memory(listSize + 1, OMRMEM_CATEGORY_VM); /* +1 for '\0' */
					if (NULL != modulesList) {
						strncpy(modulesList, optionArg, listSize + 1);
					} else {
						j9mem_free_memory(optionArg);
						rc = J9SYSPROP_ERROR_OUT_OF_MEMORY;
						goto _end;
					}
				} else {
					char *prevList = modulesList;

					listSize += strlen(prevList);
					modulesList = j9mem_allocate_memory(listSize + 2, OMRMEM_CATEGORY_VM); /* +1 for ',' and +1 for '\0' */
					if (NULL != modulesList) {
						j9str_printf(modulesList, listSize + 2, "%s,%s", prevList, optionArg);
						j9mem_free_memory(prevList);
					} else {
						j9mem_free_memory(optionArg);
						j9mem_free_memory(prevList);
						rc = J9SYSPROP_ERROR_OUT_OF_MEMORY;
						goto _end;
					}
				}
				CONSUME_ARG(j9vm_args, argIndex);

				j9mem_free_memory(optionArg);
			} else {
				j9nls_printf(PORTLIB, J9NLS_ERROR, J9NLS_VM_PROPERTY_MODULARITY_OPTION_REQUIRES_MODULES, optionName);
				rc = J9SYSPROP_ERROR_ARG_MISSING;
				goto _end;
			}

			argIndex = FIND_NEXT_ARG_IN_VMARGS_FORWARD(OPTIONAL_LIST_MATCH_USING_EQUALS, optionName, NULL, argIndex);

		} while (argIndex >= 0);

		if (NULL != modulesList) {
			rc = addSystemProperty(vm, propName, modulesList, J9SYSPROP_FLAG_VALUE_ALLOCATED);
		}
	}

_end:
	return rc;
}

/**
 * This function adds system properties for modularity command line options
 * which accept argument in the form of <value1>=<value2>
 * For example:
 * --add-exports,
 * --add-modules,
 * --add-opens,
 * --add-reads,
 * --patch-module
 * --enable-native-access --- JDK17+
 *
 * For every occurrence of the option, a new system property is created by appending index
 * to the base property name. The index corresponds to the number of times that option is used.
 * E.g. for options
 * 	 --add-reads abc=def --add-reads pqr=stu
 * system properties to be added are:
 * 	property name: jdk.module.addreads.0    property value: abc=def
 * 	property name: jdk.module.addreads.1    property value: pqr=stu
 *
 * @param [in] vm the J9JavaVM
 * @param [in] optionName name of the modularity command line option
 * @param [in] optionNameLen length of the modularity command line option
 * @param [in] basePropName base name to be used for the property to be set
 * @param [in] basePropNameLen length of the base name
 * @param [out] propertyCount pointer to a variable to receive
 * 			the number of extra properties created.  May be null
 *
 * @return returns J9SYSPROP_ERROR_NONE on success, any other J9SYSPROP_ERROR code on failure
 */
static UDATA
addPropertiesForOptionWithAssignArg(J9JavaVM *vm, const char *optionName, UDATA optionNameLen, const char *basePropName, UDATA basePropNameLen, UDATA *propertyCount)
{
	IDATA argIndex = -1;
	UDATA rc = J9SYSPROP_ERROR_NONE;
	J9VMInitArgs *j9vm_args	= vm->vmArgsArray;
	PORT_ACCESS_FROM_JAVAVM(vm);

	argIndex = FIND_ARG_IN_VMARGS_FORWARD(OPTIONAL_LIST_MATCH_USING_EQUALS, optionName, NULL);
	if (argIndex >= 0) {
		UDATA index = 0;
		UDATA indexLen = 1;

		do {
			char *optionArg = getOptionArg(vm, argIndex, optionNameLen);

			if (NULL != optionArg) {
				char *propName = NULL;
				IDATA propNameLen = basePropNameLen + indexLen + 1; /* +1 for '\0' */

				propName = j9mem_allocate_memory(propNameLen, OMRMEM_CATEGORY_VM);
				if (NULL == propName) {
					rc = J9SYSPROP_ERROR_OUT_OF_MEMORY;
					goto _end;
				}
				j9str_printf(propName, propNameLen, "%s%zu", basePropName, index);
				rc = addSystemProperty(vm, propName, optionArg, J9SYSPROP_FLAG_NAME_ALLOCATED | J9SYSPROP_FLAG_VALUE_ALLOCATED );
				if (J9SYSPROP_ERROR_NONE != rc) {
					goto _end;
				}
				CONSUME_ARG(j9vm_args, argIndex);
			} else {
				j9nls_printf(PORTLIB, J9NLS_ERROR, J9NLS_VM_PROPERTY_MODULARITY_OPTION_REQUIRES_MODULES, optionName);
				rc = J9SYSPROP_ERROR_ARG_MISSING;
				goto _end;
			}

			argIndex = FIND_NEXT_ARG_IN_VMARGS_FORWARD(OPTIONAL_LIST_MATCH_USING_EQUALS, optionName, NULL, argIndex);
			index += 1;
			indexLen = j9str_printf(NULL, 0, "%zu", index); /* get number of digits in 'index' */
		} while (argIndex >= 0);
		if (NULL != propertyCount) {
			*propertyCount = index;
		}
	}

_end:
	return rc;
}


/**
 * Process modularity related command line options and corresponding system properties.
 *
 * @param [in] vm the J9JavaVM
 *
 * @return returns J9SYSPROP_ERROR_NONE on success, any other J9SYSPROP_ERROR code on failure
 */
static UDATA
addModularitySystemProperties(J9JavaVM * vm)
{
	UDATA rc = J9SYSPROP_ERROR_NONE;

	/* Find and consume last "--upgrade-module-path" option */
	rc = addPropertyForOptionWithPathArg(vm, VMOPT_MODULE_UPGRADE_PATH, LITERAL_STRLEN(VMOPT_MODULE_UPGRADE_PATH), SYSPROP_JDK_MODULE_UPGRADE_PATH);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto _end;
	}

	/* Find and consume last "--module-path" option */
	rc = addPropertyForOptionWithPathArg(vm, VMOPT_MODULE_PATH, LITERAL_STRLEN(VMOPT_MODULE_PATH), SYSPROP_JDK_MODULE_PATH);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto _end;
	}

	/* Find all --add-modules options */
	rc = addPropertiesForOptionWithAssignArg(vm, VMOPT_ADD_MODULES, LITERAL_STRLEN(VMOPT_ADD_MODULES), SYSPROP_JDK_MODULE_ADDMODS, LITERAL_STRLEN(SYSPROP_JDK_MODULE_ADDMODS), &(vm->addModulesCount));
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto _end;
	}

	/* Find all --limit-modules options */
	rc = addPropertyForOptionWithModuleListArg(vm, VMOPT_LIMIT_MODULES, LITERAL_STRLEN(VMOPT_LIMIT_MODULES), SYSPROP_JDK_MODULE_LIMITMODS);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto _end;
	}

	/* Find all --add-reads options */
	rc = addPropertiesForOptionWithAssignArg(vm, VMOPT_ADD_READS, LITERAL_STRLEN(VMOPT_ADD_READS), SYSPROP_JDK_MODULE_ADDREADS, LITERAL_STRLEN(SYSPROP_JDK_MODULE_ADDREADS), NULL);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto _end;
	}

	/* Find all --add-exports options */
	rc = addPropertiesForOptionWithAssignArg(vm, VMOPT_ADD_EXPORTS, LITERAL_STRLEN(VMOPT_ADD_EXPORTS), SYSPROP_JDK_MODULE_ADDEXPORTS, LITERAL_STRLEN(SYSPROP_JDK_MODULE_ADDEXPORTS), NULL);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto _end;
	}

	/* Find all --add-opens options */
	rc = addPropertiesForOptionWithAssignArg(vm, VMOPT_ADD_OPENS, LITERAL_STRLEN(VMOPT_ADD_OPENS), SYSPROP_JDK_MODULE_ADDOPENS, LITERAL_STRLEN(SYSPROP_JDK_MODULE_ADDOPENS), NULL);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto _end;
	}

	/* Find all --patch-module options */
	rc = addPropertiesForOptionWithAssignArg(vm, VMOPT_PATCH_MODULE, LITERAL_STRLEN(VMOPT_PATCH_MODULE), SYSPROP_JDK_MODULE_PATCH, LITERAL_STRLEN(SYSPROP_JDK_MODULE_PATCH), NULL);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto _end;
	} else {
		/* check if SYSPROP_JDK_MODULE_PATCH has been set */
		J9VMSystemProperty *property = NULL;
		const char *propName = SYSPROP_JDK_MODULE_PATCH "0";

		if (J9SYSPROP_ERROR_NOT_FOUND != getSystemProperty(vm, propName, &property)) {
			vm->jclFlags |= J9_JCL_FLAG_JDK_MODULE_PATCH_PROP;
		}
	}

#if JAVA_SPEC_VERSION >= 17
	/* Find all --enable-native-access options */
	rc = addPropertiesForOptionWithAssignArg(vm, VMOPT_ENABLE_NATIVE_ACCESS, LITERAL_STRLEN(VMOPT_ENABLE_NATIVE_ACCESS),
		SYSPROP_JDK_MODULE_ENABLENATIVEACCESS, LITERAL_STRLEN(SYSPROP_JDK_MODULE_ENABLENATIVEACCESS), NULL);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto _end;
	}
#endif /* JAVA_SPEC_VERSION >= 17 */

#if JAVA_SPEC_VERSION >= 24
	/* Find and consume the last --illegal-native-access option. */
	rc = addPropertyForOptionWithEqualsArg(
			vm, VMOPT_ILLEGAL_NATIVE_ACCESS,
			LITERAL_STRLEN(VMOPT_ILLEGAL_NATIVE_ACCESS),
			SYSPROP_JDK_MODULE_ILLEGALNATIVEACCESS);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto _end;
	}
#endif /* JAVA_SPEC_VERSION >= 24 */

	/* Find last --illegal-access */
	rc = addPropertyForOptionWithEqualsArg(vm, VMOPT_ILLEGAL_ACCESS, LITERAL_STRLEN(VMOPT_ILLEGAL_ACCESS), SYSPROP_JDK_MODULE_ILLEGALACCESS);

_end:
	return rc;
}

static UDATA
getLibSubDir(J9JavaVM *vm, const char *subDir, char **value)
{
	UDATA rc = J9SYSPROP_ERROR_NONE;
	J9VMSystemProperty *javaHomeProp = NULL;
	PORT_ACCESS_FROM_JAVAVM(vm);

	*value= NULL;

	rc = getSystemProperty(vm, "java.home", &javaHomeProp);
	if (J9SYSPROP_ERROR_NONE == rc) {
		char* libSubDir = j9mem_allocate_memory(strlen(javaHomeProp->value) + LITERAL_STRLEN(DIR_SEPARATOR_STR) + LITERAL_STRLEN("lib") + LITERAL_STRLEN(DIR_SEPARATOR_STR) + strlen(subDir) + 1, OMRMEM_CATEGORY_VM);
		if (NULL == libSubDir) {
			rc = J9SYSPROP_ERROR_OUT_OF_MEMORY;
			goto _end;
		}
		strcpy(libSubDir, javaHomeProp->value);
		strcpy(libSubDir+ strlen(libSubDir), DIR_SEPARATOR_STR "lib" DIR_SEPARATOR_STR);
		strcpy(libSubDir + strlen(libSubDir), subDir);
		*value = libSubDir;
	} else {
		/* If we fail to get value of "java.home" property, do not return failure,
		 * But also set 'value' to NULL to indicate sub directory cannot be determined.
		 */
		rc = J9SYSPROP_ERROR_NONE;
	}

_end:
	return rc;
}

#define COM_SUN_MANAGEMENT "com.sun.management"
/**
 * Initializes the system properties container in the supplied vm.
 * @param vm
 * @return On success J9SYSPROP_ERROR_NONE, otherwise a J9SYSPROP_ERR constant.
 */
UDATA
initializeSystemProperties(J9JavaVM * vm)
{
	PORT_ACCESS_FROM_JAVAVM(vm);

	J9VMSystemProperty *javaEndorsedDirsProperty = NULL;
	jint i = 0;
	JavaVMInitArgs *initArgs = NULL;
	char *jclName = J9_JAVA_SE_DLL_NAME;
	UDATA j2seVersion = J2SE_VERSION(vm);
	const char* propValue = NULL;
	UDATA rc = J9SYSPROP_ERROR_NONE;
	const char *specificationVersion = NULL;
	BOOLEAN addManagementModule = FALSE;

	if (omrthread_monitor_init(&(vm->systemPropertiesMutex), 0) != 0) {
		return J9SYSPROP_ERROR_OUT_OF_MEMORY;
	}

	/* Count the number of -D properties and find the JCL config */
	initArgs = vm->vmArgsArray->actualVMArgs;
	for (i = 0; i < initArgs->nOptions; ++i) {
		char * optionString = initArgs->options[i].optionString;
		Assert_VM_notNull(optionString);

		if (strncmp("-Xjcl:", optionString, 6) == 0) {
			jclName = optionString + 6;
		}
	}

	/* error if not a valid JCL config */
	if (0 != strncmp(jclName, "jclse", 5)) {
		return J9SYSPROP_ERROR_INVALID_JCL;
	}

	/* Allocate the properties pool */
	if ((vm->systemProperties = pool_new(sizeof(J9VMSystemProperty), 0, 0, 0, J9_GET_CALLSITE(), OMRMEM_CATEGORY_VM, POOL_FOR_PORT(vm->portLibrary))) == NULL) {
		return J9SYSPROP_ERROR_OUT_OF_MEMORY;
	}

	if (j2seVersion >= J2SE_V11) {
		rc = addModularitySystemProperties(vm);
		if (J9SYSPROP_ERROR_NONE != rc) {
			goto fail;
		}
	}

	if (JAVA_SPEC_VERSION == 8) {
		specificationVersion = "1.8";
	} else {
		specificationVersion = JAVA_SPEC_VERSION_STRING;
	}

	/* Some properties (*.vm.*) are owned by the VM and need to be set early for all
	 * versions so they are available to jvmti agents.
	 * Other java.* properties are owned by the class libraries and setting them can be
	 * delayed until VersionProps.init() runs.
	 */
	rc = addSystemProperty(vm, "java.vm.specification.version", specificationVersion, 0);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

#if JAVA_SPEC_VERSION < 12
	/* For Java 12, the following properties are defined via java.lang.VersionProps.init(systemProperties) within System.ensureProperties() */
	rc = addSystemProperty(vm, "java.specification.version", specificationVersion, 0);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}
	{
		const char *classVersion = NULL;
		if (JAVA_SPEC_VERSION == 8) {
			classVersion = "52.0";
		} else {
			classVersion = "55.0"; /* Java 11 */
		}
		rc = addSystemProperty(vm, "java.class.version", classVersion, 0);
		if (J9SYSPROP_ERROR_NONE != rc) {
			goto fail;
		}
	}

	rc = addSystemProperty(vm, "java.vendor", JAVA_VENDOR, 0);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	rc = addSystemProperty(vm, "java.vendor.url", JAVA_VENDOR_URL, 0);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}
#endif /* JAVA_SPEC_VERSION < 12 */

#if JAVA_SPEC_VERSION == 8
#define JAVA_SPEC_MAINTENANCE_VERSION "6"
#elif JAVA_SPEC_VERSION == 11 /* JAVA_SPEC_VERSION == 8 */
#define JAVA_SPEC_MAINTENANCE_VERSION "3"
#endif /* JAVA_SPEC_VERSION == 8 */

#if defined(JAVA_SPEC_MAINTENANCE_VERSION)
	rc = addSystemProperty(vm, "java.specification.maintenance.version", JAVA_SPEC_MAINTENANCE_VERSION, 0);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}
#undef JAVA_SPEC_MAINTENANCE_VERSION
#endif /* defined(JAVA_SPEC_MAINTENANCE_VERSION) */

	rc = addSystemProperty(vm, "java.vm.vendor", JAVA_VM_VENDOR, 0);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	rc = addSystemProperty(vm, "com.ibm.oti.vm.library.version", J9_DLL_VERSION_STRING, 0);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	rc = addSystemProperty(vm, "java.vm.info", EsBuildVersionString, J9SYSPROP_FLAG_WRITEABLE);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	rc = addSystemProperty(vm, "java.vm.specification.vendor", "Oracle Corporation", 0);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	rc = addSystemProperty(vm, "java.vm.specification.name", "Java Virtual Machine Specification",0);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	/*
	 * For each of the system properties "java.fullversion", "java.vm.name"
	 * and "java.vm.version", we try to put both the name and value in
	 * allocated memory so DDR can more reliably have access to those strings.
	 *
	 * See:
	 *   - com.ibm.jvm.dtfjview.commands.OpenCommand#createContexts()
	 *   - com.ibm.j9ddr.vm29.tools.ddrinteractive.commands.CoreInfoCommand#run()
	 */
	rc = addAllocatedSystemProperty(vm, "java.fullversion", EsBuildVersionString, J9SYSPROP_FLAG_WRITEABLE);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	rc = addAllocatedSystemProperty(vm, "java.vm.name", JAVA_VM_NAME, 0);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	rc = addAllocatedSystemProperty(vm, "java.vm.version", J9JVM_VERSION_STRING, 0);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

#if defined(J9JDK_EXT_NAME)
	rc = addSystemProperty(vm, "jdk.extensions.name", J9JDK_EXT_NAME, 0);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}
#endif

#if defined(J9JDK_EXT_VERSION)
	rc = addSystemProperty(vm, "jdk.extensions.version", J9JDK_EXT_VERSION, 0);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}
#endif

#if JAVA_SPEC_VERSION < 21
	/* Don't know the JIT yet, put in a placeholder and make it writeable for now */
	rc = addSystemProperty(vm, "java.compiler", "", J9SYSPROP_FLAG_WRITEABLE);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}
#endif /* JAVA_SPEC_VERSION < 21 */

	/* We don't have enough information yet. Put in placeholders. */
#if defined(J9VM_OPT_SIDECAR) && !defined(WIN32)
	propValue = "../..";
#else
	propValue = "..";
#endif
	rc = addSystemProperty(vm, "java.home", propValue, J9SYSPROP_FLAG_WRITEABLE);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	/* We don't have enough information yet. Put in a placeholder. */
	rc = addSystemProperty(vm, "java.class.path", "", J9SYSPROP_FLAG_WRITEABLE);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	/* We don't have enough information yet. Put in a placeholder. */
	rc = addSystemProperty(vm, "java.library.path", "", J9SYSPROP_FLAG_WRITEABLE);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	/* We don't have enough information yet. Put in placeholders. */
#if defined(J9VM_OPT_SIDECAR)
	if (j2seVersion < J2SE_V11) {
		rc = addSystemProperty(vm, BOOT_PATH_SYS_PROP, "", J9SYSPROP_FLAG_WRITEABLE);
	} else {
		rc = addSystemProperty(vm, BOOT_CLASS_PATH_APPEND_PROP, "", J9SYSPROP_FLAG_WRITEABLE);
	}
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}
#endif

	/* Figure out the path separator by querying port library */
	{
		char *pathSep = (char*) j9mem_allocate_memory(2, OMRMEM_CATEGORY_VM);
		if (pathSep == NULL) {
			return J9SYSPROP_ERROR_OUT_OF_MEMORY;
		}
		pathSep[0] = (char) j9sysinfo_get_classpathSeparator();
		pathSep[1] = 0;
		rc = addSystemProperty(vm, "path.separator", pathSep, J9SYSPROP_FLAG_WRITEABLE | J9SYSPROP_FLAG_VALUE_ALLOCATED);
		if (J9SYSPROP_ERROR_NONE != rc) {
			goto fail;
		}
	}

	rc = addSystemProperty(vm, "com.ibm.oti.vm.bootstrap.library.path", "", J9SYSPROP_FLAG_WRITEABLE);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	rc = addSystemProperty(vm, "com.ibm.util.extralibs.properties", "", J9SYSPROP_FLAG_WRITEABLE);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	rc = addSystemProperty(vm, "com.ibm.jcl.checkClassPath", "", J9SYSPROP_FLAG_WRITEABLE);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	/* -Xzero option is removed from Java 9 */
	if (j2seVersion < J2SE_V11) {
		rc = addSystemProperty(vm, "com.ibm.zero.version", "2", 0);
		if (J9SYSPROP_ERROR_NONE != rc) {
			goto fail;
		}
	}

	/* Set the system agent path, which is necessary for system agents such as JDWP to load the libraries they need. */
	if (NULL != vm->j2seRootDirectory) {
		char *agentPath = NULL;
		UDATA agentPathLength = 0;

		if (J2SE_LAYOUT_VM_IN_SUBDIR == (J2SE_LAYOUT(vm) & J2SE_LAYOUT_VM_IN_SUBDIR)) {
			/* Use the parent of the j2seRootDir - find the last dir separator and declare that the end. */
			agentPathLength = strrchr(vm->j2seRootDirectory, DIR_SEPARATOR) - vm->j2seRootDirectory;
		} else {
			agentPathLength = strlen(vm->j2seRootDirectory);
		}

		agentPath = j9mem_allocate_memory(agentPathLength + 1, OMRMEM_CATEGORY_VM);
		if (NULL == agentPath) {
			return J9SYSPROP_ERROR_OUT_OF_MEMORY;
		}
		memcpy(agentPath, vm->j2seRootDirectory, agentPathLength);
		agentPath[agentPathLength] = '\0';

		rc = addSystemProperty(vm, "com.ibm.system.agent.path", agentPath, J9SYSPROP_FLAG_VALUE_ALLOCATED);
		if (J9SYSPROP_ERROR_NONE != rc) {
			goto fail;
		}
	}

#if defined(OSX) && defined(J9VM_ARCH_X86) && defined(J9VM_ENV_DATA64)
	/*
	 * The reference implementation uses "amd64" to refer to the 64-bit x86
	 * architecture everywhere except on OSX where the name "x86_64" is
	 * used: We follow suit.
	 */
	propValue = "x86_64";
#else /* defined(OSX) && defined(J9VM_ARCH_X86) && defined(J9VM_ENV_DATA64) */
	propValue = j9sysinfo_get_CPU_architecture();
	if (NULL == propValue) {
		propValue = "unknown";
	}
#endif /* defined(OSX) && defined(J9VM_ARCH_X86) && defined(J9VM_ENV_DATA64) */
	rc = addSystemProperty(vm, "os.arch", propValue, J9SYSPROP_FLAG_WRITEABLE);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

	propValue = j9sysinfo_get_OS_type();
	if (NULL == propValue) {
		propValue = "unknown";
	}
	rc = addSystemProperty(vm, "os.name", propValue, J9SYSPROP_FLAG_WRITEABLE);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

#if JAVA_SPEC_VERSION < 18
	propValue = j9sysinfo_get_OS_version();
	if (NULL != propValue) {
#if defined(WIN32)
		/* Win32 os.version is expected to be strictly numeric.
		 * The port library includes build information which derails the Java
		 * code.  Chop off the version after the major/minor. */
		char *cursor = strchr(propValue, ' ');
		if (NULL != cursor) {
			*cursor = '\0';
		}
#endif /* defined(WIN32) */
	} else {
		propValue = "unknown";
	}
	rc = addSystemProperty(vm, "os.version", propValue, J9SYSPROP_FLAG_WRITEABLE);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}
#endif /* JAVA_SPEC_VERSION < 18 */

	/* Create the -D properties. This may override any of the writeable properties above.
	    Should the command line override read-only props? */
	for (i = 0; i < initArgs->nOptions; ++i) {
		char * optionString = initArgs->options[i].optionString;

		if (strncmp("-D", optionString, 2) == 0) {
			J9VMSystemProperty *currentProp = NULL;
			char *propNameCopy = NULL;
			char *propValueCopy = NULL;
			UDATA propNameLen = 0;

			propValue = strchr(optionString + 2, '=');
			if (propValue == NULL) {
				propNameLen = strlen(optionString) - 2;
				propValue = optionString + 2 + propNameLen;
			} else {
				propNameLen = propValue - (optionString + 2);
				++propValue;
			}

			{
				UDATA valueLength = strlen(propValue);

				propNameCopy = (char *)getMUtf8String(vm, optionString + 2, propNameLen); /* get a copy of the property name */
				if (NULL == propNameCopy) {
					rc = J9SYSPROP_ERROR_OUT_OF_MEMORY;
					goto fail;
				}
				propValueCopy = (char *)getMUtf8String(vm, propValue, valueLength); /* get a copy of the property value */
				if (NULL == propValueCopy) {
					j9mem_free_memory(propNameCopy);
					rc = J9SYSPROP_ERROR_OUT_OF_MEMORY;
					goto fail;
				}
			}
			if (j2seVersion >= J2SE_V11) {
				/* defining any system property starting with "com.sun.management" should
				 * cause the jdk.management.agent module to be loaded. This occurs in
				 * jclcinit.c:initializeRequiredClasses().
				 */
				if (!addManagementModule && (0 == strncmp(propNameCopy, COM_SUN_MANAGEMENT, strlen(COM_SUN_MANAGEMENT)))) {
					vm->extendedRuntimeFlags2 |= J9_EXTENDED_RUNTIME2_LOAD_AGENT_MODULE;
					addManagementModule = TRUE;
				} else if ('\0' != propValue[0]) {
					/* Support for java.endorsed.dirs and java.ext.dirs is disabled in Java 9+. */
					if (0 == strncmp(JAVA_ENDORSED_DIRS, propNameCopy, sizeof(JAVA_ENDORSED_DIRS))) {
						j9nls_printf(PORTLIB, J9NLS_ERROR, J9NLS_VM_PROPERTY_JAVA_ENDORSED_DIR_UNSUPPORTED, optionString + 2);
						j9mem_free_memory(propNameCopy);
						j9mem_free_memory(propValueCopy);
						rc = J9SYSPROP_ERROR_UNSUPPORTED_PROP;
						goto fail;
					}
					if (0 == strncmp(JAVA_EXT_DIRS, propNameCopy, sizeof(JAVA_EXT_DIRS))) {
						j9nls_printf(PORTLIB, J9NLS_ERROR, J9NLS_VM_PROPERTY_JAVA_EXT_DIR_UNSUPPORTED, optionString + 2);
						j9mem_free_memory(propNameCopy);
						j9mem_free_memory(propValueCopy);
						rc = J9SYSPROP_ERROR_UNSUPPORTED_PROP;
						goto fail;
					}
				}
			}

			if (getSystemProperty(vm, propNameCopy, &currentProp) == J9SYSPROP_ERROR_NONE) {
				/* We already have a property of the given name, free the propNameCopy buffer */
				j9mem_free_memory(propNameCopy);

				rc = setSystemPropertyValue(vm, currentProp, propValueCopy, TRUE);
				if (J9SYSPROP_ERROR_READ_ONLY == rc) {
					/* Ignore attempts to modify read-only system properties. */
				} else if (J9SYSPROP_ERROR_NONE != rc) {
					goto fail;
				}
			} else {
				/* We don't yet have a property of the desired name, add it. */
				rc = addSystemProperty(vm, propNameCopy, propValueCopy, J9SYSPROP_FLAG_WRITEABLE
						| J9SYSPROP_FLAG_NAME_ALLOCATED | J9SYSPROP_FLAG_VALUE_ALLOCATED);
				if (J9SYSPROP_ERROR_NONE != rc) {
					goto fail;
				}
			}
		}
	}

	if (j2seVersion >= J2SE_V11) {
		/* On Java 9 support for java.endorsed.dirs is disabled. If java.home/lib/endorsed dir is found, JVM fails to startup. *
		 * Similarly, support for java.ext.dirs is disabled. If java.home/lib/ext dir is found, JVM fails to startup.
		 */
		char *defaultEndorsedDir = NULL;
		char *defaultExtDir = NULL;
		I_32 isDir = 0;

		rc = getLibSubDir(vm, "endorsed", &defaultEndorsedDir);
		if (NULL != defaultEndorsedDir) {
			isDir = j9file_attr(defaultEndorsedDir);
			j9mem_free_memory(defaultEndorsedDir);

			if (EsIsDir == isDir) {
				j9nls_printf(PORTLIB, J9NLS_ERROR, J9NLS_VM_PROPERTY_JAVA_ENDORSED_DIR_UNSUPPORTED, "<JAVA_HOME>/lib/endorsed");
				rc = J9SYSPROP_ERROR_UNSUPPORTED_PROP;
				goto fail;
			}
		}

		rc = getLibSubDir(vm, "ext", &defaultExtDir);
		if (NULL != defaultExtDir) {
			isDir = j9file_attr(defaultExtDir);
			j9mem_free_memory(defaultExtDir);

			if (EsIsDir == isDir) {
				j9nls_printf(PORTLIB, J9NLS_ERROR, J9NLS_VM_PROPERTY_JAVA_EXT_DIR_UNSUPPORTED, "<JAVA_HOME>/lib/ext");
				rc = J9SYSPROP_ERROR_UNSUPPORTED_PROP;
				goto fail;
			}
		}
	} else {
		/* look for java.endorsed.dirs and set if not set */
		if ( getSystemProperty(vm, JAVA_ENDORSED_DIRS, &javaEndorsedDirsProperty) != J9SYSPROP_ERROR_NONE ) {
			char *defaultEndorsedDir =  NULL;

			rc = getLibSubDir(vm, "endorsed", &defaultEndorsedDir);
			if (NULL != defaultEndorsedDir) {
				rc = addSystemProperty(vm, JAVA_ENDORSED_DIRS, defaultEndorsedDir, J9SYSPROP_FLAG_WRITEABLE | J9SYSPROP_FLAG_VALUE_ALLOCATED);
				if (J9SYSPROP_ERROR_NONE != rc) {
					goto fail;
				}
			} else if (J9SYSPROP_ERROR_NONE != rc) {
				goto fail;
			}
		}
	}

	rc = addSystemProperty(vm, "sun.nio.PageAlignDirectMemory", "false", J9SYSPROP_FLAG_WRITEABLE);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}

#if defined(J9VM_OPT_OPENJDK_METHODHANDLE)
	if (j2seVersion >= J2SE_V11) {
		rc = addSystemProperty(vm, "java.lang.invoke.VarHandle.VAR_HANDLE_GUARDS", "false", J9SYSPROP_FLAG_WRITEABLE);
		if (J9SYSPROP_ERROR_NONE != rc) {
			goto fail;
		}
	}

	/* TODO: https://github.com/eclipse-openj9/openj9/issues/12811 */
	rc = addSystemProperty(vm, "openjdk.methodhandles", "true", J9SYSPROP_FLAG_WRITEABLE);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}
#endif /* defined(J9VM_OPT_OPENJDK_METHODHANDLE) */

#if JAVA_SPEC_VERSION >= 23
	{
		/* JEP 471: Deprecate the Memory-Access Methods in sun.misc.Unsafe for Removal
		 * --sun-misc-unsafe-memory-access=value which is expected to be "allow", "warn", "debug" or "deny".
		 */
		IDATA argIndex = FIND_AND_CONSUME_VMARG(STARTSWITH_MATCH, VMOPT_DISABLE_SUN_MISC_UNSAFE_MEMORY_ACCESS, NULL);
		if (argIndex >= 0) {
			PORT_ACCESS_FROM_JAVAVM(vm);
			UDATA optionNameLen = LITERAL_STRLEN(VMOPT_DISABLE_SUN_MISC_UNSAFE_MEMORY_ACCESS);
			/* option name includes the '=' so go back one to get the option arg */
			char *optionArg = getOptionArg(vm, argIndex, optionNameLen - 1);

			if (NULL != optionArg) {
				if ((0 == strcmp(optionArg, "allow"))
					|| (0 == strcmp(optionArg, "warn"))
					|| (0 == strcmp(optionArg, "debug"))
					|| (0 == strcmp(optionArg, "deny"))
				) {
					rc = addSystemProperty(vm, SYSPROP_SUN_MISC_UNSAFE_MEMORY_ACCESS, optionArg, J9SYSPROP_FLAG_VALUE_ALLOCATED);
					if (J9SYSPROP_ERROR_NONE != rc) {
						goto fail;
					}
				} else {
					j9nls_printf(PORTLIB, J9NLS_ERROR, J9NLS_VM_UNRECOGNISED_SUN_MISC_UNSAFE_MEMORY_ACCESS_VALUE, optionArg);
					rc = J9SYSPROP_ERROR_INVALID_VALUE;
					goto fail;
				}
			} else {
				j9nls_printf(PORTLIB, J9NLS_ERROR, J9NLS_VM_UNRECOGNISED_SUN_MISC_UNSAFE_MEMORY_ACCESS_VALUE, "");
				rc = J9SYSPROP_ERROR_ARG_MISSING;
				goto fail;
			}
		}
	}
#endif /* JAVA_SPEC_VERSION >= 23 */

#if defined(JDK_DEBUG_LEVEL)
	rc = addSystemProperty(vm, "jdk.debug", JDK_DEBUG_LEVEL, 0);
	if (J9SYSPROP_ERROR_NONE != rc) {
		goto fail;
	}
#endif /* defined(JDK_DEBUG_LEVEL) */

	/* If we get here, all is good. */
	rc = J9SYSPROP_ERROR_NONE;

fail:
#if defined(LINUX)
	/* Copy the system properties names and values into malloced memory */
	copySystemProperties(vm);
#endif /* defined(LINUX) */
	return rc;
}
#undef COM_SUN_MANAGEMENT



void
freeSystemProperties(J9JavaVM * vm)
{
	if (NULL != vm->systemProperties) {
		PORT_ACCESS_FROM_JAVAVM(vm);
		pool_state walkState;

		J9VMSystemProperty* property = pool_startDo(vm->systemProperties, &walkState);
		while (property != NULL) {
			if (property->flags & J9SYSPROP_FLAG_NAME_ALLOCATED) {
				j9mem_free_memory(property->name);
			}
			if (property->flags & J9SYSPROP_FLAG_VALUE_ALLOCATED) {
				j9mem_free_memory(property->value);
			}
			property = pool_nextDo(&walkState);
		}

		pool_kill(vm->systemProperties);
		vm->systemProperties = NULL;
	}
	if (NULL != vm->systemPropertiesMutex) {
		omrthread_monitor_destroy(vm->systemPropertiesMutex);
		vm->systemPropertiesMutex = NULL;
	}
}


UDATA
getSystemProperty(J9JavaVM * vm, const char * name, J9VMSystemProperty ** propertyPtr)
{
	pool_state walkState;

	J9VMSystemProperty* property = pool_startDo(vm->systemProperties, &walkState);
	while (property != NULL) {
		if (strcmp(property->name, name) == 0) {
			if (NULL != propertyPtr) {
				*propertyPtr = property;
			}
			return J9SYSPROP_ERROR_NONE;
		}
		property = pool_nextDo(&walkState);
	}

	return J9SYSPROP_ERROR_NOT_FOUND;
}


/*
 * This returns J9VM_VERSION_STRING from generated file j9version.h.
 */
const char *
getJ9VMVersionString(J9JavaVM *vm) {
	return J9VM_VERSION_STRING;
}


UDATA
addSystemProperty(J9JavaVM * vm, const char* propName,  const char* propValue, UDATA flags)
{
	J9VMSystemProperty* newProp = pool_newElement(vm->systemProperties);
	if (NULL == newProp) {
		return J9SYSPROP_ERROR_OUT_OF_MEMORY;
	}

	newProp->name = (char*)propName;
	newProp->value = (char*)propValue;
	newProp->flags = flags;
	return J9SYSPROP_ERROR_NONE;
}

UDATA
setSystemPropertyValue(J9JavaVM * vm, J9VMSystemProperty * property, char * newValue, BOOLEAN allocated)
{
	/* Make sure the property is writeable */

	if (!(property->flags & J9SYSPROP_FLAG_WRITEABLE)) {
		return J9SYSPROP_ERROR_READ_ONLY;
	}

	/* If value is NULL, don't write it (used to check for read-only without modifying) */

	if (NULL != newValue) {
		PORT_ACCESS_FROM_JAVAVM(vm);
		/* If the old value of the property was allocated (not static data), then free it */

		if (J9_ARE_ANY_BITS_SET(property->flags, J9SYSPROP_FLAG_VALUE_ALLOCATED)) {
			j9mem_free_memory(property->value);
		}

		if (allocated) {
			property->flags |= J9SYSPROP_FLAG_VALUE_ALLOCATED;
		}
		property->value = newValue;
		if (strcmp(property->name, "java.home") == 0) {
			vm->javaHome = (U_8*)newValue;
		}
	}
	return J9SYSPROP_ERROR_NONE;
}

UDATA
setSystemProperty(J9JavaVM * vm, J9VMSystemProperty * property, const char * value)
{
	/* Make sure the property is writeable */

	if (J9_ARE_NO_BITS_SET(property->flags, J9SYSPROP_FLAG_WRITEABLE)) {
		return J9SYSPROP_ERROR_READ_ONLY;
	}

	/* If value is NULL, don't write it (used to check for read-only without modifying) */

	if (NULL != value) {
		/* Make a copy of the value */
		char * copiedValue = copyToMem(vm, value);

		if (NULL == copiedValue) {
			return J9SYSPROP_ERROR_OUT_OF_MEMORY;
		}
		setSystemPropertyValue(vm, property, copiedValue, TRUE);
	}

	return J9SYSPROP_ERROR_NONE;
}

/**
 * Convert a string with embedded "\u" to null-terminated modified UTF-8.
 * @param [in] vm Java VM
 * @param [in] escapeString input string
 * @param [in] escapeLength length of string, not including any terminating null.
 * @return buffer containing the transliterated string or NULL in case of error
 */
#define TRANSCODE_BUFFER_SIZE 64
static U_8*
unicodeEscapeStringToMUtf8(J9JavaVM * vm, const char* escapeString, UDATA escapeLength) {
	U_16 localUnicodeBuffer[TRANSCODE_BUFFER_SIZE]; /* handle short strings without allocating memory */
	U_16* unicodeBuffer = localUnicodeBuffer;
	UDATA bufferLength = (escapeLength+1)*2; /* This is an overestimate */
	const char *cursor;
	const char *escapeStringEnd = escapeString + escapeLength;
	IDATA unicodeDigitCounter = -1; /* for walking through "\\u1234" */
	U_16 currentChar = 0;
	UDATA unicodeCharacterCounter = 0; /* count the characters converted to Unicode */
	U_16 slashChar = 0;
	I_32 mutf8Size = 0;
	U_16 unicode = 0;
	U_8 *result = NULL;

	PORT_ACCESS_FROM_JAVAVM(vm);

	if (bufferLength > TRANSCODE_BUFFER_SIZE) {
		unicodeBuffer = (U_16*) j9mem_allocate_memory(bufferLength, OMRMEM_CATEGORY_VM);
		if (NULL == unicodeBuffer) {
			return NULL;
		}
	}

	cursor = escapeString;
	while (cursor < escapeStringEnd) {
		if (cursor[0]=='\\' && cursor[1]=='u') {
			slashChar = cursor[0];
			unicodeDigitCounter = 0;
			cursor += 2;
		}
		if ((unicodeDigitCounter >= 0) && (unicodeDigitCounter < 4)) {
			I_32 digit = 0;

			currentChar = cursor[0];
			if (currentChar >= '0' && currentChar <= '9') {
				digit = currentChar - '0';
			} else if (currentChar >= 'a' && currentChar <= 'f') {
				digit = (currentChar - 'a') + 10;
			} else if (currentChar >= 'A' && currentChar <= 'F') {
				digit = (currentChar - 'A') + 10;
			} else {
				digit = -1;
			}
			if (digit >= 0) {
				unicode = (unicode << 4) + digit;
			} else {
				/* Parse error. Give up on the conversion and go back copying the chars verbatim. */
				unicodeBuffer[unicodeCharacterCounter] = slashChar; /* Give it the first char, so that it doesn't attempt to try again! */
				unicodeCharacterCounter++;
				cursor -= (unicodeDigitCounter + 1);
				unicodeDigitCounter = -1;
				continue;
			}
			++unicodeDigitCounter;
			if (4 == unicodeDigitCounter) {
				unicodeDigitCounter = -1;
				unicodeBuffer[unicodeCharacterCounter] = unicode;
				unicodeCharacterCounter++;
			}
		} else {
			/* plain, unescaped character */
			unicodeBuffer[unicodeCharacterCounter] = cursor[0];
			unicodeCharacterCounter++;
		}
		cursor++;
	}

	mutf8Size = j9str_convert(J9STR_CODE_WIDE, J9STR_CODE_MUTF8, (char*)unicodeBuffer, unicodeCharacterCounter*2,
			NULL, 0); /* get the size of the MUTF8 */
	if (mutf8Size >= 0) {
		++mutf8Size; /* leave enough space to null-terminate the string */
		result = j9mem_allocate_memory(mutf8Size, OMRMEM_CATEGORY_VM); /* allow room for terminating null */
		if (NULL != result) {
			mutf8Size = j9str_convert(J9STR_CODE_WIDE, J9STR_CODE_MUTF8, (char*)unicodeBuffer, unicodeCharacterCounter*2,
					(char*)result, mutf8Size);
			if (mutf8Size < 0) {
				j9mem_free_memory(result);
				result = NULL;
			}
		}
	}
	if (localUnicodeBuffer != unicodeBuffer) {
		j9mem_free_memory(unicodeBuffer);
	}

	return result;
}

/*
 * Test if the character sequence is consists entirely of code points 1-127 (inclusive).
 * Code point 0 needs to be transliterated to the modified UTF-8 escape sequence.
 */
static BOOLEAN
isAscii(const char *userString, UDATA stringLength) {
	U_32 cursor;
	for (cursor = 0; cursor < stringLength; cursor++) {
		if ((0 == userString[cursor]) || J9_ARE_ANY_BITS_SET(userString[cursor], 0x080)) {
			return FALSE;
		}
	}
	return TRUE; /* hit the end of the string without encountering non-ASCII characters */
}

/*
 * Test if the character sequence contains the string "\u". May contain embedded zero bytes.
 */
static BOOLEAN
containsBackslashU(const char *userString, UDATA stringLength) {
	if (stringLength > 0) {
		U_32 cursor = 0;
		for (cursor = 0; cursor < (stringLength-1); cursor++) {
			if (('\\' == userString[cursor]) && ('u' == userString[cursor+1])) {
				/* cursor+1 is safe because the loop stops one character before the end */
				return TRUE;
			}
		}
	}
	return FALSE; /* hit the end of the string without encountering \u sequence */
}

static U_8 *
convertString(J9JavaVM *vm, I_32 fromCode, const char *userString, UDATA stringLength)
{
	PORT_ACCESS_FROM_JAVAVM(vm);
	U_8 *result = NULL;
	I_32 bufferLength = j9str_convert(fromCode, J9STR_CODE_MUTF8, userString, stringLength, NULL, 0) + 1; /* +1 for terminating null */
	if (bufferLength > 0) {
		U_8 *mutf8Buffer = j9mem_allocate_memory(bufferLength, OMRMEM_CATEGORY_VM);
		if (NULL != mutf8Buffer) {
			I_32 resultLength = j9str_convert(fromCode, J9STR_CODE_MUTF8, userString, stringLength,
					(char *)mutf8Buffer, bufferLength);
			/* j9str_convert null-terminated the string */
			if (resultLength >= 0) {
				result = mutf8Buffer;
			} else {
				j9mem_free_memory(mutf8Buffer);
			}
		}
	}
	return result;
}

U_8 *
getMUtf8String(J9JavaVM *vm, const char *userString, UDATA stringLength)
{
	PORT_ACCESS_FROM_JAVAVM(vm);

	U_8 *mutf8Buffer = NULL;
	U_8 *result = NULL;
	BOOLEAN doUnicodeConversion = J9_ARE_ANY_BITS_SET(vm->runtimeFlags, J9_RUNTIME_ARGENCODING_UNICODE)
					&& containsBackslashU(userString, stringLength);

	if (doUnicodeConversion) {
		result = unicodeEscapeStringToMUtf8(vm, userString, stringLength);
	} else if (isAscii(userString, stringLength)) { /* This is entirely ASCII. */
		UDATA bufferLength = stringLength + 1; /* leave room for the terminating null */
		mutf8Buffer = j9mem_allocate_memory(bufferLength, OMRMEM_CATEGORY_VM);
		if (NULL != mutf8Buffer) {
			memcpy(mutf8Buffer, userString, stringLength);
			mutf8Buffer[stringLength] = '\0';
		}
		result = mutf8Buffer;
	} else {
#if defined(OSX) || defined(WIN32)
		I_32 fromCode = J9STR_CODE_UTF8; /* command-line arguments should be in UTF-8 */
#else /* defined(OSX) || defined(WIN32) */
		I_32 fromCode = J9STR_CODE_PLATFORM_RAW;
#endif /* defined(OSX) || defined(WIN32) */
		if (J9_ARE_ANY_BITS_SET(vm->runtimeFlags, J9_RUNTIME_ARGENCODING_UTF8)) {
			fromCode = J9STR_CODE_UTF8;
		} else if (J9_ARE_ANY_BITS_SET(vm->runtimeFlags, J9_RUNTIME_ARGENCODING_LATIN)) {
			fromCode =  J9STR_CODE_LATIN1;
		}
		result = convertString(vm, fromCode, userString, stringLength);
		if ((NULL == result) && (J9STR_CODE_UTF8 != fromCode)) {
			result = convertString(vm, J9STR_CODE_UTF8, userString, stringLength);
		}
	}
	if (NULL == result) {
		j9mem_free_memory(mutf8Buffer);
	}
	return result;
}
