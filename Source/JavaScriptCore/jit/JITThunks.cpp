/*
 * Copyright (C) 2012 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "JITThunks.h"

#if ENABLE(JIT)

#include "Executable.h"
#include "JIT.h"
#include "JSGlobalData.h"
#include "Operations.h"

namespace JSC {

JITThunks::JITThunks()
    : m_hostFunctionStubMap(adoptPtr(new HostFunctionStubMap))
{
}

JITThunks::~JITThunks()
{
}

MacroAssemblerCodePtr JITThunks::ctiNativeCall(JSGlobalData* globalData)
{
#if ENABLE(LLINT)
    if (!globalData->canUseJIT())
        return MacroAssemblerCodePtr::createLLIntCodePtr(llint_native_call_trampoline);
#endif
    return ctiStub(globalData, nativeCallGenerator).code();
}
MacroAssemblerCodePtr JITThunks::ctiNativeConstruct(JSGlobalData* globalData)
{
#if ENABLE(LLINT)
    if (!globalData->canUseJIT())
        return MacroAssemblerCodePtr::createLLIntCodePtr(llint_native_construct_trampoline);
#endif
    return ctiStub(globalData, nativeConstructGenerator).code();
}

MacroAssemblerCodeRef JITThunks::ctiStub(JSGlobalData* globalData, ThunkGenerator generator)
{
    CTIStubMap::AddResult entry = m_ctiStubMap.add(generator, MacroAssemblerCodeRef());
    if (entry.isNewEntry)
        entry.iterator->value = generator(globalData);
    return entry.iterator->value;
}

NativeExecutable* JITThunks::hostFunctionStub(JSGlobalData* globalData, NativeFunction function, NativeFunction constructor)
{
    if (NativeExecutable* nativeExecutable = m_hostFunctionStubMap->get(function))
        return nativeExecutable;

    NativeExecutable* nativeExecutable = NativeExecutable::create(*globalData, JIT::compileCTINativeCall(globalData, function), function, MacroAssemblerCodeRef::createSelfManagedCodeRef(ctiNativeConstruct(globalData)), constructor, NoIntrinsic);
    weakAdd(*m_hostFunctionStubMap, function, PassWeak<NativeExecutable>(nativeExecutable));
    return nativeExecutable;
}

NativeExecutable* JITThunks::hostFunctionStub(JSGlobalData* globalData, NativeFunction function, ThunkGenerator generator, Intrinsic intrinsic)
{
    if (NativeExecutable* nativeExecutable = m_hostFunctionStubMap->get(function))
        return nativeExecutable;

    MacroAssemblerCodeRef code;
    if (generator) {
        if (globalData->canUseJIT())
            code = generator(globalData);
        else
            code = MacroAssemblerCodeRef();
    } else
        code = JIT::compileCTINativeCall(globalData, function);

    NativeExecutable* nativeExecutable = NativeExecutable::create(*globalData, code, function, MacroAssemblerCodeRef::createSelfManagedCodeRef(ctiNativeConstruct(globalData)), callHostFunctionAsConstructor, intrinsic);
    weakAdd(*m_hostFunctionStubMap, function, PassWeak<NativeExecutable>(nativeExecutable));
    return nativeExecutable;
}

void JITThunks::clearHostFunctionStubs()
{
    m_hostFunctionStubMap.clear();
}

} // namespace JSC

#endif // ENABLE(JIT)
