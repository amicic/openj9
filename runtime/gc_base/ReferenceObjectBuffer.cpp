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

#include "j9.h"
#include "j9cfg.h"
#include "ModronAssertions.h"

#include "ReferenceObjectBuffer.hpp"

#include "EnvironmentBase.hpp"
#include "GCExtensions.hpp"
#include "HeapRegionDescriptor.hpp"
#include "HeapRegionManager.hpp"
#include "ObjectAccessBarrier.hpp"
#include "ReferenceObjectList.hpp"

MM_ReferenceObjectBuffer::MM_ReferenceObjectBuffer(uintptr_t maxObjectCount)
	: MM_BaseVirtual()
	, _maxObjectCount(maxObjectCount)
{
	_typeId = __FUNCTION__;
	reset();
}

void
MM_ReferenceObjectBuffer::kill(MM_EnvironmentBase *env)
{
	tearDown(env);
	env->getForge()->free(this);
}

void 
MM_ReferenceObjectBuffer::reset()
{
	_head = NULL;
	_tail = NULL;
	_region = NULL;
	_referenceObjectType = 0;
	/* set object count to appear full so that we force initialization on the next add */
//	if (0 == (rand() % 10)) {
//		omrthread_sleep(1);
//	}
	_objectCount = _maxObjectCount;
}

void 
MM_ReferenceObjectBuffer::flush(MM_EnvironmentBase *env)
{
//	OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
	if (NULL != _head) {
		//if (MUTATOR_THREAD == env->getThreadType()) {
		//	omrtty_printf("MM_ReferenceObjectBuffer::flush env %zu head %p tail %p\n", env->getEnvironmentId(), _head, _tail);
		//}
		flushImpl(env);
		reset();
	}
}

UDATA
MM_ReferenceObjectBuffer::getReferenceObjectType(MM_EnvironmentBase *env, j9object_t object)
{ 
	return J9CLASS_FLAGS(J9GC_J9OBJECT_CLAZZ(object, env)) & J9AccClassReferenceMask;
}

void
MM_ReferenceObjectBuffer::add(MM_EnvironmentBase *env, j9object_t object)
{
	MM_GCExtensions *extensions = MM_GCExtensions::getExtensions(env);
//	OMRPORT_ACCESS_FROM_ENVIRONMENT(env);

	if ( (_objectCount < _maxObjectCount) && _region->isAddressInRegion(object) && (getReferenceObjectType(env, object) == _referenceObjectType)) {
		/* object is permitted in this buffer */
		Assert_MM_true(NULL != _head);
		Assert_MM_true(NULL != _tail);

		//omrtty_printf("MM_ReferenceObjectBuffer::add env %zu object %p _head %p\n", env->getEnvironmentId(), object, _head);

		extensions->accessBarrier->setReferenceLink(object, _head);
		_head = object;
		_objectCount += 1;
	} else {
		MM_HeapRegionDescriptor *region = _region;

		/* flush the buffer and start fresh */
		flush(env);
		
		extensions->accessBarrier->setReferenceLink(object, NULL);
		//if (MUTATOR_THREAD == env->getThreadType()) {
		//	omrtty_printf("MM_ReferenceObjectBuffer::add (after flush) env %zu object %p\n", env->getEnvironmentId(), object);
		//}

		_head = object;
		_tail = object;
		_objectCount = 1;
		if (NULL == region || !region->isAddressInRegion(object)) {
			/* record the type of object and the region which contains this object. Other objects will be permitted in the buffer if they match */
			MM_HeapRegionManager *regionManager = extensions->getHeap()->getHeapRegionManager();
			region = regionManager->regionDescriptorForAddress(object);
			Assert_MM_true(NULL != region);
		}
		_region = region;
		_referenceObjectType = getReferenceObjectType(env, object);

	}
}

