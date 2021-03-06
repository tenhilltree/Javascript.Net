////////////////////////////////////////////////////////////////////////////////////////////////////
// File: JavascriptContext.cpp
// 
// Copyright 2010 Noesis Innovation Inc. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
////////////////////////////////////////////////////////////////////////////////////////////////////

#include <msclr\lock.h>
#include <vcclr.h>
#include <msclr\marshal.h>
#include <signal.h>
#include "libplatform/libplatform.h"

#include "JavascriptContext.h"

#include "SystemInterop.h"
#include "JavascriptException.h"
#include "JavascriptExternal.h"
#include "JavascriptInterop.h"

using namespace msclr;
using namespace v8::platform;

////////////////////////////////////////////////////////////////////////////////////////////////////

namespace Noesis { namespace Javascript {

////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma managed(push, off)
	void GetPathsForInitialisation(char dll_path[MAX_PATH], char natives_blob_bin_path[MAX_PATH], char snapshot_blob_bin_path[MAX_PATH])
	{
		HMODULE hm = NULL;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)&GetPathsForInitialisation,  // any address in the module we're caring about
			&hm)) {
			int ret = GetLastError();
			fprintf(stderr, "GetModuleHandle error: %d\n", ret);
			raise(SIGABRT);  // Exit immediately.
		}
		int nchars = GetModuleFileNameA(hm, dll_path, MAX_PATH);
		if (nchars == 0 || nchars >= MAX_PATH) {
			int ret = GetLastError();
			fprintf(stderr, "GetModuleFileNameA error: %d\n", ret);
			raise(SIGABRT);  // Exit immediately.
		}
		// Because they can conflict with differently-versioned .bin files from Chromiu/CefSharp,
		// we'll prefer .bin files prefixed by "v8_", if present.
		strcpy_s(natives_blob_bin_path, MAX_PATH, dll_path);
		strcpy_s(snapshot_blob_bin_path, MAX_PATH, dll_path);
		if (strlen(dll_path) > MAX_PATH - 20) {
			fprintf(stderr, "Path is too long - don't want to overflow our buffers.");
			raise(SIGABRT);  // Exit immediately.
		}
		strcpy_s(strrchr(natives_blob_bin_path, '\\'), 21, "\\v8_natives_blob.bin");
		strcpy_s(strrchr(snapshot_blob_bin_path, '\\'), 22, "\\v8_snapshot_blob.bin");
		FILE *file;
		if (fopen_s(&file, natives_blob_bin_path, "r") == 0)
			fclose(file);
		else
			strcpy_s(strrchr(natives_blob_bin_path, '\\'), 18, "\\natives_blob.bin");
		if (fopen_s(&file, snapshot_blob_bin_path, "r") == 0)
			fclose(file);
		else
			strcpy_s(strrchr(snapshot_blob_bin_path, '\\'), 19, "\\snapshot_blob.bin");
	}

	// This code didn't work in managed code, probably due to too-clever smart pointers.
	void UnmanagedInitialisation()
	{
		// Get location of DLL so that v8 can use it to find its .bin files.
		char dll_path[MAX_PATH], natives_blob_bin_path[MAX_PATH], snapshot_blob_bin_path[MAX_PATH];
		GetPathsForInitialisation(dll_path, natives_blob_bin_path, snapshot_blob_bin_path);
		v8::V8::InitializeICUDefaultLocation(dll_path);
		v8::V8::InitializeExternalStartupData(natives_blob_bin_path, snapshot_blob_bin_path);
		v8::Platform *platform = v8::platform::NewDefaultPlatform().release();
		v8::V8::InitializePlatform(platform);
		v8::V8::Initialize();
	}
#pragma managed(pop)

static JavascriptContext::JavascriptContext()
{
	UnmanagedInitialisation();
}


////////////////////////////////////////////////////////////////////////////////////////////////////

// Static function so it can be called from unmanaged code.
void FatalErrorCallback(const char* location, const char* message)
{
	JavascriptContext::FatalErrorCallbackMember(location, message);
	raise(SIGABRT);  // Exit immediately.
}

void JavascriptContext::FatalErrorCallbackMember(const char* location, const char* message)
{
	// Let's hope Out of Memory doesn't stop us allocating these strings!
	// I guess we can generally count on the garbage collector to find
	// us something, because it didn't have a chance to get involved if v8
	// has just run out.
	System::String ^location_str = gcnew System::String(location);
	System::String ^message_str = gcnew System::String(message);
	if (fatalErrorHandler != nullptr) {
		fatalErrorHandler(location_str, message_str);
	} else {
		System::Console::WriteLine(location_str);
		System::Console::WriteLine(message_str);
	}
}

JavascriptContext::JavascriptContext()
{
    // Unfortunately the fatal error handler is not installed early enough to catch
    // out-of-memory errors while creating new isolates
    // (see my post Catching V8::FatalProcessOutOfMemory while creating an isolate (SetFatalErrorHandler does not work)).
    // Also, HeapStatistics are only fetchable per-isolate, so they will not
    // easily allow us to work out whether we are about to run out (although they
    // would help us determine how much memory a new isolate used).
	v8::Isolate::CreateParams create_params;
	create_params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
	isolate = v8::Isolate::New(create_params);
	v8::Locker v8ThreadLock(isolate);
	v8::Isolate::Scope isolate_scope(isolate);

    isolate->SetFatalErrorHandler(FatalErrorCallback);

	mExternals = gcnew System::Collections::Generic::Dictionary<System::Object ^, WrappedJavascriptExternal>();
	HandleScope scope(isolate);
	mContext = new Persistent<Context>(isolate, Context::New(isolate));
}

////////////////////////////////////////////////////////////////////////////////////////////////////

JavascriptContext::~JavascriptContext()
{
	{
		v8::Locker v8ThreadLock(isolate);
		v8::Isolate::Scope isolate_scope(isolate);
		for each (WrappedJavascriptExternal wrapped in mExternals->Values)
			delete wrapped.Pointer;
		delete mContext;
		delete mExternals;
	}
	if (isolate != NULL)
		isolate->Dispose();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void JavascriptContext::SetFatalErrorHandler(FatalErrorHandler^ handler)
{
	fatalErrorHandler = handler;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void JavascriptContext::TerminateExecution()
{
	isolate->TerminateExecution();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool JavascriptContext::IsExecutionTerminating()
{
	return isolate->IsExecutionTerminating();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void
JavascriptContext::SetParameter(System::String^ iName, System::Object^ iObject)
{
	SetParameter(iName, iObject, SetParameterOptions::None);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void
JavascriptContext::SetParameter(System::String^ iName, System::Object^ iObject, SetParameterOptions options)
{
	pin_ptr<const wchar_t> namePtr = PtrToStringChars(iName);
	wchar_t* name = (wchar_t*) namePtr;
	JavascriptScope scope(this);
	v8::Isolate *isolate = JavascriptContext::GetCurrentIsolate();
	HandleScope handleScope(isolate);
	
	Handle<Value> value = JavascriptInterop::ConvertToV8(iObject);

	if (options != SetParameterOptions::None) {
		Handle<v8::Object> obj = value.As<v8::Object>();
		if (!obj.IsEmpty()) {
			Local<v8::External> wrap = obj->GetInternalField(0).As<v8::External>();
			if (!wrap.IsEmpty()) {
				JavascriptExternal* external = static_cast<JavascriptExternal*>(wrap->Value());
				external->SetOptions(options);
			}
		}
	}

	Local<Context>::New(isolate, *mContext)->Global()->Set(String::NewFromTwoByte(isolate, (uint16_t*)name), value);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

System::Object^
JavascriptContext::GetParameter(System::String^ iName)
{
	pin_ptr<const wchar_t> namePtr = PtrToStringChars(iName);
	wchar_t* name = (wchar_t*) namePtr;
	JavascriptScope scope(this);
	v8::Isolate *isolate = JavascriptContext::GetCurrentIsolate();
	HandleScope handleScope(isolate);
	
	Local<Value> value = Local<Context>::New(isolate, *mContext)->Global()->Get(String::NewFromTwoByte(isolate, (uint16_t*)name));
	return JavascriptInterop::ConvertFromV8(value);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

System::Object^
JavascriptContext::Run(System::String^ iScript)
{
	pin_ptr<const wchar_t> scriptPtr = PtrToStringChars(iScript);
	wchar_t* script = (wchar_t*)scriptPtr;
	JavascriptScope scope(this);
	//SetStackLimit();
	HandleScope handleScope(JavascriptContext::GetCurrentIsolate());
	Local<Value> ret;
	
	Local<Script> compiledScript = CompileScript(script);

	{
		TryCatch tryCatch;
		ret = (*compiledScript)->Run();

		if (ret.IsEmpty())
			throw gcnew JavascriptException(tryCatch);
	}
	
	return JavascriptInterop::ConvertFromV8(ret);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

System::Object^
JavascriptContext::Run(System::String^ iScript, System::String^ iScriptResourceName)
{
	pin_ptr<const wchar_t> scriptPtr = PtrToStringChars(iScript);
	wchar_t* script = (wchar_t*)scriptPtr;
	pin_ptr<const wchar_t> scriptResourceNamePtr = PtrToStringChars(iScriptResourceName);
	wchar_t* scriptResourceName = (wchar_t*)scriptResourceNamePtr;
	JavascriptScope scope(this);
	//SetStackLimit();
	HandleScope handleScope(JavascriptContext::GetCurrentIsolate());
	Local<Value> ret;	

	Local<Script> compiledScript = CompileScript(script, scriptResourceName);
	
	{
		TryCatch tryCatch;
		ret = (*compiledScript)->Run();

		if (ret.IsEmpty())
			throw gcnew JavascriptException(tryCatch);
	}
	
	return JavascriptInterop::ConvertFromV8(ret);
}

////////////////////////////////////////////////////////////////////////////////////////////////////


//void
//JavascriptContext::SetStackLimit()
//{
//    // This stack limit needs to be set for each Run because the
//    // stack of the caller could be in completely different spots (e.g.
//    // different threads), or have moved up/down because calls/returns.
//	v8::ResourceConstraints rc;
//
//    // Copied form v8/test/cctest/test-api.cc
//    uint32_t size = 500000;
//    uint32_t* limit = &size - (size / sizeof(size));
//    // If the size is very large and the stack is very near the bottom of
//    // memory then the calculation above may wrap around and give an address
//    // that is above the (downwards-growing) stack.  In that case we return
//    // a very low address.
//    if (limit > &size)
//        limit = reinterpret_cast<uint32_t*>(sizeof(size));
//    
//    int mos = rc.max_old_space_size();
//    
//    rc.set_stack_limit((uint32_t *)(limit));
//    rc.set_max_old_space_size(1700);
//	v8::SetResourceConstraints(isolate, &rc);
//}

////////////////////////////////////////////////////////////////////////////////////////////////////

JavascriptContext^
JavascriptContext::GetCurrent()
{
	return sCurrentContext;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

v8::Isolate *
JavascriptContext::GetCurrentIsolate()
{
	return sCurrentContext->isolate;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

v8::Locker *
JavascriptContext::Enter([System::Runtime::InteropServices::Out] JavascriptContext^% old_context)
{
	v8::Locker *locker = new v8::Locker(isolate);
	isolate->Enter();
    old_context = sCurrentContext;
	sCurrentContext = this;
	HandleScope scope(isolate);
	Local<Context>::New(isolate, *mContext)->Enter();
	return locker;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void
JavascriptContext::Exit(v8::Locker *locker, JavascriptContext^ old_context)
{
	{
		HandleScope scope(isolate);
		Local<Context>::New(isolate, *mContext)->Exit();
	}
	sCurrentContext = old_context;
	isolate->Exit();
	delete locker;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

// Exposed for the benefit of a regression test.
void
JavascriptContext::Collect()
{
    while(!this->isolate->IdleNotification(1)) {};
}

////////////////////////////////////////////////////////////////////////////////////////////////////

JavascriptExternal*
JavascriptContext::WrapObject(System::Object^ iObject)
{
	WrappedJavascriptExternal external_wrapped;
	if (mExternals->TryGetValue(iObject, external_wrapped))
	{
		// We've wrapped this guy before.
		return external_wrapped.Pointer;
	}
	else
	{
		JavascriptExternal* external = new JavascriptExternal(iObject);
		mExternals[iObject] = WrappedJavascriptExternal(external);
		return external;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////

Handle<ObjectTemplate>
JavascriptContext::GetObjectWrapperTemplate()
{
	if (objectWrapperTemplate == NULL)
		objectWrapperTemplate = new Persistent<ObjectTemplate>(isolate, JavascriptInterop::NewObjectWrapperTemplate());
	return Local<ObjectTemplate>::New(isolate, *objectWrapperTemplate);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

System::String^ JavascriptContext::V8Version::get()
{
	return gcnew System::String(v8::V8::GetVersion());
}

////////////////////////////////////////////////////////////////////////////////////////////////////

Local<Script>
CompileScript(wchar_t const *source_code, wchar_t const *resource_name)
{
	// convert source
	v8::Isolate *isolate = JavascriptContext::GetCurrentIsolate();
	Local<String> source = String::NewFromTwoByte(isolate, (uint16_t const *)source_code);

	// compile
	{
		TryCatch tryCatch;

		Local<Script> script;
		if (resource_name == NULL)
		{
			script = Script::Compile(source);
		}
		else
		{
			Local<String> resource = String::NewFromTwoByte(isolate, (uint16_t const *)resource_name);
			script = Script::Compile(source, resource);
		}

		if (script.IsEmpty())
			throw gcnew JavascriptException(tryCatch);

		return script;
	}
}

} } // namespace Noesis::Javascript

////////////////////////////////////////////////////////////////////////////////////////////////////
